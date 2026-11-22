// platform/sys/Http.h against the test HTTP(S) server: bodies, streaming, keep-alive, cancel, timeouts, faults and
// the certificate rules (untrusted refused, pinned accepted, nothing sent to a server whose certificate is refused).
#include "platform/sys/Http.h"

#include "TestHttpServer.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace st;
using namespace st::test;
using Clock = std::chrono::steady_clock;

namespace {

TestHttpResponse echo(const TestHttpRequest& r) {
    TestHttpResponse out;
    out.headers = {{"X-Method", r.method}, {"X-Target", r.target}, {"X-Custom", r.header("X-Custom")}};
    out.body = r.body.empty() ? "hello" : r.body;
    return out;
}

sys::HttpSessionOptions fastOptions() {
    sys::HttpSessionOptions o;
    o.proxy.mode = sys::HttpProxy::Mode::None;
    o.connectTimeoutMs = 3000;
    o.ioTimeoutMs = 3000;
    return o;
}

}  // namespace

TEST_CASE("http: URLs") {
    sys::UrlParts u;
    REQUIRE(sys::splitUrl("https://Bucket.s3.amazonaws.com/a%20b?x=1", u));
    CHECK(u.scheme == "https");
    CHECK(u.host == "Bucket.s3.amazonaws.com");
    CHECK(u.effectivePort() == 443);
    CHECK(u.pathAndQuery == "/a%20b?x=1");
    REQUIRE(sys::splitUrl("HTTP://[::1]:9000", u));
    CHECK(u.scheme == "http");
    CHECK(u.host == "::1");
    CHECK(u.port == 9000);
    CHECK(u.pathAndQuery == "/");
    REQUIRE(sys::splitUrl("http://h?x", u));
    CHECK(u.pathAndQuery == "/?x");
    CHECK_FALSE(sys::splitUrl("ftp://h/", u));
    CHECK_FALSE(sys::splitUrl("http://user@h/", u));
    CHECK_FALSE(sys::splitUrl("http://h:0/", u));
    CHECK_FALSE(sys::splitUrl("http://h:99999/", u));
    CHECK_FALSE(sys::splitUrl("http:///x", u));
}

TEST_CASE("http: requests, headers, bodies, keep-alive") {
    TestHttpServer server(echo);
    auto http = sys::openHttpSession(fastOptions());

    sys::HttpRequest get;
    get.url = server.url() + "/a%20b?x=%2F&y";
    get.headers = {{"X-Custom", "v1"}, {"Host", "ignored"}};
    sys::HttpResponse r = http->send(get);
    REQUIRE(r.status == 200);
    CHECK(r.body == "hello");
    CHECK(r.header("x-method") == "GET");
    CHECK(r.header("X-Target") == "/a%20b?x=%2F&y");  // sent exactly as given
    CHECK(r.header("X-Custom") == "v1");

    sys::HttpRequest put;
    put.method = "PUT";
    put.url = server.url() + "/obj";
    put.body = std::make_shared<std::string>(3 * 1024 * 1024 + 17, 'z');
    uint64_t lastSent = 0;
    put.onSent = [&](uint64_t n) { lastSent = n; };
    std::string streamed;
    put.onBody = [&](std::string_view c) {
        streamed.append(c);
        return true;
    };
    r = http->send(put);
    REQUIRE(r.status == 200);
    CHECK(lastSent == put.body->size());
    CHECK(streamed == *put.body);
    CHECK(r.body.empty());

    sys::HttpRequest head;
    head.method = "HEAD";
    head.url = server.url() + "/x";
    r = http->send(head);
    CHECK(r.status == 200);
    CHECK(r.header("Content-Length") == "5");
    CHECK(r.body.empty());

    for (int i = 0; i < 5; ++i) REQUIRE(http->send(get).status == 200);
    CHECK(server.connections() <= 2);  // kept alive
}

TEST_CASE("http: redirects are not followed; errors") {
    TestHttpServer server([](const TestHttpRequest&) {
        TestHttpResponse r;
        r.status = 307;
        r.headers = {{"Location", "http://elsewhere.invalid/"}};
        return r;
    });
    auto http = sys::openHttpSession(fastOptions());
    sys::HttpRequest req;
    req.url = server.url() + "/";
    const sys::HttpResponse r = http->send(req);
    CHECK(r.status == 307);
    CHECK(r.header("Location") == "http://elsewhere.invalid/");

    int closedPort = 0;
    { closedPort = TestHttpServer(echo).port(); }
    req.url = "http://127.0.0.1:" + std::to_string(closedPort) + "/";
    const sys::HttpResponse refused = http->send(req);
    CHECK(refused.status == 0);
    CHECK_FALSE(refused.error.empty());
    CHECK_FALSE(refused.cancelled);

    req.url = "not a url";
    CHECK(http->send(req).status == 0);
}

