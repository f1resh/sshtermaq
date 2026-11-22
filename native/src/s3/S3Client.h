#pragma once
// The S3 REST API calls the file manager needs, over platform/sys/Http.h, signed with SigV4. Retries with backoff on
// throttling, server errors and dropped connections; follows a bucket's region (AWS answers 301 / 400 with the right
// one); corrects for a computer clock that is off; refreshes expired temporary credentials once.
#include "s3/Credentials.h"
#include "s3/SigV4.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace st::sys {
class HttpSession;
struct HttpResponse;
}  // namespace st::sys

namespace st::s3 {

/** A failed call, with a message for people and the service's own code. */
struct S3Error : std::runtime_error {
    S3Error(const std::string& message, int status = 0, std::string code = {})
        : std::runtime_error(message), status(status), code(std::move(code)) {}
    int status = 0;          // HTTP status; 0 = no answer
    std::string code;        // "AccessDenied", "NoSuchKey", ...
    bool cancelled = false;
    bool network = false;    // no answer (connection, timeout)
    bool certificate = false;
};

struct S3Config {
    /** "https://s3.amazonaws.com" style base URL; "" = AWS (https://s3.<region>.amazonaws.com). */
    std::string endpoint;
    std::string region = "us-east-1";
    /** true: host/bucket/key; false: bucket.host/key (AWS default). */
    bool pathStyle = false;
    int maxAttempts = 5;
    /** First retry delay; doubles each time (with jitter). */
    int retryBaseMs = 200;
    std::function<void(const std::string&)> log;
};

struct S3Bucket {
    std::string name;
    int64_t createdMs = 0;
};

struct S3Object {
    std::string key;
    uint64_t size = 0;
    int64_t lastModifiedMs = 0;
    std::string etag;
    std::string storageClass;
};

struct S3List {
    std::vector<S3Object> objects;
    /** Folders (common prefixes), each ending with the delimiter. */
    std::vector<std::string> prefixes;
    bool truncated = false;
    std::string nextToken;
};

struct S3Head {
    uint64_t size = 0;
    int64_t lastModifiedMs = 0;
    std::string etag;
    std::string contentType;
    std::string storageClass;
    /** x-amz-meta-* without the prefix, lower case. */
    std::map<std::string, std::string> metadata;
};

struct S3Part {
    int number = 0;
    std::string etag;
    uint64_t size = 0;
};

/** Headers for new objects: content type, metadata, storage class. */
struct S3PutOptions {
    std::string contentType;
    std::map<std::string, std::string> metadata;
    std::string storageClass;
};

class S3Client {
public:
    S3Client(std::shared_ptr<sys::HttpSession> http, std::shared_ptr<CredentialProvider> creds, S3Config cfg);

