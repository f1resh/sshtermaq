#include "s3/AwsConfig.h"

#include "core/Strings.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"

namespace st::s3 {

namespace {

std::filesystem::path expandHome(const std::string& p) {
    if (p == "~" || startsWith(p, "~/") || startsWith(p, "~\\"))
        return sys::homeDirectory() / sys::toPath(p.size() > 2 ? p.substr(2) : std::string());
    return sys::toPath(p);
}

std::string readFile(const std::filesystem::path& p) { return sys::readFile(p).value_or(std::string()); }

}  // namespace

std::optional<std::string> AwsConfigFiles::get(const std::string& profile, const std::string& key) const {
    auto p = profiles.find(profile);
    if (p == profiles.end()) return std::nullopt;
    auto v = p->second.find(key);
    if (v == p->second.end()) return std::nullopt;
    return v->second;
}

std::vector<std::string> AwsConfigFiles::profileNames() const {
    std::vector<std::string> out;
    for (const auto& [n, _] : profiles) out.push_back(n);
    return out;
}

void parseAwsIni(std::string_view text, bool isConfig, AwsConfigFiles& into) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    AwsSection* section = nullptr;
    std::string nestedKey;  // "s3" while reading its indented sub-settings
    int lineNo = 0;
    const std::string where = isConfig ? "config" : "credentials";
    for (const std::string& raw : split(text, '\n')) {
        ++lineNo;
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[') {
            nestedKey.clear();
            if (t.back() != ']') {
                into.warnings.push_back(where + " line " + std::to_string(lineNo) + ": unterminated section");
                section = nullptr;
                continue;
            }
            const std::string name = trim(std::string_view(t).substr(1, t.size() - 2));
            if (isConfig) {
                if (name == "default") {
                    section = &into.profiles["default"];
                } else if (startsWith(name, "profile ")) {
                    section = &into.profiles[trim(std::string_view(name).substr(8))];
                } else if (startsWith(name, "sso-session ")) {
                    section = &into.ssoSessions[trim(std::string_view(name).substr(12))];
                } else {
                    section = nullptr;  // [services ...], [plugins] and unknown kinds
                }
            } else {
                section = &into.profiles[name];
            }
            continue;
        }
        if (!section) continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            into.warnings.push_back(where + " line " + std::to_string(lineNo) + ": no '='");
            continue;
        }
        const std::string key = toLower(trim(std::string_view(t).substr(0, eq)));
        const std::string value = trim(std::string_view(t).substr(eq + 1));
        if (indented && !nestedKey.empty()) {
            (*section)[nestedKey + "." + key] = value;
            continue;
        }
        nestedKey.clear();
        if (value.empty()) {
            nestedKey = key;  // a nested block may follow
            continue;
        }
        (*section)[key] = value;
    }
}

std::filesystem::path awsConfigPath() {
    if (auto v = sys::getEnv("AWS_CONFIG_FILE"); v && !v->empty()) return expandHome(*v);
    return sys::homeDirectory() / ".aws" / "config";
}

std::filesystem::path awsCredentialsPath() {
    if (auto v = sys::getEnv("AWS_SHARED_CREDENTIALS_FILE"); v && !v->empty()) return expandHome(*v);
    return sys::homeDirectory() / ".aws" / "credentials";
}

AwsConfigFiles loadAwsConfig() { return loadAwsConfig(awsConfigPath(), awsCredentialsPath()); }

AwsConfigFiles loadAwsConfig(const std::filesystem::path& config, const std::filesystem::path& credentials) {
    AwsConfigFiles f;
    f.configPath = config;
    f.credentialsPath = credentials;
    parseAwsIni(readFile(config), true, f);
    // Credentials are parsed into a copy and laid over config, so they win key by key.
    AwsConfigFiles creds;
    parseAwsIni(readFile(credentials), false, creds);
    for (auto& [name, values] : creds.profiles)
        for (auto& [k, v] : values) f.profiles[name][k] = v;
    for (auto& w : creds.warnings) f.warnings.push_back(std::move(w));
    return f;
}

std::string defaultAwsProfile() {
    if (auto v = sys::getEnv("AWS_PROFILE"); v && !v->empty()) return *v;
    if (auto v = sys::getEnv("AWS_DEFAULT_PROFILE"); v && !v->empty()) return *v;
    return "default";
}

}  // namespace st::s3
