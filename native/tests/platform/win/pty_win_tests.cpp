// platform/sys/Pty.h on Windows: ConPTY with cmd.exe (always there), the job object, the shell list.
#include "PtyHarness.h"
#include "Scratch.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Pty.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>

using namespace st;
using st::test::PtyRun;
using st::test::Scratch;

namespace {

sys::pty::Options cmd(const std::string& args, const std::string& dir) {
    sys::pty::Options o;
    o.command = "cmd.exe /d " + args;
    o.startDir = dir;
    return o;
}

}  // namespace

TEST_CASE("pty (Windows): ConPTY is there on the Windows versions we build for", "[platform][pty]") {
    std::string reason;
    REQUIRE(sys::pty::supported(reason));
}

TEST_CASE("pty (Windows): runs a program, passes its output and exit code", "[platform][pty]") {
    Scratch tmp;
    PtyRun run;
    REQUIRE(run.start(cmd("/c \"echo hello-pty& exit 3\"", sys::fromPath(tmp.path()))));
    REQUIRE(run.waitFor("hello-pty"));
    REQUIRE(run.finish().code == 3);
    REQUIRE(run.outputEnded());
}

TEST_CASE("pty (Windows): input, resize, environment and a non-ASCII start folder", "[platform][pty]") {
    Scratch tmp;
    const auto dir = tmp.path() / sys::toPath("папка ünï");
    std::filesystem::create_directories(dir);
    sys::pty::Options o = cmd("/q /k", sys::fromPath(dir));
    o.environment = {{"ST_PTY_TEST", "value-42"}, {"ST_PTY_GONE", ""}};
    PtyRun run;
    REQUIRE(run.start(o));

    run.pty().write("echo [%ST_PTY_TEST%]\r");
    REQUIRE(run.waitFor("[value-42]"));
    run.pty().write("cd\r");
    REQUIRE(run.waitFor("папка ünï"));  // ConPTY's output is UTF-8

    run.pty().resize(117, 31);
    run.pty().write("mode con\r");
    REQUIRE(run.waitFor("117"));

    run.pty().write("exit 5\r");
    REQUIRE(run.finish().code == 5);
}

TEST_CASE("pty (Windows): kill ends the program and what it started", "[platform][pty]") {
    Scratch tmp;
    PtyRun run;
    REQUIRE(run.start(cmd("/c \"echo started& ping -n 60 127.0.0.1 >nul\"", sys::fromPath(tmp.path()))));
    REQUIRE(run.waitFor("started"));
    const auto t0 = std::chrono::steady_clock::now();
    run.pty().kill();
    run.finish();
    REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    REQUIRE(run.outputEnded());
}

TEST_CASE("pty (Windows): a missing program or start folder is a readable error", "[platform][pty]") {
    Scratch tmp;
    {
        PtyRun run;
        sys::pty::Options o;
        o.command = "\"" + sys::fromPath(tmp.path() / "no-such-program.exe") + "\"";
        o.startDir = sys::fromPath(tmp.path());
        REQUIRE_FALSE(run.start(o));
        REQUIRE(run.error().find("Cannot start") != std::string::npos);
    }
    {
        PtyRun run;
        REQUIRE_FALSE(run.start(cmd("/c exit", sys::fromPath(tmp.path() / "missing"))));
        REQUIRE(run.error().find("does not exist") != std::string::npos);
    }
}

TEST_CASE("pty (Windows): the shell list has Command Prompt and one default", "[platform][pty]") {
    const auto shells = sys::pty::listShells();
    REQUIRE(std::any_of(shells.begin(), shells.end(), [](const auto& s) {
        return s.id == "cmd" && s.command.find("cmd.exe") != std::string::npos;
    }));
    REQUIRE(std::count_if(shells.begin(), shells.end(), [](const auto& s) { return s.isDefault; }) == 1);
}
