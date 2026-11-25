// The version check (app/UpdateCheck.h): versions read strictly, GitHub's answer for releases/latest, and the request
// itself against a local test server (answers, errors, an oversized or slow answer, an untrusted certificate).
#include "app/UpdateCheck.h"

#include "platform/Platform.h"

#include "TestHttpServer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace st;
using namespace st::update;
using namespace st::test;

namespace {

std::string release(const std::string& tag, bool draft = false, bool prerelease = false, const std::string& extra = "") {
    return R"({"tag_name":")" + tag + R"(","name":"SSHTermaq )" + tag + R"(","draft":)" + (draft ? "true" : "false") +
           R"(,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"html_url":"https://github.com/f1resh/sshtermaq/releases/tag/)" + tag + R"(","body":"Fixes")" + extra + "}";
}

TestHttpResponse json(std::string body, int status = 200) {
    TestHttpResponse r;
    r.status = status;
    r.headers = {{"Content-Type", "application/json"}};
    r.body = std::move(body);
    return r;
}

}  // namespace

TEST_CASE("update: versions are N.N.N, with an optional v, and compare as numbers", "[app][update]") {
    REQUIRE(parseVersion("1.2.0"));
    CHECK(parseVersion("v1.10.3")->minor == 10);
    CHECK(parseVersion("V2.0.0")->major == 2);
    for (const char* bad : {"", "1.2", "1.2.0.4", "1.2.0-beta", "1.2.0 ", " 1.2.0", "a.b.c", "1..0", "1.2.x", "123456.0.0",
                            "vv1.2.0", "+1.2.0", "1.-2.0"})
        CHECK_FALSE(parseVersion(bad));
    auto v = [](const char* s) { return *parseVersion(s); };
    CHECK(compareVersions(v("1.2.0"), v("1.10.0")) < 0);  // not as text
    CHECK(compareVersions(v("1.10.0"), v("1.9.9")) > 0);
    CHECK(compareVersions(v("2.0.0"), v("1.99.99")) > 0);
    CHECK(compareVersions(v("1.2.1"), v("1.2.0")) > 0);
    CHECK(compareVersions(v("v1.2.0"), v("1.2.0")) == 0);
}

TEST_CASE("update: GitHub's answer gives the version and a release page built here", "[app][update]") {
    auto r = parseLatestRelease(release("1.3.0"));
    REQUIRE(r);
    CHECK(r->version == "1.3.0");
    CHECK(r->url == "https://github.com/f1resh/sshtermaq/releases/tag/1.3.0");

    r = parseLatestRelease(release("v1.3.0"));
    REQUIRE(r);
    CHECK(r->version == "1.3.0");
    CHECK(r->url == "https://github.com/f1resh/sshtermaq/releases/tag/v1.3.0");

    // The answer's own links are never used.
    r = parseLatestRelease(R"({"tag_name":"1.4.0","html_url":"https://evil.example/x","draft":false,"prerelease":false})");
    REQUIRE(r);
    CHECK(r->url == "https://github.com/f1resh/sshtermaq/releases/tag/1.4.0");

    std::string why;
    CHECK_FALSE(parseLatestRelease(release("1.3.0", true), &why));
    CHECK(why.find("draft") != std::string::npos);
    CHECK_FALSE(parseLatestRelease(release("1.3.0", false, true)));
    CHECK_FALSE(parseLatestRelease(release("1.3.0-rc1")));
    CHECK_FALSE(parseLatestRelease(release("1.3.0/../../evil")));
    CHECK_FALSE(parseLatestRelease(R"({"name":"no tag"})"));
    CHECK_FALSE(parseLatestRelease(R"({"tag_name":130})"));
    CHECK_FALSE(parseLatestRelease("[]"));
    CHECK_FALSE(parseLatestRelease("<html>rate limited</html>", &why));
    CHECK(why.find("JSON") != std::string::npos);
}

TEST_CASE("update: the request, and what it sends", "[app][update]") {
    std::string answer = release("1.3.0");
    int status = 200;
    std::vector<TestHttpRequest> seen;
    TestHttpServer server([&](const TestHttpRequest& r) {
        seen.push_back(r);
        return json(answer, status);
    });
    const std::string url = server.url() + "/repos/f1resh/sshtermaq/releases/latest";

    SECTION("a newer release") {
        const CheckResult c = checkLatestRelease("1.2.0", url);
        REQUIRE(c.ok);
        CHECK(c.newer);
        CHECK(c.latest->version == "1.3.0");
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].method == "GET");
        CHECK(seen[0].target == "/repos/f1resh/sshtermaq/releases/latest");
        CHECK(seen[0].header("User-Agent") == "SSHTermaq");  // nothing about the version, the OS or the user
        CHECK(seen[0].header("Accept") == "application/vnd.github+json");
        CHECK(seen[0].header("Authorization").empty());
        CHECK(seen[0].header("Cookie").empty());
        CHECK(seen[0].body.empty());
    }
    SECTION("the same or an older release: not newer") {
        CHECK_FALSE(checkLatestRelease("1.3.0", url).newer);
        CHECK(checkLatestRelease("1.3.0", url).ok);
        CHECK_FALSE(checkLatestRelease("1.10.0", url).newer);
    }
    SECTION("errors: a status, junk, a pre-release, an oversized answer") {
        status = 403;  // GitHub's rate limit
        CheckResult c = checkLatestRelease("1.2.0", url);
        CHECK_FALSE(c.ok);
        CHECK(c.error.find("403") != std::string::npos);
        CHECK_FALSE(c.latest);

        status = 200;
        answer = "not json";
        CHECK_FALSE(checkLatestRelease("1.2.0", url).ok);
        answer = release("1.3.0", false, true);
        CHECK_FALSE(checkLatestRelease("1.2.0", url).ok);

        answer = release("1.3.0", false, false, R"(,"padding":")" + std::string(kMaxAnswerBytes, 'x') + "\"");
        c = checkLatestRelease("1.2.0", url);
        CHECK_FALSE(c.ok);
        CHECK(c.error.find("larger") != std::string::npos);
    }
    SECTION("a running version that is not N.N.N asks nothing") {
        CHECK_FALSE(checkLatestRelease("dev", url).ok);
        CHECK(seen.empty());
    }
}

TEST_CASE("update: a slow server, no server, an untrusted certificate", "[app][update]") {
    TestHttpServer slow([](const TestHttpRequest&) {
        TestHttpResponse r = json(release("1.3.0"));
        r.delayMs = 8000;
        return r;
    });
    CheckResult c = checkLatestRelease("1.2.0", slow.url() + "/latest", 700);  // WinHTTP: after about 4 s
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.error.empty());

    int port = 0;
    {
        TestHttpServer gone([](const TestHttpRequest&) { return httpStatus(200); });
        port = gone.port();
    }
    c = checkLatestRelease("1.2.0", "http://127.0.0.1:" + std::to_string(port) + "/latest", 2000);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("no answer") != std::string::npos);

    TestHttpServerOptions o;
    o.tls = true;
    TestHttpServer selfSigned([](const TestHttpRequest&) { return json(release("1.3.0")); }, o);
    c = checkLatestRelease("1.2.0", selfSigned.url() + "/latest");
    CHECK_FALSE(c.ok);  // no pinned certificates here: only what the system trusts
    CHECK(selfSigned.requests() == 0);
}

TEST_CASE("update: this test run is not a Store package", "[app][update]") {
    const std::string channel = sys::installChannel();
    CHECK((channel == "other" || channel == "homebrew"));
    CHECK(channel != "store");
}
