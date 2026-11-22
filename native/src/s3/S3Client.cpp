#include "s3/S3Client.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "platform/sys/Http.h"
#include "s3/Dates.h"
#include "s3/Xml.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <random>
#include <thread>

namespace st::s3 {

namespace {

constexpr std::string_view kAwsSuffix = ".amazonaws.com";

bool isRetryableCode(const std::string& code) {
    return code == "SlowDown" || code == "InternalError" || code == "RequestTimeout" || code == "ServiceUnavailable" ||
           code == "Throttling" || code == "ThrottlingException" || code == "RequestLimitExceeded" ||
           code == "OperationAborted";
}

bool isExpiredCode(const std::string& code) {
    return code == "ExpiredToken" || code == "TokenRefreshRequired" || code == "ExpiredTokenException";
}

/** S3's "encoding-type=url" answers: form-style percent-encoding ('+' is a space). */
std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

uint64_t toU64(const std::string& s) {
    try {
        return s.empty() ? 0 : std::stoull(s);
    } catch (...) {
        return 0;
    }
}

S3Head headFrom(const Params& headers, uint64_t contentLength) {
    S3Head h;
    h.size = contentLength;
    for (const auto& [n, v] : headers) {
        const std::string name = toLower(n);
        if (name == "etag") h.etag = unquoteEtag(v);
        else if (name == "last-modified") h.lastModifiedMs = parseHttpDate(v).value_or(0);
        else if (name == "content-type") h.contentType = v;
        else if (name == "x-amz-storage-class") h.storageClass = v;
        else if (startsWith(name, "x-amz-meta-")) h.metadata[name.substr(11)] = v;
        else if (name == "content-range") {
            const size_t slash = v.find('/');
            if (slash != std::string::npos && v.substr(slash + 1) != "*") h.size = toU64(v.substr(slash + 1));
        }
    }
    if (h.storageClass.empty()) h.storageClass = "STANDARD";
    return h;
}

void putHeaders(const S3PutOptions& o, Params& h) {
    if (!o.contentType.empty()) h.emplace_back("Content-Type", o.contentType);
    for (const auto& [k, v] : o.metadata) h.emplace_back("x-amz-meta-" + k, v);
    if (!o.storageClass.empty() && o.storageClass != "STANDARD") h.emplace_back("x-amz-storage-class", o.storageClass);
}

}  // namespace

std::string unquoteEtag(std::string_view etag) {
    if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') etag = etag.substr(1, etag.size() - 2);
    if (startsWith(etag, "W/")) etag.remove_prefix(2);
    return std::string(etag);
}

bool isValidBucketName(std::string_view n) {
    if (n.size() < 3 || n.size() > 63) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    return std::isalnum(static_cast<unsigned char>(n.front())) && std::isalnum(static_cast<unsigned char>(n.back()));
}

std::string S3Client::Result::header(std::string_view name) const {
    for (const auto& [n, v] : headers)
        if (iequals(n, name)) return v;
    return {};
}

S3Client::S3Client(std::shared_ptr<sys::HttpSession> http, std::shared_ptr<CredentialProvider> creds, S3Config cfg)
    : http_(std::move(http)), creds_(std::move(creds)), cfg_(std::move(cfg)) {
    if (cfg_.region.empty()) cfg_.region = "us-east-1";
    if (!cfg_.endpoint.empty()) {
        sys::UrlParts u;
        if (!sys::splitUrl(cfg_.endpoint, u)) throw S3Error("Invalid endpoint: " + cfg_.endpoint);
        scheme_ = u.scheme;
        baseHost_ = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
        if (u.port && u.port != (u.scheme == "https" ? 443 : 80)) baseHost_ += ":" + std::to_string(u.port);
        // An endpoint that is AWS itself behaves like no endpoint (regions, virtual hosts).
        if (endsWith(toLower(u.host), kAwsSuffix) && startsWith(toLower(u.host), "s3") && u.scheme == "https" && !u.port)
            baseHost_.clear();
    }
}

std::string S3Client::endpointDescription() const {
    return baseHost_.empty() ? "AWS (" + cfg_.region + ")" : scheme_ + "://" + baseHost_;
}

int64_t S3Client::now() const {
    if (testNow_) return testNow_;
    return static_cast<int64_t>(std::time(nullptr)) + skewSeconds_.load();
}

std::string S3Client::regionFor(const std::string& bucket) {
    std::lock_guard lk(m_);
    auto it = regions_.find(bucket);
    return it == regions_.end() ? cfg_.region : it->second;
}

std::string S3Client::urlFor(const std::string& bucket, const std::string& key, const std::string& region,
                             std::string& host, std::string& encodedPath) const {
    const bool aws = baseHost_.empty();
    // Virtual hosts need a bucket name that is a valid host label: AWS's certificate does not cover dotted names.
    const bool pathStyle = bucket.empty() || cfg_.pathStyle || (aws && bucket.find('.') != std::string::npos) ||
                           bucket.find('_') != std::string::npos;
    const std::string base = aws ? "s3." + region + std::string(kAwsSuffix) : baseHost_;
    if (pathStyle) {
        host = base;
        encodedPath = bucket.empty() ? "/" : "/" + uriEncode(bucket) + (key.empty() ? "" : "/" + uriEncode(key, false));
    } else {
        host = bucket + "." + base;
        encodedPath = "/" + uriEncode(key, false);
    }
    return scheme_ + "://" + host + encodedPath;
}

S3Error S3Client::errorFrom(int status, const std::string& body, const Params& headers, const std::string& what) const {
    std::string code, message;
    if (!body.empty()) {
        try {
            const XmlNode e = parseXml(body);
            const XmlNode* err = e.name == "Error" ? &e : e.child("Error");
            if (err) {
                code = err->childText("Code");
                message = err->childText("Message");
            }
        } catch (const XmlError&) {
        }
    }
    if (code.empty()) {
        switch (status) {
            case 301: code = "PermanentRedirect"; break;
            case 403: code = "AccessDenied"; break;
            case 404: code = "NotFound"; break;
            case 412: code = "PreconditionFailed"; break;
            case 416: code = "InvalidRange"; break;
            case 503: code = "SlowDown"; break;
            default: break;
        }
    }
    std::string text;
    if (code == "AccessDenied" || code == "AllAccessDisabled")
        text = "Access denied: the credentials have no permission for this";
    else if (code == "NoSuchBucket")
        text = "The bucket does not exist";
    else if (code == "NoSuchKey" || code == "NotFound")
        text = "Not found";
    else if (code == "NoSuchUpload")
        text = "The interrupted upload is gone from the server (it was completed, aborted or expired)";
    else if (code == "InvalidAccessKeyId")
        text = "The access key ID is not known to the service";
    else if (code == "SignatureDoesNotMatch")
        text = "The secret access key does not match the access key ID (or this computer's clock is off)";
    else if (code == "RequestTimeTooSkewed")
        text = "This computer's clock differs from the server's by more than 15 minutes";
    else if (isExpiredCode(code))
        text = "The temporary credentials have expired";
    else if (code == "InvalidToken")
        text = "The session token is not valid";
    else if (code == "PreconditionFailed")
        text = "The file changed on the server while it was being transferred";
    else if (code == "InvalidObjectState")
        text = "The object is archived (Glacier or Deep Archive): restore it in the provider's console first";
    else if (code == "PermanentRedirect" || code == "AuthorizationHeaderMalformed")
        text = "The bucket is in another region, or the endpoint is wrong";
    else if (code == "EntityTooLarge")
        text = "The file is larger than the service allows";
    else if (code == "SlowDown")
        text = "The service asks to slow down; try again in a moment";
    else if (code == "InvalidRange")
        text = "The requested range is beyond the end of the file";
    else if (code == "BucketAlreadyOwnedByYou" || code == "BucketAlreadyExists")
        text = "The bucket already exists";
    else if (code == "MethodNotAllowed" || code == "NotImplemented")
        text = "The service does not support this operation";
    else if (!message.empty())
        text = message;
    else
        text = "The server answered HTTP " + std::to_string(status);
    std::string full = what.empty() ? text : what + ": " + text;
    if (!code.empty()) full += " (" + code + ")";
    if (!message.empty() && message != text && code != "AccessDenied") full += ". " + message;
    if (const std::string id = [&] {
            for (const auto& [n, v] : headers)
                if (iequals(n, "x-amz-request-id")) return v;
            return std::string();
        }();
        !id.empty() && cfg_.log)
        cfg_.log("S3 request " + id + " failed: HTTP " + std::to_string(status) + " " + code);
    S3Error e(full, status, code);
    return e;
}

S3Client::Result S3Client::call(Call c) {
    bool forceRefresh = false, skewFixed = false;
    int regionChanges = 0;
    std::mt19937 rng(std::random_device{}());
    const std::string what = c.method + " " + (c.bucket.empty() ? std::string("/") : c.bucket + "/" + c.key);
    for (int attempt = 1;; ++attempt) {
        if (c.cancel && c.cancel->load()) {
            S3Error e("Cancelled");
            e.cancelled = true;
            throw e;
        }
        AwsCredentials creds;
        try {
            creds = creds_->get(forceRefresh);
        } catch (const CredentialsError& e) {
            throw S3Error(e.what(), 0, "Credentials");
        }
        forceRefresh = false;
        const std::string region = c.bucket.empty() ? cfg_.region : regionFor(c.bucket);
        SignInput in;
        in.method = c.method;
        std::string url = urlFor(c.bucket, c.key, region, in.host, in.encodedPath);
        in.query = c.query;
        in.headers = c.headers;
        in.region = region;
        in.service = "s3";
        in.now = now();
        if (c.body && !c.body->empty())
            in.payloadHash = scheme_ == "https" ? std::string(kUnsignedPayload) : payloadHash(*c.body);
        const Signed s = sign(in, creds);

        sys::HttpRequest req;
        req.method = c.method;
        req.url = url + (s.query.empty() ? "" : "?" + s.query);
        req.headers = s.headers;
        req.body = c.body;
        req.onSent = c.onSent;
        req.cancel = c.cancel;
        if (c.onBody) {
            req.onBody = [&](std::string_view chunk) {
                if (c.delivered) *c.delivered += chunk.size();
                return c.onBody(chunk);
            };
        }
        sys::HttpResponse r = http_->send(req);
        auto backoff = [&] {
            const int base = cfg_.retryBaseMs << std::min(attempt - 1, 6);
            const int ms = base / 2 + static_cast<int>(rng() % static_cast<unsigned>(base / 2 + 1));
            if (cfg_.log) cfg_.log("S3: retrying " + what + " in " + std::to_string(ms) + " ms");
            for (int waited = 0; waited < ms; waited += 20) {
                if (c.cancel && c.cancel->load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        };
        if (r.status == 0) {
            S3Error e(r.cancelled ? std::string("Cancelled") : what + ": " + r.error);
            e.cancelled = r.cancelled;
            e.network = !r.cancelled;
            e.certificate = r.certificateError;
            if (r.cancelled || r.certificateError) throw e;
            if (c.delivered && *c.delivered > 0) throw e;  // the caller continues from where it got
            if (attempt < cfg_.maxAttempts) {
                if (cfg_.log) cfg_.log("S3: " + what + ": " + r.error);
                backoff();
                continue;
            }
            throw e;
        }
        if ((r.status >= 200 && r.status < 300) ||
            std::find(c.accept.begin(), c.accept.end(), r.status) != c.accept.end()) {
            Result out;
            out.status = r.status;
            out.headers = std::move(r.headers);
            out.body = std::move(r.body);
            return out;
        }
        S3Error e = errorFrom(r.status, r.body, r.headers, what);
        // The bucket lives in another region: AWS names it in a header or in the error document.
        std::string newRegion = r.header("x-amz-bucket-region");
        if (newRegion.empty() && !r.body.empty()) {
            try {
                const XmlNode doc = parseXml(r.body);
                newRegion = doc.childText("Region");
            } catch (const XmlError&) {
            }
        }
        if (!c.bucket.empty() && !newRegion.empty() && newRegion != region && regionChanges < 2 &&
            (r.status == 301 || r.status == 307 || e.code == "AuthorizationHeaderMalformed" || e.code == "PermanentRedirect" ||
             e.code == "IllegalLocationConstraintException")) {
            ++regionChanges;
            if (cfg_.log) cfg_.log("S3: bucket " + c.bucket + " is in region " + newRegion);
            std::lock_guard lk(m_);
            regions_[c.bucket] = newRegion;
            continue;
        }
        if (e.code == "RequestTimeTooSkewed" && !skewFixed) {
            skewFixed = true;
            if (auto server = parseHttpDate(r.header("Date"))) {
                const int64_t local = static_cast<int64_t>(std::time(nullptr));
                skewSeconds_ = *server / 1000 - local;
                if (cfg_.log) cfg_.log("S3: this computer's clock is " + std::to_string(-skewSeconds_.load()) +
                                       " s off the server's; correcting");
                continue;
            }
        }
        if (isExpiredCode(e.code) && attempt < cfg_.maxAttempts) {
            forceRefresh = true;
            continue;
        }
        if ((r.status >= 500 || isRetryableCode(e.code)) && attempt < cfg_.maxAttempts &&
            !(c.delivered && *c.delivered > 0)) {
            backoff();
            continue;
        }
        throw e;
    }
}

std::vector<S3Bucket> S3Client::listBuckets(const std::atomic<bool>* cancel) {
    Call c;
    c.cancel = cancel;
    const Result r = call(c);
    std::vector<S3Bucket> out;
    try {
        const XmlNode doc = parseXml(r.body);
        if (const XmlNode* b = doc.child("Buckets"))
            for (const XmlNode* n : b->all("Bucket"))
                out.push_back({n->childText("Name"), parseIso8601(n->childText("CreationDate")).value_or(0)});
    } catch (const XmlError& e) {
        throw S3Error(std::string("The bucket list could not be read: ") + e.what());
    }
    return out;
}

std::string S3Client::bucketRegion(const std::string& bucket, const std::atomic<bool>* cancel) {
    Call c;
    c.method = "HEAD";
    c.bucket = bucket;
    c.cancel = cancel;
    c.accept = {301, 400, 403, 404};
    const Result r = call(c);
    const std::string region = r.header("x-amz-bucket-region");
    if (r.status == 404) throw S3Error("The bucket " + bucket + " does not exist (NoSuchBucket)", 404, "NoSuchBucket");
    if (!region.empty()) {
        std::lock_guard lk(m_);
        regions_[bucket] = region;
    }
    if (r.status == 403 && region.empty())
        throw S3Error("Access to the bucket " + bucket + " is denied (AccessDenied)", 403, "AccessDenied");
    return region.empty() ? regionFor(bucket) : region;
}

S3List S3Client::listObjects(const std::string& bucket, const std::string& prefix, const std::string& delimiter,
                             const std::string& token, int maxKeys, const std::atomic<bool>* cancel) {
    Call c;
    c.bucket = bucket;
    c.cancel = cancel;
    c.query = {{"list-type", "2"}, {"encoding-type", "url"}, {"max-keys", std::to_string(maxKeys)}};
    if (!prefix.empty()) c.query.emplace_back("prefix", prefix);
    if (!delimiter.empty()) c.query.emplace_back("delimiter", delimiter);
    if (!token.empty()) c.query.emplace_back("continuation-token", token);
    const Result r = call(c);
    S3List out;
    try {
        const XmlNode doc = parseXml(r.body);
        const bool encoded = doc.childText("EncodingType") == "url";
        auto dec = [&](const std::string& s) { return encoded ? urlDecode(s) : s; };
        for (const XmlNode* n : doc.all("Contents")) {
            S3Object o;
            o.key = dec(n->childText("Key"));
            o.size = toU64(n->childText("Size"));
            o.lastModifiedMs = parseIso8601(n->childText("LastModified")).value_or(0);
            o.etag = unquoteEtag(n->childText("ETag"));
            o.storageClass = n->childText("StorageClass");
            if (o.storageClass.empty()) o.storageClass = "STANDARD";
            out.objects.push_back(std::move(o));
        }
        for (const XmlNode* n : doc.all("CommonPrefixes")) out.prefixes.push_back(dec(n->childText("Prefix")));
        out.truncated = doc.childText("IsTruncated") == "true";
        out.nextToken = doc.childText("NextContinuationToken");
        if (out.truncated && out.nextToken.empty()) throw S3Error("The listing of " + bucket + " has no continuation token");
    } catch (const XmlError& e) {
        throw S3Error(std::string("The listing could not be read: ") + e.what());
    }
    return out;
}

std::optional<S3Head> S3Client::headObject(const std::string& bucket, const std::string& key,
                                           const std::atomic<bool>* cancel) {
    Call c;
    c.method = "HEAD";
    c.bucket = bucket;
    c.key = key;
    c.cancel = cancel;
    c.accept = {404};
    const Result r = call(c);
    if (r.status == 404) return std::nullopt;
    return headFrom(r.headers, toU64(r.header("Content-Length")));
}

S3Head S3Client::getObject(const std::string& bucket, const std::string& key, uint64_t offset, uint64_t length,
                           const std::string& ifMatch, const std::function<bool(std::string_view)>& onBody,
                           uint64_t& delivered, const std::atomic<bool>* cancel) {
    Call c;
    c.bucket = bucket;
    c.key = key;
    c.cancel = cancel;
    c.onBody = onBody;
    c.delivered = &delivered;
    if (offset > 0 || length > 0)
        c.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-" +
                                            (length ? std::to_string(offset + length - 1) : std::string()));
    if (!ifMatch.empty()) c.headers.emplace_back("If-Match", "\"" + ifMatch + "\"");
    const Result r = call(c);
    return headFrom(r.headers, toU64(r.header("Content-Length")));
}

std::string S3Client::putObject(const std::string& bucket, const std::string& key, std::shared_ptr<const std::string> body,
                                const S3PutOptions& opts, const std::function<void(uint64_t)>& onSent,
                                const std::atomic<bool>* cancel) {
    Call c;
    c.method = "PUT";
    c.bucket = bucket;
    c.key = key;
    c.body = body ? std::move(body) : std::make_shared<const std::string>();
    c.onSent = onSent;
    c.cancel = cancel;
    putHeaders(opts, c.headers);
    const Result r = call(c);
    return unquoteEtag(r.header("ETag"));
}

std::string S3Client::createMultipartUpload(const std::string& bucket, const std::string& key, const S3PutOptions& opts) {
    Call c;
    c.method = "POST";
    c.bucket = bucket;
    c.key = key;
    c.query = {{"uploads", ""}};
    putHeaders(opts, c.headers);
    const Result r = call(c);
    try {
        const std::string id = parseXml(r.body).childText("UploadId");
        if (id.empty()) throw S3Error("The service started no upload for " + key);
        return id;
    } catch (const XmlError& e) {
        throw S3Error(std::string("The upload could not be started: ") + e.what());
    }
}

std::string S3Client::uploadPart(const std::string& bucket, const std::string& key, const std::string& uploadId,
                                 int partNumber, std::shared_ptr<const std::string> body,
                                 const std::function<void(uint64_t)>& onSent, const std::atomic<bool>* cancel) {
    Call c;
    c.method = "PUT";
    c.bucket = bucket;
    c.key = key;
    c.query = {{"partNumber", std::to_string(partNumber)}, {"uploadId", uploadId}};
    c.body = std::move(body);
    c.onSent = onSent;
    c.cancel = cancel;
    const Result r = call(c);
    const std::string etag = unquoteEtag(r.header("ETag"));
    if (etag.empty()) throw S3Error("The service did not confirm part " + std::to_string(partNumber) + " of " + key);
    return etag;
}

void S3Client::completeMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId,
                                       const std::vector<S3Part>& parts) {
    std::string xml = "<CompleteMultipartUpload>";
    for (const S3Part& p : parts)
        xml += "<Part><PartNumber>" + std::to_string(p.number) + "</PartNumber><ETag>\"" + xmlEscape(p.etag) +
               "\"</ETag></Part>";
    xml += "</CompleteMultipartUpload>";
    Call c;
    c.method = "POST";
    c.bucket = bucket;
    c.key = key;
    c.query = {{"uploadId", uploadId}};
    c.headers = {{"Content-Type", "application/xml"}};
    c.body = std::make_shared<const std::string>(std::move(xml));
    const Result r = call(c);
    // S3 may answer 200 and report a failure in the body.
    if (r.body.find("<Error>") != std::string::npos) throw errorFrom(500, r.body, r.headers, "Complete upload of " + key);
}

void S3Client::abortMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId) {
    Call c;
    c.method = "DELETE";
    c.bucket = bucket;
    c.key = key;
    c.query = {{"uploadId", uploadId}};
    c.accept = {404};
    call(c);
}

