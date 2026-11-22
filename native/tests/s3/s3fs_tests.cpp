// S3Fs (the file manager's view of S3) against the fake S3 service: browsing, folders, renames and moves, deletes,
// parallel ranged downloads (order, If-Match, retries, resume), multipart uploads (parallel parts, failures kept and
// continued, cancel aborted, old object untouched), bounded memory.
#include "platform/sys/Http.h"
#include "s3/S3Client.h"
#include "s3/S3Transport.h"
#include "sftp/S3Fs.h"

#include "TestS3Server.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

using namespace st;
using namespace st::sftpimpl;
using namespace st::test;

namespace {

struct S3FsFixture {
    TestS3Server server;
    std::shared_ptr<S3Connection> conn = std::make_shared<S3Connection>();
    std::shared_ptr<RemoteFs> fs;

    explicit S3FsFixture(const std::string& bucket = {}, TestS3Options o = {}, uint64_t partSize = 8ull << 20, int parallel = 4)
        : server(o) {
        server.createBucket("bucket");
        sys::HttpSessionOptions ho;
        ho.proxy.mode = sys::HttpProxy::Mode::None;
        ho.ioTimeoutMs = 10000;
        conn->http = sys::openHttpSession(ho);
        s3::AwsCredentials c;
        c.accessKeyId = "AKIDTEST";
        c.secretAccessKey = "test-secret";
        s3::S3Config cfg;
        cfg.endpoint = server.endpoint();
        cfg.pathStyle = true;
        cfg.retryBaseMs = 10;
        conn->client = std::make_shared<s3::S3Client>(conn->http, s3::fixedCredentials(c, "test"), cfg);
        conn->bucket = bucket;
        conn->startPath = "start/here";
        conn->partSize = partSize;
        conn->parallelParts = parallel;
        fs = openS3Fs(conn);
    }
    ~S3FsFixture() { fs->close(); }

    std::string readAll(const std::string& path, uint64_t offset = 0) {
        auto r = fs->openRead(path, offset);
        std::string out;
        for (bool done = false; !done;) out += *r->read(done);
        r->close();
        return out;
    }

    void writeAll(const std::string& path, const std::string& data, int64_t mtime = 0, size_t step = 256 * 1024,
                  const ProgressFn& progress = [](uint64_t) {}) {
        WriteSpec spec;
        spec.size = data.size();
        spec.mtime = mtime;
        auto w = fs->openWrite(path, spec);
        try {
            for (size_t off = 0; off < data.size(); off += step)
                w->write(std::make_shared<std::string>(data.substr(off, step)), progress);
            w->finish(mtime, progress);
        } catch (...) {
            w->abort();
            throw;
        }
    }
};

std::string patternData(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = TestS3Server::pattern(i);
    return s;
}

std::set<std::string> names(const std::vector<RemoteEntry>& entries) {
    std::set<std::string> out;
    for (const auto& e : entries) out.insert(e.entry.name + (e.entry.type == "dir" ? "/" : ""));
    return out;
}

}  // namespace

TEST_CASE("s3fs: buckets at the root, folders, stat, capabilities") {
    S3FsFixture f;
    f.server.createBucket("other");
    f.server.put("bucket", "a.txt", "A", 1700000000000);
    f.server.put("bucket", "docs/readme.md", "R");
    f.server.put("bucket", "docs/deep/x.bin", "X");
    f.server.put("bucket", "empty/", "");

    const RemoteCapabilities caps = f.fs->capabilities();
    CHECK(caps.rootIsBuckets);
    CHECK_FALSE(caps.chmod);
    CHECK_FALSE(caps.symlinks);
    CHECK(caps.presign);
    CHECK(caps.atomicWrites);
    CHECK_FALSE(caps.resumeUploads);
    CHECK(f.fs->protocol() == "s3");
    CHECK(f.fs->home() == "/");

    CHECK(names(f.fs->list("/", false)) == std::set<std::string>{"bucket/", "other/"});
    const auto top = f.fs->list("/bucket", false);
    CHECK(names(top) == std::set<std::string>{"a.txt", "docs/", "empty/"});
    for (const auto& e : top) {
        if (e.entry.name == "a.txt") {
            CHECK(e.entry.path == "/bucket/a.txt");
            CHECK(e.entry.size == 1);
            CHECK(e.entry.mtime == 1700000000000);
        }
    }
    CHECK(names(f.fs->list("/bucket/docs/", false)) == std::set<std::string>{"readme.md", "deep/"});
    CHECK(f.fs->list("/bucket/empty", false).empty());  // the marker itself is not listed

    CHECK(f.fs->stat("/", true)->type == "dir");
    CHECK(f.fs->stat("/bucket", true)->type == "dir");
    CHECK_FALSE(f.fs->stat("/nobucket", true));
    CHECK(f.fs->stat("/bucket/a.txt", true)->type == "file");
    CHECK(f.fs->stat("/bucket/docs", true)->type == "dir");
    CHECK(f.fs->stat("/bucket/empty", true)->type == "dir");
    CHECK_FALSE(f.fs->stat("/bucket/missing", true));
    CHECK(f.fs->realpath("/bucket/docs/../a.txt") == "/bucket/a.txt");

    f.fs->mkdir("/bucket/new folder");
    CHECK(f.server.get("bucket", "new folder/"));
    CHECK(f.fs->stat("/bucket/new folder", true)->type == "dir");
    CHECK_THROWS_AS(f.fs->mkdir("/newbucket"), SftpError);
    CHECK_THROWS_AS(f.fs->chmod("/bucket/a.txt", 0600), SftpError);
    CHECK_THROWS_AS(f.fs->rmdir("/bucket/docs"), SftpError);  // not empty
    f.fs->rmdir("/bucket/new folder");
    CHECK_FALSE(f.server.get("bucket", "new folder/"));
    CHECK_THROWS_AS(f.fs->removeTree("/bucket", nullptr), SftpError);
    CHECK_THROWS_AS(f.fs->unlink("/bucket"), SftpError);
}

