#pragma once
// A fake S3 service for the tests, on TestHttpServer: buckets and objects in memory, ListObjectsV2, ranged GET with
// If-Match, PUT, multipart (with ListParts and Abort), CopyObject / UploadPartCopy, DeleteObject(s), and AWS's error
// documents. It checks every request's SigV4 signature against what it received (the host, path, query and headers
// as WinHTTP / NSURLSession really sent them), and can inject faults per request.
#include "TestHttpServer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace st::test {

struct TestS3Object {
    std::string data;
    /** A generated object instead of `data`: `generatedSize` bytes of pattern(offset). */
    uint64_t generatedSize = 0;
    std::string etag;
    int64_t lastModifiedMs = 0;
    std::string contentType;
    std::map<std::string, std::string> metadata;
    std::string storageClass = "STANDARD";
    uint64_t size() const { return generatedSize ? generatedSize : data.size(); }
};

struct TestS3Options {
    bool tls = false;
    std::string accessKeyId = "AKIDTEST";
    std::string secret = "test-secret";
    /** Accept anonymous (unsigned) requests. */
    bool allowAnonymous = false;
    std::string region = "us-east-1";
    /** Buckets that live in another region: signed for the wrong one -> AuthorizationHeaderMalformed + <Region>. */
    std::map<std::string, std::string> bucketRegions;
    /** Uploads keep only the size and MD5 of the data (memory tests), objects read back as the pattern. */
    bool discardData = false;
    /** Fixed port (0 = any free one). */
    int port = 0;
};

class TestS3Server {
public:
    explicit TestS3Server(TestS3Options options = {});
    ~TestS3Server();

    std::string endpoint() const;
    TestHttpServer& http();

    void createBucket(const std::string& name);
    void put(const std::string& bucket, const std::string& key, std::string data, int64_t lastModifiedMs = 0,
             std::map<std::string, std::string> metadata = {});
    /** A big object of pattern bytes (see pattern()). */
    void putGenerated(const std::string& bucket, const std::string& key, uint64_t size);
    std::optional<TestS3Object> get(const std::string& bucket, const std::string& key) const;
    std::vector<std::string> keys(const std::string& bucket) const;
    /** Multipart uploads started and neither completed nor aborted. */
    int openUploads() const;
    /** Requests whose signature did not match (they were answered 403 SignatureDoesNotMatch). */
    int badSignatures() const;
    /** Count of requests per operation ("GetObject", "UploadPart", ...). */
    int count(const std::string& operation) const;
    /** Called before each request is served; an answer replaces the normal one (fault injection). */
    void setFault(std::function<std::optional<TestHttpResponse>(const TestHttpRequest&, const std::string& operation)> f);
    /** The highest number of requests that were being served at the same time. */
    int maxConcurrent() const;

    /** The byte at `offset` of a generated object. */
    static char pattern(uint64_t offset) { return static_cast<char>((offset * 31 + offset / 4093) & 0xff); }

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    std::unique_ptr<TestHttpServer> http_;
};

}  // namespace st::test