    std::vector<S3Bucket> listBuckets(const std::atomic<bool>* cancel = nullptr);
    /** The bucket's region (HeadBucket); remembered for later calls. Throws S3Error (NoSuchBucket, AccessDenied). */
    std::string bucketRegion(const std::string& bucket, const std::atomic<bool>* cancel = nullptr);
    S3List listObjects(const std::string& bucket, const std::string& prefix, const std::string& delimiter,
                       const std::string& continuationToken, int maxKeys = 1000, const std::atomic<bool>* cancel = nullptr);
    /** nullopt when there is no such key. */
    std::optional<S3Head> headObject(const std::string& bucket, const std::string& key,
                                     const std::atomic<bool>* cancel = nullptr);
    /**
     * Streams bytes [offset, offset + length) (length 0 = to the end) to `onBody`. `ifMatch`: the ETag the object
     * must still have (412 -> S3Error code "PreconditionFailed"). Not retried once bytes were delivered: `delivered`
     * says how many, so the caller can continue from there.
     */
    S3Head getObject(const std::string& bucket, const std::string& key, uint64_t offset, uint64_t length,
                     const std::string& ifMatch, const std::function<bool(std::string_view)>& onBody,
                     uint64_t& delivered, const std::atomic<bool>* cancel = nullptr);
    /** Returns the ETag. */
    std::string putObject(const std::string& bucket, const std::string& key, std::shared_ptr<const std::string> body,
                          const S3PutOptions& opts, const std::function<void(uint64_t)>& onSent = {},
                          const std::atomic<bool>* cancel = nullptr);
    std::string createMultipartUpload(const std::string& bucket, const std::string& key, const S3PutOptions& opts);
    std::string uploadPart(const std::string& bucket, const std::string& key, const std::string& uploadId, int partNumber,
                           std::shared_ptr<const std::string> body, const std::function<void(uint64_t)>& onSent = {},
                           const std::atomic<bool>* cancel = nullptr);
    void completeMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId,
                                 const std::vector<S3Part>& parts);
    void abortMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId);
    /** The parts uploaded so far (for resuming); throws NoSuchUpload when the upload is gone. */
    std::vector<S3Part> listParts(const std::string& bucket, const std::string& key, const std::string& uploadId);
    /** A server-side copy (up to 5 GB). Metadata and content type are kept unless `replace` is given; S3 would make
     *  the copy STANDARD, so the source's `storageClass` (from a listing or HEAD) is passed on. */
    void copyObject(const std::string& srcBucket, const std::string& srcKey, const std::string& dstBucket,
                    const std::string& dstKey, const std::string& storageClass, const S3PutOptions* replace = nullptr,
                    const std::atomic<bool>* cancel = nullptr);
    /** One part of a multipart copy: bytes [first, last] of the source. Returns the part's ETag. */
    std::string uploadPartCopy(const std::string& srcBucket, const std::string& srcKey, const std::string& dstBucket,
                               const std::string& dstKey, const std::string& uploadId, int partNumber, uint64_t first,
                               uint64_t last, const std::atomic<bool>* cancel = nullptr);
    void deleteObject(const std::string& bucket, const std::string& key, const std::atomic<bool>* cancel = nullptr);
    /** Up to 1000 keys per call (more are split). Returns "key: message" for the keys that could not be deleted. */
    std::vector<std::string> deleteObjects(const std::string& bucket, const std::vector<std::string>& keys,
                                           const std::atomic<bool>* cancel = nullptr);
    /** A presigned GET URL, valid for `seconds` (at most 7 days). */
    std::string presignGet(const std::string& bucket, const std::string& key, int64_t seconds);

    /** The base URL of the endpoint (for messages). */
    std::string endpointDescription() const;
    /** Tests: a fixed clock (Unix seconds); 0 = the real one. */
    void setClockForTests(int64_t unixSeconds) { testNow_ = unixSeconds; }

private:
    struct Call {
        std::string method = "GET";
        std::string bucket;  // "" = the service (ListBuckets)
        std::string key;
        Params query;
        Params headers;
        std::shared_ptr<const std::string> body;
        std::function<bool(std::string_view)> onBody;
        std::function<void(uint64_t)> onSent;
        const std::atomic<bool>* cancel = nullptr;
        /** Statuses that are not errors (404 for HEAD and probes). */
        std::vector<int> accept;
        /** Streamed GET: do not retry after bytes arrived. */
        uint64_t* delivered = nullptr;
    };
    struct Result {
        int status = 0;
        Params headers;
        std::string body;
        std::string header(std::string_view name) const;
    };

    Result call(Call c);
    std::string regionFor(const std::string& bucket);
    std::string urlFor(const std::string& bucket, const std::string& key, const std::string& region, std::string& host,
                       std::string& encodedPath) const;
    int64_t now() const;
    S3Error errorFrom(int status, const std::string& body, const Params& headers, const std::string& what) const;

    std::shared_ptr<sys::HttpSession> http_;
    std::shared_ptr<CredentialProvider> creds_;
    S3Config cfg_;
    std::string scheme_ = "https";
    std::string baseHost_;  // "" = AWS
    std::mutex m_;
    std::map<std::string, std::string> regions_;
    std::atomic<int64_t> skewSeconds_{0};
    int64_t testNow_ = 0;
};

/** The ETag without quotes. */
std::string unquoteEtag(std::string_view etag);
/** S3 bucket naming rules (3-63 characters, lower case, digits, '.', '-'). */
bool isValidBucketName(std::string_view name);

}  // namespace st::s3
