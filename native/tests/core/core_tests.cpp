#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Types.h"
#include "platform/sys/Fs.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <thread>
#include <vector>

using namespace st;

TEST_CASE("strings: UTF-8 path roundtrip and helpers") {
    std::string s = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80";
    REQUIRE(sys::fromPath(sys::toPath(s)) == s);
    REQUIRE(sys::fromGenericPath(sys::toPath("a") / sys::toPath(s)) == "a/" + s);
    REQUIRE(split("a,,b", ',').size() == 3);
    REQUIRE(split("a,,b", ',', true).size() == 2);
    REQUIRE(trim("  x y \t") == "x y");
}

TEST_CASE("strings: UTF-8 validation and UTF-16LE conversion") {
    REQUIRE(isValidUtf8(""));
    REQUIRE(isValidUtf8("plain ascii"));
    REQUIRE(isValidUtf8("h\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
    REQUIRE_FALSE(isValidUtf8("\xC3"));              // truncated
    REQUIRE_FALSE(isValidUtf8("\xC0\xAF"));          // overlong '/'
    REQUIRE_FALSE(isValidUtf8("\xED\xA0\x80"));      // UTF-16 surrogate
    REQUIRE_FALSE(isValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    REQUIRE_FALSE(isValidUtf8("\xE9t\xE9"));         // Latin-1 bytes

    const std::string text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // A é € 😀
    const std::string le = utf8ToUtf16le(text);
    REQUIRE(le == std::string("A\0\xE9\0\xAC\x20\x3D\xD8\x00\xDE", 10));
    REQUIRE(utf16leToUtf8(le) == text);
    REQUIRE(utf16leToUtf8(std::string("a\0\0\0", 4)) == std::string("a\0", 2));    // NULs are kept
    REQUIRE(utf16leToUtf8(std::string("a\0b", 3)) == "a");                          // odd trailing byte ignored
    REQUIRE(utf16leToUtf8(std::string("\x00\xD8" "a\0", 4)) == "\xEF\xBF\xBD" "a");  // lone high surrogate
    REQUIRE(utf16leToUtf8(std::string("\x00\xDC", 2)) == "\xEF\xBF\xBD");             // lone low surrogate
    REQUIRE(utf8ToUtf16le("\xFF") == std::string("\xFD\xFF", 2));                     // invalid UTF-8 -> U+FFFD
}

TEST_CASE("crypto: base64 + fingerprint") {
    std::string out;
    REQUIRE(crypto::base64Encode("hello") == "aGVsbG8=");
    REQUIRE(crypto::base64Decode("aGVsbG8", out));
    REQUIRE(out == "hello");
    REQUIRE(crypto::fingerprintSha256("abc").rfind("SHA256:", 0) == 0);
    std::string blob = std::string("\0\0\0\x0bssh-ed25519", 15) + std::string(36, 'x');
    REQUIRE(crypto::keyTypeOfBlob(blob) == "ssh-ed25519");
}

TEST_CASE("types: session defaults and lenient parsing") {
    SessionConfig s = sessionFromJson(Json{{"host", "example.com"}, {"protocol", "telnet"}, {"folder", "/a//b/"}});
    REQUIRE(s.host == "example.com");
    REQUIRE(s.protocol == Protocol::Telnet);
    REQUIRE(s.folder == "a/b");
    REQUIRE(s.connection.terminalType == "xterm-256color");
    REQUIRE(s.ssh.auth.tryAgent);
    Json j = s;
    REQUIRE(j["protocol"] == "telnet");
    REQUIRE(j["keyboard"]["backspace"] == "del");
    REQUIRE(j["serial"]["flowControl"] == "xonxoff");
}

namespace {
struct InlineDispatcher : Dispatcher {
    void post(std::function<void()> fn) override { fn(); }
    bool isUiThread() const override { return true; }
};
}  // namespace

TEST_CASE("bridge: invoke, send and events") {
    InlineDispatcher d;
    Bridge b(d);
    std::vector<std::string> out;
    b.setSink([&](const std::string& s) { out.push_back(s); });
    b.handleSync("t:add", [](const Json& a) { return Json(argInt(a, 0) + argInt(a, 1)); });
    b.handleSync("t:fail", [](const Json&) -> Json { throw std::runtime_error("boom"); });
    int sent = 0;
    b.on("t:ping", [&](const Json&) { ++sent; });

    b.onWebMessage(R"({"t":"req","id":1,"ch":"t:add","args":[2,3]})");
    b.onWebMessage(R"({"t":"req","id":2,"ch":"t:fail","args":[]})");
    b.onWebMessage(R"({"t":"req","id":3,"ch":"t:missing","args":[]})");
    b.onWebMessage(R"({"t":"send","ch":"t:ping","args":[]})");
    b.emit("t:evt", Json::array({"x", 1}));

    REQUIRE(out.size() == 4);
    auto r1 = Json::parse(out[0]);
    REQUIRE(r1["ok"] == true);
    REQUIRE(r1["result"] == 5);
    auto r2 = Json::parse(out[1]);
    REQUIRE(r2["ok"] == false);
    REQUIRE(r2["error"] == "boom");
    REQUIRE(Json::parse(out[2])["ok"] == false);
    REQUIRE(sent == 1);
    auto e = Json::parse(out[3]);
    REQUIRE(e["t"] == "evt");
    REQUIRE(e["ch"] == "t:evt");
    REQUIRE(e["args"][1] == 1);
}

namespace {
/** Messages posted from other threads wait until pump(), like the UI thread's queue. */
struct QueueDispatcher : Dispatcher {
    std::vector<std::function<void()>> queued;
    std::thread::id ui = std::this_thread::get_id();
    void post(std::function<void()> fn) override { queued.push_back(std::move(fn)); }
    bool isUiThread() const override { return std::this_thread::get_id() == ui; }
    void pump() {
        auto q = std::move(queued);
        queued.clear();
        for (auto& f : q) f();
    }
};
}  // namespace

TEST_CASE("bridge: a reloaded page gets no answers or events meant for the page before it") {
    QueueDispatcher d;
    Bridge b(d);
    std::vector<Json> out;
    b.setSink([&](const std::string& s) { out.push_back(Json::parse(s)); });
    std::vector<Bridge::Responder> held;
    b.handle("t:later", [&](const Json&, Bridge::Responder r) { held.push_back(r); });
    b.handleSync("t:now", [](const Json& a) { return Json(argInt(a, 0)); });

    // The first page asks (id 1) and is answered later.
    b.onWebMessage(R"({"t":"req","id":1,"ch":"t:later","args":[]})");
    REQUIRE(held.size() == 1);
    // An event emitted from another thread is still queued when the page reloads.
    std::thread([&] { b.emit("t:old", Json::array({1})); }).join();
    REQUIRE(d.queued.size() == 1);

    b.newPage();
    d.pump();
    CHECK(out.empty());  // the queued event was for the old page

    // The new page starts its ids at 1 again: its own answer arrives, the old page's late one does not.
    b.onWebMessage(R"({"t":"req","id":1,"ch":"t:now","args":[42]})");
    REQUIRE(out.size() == 1);
    CHECK(out[0]["id"] == 1);
    CHECK(out[0]["result"] == 42);
    held[0].resolve(Json("stale"));
    std::thread([&] { held[0].reject("also stale"); }).join();
    d.pump();
    CHECK(out.size() == 1);

    // Old behaviour within one page: answers and events (also from other threads) arrive.
    b.onWebMessage(R"({"t":"req","id":2,"ch":"t:later","args":[]})");
    REQUIRE(held.size() == 2);
    std::thread([&] {
        held[1].resolve(Json("fresh"));
        b.emit("t:new", Json::array({2}));
    }).join();
    d.pump();
    REQUIRE(out.size() == 3);
    CHECK(out[1]["id"] == 2);
    CHECK(out[1]["result"] == "fresh");
    CHECK(out[2]["ch"] == "t:new");

    // A request the old page never got an answer to, dropped after the reload, says nothing to the new page.
    b.onWebMessage(R"({"t":"req","id":3,"ch":"t:later","args":[]})");
    b.newPage();
    held.clear();  // the last copy goes: "dropped without a response", for the old page only
    d.pump();
    CHECK(out.size() == 3);
}

TEST_CASE("log: a full log is kept as <name>.1.log and a new one starts") {
    namespace fs = std::filesystem;
    REQUIRE(applog::previousLogFile(fs::path("logs") / "sshtermaq.log") == fs::path("logs") / "sshtermaq.1.log");

    const fs::path dir = fs::temp_directory_path() / ("st-log-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const fs::path file = dir / "app.log";
    {
        std::ofstream(dir / "app.1.log") << "oldest";
        std::ofstream big(file, std::ios::binary);
        big << std::string(static_cast<size_t>(applog::kRotateBytes) + 1, 'x');
    }
    applog::init(file);
    applog::info("after rotation");
    applog::close();
    CHECK(fs::file_size(dir / "app.1.log") == applog::kRotateBytes + 1);
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    CHECK(line.find("[INFO] after rotation") != std::string::npos);

    // Under the limit: appended to, not rotated.
    applog::init(file);
    applog::info("second start");
    applog::close();
    CHECK(fs::file_size(dir / "app.1.log") == applog::kRotateBytes + 1);
    std::ifstream again(file);
    std::string all((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    CHECK(all.find("after rotation") != std::string::npos);
    CHECK(all.find("second start") != std::string::npos);
    again.close();
    in.close();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
