#pragma once
// The version check (1.2.0 item 12): one HTTPS request for the project's latest release on GitHub, at most once a day
// (the UI keeps the times: ui/src/store/updates.ts). Nothing about the user or the computer is sent beyond what any
// HTTPS request shows; the request has a plain `User-Agent: SSHTermaq`. It never downloads or installs anything.
//
// The answer is data from the network: the version is read strictly (N.N.N), drafts and pre-releases are not
// offered, the answer is at most kMaxAnswerBytes, and the only link handed on is the release page built here from
// the checked version (never a URL from the answer).
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace st::update {

inline constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/f1resh/sshtermaq/releases/latest";
inline constexpr const char* kReleasePagePrefix = "https://github.com/f1resh/sshtermaq/releases/tag/";
inline constexpr size_t kMaxAnswerBytes = 1 << 20;
inline constexpr int kTimeoutMs = 10000;

struct Version {
    int major = 0, minor = 0, patch = 0;
};

/** "1.2.0" or "v1.2.0": three numbers of at most 5 digits each, nothing else. */
std::optional<Version> parseVersion(std::string_view text);
/** <0, 0, >0. */
int compareVersions(const Version& a, const Version& b);

struct Release {
    /** "1.3.0" (without a leading "v"). */
    std::string version;
    /** kReleasePagePrefix + the release's tag. */
    std::string url;
};

/** Reads GitHub's answer for releases/latest. nullopt, with the reason in `why`, for anything that is not a published
 *  release with an N.N.N tag. */
std::optional<Release> parseLatestRelease(std::string_view body, std::string* why = nullptr);

struct CheckResult {
    bool ok = false;
    /** When !ok: why ("No answer: ...", "GitHub answered 403", ...). */
    std::string error;
    std::optional<Release> latest;
    /** latest is newer than the running version. */
    bool newer = false;
};

/** Blocking: asks `url` (GitHub, or a test server) through the system's proxy settings, with kTimeoutMs, and compares
 *  the answer with `currentVersion`. Writes one line to the app log. */
CheckResult checkLatestRelease(const std::string& currentVersion, const std::string& url = kLatestReleaseUrl,
                               int timeoutMs = kTimeoutMs);

}  // namespace st::update
