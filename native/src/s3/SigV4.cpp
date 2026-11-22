#include "s3/SigV4.h"

#include "core/Crypto.h"
#include "core/Strings.h"

#include <algorithm>
#include <cstdio>

namespace st::s3 {

namespace {

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

/** Trims and collapses runs of spaces, tabs and line breaks (SigV4 canonical header values; a folded value becomes
 *  one line). */
std::string canonicalValue(std::string_view v) {
    std::string out;
    bool space = false;
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

struct Canonical {
    std::string headers;        // "name:value\n" ...
    std::string signedHeaders;  // "a;b;c"
};

Canonical canonicalHeaders(const Params& headers) {
    std::vector<std::pair<std::string, std::string>> h;
    for (const auto& [n, v] : headers) {
        std::string name = toLower(n);
        auto it = std::find_if(h.begin(), h.end(), [&](const auto& p) { return p.first == name; });
        if (it != h.end())
            it->second += "," + canonicalValue(v);
        else
            h.emplace_back(std::move(name), canonicalValue(v));
    }
    std::sort(h.begin(), h.end());
    Canonical c;
    for (const auto& [n, v] : h) {
        c.headers += n + ":" + v + "\n";
        if (!c.signedHeaders.empty()) c.signedHeaders += ';';
        c.signedHeaders += n;
    }
    return c;
}

std::string signingKey(const std::string& secret, const std::string& date, const std::string& region,
                       const std::string& service) {
    std::string k = crypto::hmacSha256("AWS4" + secret, date);
    k = crypto::hmacSha256(k, region);
    k = crypto::hmacSha256(k, service);
    return crypto::hmacSha256(k, "aws4_request");
}

bool hasHeader(const Params& headers, std::string_view name) {
    return std::any_of(headers.begin(), headers.end(), [&](const auto& p) { return iequals(p.first, name); });
}

/** Fills canonicalRequest, stringToSign and signature of `out`. */
void computeSignature(const SignInput& in, const AwsCredentials& creds, const std::string& query,
                      const Canonical& ch, const std::string& payload, const std::string& date, Signed& out) {
    const std::string day = date.substr(0, 8);
    const std::string scope = day + "/" + in.region + "/" + in.service + "/aws4_request";
    out.canonicalRequest = in.method + "\n" + in.encodedPath + "\n" + query + "\n" + ch.headers + "\n" +
                           ch.signedHeaders + "\n" + payload;
    out.stringToSign = "AWS4-HMAC-SHA256\n" + date + "\n" + scope + "\n" + hexEncode(crypto::sha256(out.canonicalRequest));
    std::string key = signingKey(creds.secretAccessKey, day, in.region, in.service);
    out.signature = hexEncode(crypto::hmacSha256(key, out.stringToSign));
    crypto::cleanse(key);
}

}  // namespace

std::string uriEncode(std::string_view s, bool encodeSlash) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (unreserved(c) || (c == '/' && !encodeSlash)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string amzDate(int64_t t) {
    // Civil date from days since 1970-01-01 (H. Hinnant's algorithm).
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t secs = t - days * 86400;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02dT%02d%02d%02dZ", static_cast<int>(y), static_cast<int>(m),
                  static_cast<int>(d), static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60),
                  static_cast<int>(secs % 60));
    return buf;
}

std::string payloadHash(std::string_view body) { return hexEncode(crypto::sha256(body)); }

std::string canonicalQuery(const Params& query) {
    std::vector<std::pair<std::string, std::string>> q;
    q.reserve(query.size());
    for (const auto& [n, v] : query) q.emplace_back(uriEncode(n), uriEncode(v));
    std::sort(q.begin(), q.end());
    std::string out;
    for (const auto& [n, v] : q) {
        if (!out.empty()) out += '&';
        out += n + "=" + v;
    }
    return out;
}

Signed sign(const SignInput& in, const AwsCredentials& creds) {
    Signed out;
    out.headers = in.headers;
    if (!hasHeader(out.headers, "host")) out.headers.insert(out.headers.begin(), {"Host", in.host});
    out.query = canonicalQuery(in.query);
    if (creds.anonymous) return out;

    const std::string date = amzDate(in.now);
    out.headers.emplace_back("X-Amz-Date", date);
    if (in.service == "s3" && !hasHeader(out.headers, "x-amz-content-sha256"))
        out.headers.emplace_back("X-Amz-Content-Sha256", in.payloadHash);
    if (!creds.sessionToken.empty()) out.headers.emplace_back("X-Amz-Security-Token", creds.sessionToken);

    const Canonical ch = canonicalHeaders(out.headers);
    computeSignature(in, creds, out.query, ch, in.payloadHash, date, out);
    const std::string day = date.substr(0, 8);
    out.headers.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=" + creds.accessKeyId + "/" + day + "/" +
                                                  in.region + "/" + in.service + "/aws4_request, SignedHeaders=" +
                                                  ch.signedHeaders + ", Signature=" + out.signature);
    return out;
}

Signed presign(const SignInput& in, const AwsCredentials& creds, int64_t expiresSeconds) {
    Signed out;
    out.headers = in.headers;
    if (!hasHeader(out.headers, "host")) out.headers.insert(out.headers.begin(), {"Host", in.host});
    if (creds.anonymous) {
        out.query = canonicalQuery(in.query);
        return out;
    }
    const std::string date = amzDate(in.now);
    const std::string day = date.substr(0, 8);
    Params q = in.query;
    q.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    q.emplace_back("X-Amz-Credential", creds.accessKeyId + "/" + day + "/" + in.region + "/" + in.service + "/aws4_request");
    q.emplace_back("X-Amz-Date", date);
    q.emplace_back("X-Amz-Expires", std::to_string(std::clamp<int64_t>(expiresSeconds, 1, 604800)));
    if (!creds.sessionToken.empty()) q.emplace_back("X-Amz-Security-Token", creds.sessionToken);
    const Canonical ch = canonicalHeaders(out.headers);
    q.emplace_back("X-Amz-SignedHeaders", ch.signedHeaders);
    const std::string query = canonicalQuery(q);
    // S3 links are for any body; other services sign the body's hash.
    const std::string payload = in.service == "s3" ? std::string(kUnsignedPayload) : in.payloadHash;
    computeSignature(in, creds, query, ch, payload, date, out);
    out.query = query + "&X-Amz-Signature=" + out.signature;
    return out;
}

}  // namespace st::s3
