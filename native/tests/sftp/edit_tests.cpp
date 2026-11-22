// Edit: download to a temp copy, "launch the editor" (a test hook), upload every saved change, ask when the server
// copy changed meanwhile, stop watching on request.
#include "SftpFixture.h"

#include "platform/sys/Fs.h"
#include "sftp/LocalOpen.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <thread>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/** Count of onEdit events with the given state for an edit id. */
size_t editEvents(SftpFixture& f, const std::string& editId, const std::string& state) {
    size_t n = 0;
    for (const auto& e : f.h.eventsOn("sftp:onEdit"))
        if (e["args"][0]["editId"] == editId && e["args"][0]["state"] == state) ++n;
    return n;
}

bool waitEditState(SftpFixture& f, const std::string& editId, const std::string& state, size_t count,
                   std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (editEvents(f, editId, state) >= count) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

}  // namespace

TEST_CASE("edit: saved changes are uploaded", "[edit]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path();
    spit(r / "config.ini", "a=1\n");
    setMtime(r / "config.ini", 1700000000);

    std::mutex lm;
    std::vector<std::pair<std::string, std::string>> launched;
    f.sftp->setEditLauncherForTests([&](const std::string& path, const std::string& editor) {
        std::lock_guard lk(lm);
        launched.emplace_back(path, editor);
    });

    const std::string editId = f.h.invoke("sftp:edit", Json::array({f.id, "/config.ini", Json{{"editor", "C:\\Tools\\ed.exe"}}}));
    REQUIRE(waitEditState(f, editId, "opened", 1));
    fs::path local;
    {
        std::lock_guard lk(lm);
        REQUIRE(launched.size() == 1);
        CHECK(launched[0].second == "C:\\Tools\\ed.exe");
        local = sys::toPath(launched[0].first);
    }
    CHECK(slurp(local) == "a=1\n");
    CHECK(local.filename() == "config.ini");

    // First save
    spit(local, "a=2\n");
    REQUIRE(waitEditState(f, editId, "uploaded", 1));
    CHECK(slurp(r / "config.ini") == "a=2\n");
    CHECK(editEvents(f, editId, "uploading") >= 1);

    // Second save
    std::this_thread::sleep_for(1100ms);  // a different mtime second on the server
    spit(local, "a=3\n");
    REQUIRE(waitEditState(f, editId, "uploaded", 2));
    CHECK(slurp(r / "config.ini") == "a=3\n");

    SECTION("a server copy changed meanwhile asks first") {
        spit(r / "config.ini", "changed by someone else\n");
        setMtime(r / "config.ini", 1800000000);
        spit(local, "a=4\n");
        Json c = f.waitConflict();
        CHECK(c["reason"] == "changed");
        CHECK(c["direction"] == "upload");
        SECTION("keep the server copy") {
            f.resolve(c, "skip");
            REQUIRE(waitEditState(f, editId, "error", 1));
            CHECK(slurp(r / "config.ini") == "changed by someone else\n");
        }
        SECTION("overwrite it") {
            f.resolve(c, "overwrite");
            REQUIRE(waitEditState(f, editId, "uploaded", 3));
            CHECK(slurp(r / "config.ini") == "a=4\n");
        }
    }

    SECTION("stop watching") {
        f.h.send("sftp:stopEdit", Json::array({editId}));
        REQUIRE(waitEditState(f, editId, "closed", 1));
        spit(local, "ignored\n");
        std::this_thread::sleep_for(2000ms);
        CHECK(slurp(r / "config.ini") == "a=3\n");
    }
}

TEST_CASE("edit: missing files fail cleanly", "[edit]") {
    SftpFixture f;
    f.sftp->setEditLauncherForTests([](const std::string&, const std::string&) { FAIL("must not launch"); });
    CHECK_FALSE(f.h.invokeError("sftp:edit", Json::array({f.id, "/nope.txt", Json::object()})).empty());
    CHECK(f.h.eventsOn("sftp:onEdit").empty());
}

