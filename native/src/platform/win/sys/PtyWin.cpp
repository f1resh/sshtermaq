// Pseudo-terminals on Windows (platform/sys/Pty.h): ConPTY. CreatePseudoConsole and friends are looked up at run time
// (Windows 10 1809+), so the program still starts on older Windows, where local terminals are reported unsupported.
// The pseudo console writes UTF-8 with VT sequences to our output pipe and reads UTF-8 / VT input from our input pipe.
// Its output pipe only ends once the console is closed: after the program exits, finishOutput() closes it (on older
// Windows 10, ClosePseudoConsole waits until the output has been read, so read() must keep running meanwhile).
// A job object (KILL_ON_JOB_CLOSE) holds the program and everything it starts.
#include "platform/sys/Pty.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <set>
#include <vector>

namespace st::sys::pty {

namespace {

// Declared here rather than taken from the SDK headers: they only declare ConPTY for NTDDI_WIN10_RS5 and later.
using PseudoConsole = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(PseudoConsole, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(PseudoConsole);
constexpr DWORD_PTR kAttributePseudoConsole = 0x00020016;  // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    bool ok() const { return create && resize && close; }
};

const ConPtyApi& api() {
    static const ConPtyApi a = [] {
        ConPtyApi r;
        if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
            r.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(k, "CreatePseudoConsole"));
            r.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(k, "ResizePseudoConsole"));
            r.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(k, "ClosePseudoConsole"));
        }
        return r;
    }();
    return a;
}

COORD size(int cols, int rows) {
    return COORD{static_cast<SHORT>(std::clamp(cols, 1, 32767)), static_cast<SHORT>(std::clamp(rows, 1, 32767))};
}

void closeHandle(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
}

bool sameName(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towupper(x) == std::towupper(y); });
}

/** This process' environment with `overrides` applied (names compared case-insensitively, as Windows does), as a
 *  CreateProcess environment block. */
std::vector<wchar_t> environmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::pair<std::wstring, std::wstring>> wide;
    for (const auto& [k, v] : overrides) wide.emplace_back(widen(k), widen(v));
    std::vector<wchar_t> block;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring_view entry(p);
            // "=C:=C:\dir" entries (the current folder per drive) start with '='; keep them as they are.
            const size_t eq = entry.find(L'=', 1);
            const std::wstring_view name = entry.substr(0, eq == std::wstring_view::npos ? entry.size() : eq);
            const bool replaced =
                std::any_of(wide.begin(), wide.end(), [&](const auto& kv) { return sameName(kv.first, name); });
            if (!replaced) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [k, v] : wide) {
        if (k.empty() || v.empty()) continue;  // empty value: removed
        block.insert(block.end(), k.begin(), k.end());
        block.push_back(L'=');
        block.insert(block.end(), v.begin(), v.end());
        block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

}  // namespace

bool supported(std::string& reason) {
    if (api().ok()) return true;
    reason = "Local terminals need Windows 10 version 1809 or later (the Windows pseudo console)";
    return false;
}

struct Pty::Impl {
    PseudoConsole console = nullptr;
    HANDLE inputWrite = nullptr;  // we write the program's input here
    HANDLE outputRead = nullptr;  // and read its output here
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    std::mutex consoleMutex;  // console: resize vs close
    bool consoleClosed = false;

    void closeConsole() {
        PseudoConsole c = nullptr;
        {
            std::lock_guard lock(consoleMutex);
            if (consoleClosed) return;
            consoleClosed = true;
            c = console;
            console = nullptr;
        }
        if (c) api().close(c);
    }

    ~Impl() {
        if (job) TerminateJobObject(job, 1);
        else if (process) TerminateProcess(process, 1);
        // Our end of the output first: the console then cannot block on output nobody reads while it closes.
        closeHandle(outputRead);
        closeHandle(inputWrite);
        closeConsole();
        closeHandle(process);
        closeHandle(job);
    }
};

Pty::Pty() : impl_(std::make_unique<Impl>()) {}
Pty::~Pty() = default;

