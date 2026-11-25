#include "app/UpdateCheck.h"

#include "core/Json.h"
#include "core/Log.h"
#include "platform/sys/Http.h"

#include <cctype>

namespace st::update {

std::optional<Version> parseVersion(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    int parts[3] = {0, 0, 0};
    size_t i = 0;
    for (int n = 0; n < 3; ++n) {
        if (n > 0) {
            if (i >= text.size() || text[i] != '.') return std::nullopt;
            ++i;
        }
        const size_t start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
        if (i == start || i - start > 5) return std::nullopt;
        parts[n] = std::stoi(std::string(text.substr(start, i - start)));
    }
    if (i != text.size()) return std::nullopt;  // "1.2.0-beta", "1.2.0.4", "1.2.0 "
    return Version{parts[0], parts[1], parts[2]};
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::optional<Release> parseLatestRelease(std::string_view body, std::string* why) {
    auto fail = [why](const char* reason) -> std::optional<Release> {
        if (why) *why = reason;
        return std::nullopt;
    };
    const Json j = Json::parse(body.begin(), body.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("the answer is not JSON");
    if (j.value("draft", false) || j.value("prerelease", false)) return fail("a draft or pre-release");
    const auto tag = j.find("tag_name");
    if (tag == j.end() || !tag->is_string()) return fail("no tag_name");
    const std::string tagName = tag->get<std::string>();
    const auto v = parseVersion(tagName);
    if (!v) return fail("the tag is not a version (N.N.N)");
    Release r;
    r.version = std::to_string(v->major) + "." + std::to_string(v->minor) + "." + std::to_string(v->patch);
    r.url = std::string(kReleasePagePrefix) + tagName;  // tagName is [vV]?digits.digits.digits: safe in a URL
    return r;
}

CheckResult checkLatestRelease(const std::string& currentVersion, const std::string& url, int timeoutMs) {
    CheckResult out;
    auto failed = [&](std::string e) {
        out.error = std::move(e);
        applog::warn("Version check failed: " + out.error);
        return out;
    };
    const auto current = parseVersion(currentVersion);
    if (!current) return failed("the running version \"" + currentVersion + "\" is not N.N.N");

    sys::HttpSessionOptions o;  // User-Agent "SSHTermaq", the system's proxy settings
    o.connectTimeoutMs = timeoutMs;
    o.ioTimeoutMs = timeoutMs;
    std::shared_ptr<sys::HttpSession> http;
    try {
        http = sys::openHttpSession(o);
    } catch (const std::exception& e) {
        return failed(e.what());
    }
    sys::HttpRequest req;
    req.url = url;
    req.headers = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    std::string body;
    bool tooBig = false;
    req.onBody = [&](std::string_view chunk) {
        if (body.size() + chunk.size() > kMaxAnswerBytes) {
            tooBig = true;
            return false;
        }
        body.append(chunk);
        return true;
    };
    const sys::HttpResponse res = http->send(req);
    http->close();
    if (tooBig) return failed("the answer is larger than " + std::to_string(kMaxAnswerBytes / 1024) + " KB");
    if (res.status == 0) return failed(res.timedOut ? "no answer in time" : "no answer: " + res.error);
    if (res.status != 200) return failed("the server answered " + std::to_string(res.status));
    std::string why;
    auto latest = parseLatestRelease(body, &why);
    if (!latest) return failed("unusable answer (" + why + ")");
    out.ok = true;
    out.newer = compareVersions(*parseVersion(latest->version), *current) > 0;
    out.latest = std::move(latest);
    applog::info("Version check: the latest release is " + out.latest->version +
                 (out.newer ? " (newer than " + currentVersion + ")" : " (this is " + currentVersion + ")"));
    return out;
}

}  // namespace st::update
