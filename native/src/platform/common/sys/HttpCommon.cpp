// The OS-independent parts of platform/sys/Http.h.
#include "platform/sys/Http.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace st::sys {

namespace {

bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::string HttpResponse::header(std::string_view name) const {
    for (const auto& [n, v] : headers)
        if (sameText(n, name)) return v;
    return {};
}

bool splitUrl(std::string_view url, UrlParts& out) {
    out = UrlParts{};
    const size_t sep = url.find("://");
    if (sep == std::string_view::npos) return false;
    std::string scheme(url.substr(0, sep));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "http" && scheme != "https") return false;
    out.scheme = scheme;
    std::string_view rest = url.substr(sep + 3);
    const size_t slash = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, slash);
    out.pathAndQuery = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    if (!out.pathAndQuery.empty() && out.pathAndQuery[0] == '?') out.pathAndQuery = "/" + out.pathAndQuery;
    if (authority.find('@') != std::string_view::npos || authority.empty()) return false;
    std::string_view port;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        out.host = std::string(authority.substr(1, close - 1));
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return false;
            port = after.substr(1);
        }
    } else {
        const size_t colon = authority.find(':');
        out.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) port = authority.substr(colon + 1);
    }
    if (out.host.empty()) return false;
    if (!port.empty()) {
        auto [p, ec] = std::from_chars(port.data(), port.data() + port.size(), out.port);
        if (ec != std::errc() || p != port.data() + port.size() || out.port < 1 || out.port > 65535) return false;
    }
    return true;
}

}  // namespace st::sys