bool Pty::start(const Options& o, std::string& error) {
    std::string reason;
    if (!supported(reason)) {
        error = reason;
        return false;
    }
    if (o.command.empty()) {
        error = "No program to start";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path dir(widen(o.startDir));
    if (o.startDir.empty() || !std::filesystem::is_directory(dir, ec)) {
        error = "The start folder \"" + o.startDir + "\" does not exist";
        return false;
    }

    // The console reads inputRead and writes outputWrite; we keep the other ends. Not inheritable: the program gets the
    // console through the attribute below, not through handles.
    HANDLE inputRead = nullptr, outputWrite = nullptr;
    if (!CreatePipe(&inputRead, &impl_->inputWrite, nullptr, 0) || !CreatePipe(&impl_->outputRead, &outputWrite, nullptr, 0)) {
        error = "Cannot create the terminal's pipes: " + lastErrorMessage();
        closeHandle(inputRead);
        return false;
    }
    const HRESULT hr = api().create(size(o.cols, o.rows), inputRead, outputWrite, 0, &impl_->console);
    closeHandle(inputRead);  // the console has its own copies now
    closeHandle(outputWrite);
    if (FAILED(hr)) {
        error = "Cannot create the pseudo console: " + errorMessage(static_cast<unsigned long>(hr));
        impl_->console = nullptr;
        return false;
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) {
        error = "Cannot start \"" + o.command + "\": " + lastErrorMessage();
        return false;
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attrGuard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, kAttributePseudoConsole, impl_->console, sizeof(impl_->console), nullptr, nullptr)) {
        error = "Cannot start \"" + o.command + "\": " + lastErrorMessage();
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // No standard handles of ours: otherwise a program could write to them instead of the pseudo console.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;
    std::wstring cmdline = widen(o.command);
    std::vector<wchar_t> env = environmentBlock(o.environment);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data(),
                        dir.c_str(), &si.StartupInfo, &pi)) {
        error = "Cannot start \"" + o.command + "\": " + lastErrorMessage();
        return false;
    }
    impl_->process = pi.hProcess;
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    if (impl_->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &li, sizeof(li)) ||
            !AssignProcessToJobObject(impl_->job, pi.hProcess))
            closeHandle(impl_->job);  // without the job, kill() ends the program alone
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return true;
}

std::size_t Pty::read(char* buf, std::size_t size) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
    if (!impl_->outputRead || !ReadFile(impl_->outputRead, buf, want, &got, nullptr)) return 0;
    return got;
}

bool Pty::write(std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        const DWORD n = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1u << 20));
        if (!impl_->inputWrite || !WriteFile(impl_->inputWrite, bytes.data(), n, &written, nullptr)) return false;
        bytes.remove_prefix(written);
    }
    return true;
}

void Pty::resize(int cols, int rows) {
    std::lock_guard lock(impl_->consoleMutex);
    if (impl_->console && !impl_->consoleClosed) api().resize(impl_->console, size(cols, rows));
}

ExitStatus Pty::wait() {
    ExitStatus st;
    if (!impl_->process) return st;
    WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->process, &code)) st.code = static_cast<int>(code);
    return st;
}

void Pty::finishOutput() { impl_->closeConsole(); }

void Pty::kill() {
    if (impl_->job) TerminateJobObject(impl_->job, 1);
    else if (impl_->process) TerminateProcess(impl_->process, 1);
}

// --------------------------------------------------------------------------------------------------- shells ---

namespace {

std::wstring envW(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

bool isFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || !bytes)
        return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS)
        return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

std::wstring joinPath(std::wstring dir, std::wstring_view rest) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir + std::wstring(rest);
}

/** `"path" args`: the program quoted, so paths with spaces work. */
std::string commandLine(const std::wstring& program, std::string_view args = {}) {
    std::string s = "\"" + narrow(program) + "\"";
    if (!args.empty()) s += " " + std::string(args);
    return s;
}

std::wstring searchPath(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring systemDir() {
    wchar_t buf[MAX_PATH];
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring(L"C:\\Windows\\System32");
}

}  // namespace

