// The S3 client against the fake S3 service: every call, region redirects, retries, expired credentials, presigned
// links, http:// (signed bodies) and https:// with a pinned certificate. The fake checks every signature.
#include "platform/sys/Http.h"
#include "s3/S3Client.h"

#include "TestS3Server.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace st;
using namespace st::s3;
using namespace st::test;

namespace {

sys::HttpSessionOptions httpOptions() {
    sys::HttpSessionOptions o;
    o.proxy.mode = sys::HttpProxy::Mode::None;
    o.connectTimeoutMs = 3000;
    o.ioTimeoutMs = 5000;
    return o;
}

std::shared_ptr<CredentialProvider> testKeys(const std::string& secret = "test-secret") {
    AwsCredentials c;
    c.accessKeyId = "AKIDTEST";
    c.secretAccessKey = secret;
    return fixedCredentials(c, "test");
}

struct Fixture {
    TestS3Server server;
    std::shared_ptr<sys::HttpSession> http = sys::openHttpSession(httpOptions());
    std::vector<std::string> logLines;
    S3Client client;

    explicit Fixture(TestS3Options o = {}, std::shared_ptr<CredentialProvider> creds = testKeys())
        : server(std::move(o)), client(http, std::move(creds), config()) {
        server.createBucket("bucket");
    }
    S3Config config() {
        S3Config c;
        c.endpoint = server.endpoint();
        c.pathStyle = true;
        c.retryBaseMs = 10;
        c.log = [this](const std::string& s) { logLines.push_back(s); };
        return c;
    }
};

std::string getAll(S3Client& c, const std::string& bucket, const std::string& key, uint64_t offset = 0, uint64_t length = 0,
                   const std::string& ifMatch = {}) {
    std::string out;
    uint64_t delivered = 0;
    c.getObject(bucket, key, offset, length, ifMatch, [&](std::string_view d) {
        out.append(d);
        return true;
    }, delivered);
    return out;
}

}  // namespace

TEST_CASE("s3 client: buckets, objects, metadata, listings") {
    Fixture f;
    f.server.createBucket("second");
    const auto buckets = f.client.listBuckets();
    REQUIRE(buckets.size() == 2);
    CHECK(buckets[0].name == "bucket");
    CHECK(buckets[0].createdMs > 0);

    S3PutOptions opts;
    opts.contentType = "text/plain";
    opts.metadata = {{"mtime", "1700000000"}};
    const std::string weird = "dir one/f+ü %41 & <x>.txt";
    const std::string etag = f.client.putObject("bucket", weird, std::make_shared<std::string>("hello world"), opts);
    CHECK(etag.size() == 32);
    const auto head = f.client.headObject("bucket", weird);
    REQUIRE(head);
    CHECK(head->size == 11);
    CHECK(head->etag == etag);
    CHECK(head->contentType == "text/plain");
    CHECK(head->metadata.at("mtime") == "1700000000");
    CHECK(head->lastModifiedMs > 0);
    CHECK_FALSE(f.client.headObject("bucket", "missing"));
    CHECK(getAll(f.client, "bucket", weird) == "hello world");
    CHECK(getAll(f.client, "bucket", weird, 6) == "world");
    CHECK(getAll(f.client, "bucket", weird, 2, 3) == "llo");

    for (int i = 0; i < 5; ++i) f.server.put("bucket", "dir one/sub/" + std::to_string(i), "x");
    f.server.put("bucket", "top.txt", "t");
    S3List root = f.client.listObjects("bucket", "", "/", "");
    CHECK(root.prefixes == std::vector<std::string>{"dir one/"});
    REQUIRE(root.objects.size() == 1);
    CHECK(root.objects[0].key == "top.txt");
    S3List dir = f.client.listObjects("bucket", "dir one/", "/", "");
    CHECK(dir.prefixes == std::vector<std::string>{"dir one/sub/"});
    REQUIRE(dir.objects.size() == 1);
    CHECK(dir.objects[0].key == weird);
    CHECK(dir.objects[0].storageClass == "STANDARD");
    // Pages of two.
    std::vector<std::string> all;
    std::string token;
    int pages = 0;
    do {
        S3List page = f.client.listObjects("bucket", "dir one/sub/", "/", token, 2);
        for (auto& o : page.objects) all.push_back(o.key);
        token = page.nextToken;
        ++pages;
        if (!page.truncated) break;
    } while (pages < 10);
    CHECK(pages == 3);
    CHECK(all.size() == 5);
    CHECK(f.server.badSignatures() == 0);
}