TEST_CASE("s3fs: a session bucket is the root; the start folder is home") {
    S3FsFixture f("bucket");
    f.server.put("bucket", "start/here/f", "F");
    CHECK_FALSE(f.fs->capabilities().rootIsBuckets);
    CHECK(f.fs->home() == "/start/here");
    CHECK(names(f.fs->list("/", false)) == std::set<std::string>{"start/"});
    CHECK(f.readAll("/start/here/f") == "F");
}

TEST_CASE("s3fs: rename and move, files and folders, inside and between buckets") {
    S3FsFixture f;
    f.server.createBucket("other");
    f.server.put("bucket", "f.txt", "file", 0, {{"mtime", "1600000000"}});
    for (int i = 0; i < 120; ++i) f.server.put("bucket", "dir/sub/" + std::to_string(i), std::to_string(i));
    f.server.put("bucket", "dir/", "");

    f.fs->rename("/bucket/f.txt", "/bucket/g.txt");
    CHECK_FALSE(f.server.get("bucket", "f.txt"));
    REQUIRE(f.server.get("bucket", "g.txt"));
    CHECK(f.server.get("bucket", "g.txt")->metadata.at("mtime") == "1600000000");
    CHECK(f.fs->stat("/bucket/g.txt", true)->mtime == 1600000000000);  // the upload's own time, not the copy's

    f.server.put("bucket", "exists", "x");
    CHECK_THROWS_AS(f.fs->rename("/bucket/g.txt", "/bucket/exists"), SftpError);
    CHECK_THROWS_AS(f.fs->rename("/bucket", "/renamed"), SftpError);
    CHECK_THROWS_AS(f.fs->rename("/bucket/dir", "/bucket/dir/inside"), SftpError);

    f.fs->rename("/bucket/dir", "/other/moved");
    CHECK(f.server.keys("other").size() == 121);
    REQUIRE(f.server.get("other", "moved/sub/7"));
    CHECK(f.server.get("other", "moved/sub/7")->data == "7");
    CHECK(f.server.get("other", "moved/"));
    for (const auto& k : f.server.keys("bucket")) CHECK(k.rfind("dir/", 0) != 0);
    CHECK(f.server.badSignatures() == 0);
}

TEST_CASE("s3fs: removing a folder tree in batches") {
    S3FsFixture f;
    for (int i = 0; i < 2500; ++i) f.server.put("bucket", "big/" + std::to_string(i % 7) + "/" + std::to_string(i), "");
    f.server.put("bucket", "bigger", "not inside");
    f.fs->removeTree("/bucket/big", nullptr);
    CHECK(f.server.keys("bucket") == std::vector<std::string>{"bigger"});
    CHECK(f.server.count("DeleteObjects") == 3);
    f.fs->removeTree("/bucket/bigger", nullptr);  // a file
    CHECK(f.server.keys("bucket").empty());
    CHECK_THROWS_AS(f.fs->removeTree("/bucket/none", nullptr), SftpError);
}

