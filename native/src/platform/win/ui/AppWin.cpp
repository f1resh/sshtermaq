// Process and UI-thread services on Windows: SEH logging, OLE, the message loop, a named mutex per data folder and
// WM_COPYDATA forwarding to the running instance, MessageBox.
#include "platform/ui/App.h"

#include "core/Log.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"
#include "platform/win/ui/HostWindowWin.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <iterator>

namespace st::ui {

namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Unhandled SEH exception 0x%08lX at %p",
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0UL,
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    applog::error(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void installCrashLogging() { SetUnhandledExceptionFilter(crashFilter); }

UiThread::UiThread() {
    // OLE (not just COM): drag & drop needs it on the UI thread.
    initialized_ = SUCCEEDED(OleInitialize(nullptr));
}

UiThread::~UiThread() {
    if (initialized_) OleUninitialize();
}

int runEventLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

namespace {
std::string g_instanceKey;  // this process's data folder key, for forwardToPrimary()
}  // namespace

InstanceLock::InstanceLock(const std::string& key) {
    g_instanceKey = key;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, widen("Local\\SSHTermaq-" + key).c_str());
    primary_ = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    handle_ = mutex;
}

InstanceLock::~InstanceLock() {
    if (handle_) {
        if (primary_) ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

bool InstanceLock::forwardToPrimary() {
    // The window of the instance that holds this data folder's lock (it may still be starting).
    HWND target = nullptr;
    for (int i = 0; i < 40 && !target; ++i) {
        target = findInstanceWindow(g_instanceKey);
        if (!target) Sleep(50);
    }
    // None: the lock may be held by 1.1.0 or older (still running after an update), whose window has no mark and
    // takes only the old message.
    bool old = false;
    if (!target) {
        target = findUnmarkedWindow();
        old = target != nullptr;
        if (old) applog::info("Second start: handing the command line to an older version's window");
    }
    if (!target) return false;
    wchar_t cwd[MAX_PATH * 4] = {};
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
    std::wstring payload = old ? std::wstring(cwd) + L'\0' + GetCommandLineW()
                               : commandLinePayload(g_instanceKey, cwd, GetCommandLineW());
    COPYDATASTRUCT cds{};
    cds.dwData = old ? kCopyDataCommandLineOld : kCopyDataCommandLine;
    cds.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    cds.lpData = payload.data();
    DWORD_PTR result = 0;
    // Let the running instance take the foreground.
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    AllowSetForegroundWindow(pid);
    return SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000,
                               &result) != 0;
}

void showMessage(const std::string& text, MessageKind kind) {
    MessageBoxW(nullptr, widen(text).c_str(), L"SSHTermaq", kind == MessageKind::Error ? MB_ICONERROR : MB_OK | MB_ICONINFORMATION);
}

std::vector<std::string> splitCommandLine(const std::wstring& commandLine) {
    std::vector<std::string> out;
    if (commandLine.empty()) return out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) out.push_back(narrow(argv[i]));
    LocalFree(argv);
    return out;
}

namespace {

constexpr wchar_t kMarkedProp[] = L"SSHTermaq.Instance";  // on every marked window, whatever its key

std::wstring instanceProp(const std::string& instanceKey) { return std::wstring(kMarkedProp) + L"." + widen(instanceKey); }

bool isMainWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    return GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) > 0 && std::wcscmp(cls, kWindowClass) == 0;
}

/** The first top-level main window for which `match` is true. */
template <class Match>
HWND findMainWindow(Match match) {
    struct Search {
        Match* match;
        HWND found = nullptr;
    } search{&match};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            if (!isMainWindow(hwnd) || !(*s->match)(hwnd)) return TRUE;
            s->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

}  // namespace

void markInstanceWindow(HWND hwnd, const std::string& instanceKey) {
    SetPropW(hwnd, kMarkedProp, reinterpret_cast<HANDLE>(1));
    SetPropW(hwnd, instanceProp(instanceKey).c_str(), reinterpret_cast<HANDLE>(1));
}

void unmarkInstanceWindow(HWND hwnd, const std::string& instanceKey) {
    RemovePropW(hwnd, instanceProp(instanceKey).c_str());
    RemovePropW(hwnd, kMarkedProp);
}

HWND findInstanceWindow(const std::string& instanceKey) {
    const std::wstring prop = instanceProp(instanceKey);
    return findMainWindow([&prop](HWND hwnd) { return GetPropW(hwnd, prop.c_str()) != nullptr; });
}

HWND findUnmarkedWindow() {
    return findMainWindow([](HWND hwnd) { return GetPropW(hwnd, kMarkedProp) == nullptr; });
}

std::wstring commandLinePayload(const std::string& instanceKey, const std::wstring& cwd, const std::wstring& commandLine) {
    return widen(instanceKey) + L'\0' + cwd + L'\0' + commandLine;
}

std::optional<ForwardedCommandLine> readCommandLinePayload(std::wstring payload, const std::string& instanceKey) {
    while (!payload.empty() && payload.back() == L'\0') payload.pop_back();
    const size_t a = payload.find(L'\0');
    if (a == std::wstring::npos || instanceKey.empty() || payload.compare(0, a, widen(instanceKey)) != 0) return std::nullopt;
    const size_t b = payload.find(L'\0', a + 1);
    if (b == std::wstring::npos) return std::nullopt;
    return ForwardedCommandLine{payload.substr(a + 1, b - a - 1), payload.substr(b + 1)};
}

}  // namespace st::ui