std::vector<Shell> listShells() {
    std::vector<Shell> out;
    const std::wstring sys32 = systemDir();
    std::wstring programFiles = envW(L"ProgramW6432");
    if (programFiles.empty()) programFiles = envW(L"ProgramFiles");
    if (programFiles.empty()) programFiles = L"C:\\Program Files";

    // PowerShell 7: its default folder, else whatever pwsh.exe PATH finds (also the Store version's alias).
    std::wstring pwsh = joinPath(programFiles, L"PowerShell\\7\\pwsh.exe");
    if (!isFile(pwsh)) pwsh = searchPath(L"pwsh.exe");
    const bool hasPwsh = !pwsh.empty();

    std::wstring cmd = envW(L"ComSpec");
    if (cmd.empty() || !isFile(cmd)) cmd = joinPath(sys32, L"cmd.exe");
    out.push_back({"cmd", "Command Prompt", commandLine(cmd), {}, false});

    const std::wstring powershell = joinPath(sys32, L"WindowsPowerShell\\v1.0\\powershell.exe");
    if (isFile(powershell)) out.push_back({"powershell", "Windows PowerShell", commandLine(powershell, "-NoLogo"), {}, !hasPwsh});
    if (hasPwsh) out.push_back({"pwsh", "PowerShell 7", commandLine(pwsh, "-NoLogo"), {}, true});

    // Git for Windows: its registry entry (machine or user install), else its default folder.
    std::wstring git = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GitForWindows", L"InstallPath");
    if (git.empty()) git = regString(HKEY_CURRENT_USER, L"SOFTWARE\\GitForWindows", L"InstallPath");
    if (git.empty()) git = joinPath(programFiles, L"Git");
    const std::wstring gitBash = joinPath(git, L"bin\\bash.exe");
    if (isFile(gitBash)) out.push_back({"git-bash", "Git Bash", commandLine(gitBash, "--login -i"), {}, false});

    // Cygwin and MSYS2: a login shell that stays in the start folder (CHERE_INVOKING) instead of going home.
    std::wstring cygwin = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Cygwin\\setup", L"rootdir");
    if (cygwin.empty()) cygwin = L"C:\\cygwin64";
    const std::wstring cygBash = joinPath(cygwin, L"bin\\bash.exe");
    if (isFile(cygBash)) out.push_back({"cygwin", "Cygwin", commandLine(cygBash, "--login -i"), {{"CHERE_INVOKING", "1"}}, false});
    const std::wstring msysBash = L"C:\\msys64\\usr\\bin\\bash.exe";
    if (isFile(msysBash))
        out.push_back({"msys2", "MSYS2 (UCRT64)", commandLine(msysBash, "--login -i"),
                       {{"MSYSTEM", "UCRT64"}, {"CHERE_INVOKING", "1"}}, false});

    // WSL distributions, from the registry (no need to start wsl.exe); Docker Desktop's own ones are not shells.
    const std::wstring wsl = joinPath(sys32, L"wsl.exe");
    if (isFile(wsl)) {
        HKEY lxss = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &lxss) == ERROR_SUCCESS) {
            std::set<std::string> seen;
            wchar_t sub[256];
            for (DWORD i = 0;; ++i) {
                DWORD len = static_cast<DWORD>(std::size(sub));
                if (RegEnumKeyExW(lxss, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                const std::string name = narrow(regString(lxss, sub, L"DistributionName"));
                if (name.empty() || name.rfind("docker-desktop", 0) == 0 || !seen.insert(name).second) continue;
                // Unquoted: wsl.exe parses its own command line and keeps quotes as part of the name
                // (WSL_E_DISTRO_NOT_FOUND). Distribution names have no spaces (letters, digits, '.', '-', '_').
                out.push_back({"wsl:" + name, name + " (WSL)", commandLine(wsl, "-d " + name + " --cd ~"), {}, false});
            }
            RegCloseKey(lxss);
        }
    }
    return out;
}

}  // namespace st::sys::pty
