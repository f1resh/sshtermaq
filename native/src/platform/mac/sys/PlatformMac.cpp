#include "platform/Platform.h"

#include <sys/stat.h>
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

const char* installChannel() {
    // A cask keeps its folder in the Caskroom of the prefix Homebrew uses (Apple silicon, Intel).
    struct stat st {};
    for (const char* dir : {"/opt/homebrew/Caskroom/sshtermaq", "/usr/local/Caskroom/sshtermaq"})
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return "homebrew";
    return "other";
}

}  // namespace st::sys