TEST_CASE("s3 client: If-Match, ranges past the end, errors with readable messages") {
    Fixture f;
    f.server.put("bucket", "a", "0123456789");
    const auto h = f.client.headObject("bucket", "a");
    REQUIRE(h);
    CHECK(getAll(f.client, "bucket", "a", 0, 4, h->etag) == "0123");
    try {
        getAll(f.client, "bucket", "a", 0, 4, "not-the-etag");
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.code == "PreconditionFailed");
        CHECK(std::string(e.what()).find("changed on the server") != std::string::npos);
    }
    try {
        getAll(f.client, "bucket", "a", 10);
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.code == "InvalidRange");
    }
    try {
        f.client.listObjects("nobucket", "", "/", "");
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.code == "NoSuchBucket");
        CHECK(e.status == 404);
        CHECK(std::string(e.what()).find("does not exist") != std::string::npos);
    }
    S3Client wrong(f.http, testKeys("wrong-secret"), f.config());
    try {
        wrong.listBuckets();
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.code == "SignatureDoesNotMatch");
        CHECK(std::string(e.what()).find("secret access key") != std::string::npos);
    }
    try {
        f.client.bucketRegion("nobucket");
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.code == "NoSuchBucket");
    }
    CHECK(f.client.bucketRegion("bucket") == "us-east-1");
}

TEST_CASE("s3 client: multipart upload, list parts, abort; copies; batch delete") {
    Fixture f;
    const std::string id = f.client.createMultipartUpload("bucket", "big", S3PutOptions{"application/octet-stream", {{"mtime", "5"}}, ""});
    const std::string part1(5 * 1024 * 1024, 'a'), part2 = "tail";
    std::vector<S3Part> parts = {{1, f.client.uploadPart("bucket", "big", id, 1, std::make_shared<std::string>(part1)), 0},
                                 {2, f.client.uploadPart("bucket", "big", id, 2, std::make_shared<std::string>(part2)), 0}};
    const auto listed = f.client.listParts("bucket", "big", id);
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].size == part1.size());
    CHECK(listed[1].etag == parts[1].etag);
    CHECK(f.server.openUploads() == 1);
    f.client.completeMultipartUpload("bucket", "big", id, parts);
    CHECK(f.server.openUploads() == 0);
    const auto big = f.server.get("bucket", "big");
    REQUIRE(big);
    CHECK(big->data == part1 + part2);
    CHECK(big->etag.find("-2") != std::string::npos);
    CHECK(big->metadata.at("mtime") == "5");

    const std::string id2 = f.client.createMultipartUpload("bucket", "gone", {});
    f.client.abortMultipartUpload("bucket", "gone", id2);
    CHECK(f.server.openUploads() == 0);
    try {
        f.client.listParts("bucket", "gone", id2);
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.code == "NoSuchUpload");
    }

    // Copies keep metadata and the storage class we pass on.
    f.server.put("bucket", "src", "copy me", 0, {{"mtime", "42"}});
    f.client.copyObject("bucket", "src", "bucket", "dst/copy", "STANDARD_IA");
    auto copied = f.server.get("bucket", "dst/copy");
    REQUIRE(copied);
    CHECK(copied->data == "copy me");
    CHECK(copied->metadata.at("mtime") == "42");
    CHECK(copied->storageClass == "STANDARD_IA");
    // Part copies.
    const std::string id3 = f.client.createMultipartUpload("bucket", "joined", {});
    const std::string e1 = f.client.uploadPartCopy("bucket", "src", "bucket", "joined", id3, 1, 0, 3);
    const std::string e2 = f.client.uploadPartCopy("bucket", "src", "bucket", "joined", id3, 2, 4, 6);
    f.client.completeMultipartUpload("bucket", "joined", id3, {{1, e1, 0}, {2, e2, 0}});
    CHECK(f.server.get("bucket", "joined")->data == "copy me");

    for (int i = 0; i < 1500; ++i) f.server.put("bucket", "many/" + std::to_string(i), "");
    f.server.put("bucket", "many/undeletable", "");
    std::vector<std::string> keys;
    for (int i = 0; i < 1500; ++i) keys.push_back("many/" + std::to_string(i));
    keys.push_back("many/undeletable");
    const auto failed = f.client.deleteObjects("bucket", keys);
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].find("many/undeletable") == 0);
    CHECK(f.server.count("DeleteObjects") == 2);
    CHECK(f.server.keys("bucket").size() == 5);  // big, src, dst/copy, joined, many/undeletable (and no "gone")
    f.client.deleteObject("bucket", "src");
    f.client.deleteObject("bucket", "src");  // already gone: no error
    CHECK_FALSE(f.server.get("bucket", "src"));
    CHECK(f.server.badSignatures() == 0);
}

TEST_CASE("s3 client: a bucket in another region is followed") {
    TestS3Options o;
    o.bucketRegions = {{"faraway", "eu-west-1"}};
    Fixture f(o);
    f.server.createBucket("faraway");
    f.server.put("faraway", "k", "v");
    CHECK(getAll(f.client, "faraway", "k") == "v");
    CHECK(f.client.bucketRegion("faraway") == "eu-west-1");
    // Later calls sign for the right region straight away.
    const int before = f.server.count("GetObject");
    CHECK(getAll(f.client, "faraway", "k") == "v");
    CHECK(f.server.count("GetObject") == before + 1);
}