TEST_CASE("edit: the temp folder for opened files falls back when the system's is missing", "[edit]") {
    // %TEMP% naming a folder that does not exist makes std::filesystem::temp_directory_path() fail ("" here).
    TempDir dir;
    const fs::path system = dir.path() / "system", tmp = dir.path() / "tmp", home = dir.path() / "home";
    std::map<std::string, std::string> vars;
    auto env = [&vars](const std::string& name) -> std::optional<std::string> {
        const auto it = vars.find(name);
        return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
    CHECK(sftpimpl::chooseTempFolder(system, env, home) == system);
    CHECK(sftpimpl::chooseTempFolder({}, env, home) == home);
    vars["TEMP"] = sys::fromPath(tmp);
    CHECK(sftpimpl::chooseTempFolder({}, env, home) == tmp);   // a missing folder is fine: it is created later
    vars["TMP"] = "relative/folder";                             // only absolute paths count
    CHECK(sftpimpl::chooseTempFolder({}, env, home) == tmp);
    vars["TMPDIR"] = sys::fromPath(system);
    CHECK(sftpimpl::chooseTempFolder({}, env, home) == system);
    CHECK(sftpimpl::chooseTempFolder({}, env, {}) == system);
    vars.clear();
    CHECK(sftpimpl::chooseTempFolder({}, env, {}).empty());
}

/* ---- a reloaded page: its file sessions end quietly (review 6) ---- */

namespace {

/** Events of the file manager's channels about one file session. */
size_t sessionEvents(SftpFixture& f, const std::string& sftpId) {
    size_t n = 0;
    for (const char* ch : {"sftp:onProgress", "sftp:onConflict", "sftp:onEdit", "sftp:onFind"})
        for (const auto& e : f.h.eventsOn(ch))
            if (e["args"][0].value("sftpId", "") == sftpId) ++n;
    return n;
}

/** An edit being watched and a download waiting at a conflict question, on `f`'s file session. */
std::pair<std::string, fs::path> busySession(SftpFixture& f, std::mutex& lm, std::vector<std::string>& launched) {
    const fs::path r = f.remoteRoot.path();
    spit(r / "config.ini", "a=1\n");
    spit(r / "report.txt", "remote");
    spit(f.local.path() / "report.txt", "local");
    f.sftp->setEditLauncherForTests([&](const std::string& path, const std::string&) {
        std::lock_guard lk(lm);
        launched.push_back(path);
    });
    const std::string editId = f.h.invoke("sftp:edit", Json::array({f.id, "/config.ini", Json::object()}));
    REQUIRE(waitEditState(f, editId, "opened", 1));
    f.download("/report.txt", f.local.path());
    f.waitConflict();
    std::lock_guard lk(lm);
    return {editId, sys::toPath(launched.at(0))};
}

}  // namespace

TEST_CASE("edit: a page reset ends the file sessions without a word to the new page", "[edit][reload]") {
    SftpFixture f;
    std::mutex lm;
    std::vector<std::string> launched;
    const auto [editId, local] = busySession(f, lm, launched);
    const size_t before = sessionEvents(f, f.id);

    CHECK(f.sessions->closeForPageReset() == 1);
    CHECK(f.sftp->closeForPageReset() == 1);
    spit(local, "a=2\n");  // a save after the reset is not uploaded: the edit is no longer watched
    std::this_thread::sleep_for(2500ms);
    CHECK(slurp(f.remoteRoot.path() / "config.ini") == "a=1\n");
    CHECK(slurp(f.local.path() / "report.txt") == "local");  // the waiting download went nowhere
    CHECK(sessionEvents(f, f.id) == before);                  // no "cancelled", no "closed"
    CHECK(f.h.invokeError("sftp:list", Json::array({f.id, "/"})).find("Unknown or closed") != std::string::npos);
    CHECK(f.sftp->closeForPageReset() == 0);
}

TEST_CASE("edit: closing a file session the normal way still reports its edits and transfers", "[edit][reload]") {
    SftpFixture f;
    std::mutex lm;
    std::vector<std::string> launched;
    const auto [editId, local] = busySession(f, lm, launched);
    (void)local;
    f.h.send("sftp:close", Json::array({f.id}));
    REQUIRE(waitEditState(f, editId, "closed", 1));
    CHECK(f.h.waitEvent([&](const Json& e) {
        return e["ch"] == "sftp:onProgress" && e["args"][0]["sftpId"] == f.id && e["args"][0]["state"] == "cancelled";
    }, 10s));
    f.id.clear();  // closed already
}
