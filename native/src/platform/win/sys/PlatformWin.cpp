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

}  // namespace st::sys