std::vector<S3Part> S3Client::listParts(const std::string& bucket, const std::string& key, const std::string& uploadId) {
    std::vector<S3Part> out;
    std::string marker;
    for (int page = 0; page < 20; ++page) {
        Call c;
        c.bucket = bucket;
        c.key = key;
        c.query = {{"uploadId", uploadId}, {"max-parts", "1000"}};
        if (!marker.empty()) c.query.emplace_back("part-number-marker", marker);
        const Result r = call(c);
        try {
            const XmlNode doc = parseXml(r.body);
            for (const XmlNode* p : doc.all("Part"))
                out.push_back({std::stoi(p->childText("PartNumber")), unquoteEtag(p->childText("ETag")),
                               toU64(p->childText("Size"))});
            if (doc.childText("IsTruncated") != "true") break;
            marker = doc.childText("NextPartNumberMarker");
            if (marker.empty()) break;
        } catch (const std::exception& e) {
            throw S3Error(std::string("The uploaded parts could not be read: ") + e.what());
        }
    }
    std::sort(out.begin(), out.end(), [](const S3Part& a, const S3Part& b) { return a.number < b.number; });
    return out;
}

void S3Client::copyObject(const std::string& srcBucket, const std::string& srcKey, const std::string& dstBucket,
                          const std::string& dstKey, const std::string& storageClass, const S3PutOptions* replace,
                          const std::atomic<bool>* cancel) {
    Call c;
    c.method = "PUT";
    c.bucket = dstBucket;
    c.key = dstKey;
    c.cancel = cancel;
    c.headers = {{"x-amz-copy-source", uriEncode(srcBucket) + "/" + uriEncode(srcKey, false)}};
    if (replace) {
        c.headers.emplace_back("x-amz-metadata-directive", "REPLACE");
        putHeaders(*replace, c.headers);
    } else if (!storageClass.empty() && storageClass != "STANDARD") {
        c.headers.emplace_back("x-amz-storage-class", storageClass);
    }
    const Result r = call(c);
    if (r.body.find("<Error>") != std::string::npos) throw errorFrom(500, r.body, r.headers, "Copy " + srcKey);
}

