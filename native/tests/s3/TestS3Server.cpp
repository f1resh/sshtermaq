#include "TestS3Server.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "s3/Xml.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <set>

namespace st::test {

namespace {

std::string enc(std::string_view s, bool slash) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || (c == '/' && !slash)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void civil(int64_t ms, int& y, int& mo, int& d, int& h, int& mi, int& s, int& wd) {
    int64_t t = ms / 1000;
    int64_t days = t / 86400;
    int64_t secs = t % 86400;
    wd = static_cast<int>((days + 4) % 7);
    days += 719468;
    const int64_t era = days / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (mo <= 2 ? 1 : 0));
    h = static_cast<int>(secs / 3600);
    mi = static_cast<int>(secs / 60 % 60);
    s = static_cast<int>(secs % 60);
}

std::string httpDate(int64_t ms) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, mo, d, h, mi, s, wd;
    civil(ms, y, mo, d, h, mi, s, wd);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[wd], d, kMonths[mo - 1], y, h, mi, s);
    return buf;
}

std::string isoDate(int64_t ms) {
    int y, mo, d, h, mi, s, wd;
    civil(ms, y, mo, d, h, mi, s, wd);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, h, mi, s, static_cast<int>(ms % 1000));
    return buf;
}

std::string md5hex(std::string_view s) { return hexEncode(crypto::md5(s)); }

TestHttpResponse xml(int status, std::string body) {
    TestHttpResponse r;
    r.status = status;
    r.headers = {{"Content-Type", "application/xml"}, {"x-amz-request-id", "TEST"}};
    r.body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + body;
    return r;
}

TestHttpResponse error(int status, const std::string& code, const std::string& message, const std::string& extra = {}) {
    return xml(status, "<Error><Code>" + code + "</Code><Message>" + s3::xmlEscape(message) + "</Message>" + extra +
                           "<RequestId>TEST</RequestId></Error>");
}

struct Upload {
    std::string bucket, key;
    TestS3Object proto;
    std::map<int, TestS3Object> parts;
};

}  // namespace

struct TestS3Server::Impl {
    TestS3Options opts;
    mutable std::mutex m;
    std::map<std::string, std::map<std::string, TestS3Object>> buckets;
    std::map<std::string, Upload> uploads;
    std::map<std::string, int> counts;
    int badSignatures = 0;
    int nextUpload = 1;
    std::function<std::optional<TestHttpResponse>(const TestHttpRequest&, const std::string&)> fault;
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::string host;  // "127.0.0.1:port"

    // ---- signatures ----