TEST_CASE("http: a dropped connection and a stalled answer fail; cancel is quick") {
    TestHttpResponse fault;
    TestHttpServer server([&](const TestHttpRequest&) { return fault; });
    sys::HttpSessionOptions o = fastOptions();
    o.ioTimeoutMs = 700;
    auto http = sys::openHttpSession(o);
    sys::HttpRequest req;
    req.url = server.url() + "/f";

    fault = TestHttpResponse{};
    fault.body = std::string(100000, 'b');
    fault.dropAfterBytes = 5000;
    sys::HttpResponse r = http->send(req);
    CHECK(r.status == 0);
    CHECK_FALSE(r.error.empty());

    fault = TestHttpResponse{};
    fault.closeWithoutAnswer = true;
    CHECK(http->send(req).status == 0);

    fault = TestHttpResponse{};
    fault.body = std::string(100000, 'b');
    fault.stallAfterBodyBytes = 1000;
    auto t0 = Clock::now();
    r = http->send(req);
    const auto stalledMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CAPTURE(stalledMs, r.error);
    CHECK(r.status == 0);
    CHECK(r.timedOut);
    // WinHTTP checks its timeouts every 4 seconds or so: 300 ms to 3 s all end after about 4 s, 6 s after 8 s. The
    // stalled server never answers, so ending at all is the point; 10 s leaves room for a busy machine.
    CHECK(stalledMs < 10000);

    fault = TestHttpResponse{};
    fault.delayMs = 10000;
    std::atomic<bool> cancel{false};
    req.cancel = &cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel = true;
    });
    t0 = Clock::now();
    r = http->send(req);
    canceller.join();
    CHECK(r.cancelled);
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(1500));

    // onBody returning false stops the download.
    fault = TestHttpResponse{};
    fault.generatedSize = 50 * 1024 * 1024;
    fault.generate = [](uint64_t, char* out, size_t n) { std::fill(out, out + n, 'g'); };
    sys::HttpRequest big;
    big.url = server.url() + "/big";
    size_t got = 0;
    big.onBody = [&](std::string_view c) {
        got += c.size();
        return got < 1024 * 1024;
    };
    r = http->send(big);
    CHECK(r.cancelled);
    CHECK(got < 10 * 1024 * 1024);
}

TEST_CASE("http: parallel requests on one session") {
    TestHttpServer server(echo);
    auto http = sys::openHttpSession(fastOptions());
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&, i] {
            for (int k = 0; k < 10; ++k) {
                sys::HttpRequest req;
                req.method = "POST";
                req.url = server.url() + "/p";
                req.body = std::make_shared<std::string>("body-" + std::to_string(i) + "-" + std::to_string(k));
                const sys::HttpResponse r = http->send(req);
                if (r.status == 200 && r.body == *req.body) ++ok;
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(ok == 80);
}

TEST_CASE("http: close aborts running requests") {
    TestHttpServer server([](const TestHttpRequest&) {
        TestHttpResponse r;
        r.delayMs = 10000;
        return r;
    });
    auto http = sys::openHttpSession(fastOptions());
    sys::HttpResponse r;
    std::thread t([&] {
        sys::HttpRequest req;
        req.url = server.url() + "/slow";
        r = http->send(req);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = Clock::now();
    http->close();
    t.join();
    CHECK(r.status == 0);
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(1500));
    sys::HttpRequest after;
    after.url = server.url() + "/";
    CHECK(http->send(after).status == 0);
}

TEST_CASE("http: certificates - untrusted refused, probed, pinned") {
    TestHttpServerOptions tls;
    tls.tls = true;
    TestHttpServer server(echo, tls);
    REQUIRE(server.certificateSha256().size() == 64);

    auto http = sys::openHttpSession(fastOptions());
    sys::HttpRequest req;
    req.url = server.url() + "/secret";
    req.headers = {{"Authorization", "AWS4-HMAC-SHA256 must-not-leak"}};
    sys::HttpResponse r = http->send(req);
    CHECK(r.status == 0);
    CHECK(r.certificateError);
    CHECK(server.requests() == 0);

    const sys::HttpCertificate cert = http->probeCertificate(server.url() + "/");
    REQUIRE(cert.ok);
    CHECK(cert.sha256 == server.certificateSha256());
    CHECK_FALSE(cert.trusted);
    CHECK_FALSE(cert.problem.empty());
    CHECK(cert.subject == "st-test");
    CHECK(cert.issuer == "st-test");
    CHECK(cert.validTo.size() == 10);
    CHECK(server.log() == std::vector<std::string>{"HEAD /"});  // the probe sends nothing signed

    // A wrong pin: refused before the request is written.
    http->setPinnedCertificates({std::string(64, 'a')});
    r = http->send(req);
    CHECK(r.status == 0);
    CHECK(r.certificateError);
    CHECK(server.requests() == 1);

    // The right pin: accepted.
    http->setPinnedCertificates({server.certificateSha256()});
    r = http->send(req);
    CHECK(r.status == 200);
    CHECK(r.header("X-Target") == "/secret");

    // A pinned session refuses a server with another (unpinned) certificate.
    TestHttpServer other(echo, tls);
    sys::HttpRequest toOther;
    toOther.url = other.url() + "/secret";
    r = http->send(toOther);
    CHECK(r.certificateError);
    CHECK(other.requests() == 0);

    // A plain http:// URL has no certificate.
    TestHttpServer plain(echo);
    CHECK_FALSE(http->probeCertificate(plain.url() + "/").ok);
}
