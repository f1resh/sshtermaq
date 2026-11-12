// platform/sys/Pty.h on POSIX: a pty with /bin/sh (always there), signals, the process group, the shell list.
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

sys::pty::Options sh(const std::string& args, const std::string& dir) {
    sys::pty::Options o;
    o.command = "/bin/sh " + args;
    o.startDir = dir;
    return o;
}

}  // namespace

TEST_CASE("pty (POSIX): runs a program, passes its output and exit code", "[platform][pty]") {
    Scratch tmp;
    PtyRun run;
    REQUIRE(run.start(sh("-c 'echo hello-pty; exit 3'", sys::fromPath(tmp.path()))));
    REQUIRE(run.waitFor("hello-pty"));
    const auto st = run.finish();
    REQUIRE(st.code == 3);
    REQUIRE(st.signal.empty());
    REQUIRE(run.outputEnded());
}

TEST_CASE("pty (POSIX): the command line is split as a shell would", "[platform][pty]") {
    Scratch tmp;
    PtyRun run;
    REQUIRE(run.start(sh(R"(-c "echo \"[a  b]\" '[c  d]'")", sys::fromPath(tmp.path()))));
    REQUIRE(run.waitFor("[a  b] [c  d]"));
    REQUIRE(run.finish().code == 0);
}

TEST_CASE("pty (POSIX): input, resize, environment and a non-ASCII start folder", "[platform][pty]") {
    Scratch tmp;
    const auto dir = tmp.path() / sys::toPath("папка ünï");
    std::filesystem::create_directories(dir);
    sys::pty::Options o = sh("", sys::fromPath(dir));
    o.environment = {{"ST_PTY_TEST", "value-42"}, {"PS1", "$ "}};
    PtyRun run;
    REQUIRE(run.start(o));

    run.pty().write("echo \"[$ST_PTY_TEST]\"\n");
    REQUIRE(run.waitFor("[value-42]"));
    run.pty().write("pwd\n");
    REQUIRE(run.waitFor("папка ünï"));

    run.pty().resize(117, 31);
    run.pty().write("stty size\n");
    REQUIRE(run.waitFor("31 117"));

    run.pty().write("exit 5\n");
    REQUIRE(run.finish().code == 5);
}

TEST_CASE("pty (POSIX): the program gets default signal handling, not ours", "[platform][pty]") {
    // We ignore SIGPIPE; a shell inheriting that would never die of it.
    Scratch tmp;
    PtyRun run;
    REQUIRE(run.start(sh("-c 'kill -PIPE $$; echo survived'", sys::fromPath(tmp.path()))));
    const auto st = run.finish();
    REQUIRE(st.signal == "PIPE");
    REQUIRE(run.output().find("survived") == std::string::npos);
}

TEST_CASE("pty (POSIX): kill hangs the program up", "[platform][pty]") {
    Scratch tmp;
    PtyRun run;
    REQUIRE(run.start(sh("-c 'echo started; sleep 60'", sys::fromPath(tmp.path()))));
    REQUIRE(run.waitFor("started"));
    const auto t0 = std::chrono::steady_clock::now();
    run.pty().kill();
    const auto st = run.finish();
    REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    REQUIRE((st.signal == "HUP" || st.signal == "KILL"));
    REQUIRE(run.outputEnded());
}

TEST_CASE("pty (POSIX): a missing program or start folder is a readable error", "[platform][pty]") {
    Scratch tmp;
    {
        PtyRun run;
        sys::pty::Options o;
        o.command = "no-such-program-st --version";
        o.startDir = sys::fromPath(tmp.path());
        REQUIRE_FALSE(run.start(o));
        REQUIRE(run.error().find("was not found") != std::string::npos);
    }
    {
        PtyRun run;
        REQUIRE_FALSE(run.start(sh("-c true", sys::fromPath(tmp.path() / "missing"))));
        REQUIRE(run.error().find("does not exist") != std::string::npos);
    }
}

TEST_CASE("pty (POSIX): the shell list starts with the login shell, the default", "[platform][pty]") {
    const auto shells = sys::pty::listShells();
    REQUIRE_FALSE(shells.empty());
    REQUIRE(shells.front().isDefault);
    REQUIRE(std::count_if(shells.begin(), shells.end(), [](const auto& s) { return s.isDefault; }) == 1);
    REQUIRE(shells.front().command.find(" -l") != std::string::npos);
}