    /** Empty when the signature checks out (or the request is anonymous and allowed); else the error. */
    std::optional<TestHttpResponse> checkSignature(const TestHttpRequest& r, std::string& region) {
        const std::string auth = r.header("Authorization");
        const bool presigned = r.has("X-Amz-Signature");
        if (auth.empty() && !presigned) {
            if (opts.allowAnonymous) return std::nullopt;
            return error(403, "AccessDenied", "Anonymous access is not allowed");
        }
        std::string credential, signedHeaders, signature, date, payload;
        if (presigned) {
            credential = r.param("X-Amz-Credential");
            signedHeaders = r.param("X-Amz-SignedHeaders");
            signature = r.param("X-Amz-Signature");
            date = r.param("X-Amz-Date");
            payload = "UNSIGNED-PAYLOAD";
            const int64_t expires = std::stoll(r.param("X-Amz-Expires").empty() ? "0" : r.param("X-Amz-Expires"));
            if (expires < 1 || expires > 604800) return error(400, "AuthorizationQueryParametersError", "bad X-Amz-Expires");
        } else {
            auto field = [&](const std::string& name) {
                const size_t p = auth.find(name + "=");
                if (p == std::string::npos) return std::string();
                const size_t start = p + name.size() + 1;
                const size_t end = auth.find(',', start);
                return auth.substr(start, end == std::string::npos ? std::string::npos : end - start);
            };
            if (!startsWith(auth, "AWS4-HMAC-SHA256 ")) return error(400, "InvalidArgument", "Unsupported Authorization");
            credential = field("Credential");
            signedHeaders = field("SignedHeaders");
            signature = field("Signature");
            date = r.header("x-amz-date");
            payload = r.header("x-amz-content-sha256");
            if (payload.empty()) return error(400, "InvalidRequest", "Missing x-amz-content-sha256");
            if (payload != "UNSIGNED-PAYLOAD" && payload != hexEncode(crypto::sha256(r.body)))
                return error(400, "XAmzContentSHA256Mismatch", "The body does not match x-amz-content-sha256");
        }
        const auto parts = split(credential, '/');
        if (parts.size() != 5 || parts[0] != opts.accessKeyId) return error(403, "InvalidAccessKeyId", "Unknown key");
        region = parts[2];
        // Canonical query: every parameter but the signature, decoded and encoded again the SigV4 way.
        std::vector<std::pair<std::string, std::string>> q;
        for (std::string_view rest = r.query; !rest.empty();) {
            const size_t amp = rest.find('&');
            const std::string_view part = rest.substr(0, amp);
            rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
            const size_t eq = part.find('=');
            const std::string name = percentDecode(part.substr(0, eq));
            if (name == "X-Amz-Signature") continue;
            q.emplace_back(enc(name, true), enc(eq == std::string_view::npos ? std::string() : percentDecode(part.substr(eq + 1)), true));
        }
        std::sort(q.begin(), q.end());
        std::string cq;
        for (const auto& [n, v] : q) cq += (cq.empty() ? "" : "&") + n + "=" + v;
        std::string ch;
        for (const std::string& h : split(signedHeaders, ';')) {
            std::string v = r.header(h);
            ch += h + ":" + trim(v) + "\n";
        }
        const std::string canonical = r.method + "\n" + r.path + "\n" + cq + "\n" + ch + "\n" + signedHeaders + "\n" + payload;
        const std::string scope = parts[1] + "/" + parts[2] + "/" + parts[3] + "/aws4_request";
        const std::string sts = "AWS4-HMAC-SHA256\n" + date + "\n" + scope + "\n" + hexEncode(crypto::sha256(canonical));
        std::string k = crypto::hmacSha256("AWS4" + opts.secret, parts[1]);
        k = crypto::hmacSha256(k, parts[2]);
        k = crypto::hmacSha256(k, parts[3]);
        k = crypto::hmacSha256(k, "aws4_request");
        if (hexEncode(crypto::hmacSha256(k, sts)) != signature) {
            std::lock_guard lk(m);
            ++badSignatures;
            return error(403, "SignatureDoesNotMatch",
                         "The request signature we calculated does not match the signature you provided.",
                         "<CanonicalRequest>" + s3::xmlEscape(canonical) + "</CanonicalRequest>");
        }
        return std::nullopt;
    }

    // ---- requests ----

    static std::string operation(const TestHttpRequest& r, const std::string& bucket, const std::string& key) {
        if (bucket.empty()) return "ListBuckets";
        if (key.empty()) {
            if (r.method == "HEAD") return "HeadBucket";
            if (r.method == "POST" && r.has("delete")) return "DeleteObjects";
            if (r.method == "GET") return r.param("list-type") == "2" ? "ListObjectsV2" : "ListObjects";
            return "Bucket" + r.method;
        }
        const bool copy = !r.header("x-amz-copy-source").empty();
        if (r.method == "HEAD") return "HeadObject";
        if (r.method == "GET") return r.has("uploadId") ? "ListParts" : "GetObject";
        if (r.method == "PUT") {
            if (r.has("partNumber")) return copy ? "UploadPartCopy" : "UploadPart";
            return copy ? "CopyObject" : "PutObject";
        }
        if (r.method == "POST") return r.has("uploads") ? "CreateMultipartUpload" : "CompleteMultipartUpload";
        if (r.method == "DELETE") return r.has("uploadId") ? "AbortMultipartUpload" : "DeleteObject";
        return "Unknown";
    }

