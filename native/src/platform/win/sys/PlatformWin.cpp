#include "platform/Platform.h"

#include "platform/detail/WinApi.h"

namespace st::sys {

const char* osName() { return "win32"; }

const char* archName() {
#if defined(_M_ARM64)
    return "arm64";
#else
    return "x64";
#endif
}

std::string osVersion() {
    // RtlGetVersion tells the real version; GetVersionEx is capped by the manifest's compatibility list.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    if (!fn || fn(&v) != 0) return "Windows (unknown version)";
    return "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
           std::to_string(v.dwBuildNumber);
}

const char* installChannel() {
    // GetCurrentPackageFullName (Windows 8+) fails with APPMODEL_ERROR_NO_PACKAGE outside an MSIX package. Looked up
    // at run time, as osVersion() does, so no SDK header for app packages is needed.
    using Fn = LONG(WINAPI*)(UINT32*, PWSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto fn = kernel ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetCurrentPackageFullName"))) : nullptr;
    if (!fn) return "other";
    UINT32 length = 0;
    const LONG rc = fn(&length, nullptr);
    return rc == ERROR_INSUFFICIENT_BUFFER ? "store" : "other";
}

}  // namespace st::sys
