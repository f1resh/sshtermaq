#include "platform/Platform.h"

#include <sys/sysctl.h>

namespace st::sys {

const char* osName() { return "darwin"; }

const char* archName() {
#if defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

std::string osVersion() {
    char version[64] = {};
    size_t size = sizeof version;
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0) return "macOS (unknown version)";
    return std::string("macOS ") + version;
}

}  // namespace st::sys