    TestHttpResponse handle(const TestHttpRequest& r) {
        const int now = ++concurrent;
        for (int seen = maxConcurrent.load(); now > seen && !maxConcurrent.compare_exchange_weak(seen, now);) {
        }
        struct Leave {
            std::atomic<int>& c;
            ~Leave() { --c; }
        } leave{concurrent};

        // Bucket and key from the path (path-style) or the host (virtual-hosted).
        std::string bucket, key;
        const std::string hostHeader = r.header("Host");
        std::string path = r.path;
        if (hostHeader != host && hostHeader.size() > host.size() && endsWith(hostHeader, "." + host)) {
            bucket = hostHeader.substr(0, hostHeader.size() - host.size() - 1);
            key = percentDecode(path.size() > 1 ? path.substr(1) : std::string());
        } else {
            const std::string rest = path.size() > 1 ? path.substr(1) : std::string();
            const size_t slash = rest.find('/');
            bucket = percentDecode(rest.substr(0, slash));
            key = slash == std::string::npos ? std::string() : percentDecode(rest.substr(slash + 1));
        }
        const std::string op = operation(r, bucket, key);
        std::function<std::optional<TestHttpResponse>(const TestHttpRequest&, const std::string&)> f;
        {
            std::lock_guard lk(m);
            ++counts[op];
            f = fault;
        }
        if (f)
            if (auto injected = f(r, op)) return *injected;

        std::string region;
        if (auto bad = checkSignature(r, region)) return *bad;
        if (!bucket.empty() && !region.empty()) {
            const auto it = opts.bucketRegions.find(bucket);
            const std::string actual = it == opts.bucketRegions.end() ? opts.region : it->second;
            if (region != actual) {
                TestHttpResponse e = error(400, "AuthorizationHeaderMalformed",
                                           "the region '" + region + "' is wrong; expecting '" + actual + "'",
                                           "<Region>" + actual + "</Region>");
                e.headers.emplace_back("x-amz-bucket-region", actual);
                if (r.method == "HEAD") e.status = 301;
                return e;
            }
        }
        std::lock_guard lk(m);
        if (op == "ListBuckets") {
            std::string out = "<ListAllMyBucketsResult><Owner><ID>test</ID></Owner><Buckets>";
            for (const auto& [name, _] : buckets)
                out += "<Bucket><Name>" + s3::xmlEscape(name) + "</Name><CreationDate>2026-01-01T00:00:00.000Z</CreationDate></Bucket>";
            return xml(200, out + "</Buckets></ListAllMyBucketsResult>");
        }
        auto b = buckets.find(bucket);
        if (b == buckets.end()) {
            if (op == "HeadBucket") return TestHttpResponse{404};
            return error(404, "NoSuchBucket", "The specified bucket does not exist");
        }
        auto& objects = b->second;
        if (op == "HeadBucket") {
            TestHttpResponse ok;
            ok.headers = {{"x-amz-bucket-region", region.empty() ? opts.region : region}};
            return ok;
        }
        if (op == "ListObjectsV2") return list(r, objects);
        if (op == "DeleteObjects") {
            std::string out = "<DeleteResult>";
            try {
                const s3::XmlNode doc = s3::parseXml(r.body);
                if (r.header("Content-MD5") != crypto::base64Encode(crypto::md5(r.body)))
                    return error(400, "InvalidDigest", "Content-MD5 does not match");
                for (const s3::XmlNode* o : doc.all("Object")) {
                    const std::string k = o->childText("Key");
                    if (k.find("undeletable") != std::string::npos)
                        out += "<Error><Key>" + s3::xmlEscape(k) + "</Key><Code>AccessDenied</Code><Message>Access Denied</Message></Error>";
                    else
                        objects.erase(k);
                }
            } catch (const s3::XmlError&) {
                return error(400, "MalformedXML", "bad XML");
            }
            return xml(200, out + "</DeleteResult>");
        }
        if (op == "HeadObject" || op == "GetObject") {
            auto o = objects.find(key);
            if (o == objects.end()) return op == "HeadObject" ? TestHttpResponse{404} : error(404, "NoSuchKey", "The specified key does not exist.");
            return getObject(r, o->second, op == "HeadObject");
        }
        if (op == "PutObject") {
            TestS3Object o = fromHeaders(r);
            o.data = r.body;
            o.etag = md5hex(r.body);
            if (opts.discardData) {
                o.generatedSize = r.body.size();
                o.data.clear();
            }
            TestHttpResponse ok;
            ok.headers = {{"ETag", "\"" + o.etag + "\""}};
            objects[key] = std::move(o);
            return ok;
        }
        if (op == "CopyObject" || op == "UploadPartCopy") {
            std::string src = percentDecode(r.header("x-amz-copy-source"));
            if (startsWith(src, "/")) src.erase(0, 1);
            const size_t slash = src.find('/');
            auto sb = buckets.find(src.substr(0, slash));
            if (sb == buckets.end() || slash == std::string::npos) return error(404, "NoSuchBucket", "source bucket");
            auto so = sb->second.find(src.substr(slash + 1));
            if (so == sb->second.end()) return error(404, "NoSuchKey", "source key");
            if (so->second.storageClass == "GLACIER" || so->second.storageClass == "DEEP_ARCHIVE")
                return error(403, "InvalidObjectState", "The operation is not valid for the object's storage class");
            const TestS3Object source = so->second;
            if (op == "UploadPartCopy") {
                auto u = uploads.find(r.param("uploadId"));
                if (u == uploads.end()) return error(404, "NoSuchUpload", "The specified upload does not exist.");
                const std::string range = r.header("x-amz-copy-source-range");
                uint64_t a = 0, z = source.size() - 1;
                if (std::sscanf(range.c_str(), "bytes=%llu-%llu", reinterpret_cast<unsigned long long*>(&a),
                                reinterpret_cast<unsigned long long*>(&z)) != 2 || z >= source.size() || a > z)
                    return error(400, "InvalidArgument", "bad copy range");
                TestS3Object part;
                part.data = bytes(source, a, z - a + 1);
                part.etag = md5hex(part.data);
                u->second.parts[std::stoi(r.param("partNumber"))] = part;
                return xml(200, "<CopyPartResult><ETag>\"" + part.etag + "\"</ETag></CopyPartResult>");
            }
            TestS3Object copy = source;
            if (r.header("x-amz-metadata-directive") == "REPLACE") {
                TestS3Object h = fromHeaders(r);
                copy.metadata = h.metadata;
                copy.contentType = h.contentType;
            }
            const std::string sc = r.header("x-amz-storage-class");
            copy.storageClass = sc.empty() ? "STANDARD" : sc;  // as AWS: a copy is STANDARD unless told otherwise
            copy.lastModifiedMs = nowMs();
            objects[key] = copy;
            return xml(200, "<CopyObjectResult><ETag>\"" + copy.etag + "\"</ETag><LastModified>" + isoDate(copy.lastModifiedMs) +
                                "</LastModified></CopyObjectResult>");
        }
        if (op == "CreateMultipartUpload") {
            const std::string id = "upload-" + std::to_string(nextUpload++);
            uploads[id] = Upload{bucket, key, fromHeaders(r), {}};
            return xml(200, "<InitiateMultipartUploadResult><Bucket>" + s3::xmlEscape(bucket) + "</Bucket><Key>" +
                                s3::xmlEscape(key) + "</Key><UploadId>" + id + "</UploadId></InitiateMultipartUploadResult>");
        }
        auto u = uploads.find(r.param("uploadId"));
        if (op == "UploadPart" || op == "ListParts" || op == "CompleteMultipartUpload" || op == "AbortMultipartUpload") {
            if (u == uploads.end()) return error(404, "NoSuchUpload", "The specified upload does not exist.");
        }
        if (op == "UploadPart") {
            TestS3Object part;
            part.etag = md5hex(r.body);
            if (opts.discardData)
                part.generatedSize = r.body.size();
            else
                part.data = r.body;
            u->second.parts[std::stoi(r.param("partNumber"))] = part;
            TestHttpResponse ok;
            ok.headers = {{"ETag", "\"" + part.etag + "\""}};
            return ok;
        }
        if (op == "ListParts") {
            std::string out = "<ListPartsResult><UploadId>" + u->first + "</UploadId><IsTruncated>false</IsTruncated>";
            for (const auto& [n, p] : u->second.parts)
                out += "<Part><PartNumber>" + std::to_string(n) + "</PartNumber><ETag>\"" + p.etag + "\"</ETag><Size>" +
                       std::to_string(p.size()) + "</Size></Part>";
            return xml(200, out + "</ListPartsResult>");
        }
        if (op == "AbortMultipartUpload") {
            uploads.erase(u);
            return TestHttpResponse{204};
        }
        if (op == "CompleteMultipartUpload") {
            TestS3Object o = u->second.proto;
            std::string md5s;
            int last = 0, n = 0;
            try {
                const s3::XmlNode doc = s3::parseXml(r.body);
                for (const s3::XmlNode* p : doc.all("Part")) {
                    const int num = std::stoi(p->childText("PartNumber"));
                    const std::string etag = p->childText("ETag");
                    auto part = u->second.parts.find(num);
                    if (num <= last) return error(400, "InvalidPartOrder", "parts out of order");
                    if (part == u->second.parts.end() || "\"" + part->second.etag + "\"" != etag)
                        return error(400, "InvalidPart", "One or more of the specified parts could not be found.");
                    last = num;
                    ++n;
                    std::string raw;
                    hexDecode(part->second.etag, raw);
                    md5s += raw;
                    if (part->second.generatedSize)
                        o.generatedSize += part->second.generatedSize;
                    else
                        o.data += part->second.data;
                }
            } catch (const std::exception&) {
                return error(400, "MalformedXML", "bad XML");
            }
            if (n == 0) return error(400, "MalformedXML", "no parts");
            if (opts.discardData) {
                o.generatedSize += o.data.size();
                o.data.clear();
            }
            o.etag = md5hex(md5s) + "-" + std::to_string(n);
            o.lastModifiedMs = nowMs();
            buckets[u->second.bucket][u->second.key] = o;
            uploads.erase(u);
            return xml(200, "<CompleteMultipartUploadResult><Key>" + s3::xmlEscape(key) + "</Key><ETag>\"" + o.etag +
                                "\"</ETag></CompleteMultipartUploadResult>");
        }
        if (op == "DeleteObject") {
            objects.erase(key);
            return TestHttpResponse{204};
        }
        return error(501, "NotImplemented", op + " is not implemented by the test server");
    }

