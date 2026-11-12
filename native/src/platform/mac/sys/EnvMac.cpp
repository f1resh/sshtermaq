#include "platform/sys/Env.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

#include <climits>
#include <cstdlib>
#include <vector>

namespace fs = std::filesystem;

namespace st::sys {

fs::path executablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    char resolved[PATH_MAX];
    if (realpath(buf.data(), resolved)) return resolved;
    return buf.data();
}

std::int64_t executableBuildTime() {
    const fs::path exe = executablePath();
    struct stat st {};
    if (exe.empty() || stat(exe.c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_mtime);
}

fs::path userDataRoot() {
    fs::path home = homeDirectory();
    if (home.empty()) return {};
    return home / "Library" / "Application Support";
}

std::string terminalLocale() {
    // The language and region chosen in System Settings, as Terminal.app sets LANG: "en_GB", "ru_RU", ...
    std::string id;
    if (CFLocaleRef loc = CFLocaleCopyCurrent()) {
        if (CFStringRef s = CFLocaleGetIdentifier(loc)) {
            char buf[128];
            if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) id = buf;
        }
        CFRelease(loc);
    }
    id = id.substr(0, id.find('@'));  // "en_GB@rg=..." keeps only the language and region
    if (id.size() < 5 || id[2] != '_') id = "en_US";
    return id + ".UTF-8";
}

}  // namespace st::sys