TEST_CASE("s3fs: downloads in parallel ranges, in order, resumed from an offset") {
    S3FsFixture f;
    const uint64_t size = 40ull << 20;  // 5 ranges
    f.server.putGenerated("bucket", "big.bin", size);
    // The server counts requests only while it prepares an answer (the body streams after that), so a fast machine
    // can finish each range before the next arrives. The first range waits until a second one is in (at most 5 s):
    // with ranges sent in parallel that is at once; sent one after another, the second never comes and the check
    // below fails.
    std::atomic<int> arrived{0};
    f.server.setFault([&](const TestHttpRequest&, const std::string& op) -> std::optional<TestHttpResponse> {
        if (op == "GetObject" && ++arrived == 1) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (arrived < 2 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    });
    std::string data = f.readAll("/bucket/big.bin");
    f.server.setFault(nullptr);
    REQUIRE(data.size() == size);
    CHECK(data == patternData(size));
    CHECK(f.server.count("GetObject") == 5);
    CHECK(f.server.maxConcurrent() >= 2);

    const std::string tail = f.readAll("/bucket/big.bin", size - 1000);
    CHECK(tail == patternData(size).substr(size - 1000));

    f.server.put("bucket", "small", "tiny", 0, {{"mtime", "1234567890"}});
    auto r = f.fs->openRead("/bucket/small", 0);
    CHECK(r->size() == 4);
    CHECK(r->mtime() == 1234567890);
    bool done = false;
    CHECK(*r->read(done) == "tiny");
    CHECK(done);

    f.server.put("bucket", "empty", "");
    CHECK(f.readAll("/bucket/empty").empty());
}

TEST_CASE("s3fs: a dropped range is continued; a file changed meanwhile fails") {
    S3FsFixture f;
    const uint64_t size = 20ull << 20;
    f.server.putGenerated("bucket", "big.bin", size);
    std::atomic<int> drops{2};
    f.server.setFault([&](const TestHttpRequest& r, const std::string& op) -> std::optional<TestHttpResponse> {
        if (op != "GetObject" || r.header("Range").rfind("bytes=0-", 0) != 0 || drops-- <= 0) return std::nullopt;
        // Half of the first range, then the connection is gone.
        TestHttpResponse out;
        out.status = 206;
        out.generatedSize = 8ull << 20;
        out.generate = [](uint64_t off, char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) p[i] = TestS3Server::pattern(off + i);
        };
        out.headers = {{"Content-Range", "bytes 0-8388607/" + std::to_string(20ull << 20)}, {"ETag", "\"gen20971520\""}};
        out.dropAfterBytes = 3 << 20;
        return out;
    });
    CHECK(f.readAll("/bucket/big.bin") == patternData(size));
    f.server.setFault({});

    // The object is replaced after the download started: the next range's If-Match fails (80 MB: 10 ranges, of
    // which only the first 4 are asked for before the change).
    f.server.putGenerated("bucket", "big.bin", 80ull << 20);
    auto r = f.fs->openRead("/bucket/big.bin", 0);
    bool done = false;
    r->read(done);
    f.server.put("bucket", "big.bin", "replaced");
    try {
        for (;;) {
            r->read(done);
            if (done) break;
        }
        r->close();
        FAIL("the change was not noticed");
    } catch (const SftpError& e) {
        CHECK(std::string(e.what()).find("changed on the server") != std::string::npos);
    }
}