    static TestS3Object fromHeaders(const TestHttpRequest& r) {
        TestS3Object o;
        o.contentType = r.header("Content-Type");
        o.lastModifiedMs = nowMs();
        for (const auto& [n, v] : r.headers) {
            const std::string name = toLower(n);
            if (startsWith(name, "x-amz-meta-")) o.metadata[name.substr(11)] = v;
        }
        const std::string sc = r.header("x-amz-storage-class");
        if (!sc.empty()) o.storageClass = sc;
        return o;
    }

    static std::string bytes(const TestS3Object& o, uint64_t offset, uint64_t n) {
        if (!o.generatedSize) return o.data.substr(static_cast<size_t>(offset), static_cast<size_t>(n));
        std::string out(static_cast<size_t>(n), '\0');
        for (uint64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = pattern(offset + i);
        return out;
    }

    TestHttpResponse getObject(const TestHttpRequest& r, const TestS3Object& o, bool head) {
        const std::string quoted = "\"" + o.etag + "\"";
        if (const std::string im = r.header("If-Match"); !im.empty() && im != quoted)
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        TestHttpResponse out;
        out.headers = {{"ETag", quoted}, {"Last-Modified", httpDate(o.lastModifiedMs)}, {"Accept-Ranges", "bytes"}};
        if (!o.contentType.empty()) out.headers.emplace_back("Content-Type", o.contentType);
        if (o.storageClass != "STANDARD") out.headers.emplace_back("x-amz-storage-class", o.storageClass);
        for (const auto& [k, v] : o.metadata) out.headers.emplace_back("x-amz-meta-" + k, v);
        const uint64_t size = o.size();
        uint64_t first = 0, last = size ? size - 1 : 0;
        if (const std::string range = r.header("Range"); !range.empty() && !head) {
            unsigned long long a = 0, z = 0;
            const int got = std::sscanf(range.c_str(), "bytes=%llu-%llu", &a, &z);
            if (got < 1 || a >= size) return error(416, "InvalidRange", "The requested range is not satisfiable");
            first = a;
            if (got == 2) last = std::min<uint64_t>(z, size - 1);
            out.status = 206;
            out.headers.emplace_back("Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size));
        }
        const uint64_t length = size ? last - first + 1 : 0;
        if (head) {
            out.headers.emplace_back("Content-Length", std::to_string(size));
            return out;
        }
        if (o.generatedSize) {
            out.generatedSize = length;
            out.generate = [first](uint64_t off, char* p, size_t n) {
                for (size_t i = 0; i < n; ++i) p[i] = pattern(first + off + i);
            };
        } else {
            out.body = o.data.substr(static_cast<size_t>(first), static_cast<size_t>(length));
        }
        return out;
    }

    TestHttpResponse list(const TestHttpRequest& r, const std::map<std::string, TestS3Object>& objects) {
        const std::string prefix = r.param("prefix");
        const std::string delimiter = r.param("delimiter");
        const bool url = r.param("encoding-type") == "url";
        const int maxKeys = std::clamp(r.param("max-keys").empty() ? 1000 : std::stoi(r.param("max-keys")), 1, 1000);
        std::string after;
        if (const std::string tok = r.param("continuation-token"); !tok.empty()) crypto::base64Decode(tok, after);
        auto e = [&](const std::string& s) { return s3::xmlEscape(url ? enc(s, false) : s); };
        std::string body;
        std::set<std::string> prefixes;
        int n = 0;
        std::string lastSeen;
        bool truncated = false;
        for (auto it = objects.lower_bound(std::max(prefix, after)); it != objects.end(); ++it) {
            const std::string& k = it->first;
            if (!startsWith(k, prefix)) break;
            if (!after.empty() && k <= after) continue;
            std::string folder;
            if (!delimiter.empty()) {
                const size_t d = k.find(delimiter, prefix.size());
                if (d != std::string::npos) folder = k.substr(0, d + delimiter.size());
            }
            if (!folder.empty() && prefixes.count(folder)) continue;
            if (n == maxKeys) {
                truncated = true;
                break;
            }
            ++n;
            if (!folder.empty()) {
                prefixes.insert(folder);
                body += "<CommonPrefixes><Prefix>" + e(folder) + "</Prefix></CommonPrefixes>";
                lastSeen = folder + "\xff";  // skips everything in that folder on the next page
            } else {
                const TestS3Object& o = it->second;
                body += "<Contents><Key>" + e(k) + "</Key><LastModified>" + isoDate(o.lastModifiedMs) + "</LastModified><ETag>\"" +
                        o.etag + "\"</ETag><Size>" + std::to_string(o.size()) + "</Size><StorageClass>" + o.storageClass +
                        "</StorageClass></Contents>";
                lastSeen = k;
            }
        }
        std::string out = "<ListBucketResult><Name>bucket</Name><Prefix>" + e(prefix) + "</Prefix><KeyCount>" +
                          std::to_string(n) + "</KeyCount><MaxKeys>" + std::to_string(maxKeys) + "</MaxKeys>";
        if (url) out += "<EncodingType>url</EncodingType>";
        out += std::string("<IsTruncated>") + (truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated) out += "<NextContinuationToken>" + crypto::base64Encode(lastSeen) + "</NextContinuationToken>";
        return xml(200, out + body + "</ListBucketResult>");
    }
};

TestS3Server::TestS3Server(TestS3Options options) : impl_(std::make_shared<Impl>()) {
    impl_->opts = std::move(options);
    TestHttpServerOptions h;
    h.tls = impl_->opts.tls;
    h.port = impl_->opts.port;
    std::weak_ptr<Impl> weak = impl_;
    http_ = std::make_unique<TestHttpServer>(
        [weak](const TestHttpRequest& r) {
            auto i = weak.lock();
            return i ? i->handle(r) : TestHttpResponse{503};
        },
        h);
    impl_->host = "127.0.0.1:" + std::to_string(http_->port());
}

TestS3Server::~TestS3Server() { http_->stop(); }

std::string TestS3Server::endpoint() const { return http_->url(); }
TestHttpServer& TestS3Server::http() { return *http_; }

void TestS3Server::createBucket(const std::string& name) {
    std::lock_guard lk(impl_->m);
    impl_->buckets[name];
}

void TestS3Server::put(const std::string& bucket, const std::string& key, std::string data, int64_t lastModifiedMs,
                       std::map<std::string, std::string> metadata) {
    TestS3Object o;
    o.etag = md5hex(data);
    o.data = std::move(data);
    o.lastModifiedMs = lastModifiedMs ? lastModifiedMs : nowMs();
    o.metadata = std::move(metadata);
    std::lock_guard lk(impl_->m);
    impl_->buckets[bucket][key] = std::move(o);
}

void TestS3Server::putGenerated(const std::string& bucket, const std::string& key, uint64_t size) {
    TestS3Object o;
    o.generatedSize = size;
    o.etag = "gen" + std::to_string(size);
    o.lastModifiedMs = nowMs();
    std::lock_guard lk(impl_->m);
    impl_->buckets[bucket][key] = std::move(o);
}

std::optional<TestS3Object> TestS3Server::get(const std::string& bucket, const std::string& key) const {
    std::lock_guard lk(impl_->m);
    auto b = impl_->buckets.find(bucket);
    if (b == impl_->buckets.end()) return std::nullopt;
    auto o = b->second.find(key);
    if (o == b->second.end()) return std::nullopt;
    return o->second;
}

std::vector<std::string> TestS3Server::keys(const std::string& bucket) const {
    std::lock_guard lk(impl_->m);
    std::vector<std::string> out;
    auto b = impl_->buckets.find(bucket);
    if (b != impl_->buckets.end())
        for (const auto& [k, _] : b->second) out.push_back(k);
    return out;
}

int TestS3Server::openUploads() const {
    std::lock_guard lk(impl_->m);
    return static_cast<int>(impl_->uploads.size());
}

int TestS3Server::badSignatures() const {
    std::lock_guard lk(impl_->m);
    return impl_->badSignatures;
}

int TestS3Server::count(const std::string& operation) const {
    std::lock_guard lk(impl_->m);
    auto it = impl_->counts.find(operation);
    return it == impl_->counts.end() ? 0 : it->second;
}

void TestS3Server::setFault(std::function<std::optional<TestHttpResponse>(const TestHttpRequest&, const std::string&)> f) {
    std::lock_guard lk(impl_->m);
    impl_->fault = std::move(f);
}

int TestS3Server::maxConcurrent() const { return impl_->maxConcurrent.load(); }

}  // namespace st::test
