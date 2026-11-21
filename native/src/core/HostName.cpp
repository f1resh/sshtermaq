#include "core/HostName.h"

#include <algorithm>
#include <vector>

namespace st {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::vector<std::string_view> splitView(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool isLabel(std::string_view l) {
    if (l.empty() || l.size() > 63) return false;
    if (!(isAlnum(l[0]) || l[0] == '_')) return false;
    return std::all_of(l.begin(), l.end(), [](char c) { return isAlnum(c) || c == '_' || c == '-'; });
}

}  // namespace

bool isIPv4(std::string_view s) {
    const auto parts = splitView(s, '.');
    if (parts.size() != 4) return false;
    for (std::string_view p : parts) {
        if (p.empty() || p.size() > 3 || !std::all_of(p.begin(), p.end(), isDigit)) return false;
        if (p.size() > 1 && p[0] == '0') return false;
        int v = 0;
        for (char c : p) v = v * 10 + (c - '0');
        if (v > 255) return false;
    }
    return true;
}

bool isIPv6(std::string_view s) {
    if (!s.empty() && s.front() == '[') s.remove_prefix(1);
    if (!s.empty() && s.back() == ']') s.remove_suffix(1);
    if (const size_t pct = s.rfind('%'); pct != std::string_view::npos) {  // zone id: letters, digits, '_', '.', '-'
        const std::string_view zone = s.substr(pct + 1);
        if (zone.empty() || !std::all_of(zone.begin(), zone.end(),
                                         [](char c) { return isAlnum(c) || c == '_' || c == '.' || c == '-'; }))
            return false;
        s = s.substr(0, pct);
    }
    if (s.find(':') == std::string_view::npos) return false;
    int compressions = 0;  // non-overlapping "::", as the UI's s.match(/::/g) counts them
    for (size_t at = s.find("::"); at != std::string_view::npos; at = s.find("::", at + 2)) ++compressions;
    if (compressions > 1) return false;
    const auto parts = splitView(s, ':');
    if (parts.size() > 8) return false;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string_view p = parts[i];
        if (p.empty()) continue;
        if (p.size() <= 4 && std::all_of(p.begin(), p.end(), isHex)) continue;
        if (i + 1 == parts.size() && isIPv4(p)) continue;
        return false;
    }
    return true;
}

bool isValidHostName(std::string_view s) {
    // The UI trims first and refuses whitespace anywhere; callers here pass trimmed text, so any space fails below.
    if (s.empty()) return false;
    if (isIPv4(s) || isIPv6(s)) return true;
    if (s.size() > 253) return false;
    if (s.back() == '.') s.remove_suffix(1);
    const auto labels = splitView(s, '.');
    return std::all_of(labels.begin(), labels.end(), isLabel);
}

}  // namespace st
