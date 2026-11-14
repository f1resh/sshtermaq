// The local terminal transport with this computer's default shell (Windows: PowerShell; macOS: the login shell).
#include "TestTransport.h"
#include "core/Types.h"
#include "transport/LocalTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace st;
using st::test::RecordingListener;
using namespace std::chrono_literals;

namespace {

std::unique_ptr<Transport> startLocal(RecordingListener& l, const std::string& startDir = {}) {
    TransportContext ctx;
    ctx.connId = "local-test";
    ctx.session = makeDefaultSession();
    ctx.session.protocol = Protocol::Local;
    ctx.session.local.startDir = startDir;
    ctx.listener = &l;
    auto t = makeLocalTransport(std::move(ctx));
    t->start();
    return t;
}

}  // namespace

TEST_CASE("local: program names for labels", "[local]") {
    REQUIRE(localProgramName("\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo") == "pwsh");
    REQUIRE(localProgramName("C:\\Windows\\System32\\cmd.exe") == "cmd");
    REQUIRE(localProgramName("/bin/zsh -l") == "zsh");
    REQUIRE(localProgramName("'/opt/my shells/fish' -l") == "fish");
    REQUIRE(localProgramName("") == "Local terminal");
}

TEST_CASE("local: there is a default shell", "[local]") {
    const auto shells = listLocalShells();
    REQUIRE_FALSE(shells.empty());
    REQUIRE(std::count_if(shells.begin(), shells.end(), [](const LocalShellInfo& s) { return s.isDefault; }) == 1);
}

TEST_CASE("local: the default shell runs, takes input and reports its exit code", "[local]") {
    RecordingListener l;
    auto t = startLocal(l);
    REQUIRE(l.waitConnected(20s));
    t->resize(100, 30, 0, 0);
    t->write("exit 3\r");
    REQUIRE(l.waitClosed(30s));
    std::lock_guard lock(l.m);
    REQUIRE(l.closeCount == 1);
    REQUIRE(l.info.hasExitCode);
    REQUIRE(l.info.exitCode == 3);
    REQUIRE_FALSE(l.info.clean);  // a non-zero exit code is not a clean end
    REQUIRE_FALSE(l.info.error);
}

TEST_CASE("local: closing ends the shell, cleanly and once", "[local]") {
    RecordingListener l;
    auto t = startLocal(l);
    REQUIRE(l.waitConnected(20s));
    t->close();
    REQUIRE(l.waitClosed(15s));
    std::lock_guard lock(l.m);
    REQUIRE(l.closeCount == 1);
    REQUIRE(l.info.clean);
    REQUIRE(l.info.reason == "Closed");
}

TEST_CASE("local: a missing start folder fails with a readable reason", "[local]") {
    RecordingListener l;
    auto t = startLocal(l, "/no/such/folder/st-local-test");
    REQUIRE(l.waitClosed(10s));
    std::lock_guard lock(l.m);
    REQUIRE(l.closeCount == 1);
    REQUIRE(l.info.error);
    REQUIRE(l.info.reason.find("does not exist") != std::string::npos);
}