std::string S3Client::uploadPartCopy(const std::string& srcBucket, const std::string& srcKey, const std::string& dstBucket,
                                     const std::string& dstKey, const std::string& uploadId, int partNumber,
                                     uint64_t first, uint64_t last, const std::atomic<bool>* cancel) {
    Call c;
    c.method = "PUT";
    c.bucket = dstBucket;
    c.key = dstKey;
    c.cancel = cancel;
    c.query = {{"partNumber", std::to_string(partNumber)}, {"uploadId", uploadId}};
    c.headers = {{"x-amz-copy-source", uriEncode(srcBucket) + "/" + uriEncode(srcKey, false)},
                 {"x-amz-copy-source-range", "bytes=" + std::to_string(first) + "-" + std::to_string(last)}};
    const Result r = call(c);
    try {
        const XmlNode doc = parseXml(r.body);
        if (doc.name == "Error") throw errorFrom(500, r.body, r.headers, "Copy " + srcKey);
        return unquoteEtag(doc.childText("ETag"));
    } catch (const XmlError& e) {
        throw S3Error(std::string("The copied part could not be read: ") + e.what());
    }
}

void S3Client::deleteObject(const std::string& bucket, const std::string& key, const std::atomic<bool>* cancel) {
    Call c;
    c.method = "DELETE";
    c.bucket = bucket;
    c.key = key;
    c.cancel = cancel;
    c.accept = {404};
    call(c);
}