TEST_CASE("s3fs: small uploads are one PUT with the file's time; big ones multipart, in parallel") {
    S3FsFixture f;
    f.writeAll("/bucket/note.txt", "hello", 1700000001);
    auto o = f.server.get("bucket", "note.txt");
    REQUIRE(o);
    CHECK(o->data == "hello");
    CHECK(o->metadata.at("mtime") == "1700000001");
    CHECK(o->contentType == "text/plain");
    CHECK(f.fs->stat("/bucket/note.txt", true)->mtime == 1700000001000);
    CHECK(f.server.count("PutObject") == 1);

    const std::string big = patternData((40ull << 20) + 123);
    uint64_t progressed = 0;
    // As for downloads: the first part waits until a second one is in (at most 5 s), so parts sent in parallel are
    // seen as such on any machine.
    std::atomic<int> arrived{0};
    f.server.setFault([&](const TestHttpRequest&, const std::string& op) -> std::optional<TestHttpResponse> {
        if (op == "UploadPart" && ++arrived == 1) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (arrived < 2 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    });
    f.writeAll("/bucket/big.bin", big, 1700000002, 256 * 1024, [&](uint64_t n) { progressed += n; });
    f.server.setFault(nullptr);
    o = f.server.get("bucket", "big.bin");
    REQUIRE(o);
    CHECK(o->data == big);
    CHECK(o->etag.find("-6") != std::string::npos);
    CHECK(o->metadata.at("mtime") == "1700000002");
    CHECK(progressed == big.size());
    CHECK(f.server.count("UploadPart") == 6);
    CHECK(f.server.maxConcurrent() >= 2);
    CHECK(f.server.openUploads() == 0);
    CHECK_THROWS_AS(f.fs->openWrite("/bucket", WriteSpec{}), SftpError);
    CHECK(f.server.badSignatures() == 0);
}

TEST_CASE("s3fs: a failed multipart upload is continued; a cancelled one is aborted and the old object kept") {
    S3FsFixture f;
    const std::string big = patternData(40ull << 20);
    // The bytes counted as buffered come back to this after every upload, failed or cancelled ones too.
    resetS3BufferedHighWater();
    const uint64_t base = s3BufferedHighWater();
    const auto bufferedNow = [] {
        resetS3BufferedHighWater();
        return s3BufferedHighWater();
    };
    std::atomic<bool> failPart3{true};
    f.server.setFault([&](const TestHttpRequest& r, const std::string& op) -> std::optional<TestHttpResponse> {
        if (op != "UploadPart" || r.param("partNumber") != "3" || !failPart3) return std::nullopt;
        TestHttpResponse e;
        e.status = 400;
        e.body = "<Error><Code>InvalidRequest</Code><Message>injected</Message></Error>";
        return e;
    });
    CHECK_THROWS_AS(f.writeAll("/bucket/big.bin", big, 5), SftpError);
    CHECK(f.server.openUploads() == 1);  // kept for the next try
    CHECK_FALSE(f.server.get("bucket", "big.bin"));
    CHECK(bufferedNow() == base);

    failPart3 = false;
    const int before = f.server.count("UploadPart");
    f.writeAll("/bucket/big.bin", big, 5);
    CHECK(bufferedNow() == base);
    CHECK(f.server.get("bucket", "big.bin")->data == big);
    CHECK(f.server.openUploads() == 0);
    CHECK(f.server.count("ListParts") == 1);
    CHECK(f.server.count("UploadPart") - before < 5);  // only what was missing

    // Cancel: the multipart upload is aborted and the existing object stays as it was. Cancelled at the first
    // progress report: a fast machine may report the whole upload in one or two, but always before completing it.
    f.server.put("bucket", "keep.bin", "old content");
    CHECK_THROWS_AS(f.writeAll("/bucket/keep.bin", big, 6, 256 * 1024, [&](uint64_t) { throw Cancelled(); }), Cancelled);
    CHECK(bufferedNow() == base);
    CHECK(f.server.openUploads() == 0);
    CHECK(f.server.get("bucket", "keep.bin")->data == "old content");
}

TEST_CASE("s3fs: memory stays bounded by the parts in flight") {
    TestS3Options o;
    o.discardData = true;
    S3FsFixture f({}, o, 8ull << 20, 4);
    const uint64_t size = 96ull << 20;
    f.server.putGenerated("bucket", "big.bin", size);

    // Measured from what is counted now (the counter is shared by the whole process), and back to it afterwards.
    resetS3BufferedHighWater();
    const uint64_t base = s3BufferedHighWater();
    {
        auto r = f.fs->openRead("/bucket/big.bin", 0);
        uint64_t got = 0;
        for (bool done = false; !done;) got += r->read(done)->size();
        CHECK(got == size);
    }
    const uint64_t readHigh = s3BufferedHighWater() - base;
    CHECK(readHigh <= 4 * (8ull << 20) + (1ull << 20));
    resetS3BufferedHighWater();
    CHECK(s3BufferedHighWater() == base);

    WriteSpec spec;
    spec.size = size;
    {
        auto w = f.fs->openWrite("/bucket/up.bin", spec);
        const std::string chunk(256 * 1024, 'u');
        for (uint64_t off = 0; off < size; off += chunk.size()) w->write(std::make_shared<std::string>(chunk), [](uint64_t) {});
        w->finish(0, [](uint64_t) {});
    }
    // 4 parts in flight, 1 waiting for room, and the start of the next.
    const uint64_t writeHigh = s3BufferedHighWater() - base;
    CHECK(writeHigh <= 5 * (8ull << 20) + (1ull << 20));
    resetS3BufferedHighWater();
    CHECK(s3BufferedHighWater() == base);
    CHECK(f.server.get("bucket", "up.bin")->size() == size);
}

TEST_CASE("s3fs: presigned links") {
    S3FsFixture f;
    f.server.put("bucket", "a b.txt", "shared");
    const std::string url = f.fs->presign("/bucket/a b.txt", 3600);
    sys::HttpRequest req;
    req.url = url;
    const auto r = f.conn->http->send(req);
    CHECK(r.status == 200);
    CHECK(r.body == "shared");
    CHECK_THROWS_AS(f.fs->presign("/bucket", 60), SftpError);
}
