// A second start hands its command line to the window of its own data folder (review 7): main windows carry their
// data folder's instance key, the message carries it too, and a message for another key is refused.
#include "platform/win/ui/HostWindowWin.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace st;

namespace {

/** A bare top-level window of the main window's class (not shown), standing in for one SSHTermaq instance. */
struct FakeMainWindow {
    HWND hwnd = nullptr;
    FakeMainWindow() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = ui::kWindowClass;
        RegisterClassExW(&wc);  // fails harmlessly when it exists
        hwnd = CreateWindowExW(0, ui::kWindowClass, L"SSHTermaq test", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    }
    ~FakeMainWindow() {
        if (hwnd) DestroyWindow(hwnd);
    }
};

}  // namespace

TEST_CASE("instance: a second start finds the window of its own data folder only", "[app][instance]") {
    // Keys unique to this run, so a real SSHTermaq running on this machine is never matched.
    const std::string keyA = "testA" + std::to_string(GetCurrentProcessId());
    const std::string keyB = "testB" + std::to_string(GetCurrentProcessId());
    FakeMainWindow a, b;
    REQUIRE(a.hwnd);
    REQUIRE(b.hwnd);
    CHECK(ui::findInstanceWindow(keyA) == nullptr);

    ui::markInstanceWindow(a.hwnd, keyA);
    ui::markInstanceWindow(b.hwnd, keyB);
    CHECK(ui::findInstanceWindow(keyA) == a.hwnd);
    CHECK(ui::findInstanceWindow(keyB) == b.hwnd);
    CHECK(ui::findInstanceWindow("testC" + std::to_string(GetCurrentProcessId())) == nullptr);

    // Before the fix any window of the class was taken: an unmarked one (1.1.0 and older) is only the fallback.
    FakeMainWindow old;
    REQUIRE(old.hwnd);
    CHECK(ui::findInstanceWindow(keyA) == a.hwnd);
    const HWND unmarked = ui::findUnmarkedWindow();
    CHECK(unmarked != a.hwnd);
    CHECK(unmarked != b.hwnd);
    CHECK(unmarked != nullptr);

    // A window that goes is unmarked first.
    ui::unmarkInstanceWindow(a.hwnd, keyA);
    CHECK(ui::findInstanceWindow(keyA) == nullptr);
    CHECK(ui::findInstanceWindow(keyB) == b.hwnd);
    ui::unmarkInstanceWindow(b.hwnd, keyB);
}

TEST_CASE("instance: the forwarded command line carries its data folder's key", "[app][instance]") {
    const std::wstring cmd = L"SSHTermaq.exe -pw \"s3cr3t\" user@host";
    const std::wstring payload = ui::commandLinePayload("0123456789abcdef", L"C:\\Users\\me", cmd);

    auto mine = ui::readCommandLinePayload(payload, "0123456789abcdef");
    REQUIRE(mine);
    CHECK(mine->cwd == L"C:\\Users\\me");
    CHECK(mine->commandLine == cmd);
    // With the trailing NUL WM_COPYDATA carries.
    auto withNul = ui::readCommandLinePayload(payload + L'\0', "0123456789abcdef");
    REQUIRE(withNul);
    CHECK(withNul->commandLine == cmd);

    // Another data folder's instance refuses it, and so does a payload without a key (the old format, cwd first).
    CHECK_FALSE(ui::readCommandLinePayload(payload, "fedcba9876543210"));
    CHECK_FALSE(ui::readCommandLinePayload(std::wstring(L"C:\\Users\\me") + L'\0' + cmd, "0123456789abcdef"));
    CHECK_FALSE(ui::readCommandLinePayload(L"", "0123456789abcdef"));
    CHECK_FALSE(ui::readCommandLinePayload(payload, ""));
    // A key that is only the start of another key does not match.
    CHECK_FALSE(ui::readCommandLinePayload(ui::commandLinePayload("0123456789abcdef00", L"C:\\", cmd), "0123456789abcdef"));
    // An empty working folder is fine.
    auto noCwd = ui::readCommandLinePayload(ui::commandLinePayload("k", L"", cmd), "k");
    REQUIRE(noCwd);
    CHECK(noCwd->cwd.empty());
    CHECK(ui::splitCommandLine(noCwd->commandLine) == std::vector<std::string>{"-pw", "s3cr3t", "user@host"});
}