std::vector<std::string> S3Client::deleteObjects(const std::string& bucket, const std::vector<std::string>& keys,
                                                 const std::atomic<bool>* cancel) {
    std::vector<std::string> failed;
    for (size_t start = 0; start < keys.size(); start += 1000) {
        std::string xml = "<Delete><Quiet>true</Quiet>";
        for (size_t i = start; i < std::min(keys.size(), start + 1000); ++i)
            xml += "<Object><Key>" + xmlEscape(keys[i]) + "</Key></Object>";
        xml += "</Delete>";
        Call c;
        c.method = "POST";
        c.bucket = bucket;
        c.cancel = cancel;
        c.query = {{"delete", ""}};
        c.headers = {{"Content-Type", "application/xml"}, {"Content-MD5", crypto::base64Encode(crypto::md5(xml))}};
        c.body = std::make_shared<const std::string>(std::move(xml));
        const Result r = call(c);
        try {
            const XmlNode doc = parseXml(r.body);
            for (const XmlNode* e : doc.all("Error"))
                failed.push_back(e->childText("Key") + ": " + e->childText("Message") + " (" + e->childText("Code") + ")");
        } catch (const XmlError&) {
        }
    }
    return failed;
}

std::string S3Client::presignGet(const std::string& bucket, const std::string& key, int64_t seconds) {
    AwsCredentials creds;
    try {
        creds = creds_->get(false);
    } catch (const CredentialsError& e) {
        throw S3Error(e.what(), 0, "Credentials");
    }
    SignInput in;
    in.method = "GET";
    in.region = regionFor(bucket);
    in.now = now();
    const std::string url = urlFor(bucket, key, in.region, in.host, in.encodedPath);
    const Signed s = presign(in, creds, seconds);
    return url + (s.query.empty() ? "" : "?" + s.query);
}

}  // namespace st::s3