TEST_CASE("s3 client: retries on throttling, server errors and dropped connections") {
    Fixture f;
    f.server.put("bucket", "k", "value");
    std::atomic<int> failures{3};
    f.server.setFault([&](const TestHttpRequest&, const std::string& op) -> std::optional<TestHttpResponse> {
        if (op != "GetObject" || failures <= 0) return std::nullopt;
        const int n = failures--;
        TestHttpResponse r;
        if (n == 3) {
            r.status = 503;
            r.body = "<Error><Code>SlowDown</Code><Message>Please reduce your request rate.</Message></Error>";
        } else if (n == 2) {
            r.status = 500;
            r.body = "<Error><Code>InternalError</Code><Message>We encountered an internal error.</Message></Error>";
        } else {
            r.closeWithoutAnswer = true;
        }
        return r;
    });
    CHECK(getAll(f.client, "bucket", "k") == "value");
    CHECK(f.server.count("GetObject") == 4);

    // Not retried: client errors.
    f.server.setFault([](const TestHttpRequest&, const std::string&) -> std::optional<TestHttpResponse> {
        TestHttpResponse r;
        r.status = 403;
        r.body = "<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>";
        return r;
    });
    const int before = f.server.http().requests();
    CHECK_THROWS_AS(f.client.listBuckets(), S3Error);
    CHECK(f.server.http().requests() == before + 1);

    // Retries stop after maxAttempts.
    f.server.setFault([](const TestHttpRequest&, const std::string&) -> std::optional<TestHttpResponse> {
        TestHttpResponse r;
        r.status = 503;
        return r;
    });
    const int before2 = f.server.http().requests();
    CHECK_THROWS_AS(f.client.listBuckets(), S3Error);
    CHECK(f.server.http().requests() == before2 + 5);
}

TEST_CASE("s3 client: expired credentials are refreshed once") {
    std::atomic<int> fetches{0};
    auto creds = refreshingCredentials(
        [&] {
            ++fetches;
            AwsCredentials c;
            c.accessKeyId = "AKIDTEST";
            c.secretAccessKey = "test-secret";
            c.sessionToken = "token" + std::to_string(fetches.load());
            c.expiresAt = 4102444800;
            return c;
        },
        "test");
    Fixture f({}, creds);
    std::atomic<bool> once{true};
    f.server.setFault([&](const TestHttpRequest& r, const std::string&) -> std::optional<TestHttpResponse> {
        if (!once.exchange(false)) return std::nullopt;
        CHECK(r.header("x-amz-security-token") == "token1");
        TestHttpResponse e;
        e.status = 400;
        e.body = "<Error><Code>ExpiredToken</Code><Message>The provided token has expired.</Message></Error>";
        return e;
    });
    CHECK(f.client.listBuckets().size() == 1);
    CHECK(fetches == 2);
}

TEST_CASE("s3 client: presigned links work without credentials") {
    Fixture f;
    f.server.put("bucket", "shared file.txt", "for everyone");
    const std::string url = f.client.presignGet("bucket", "shared file.txt", 3600);
    CHECK(url.find("X-Amz-Expires=3600") != std::string::npos);
    sys::HttpRequest req;
    req.url = url;
    const sys::HttpResponse r = f.http->send(req);
    CHECK(r.status == 200);
    CHECK(r.body == "for everyone");
    CHECK(f.server.badSignatures() == 0);
}

TEST_CASE("s3 client: anonymous access to a public bucket") {
    TestS3Options o;
    o.allowAnonymous = true;
    AwsCredentials anon;
    anon.anonymous = true;
    Fixture f(o, fixedCredentials(anon, "anonymous"));
    f.server.put("bucket", "public", "open data");
    CHECK(getAll(f.client, "bucket", "public") == "open data");
}

TEST_CASE("s3 client: https with a pinned self-signed certificate") {
    TestS3Options o;
    o.tls = true;
    TestS3Server server(o);
    server.createBucket("bucket");
    auto http = sys::openHttpSession(httpOptions());
    S3Config cfg;
    cfg.endpoint = server.endpoint();
    cfg.pathStyle = true;
    cfg.maxAttempts = 1;
    S3Client client(http, testKeys(), cfg);
    try {
        client.listBuckets();
        FAIL("no error");
    } catch (const S3Error& e) {
        CHECK(e.certificate);
    }
    CHECK(server.http().requests() == 0);
    http->setPinnedCertificates({server.http().certificateSha256()});
    CHECK(client.listBuckets().size() == 1);
    // https: bodies are not hashed (UNSIGNED-PAYLOAD); the fake accepts that.
    client.putObject("bucket", "k", std::make_shared<std::string>("over tls"), {});
    CHECK(server.get("bucket", "k")->data == "over tls");
    CHECK(server.badSignatures() == 0);
}
