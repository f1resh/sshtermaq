#include "s3/Credentials.h"

#include "core/Crypto.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Http.h"
#include "platform/sys/Net.h"
#include "platform/sys/Process.h"
#include "s3/Dates.h"
#include "s3/Xml.h"

#include <chrono>
#include <ctime>
#include <set>

namespace st::s3 {

namespace {

constexpr int64_t kRefreshBeforeSeconds = 300;

int64_t realNow() { return static_cast<int64_t>(std::time(nullptr)); }

std::string mask(const std::string& id) {
    return id.size() > 8 ? id.substr(0, 4) + "…" + id.substr(id.size() - 4) : id;
}

class Fixed final : public CredentialProvider {
public:
    Fixed(AwsCredentials c, std::string d) : c_(std::move(c)), d_(std::move(d)) {}
    AwsCredentials get(bool) override { return c_; }
    std::string describe() const override { return d_; }

private:
    AwsCredentials c_;
    std::string d_;
};

class Refreshing final : public CredentialProvider {
public:
    Refreshing(std::function<AwsCredentials()> f, std::string d, std::function<int64_t()> now)
        : fetch_(std::move(f)), d_(std::move(d)), now_(now ? std::move(now) : std::function<int64_t()>(realNow)) {}

    AwsCredentials get(bool force) override {
        std::lock_guard lk(m_);
        const bool stale = !have_ || (c_.expiresAt && c_.expiresAt - kRefreshBeforeSeconds <= now_());
        if (force || stale) {
            c_ = fetch_();
            have_ = true;
        }
        return c_;
    }
    std::string describe() const override { return d_; }

private:
    std::function<AwsCredentials()> fetch_;
    std::string d_;
    std::function<int64_t()> now_;
    std::mutex m_;
    AwsCredentials c_;
    bool have_ = false;
};

std::string formEncode(const Params& p) {
    std::string out;
    for (const auto& [k, v] : p) {
        if (!out.empty()) out += '&';
        out += uriEncode(k) + "=" + uriEncode(v);
    }
    return out;
}

/** Resolves a profile into credentials; `visited` catches source_profile loops. */
class ProfileResolver {
public:
    ProfileResolver(const AwsConfigFiles& f, const ProfileContext& ctx) : files_(f), ctx_(ctx) {}

    std::shared_ptr<CredentialProvider> provider(const std::string& name) {
        std::set<std::string> visited;
        return resolve(name, visited, true);
    }

private:
    std::optional<std::string> get(const std::string& p, const std::string& k) const { return files_.get(p, k); }
    std::string value(const std::string& p, const std::string& k) const { return get(p, k).value_or(std::string()); }
    int64_t now() const { return ctx_.now ? ctx_.now() : realNow(); }
    void log(const std::string& s) const {
        if (ctx_.log) ctx_.log(s);
    }

    std::shared_ptr<CredentialProvider> resolve(const std::string& name, std::set<std::string>& visited, bool top) {
        if (!files_.profiles.count(name)) {
            throw CredentialsError("The profile \"" + name + "\" is not in " + sys::fromPath(files_.configPath) +
                                   " or " + sys::fromPath(files_.credentialsPath));
        }
        if (!visited.insert(name).second)
            throw CredentialsError("The profile \"" + name + "\" refers back to itself through source_profile");

        const std::string roleArn = value(name, "role_arn");
        const std::string source = value(name, "source_profile");
        if (!roleArn.empty()) {
            std::shared_ptr<CredentialProvider> base;
            const std::string credSource = value(name, "credential_source");
            if (source == name && hasStaticKeys(name)) {
                base = staticKeys(name);  // a profile that is its own source assumes the role with its own keys
            } else if (!source.empty()) {
                base = resolve(source, visited, false);
            } else if (credSource == "Environment") {
                auto env = credentialsFromEnvironment();
                if (!env) throw CredentialsError("The profile \"" + name + "\" takes its keys from the environment, but AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY are not set");
                base = fixedCredentials(*env, "environment");
            } else if (!credSource.empty()) {
                throw CredentialsError("credential_source = " + credSource + " (profile \"" + name +
                                       "\") is not supported: SSHTermaq runs on a desktop, not on EC2 or ECS");
            } else {
                throw CredentialsError("The profile \"" + name + "\" has role_arn but neither source_profile nor credential_source");
            }
            return assumeRole(name, roleArn, base);
        }
        if (hasStaticKeys(name)) return staticKeys(name);
        if (const std::string cmd = value(name, "credential_process"); !cmd.empty()) {
            auto run = ctx_.runProcess ? ctx_.runProcess : [log = ctx_.log](const std::string& c) {
                return runCredentialProcess(c, log);
            };
            return refreshingCredentials([cmd, run] { return parseCredentialProcessOutput(run(cmd)); },
                                         "profile " + name + ": credential_process", ctx_.now);
        }
        if (get(name, "sso_session") || get(name, "sso_start_url")) return sso(name);
        if (get(name, "web_identity_token_file"))
            throw CredentialsError("web_identity_token_file (profile \"" + name + "\") is not supported");
        (void)top;
        throw CredentialsError("The profile \"" + name + "\" has no credentials (no keys, credential_process, role or SSO)");
    }

    std::shared_ptr<CredentialProvider> staticKeys(const std::string& name) const {
        AwsCredentials c;
        c.accessKeyId = value(name, "aws_access_key_id");
        c.secretAccessKey = value(name, "aws_secret_access_key");
        c.sessionToken = value(name, "aws_session_token");
        return fixedCredentials(c, "profile " + name + ": access key " + mask(c.accessKeyId));
    }

    bool hasStaticKeys(const std::string& name) const {
        return get(name, "aws_access_key_id") && get(name, "aws_secret_access_key");
    }

    std::string stsEndpoint(const std::string& profile) const {
        if (!ctx_.stsEndpoint.empty()) return ctx_.stsEndpoint;
        const std::string region = value(profile, "region");
        return region.empty() ? "https://sts.amazonaws.com" : "https://sts." + region + ".amazonaws.com";
    }

    std::shared_ptr<CredentialProvider> assumeRole(const std::string& name, const std::string& roleArn,
                                                   std::shared_ptr<CredentialProvider> base) {
        const std::string mfa = value(name, "mfa_serial");
        const std::string external = value(name, "external_id");
        std::string session = value(name, "role_session_name");
        if (session.empty()) session = "sshtermaq-" + std::to_string(now());
        const std::string duration = value(name, "duration_seconds");
        const std::string endpoint = stsEndpoint(name);
        std::string region = value(name, "region");
        if (region.empty() || endpoint == "https://sts.amazonaws.com") region = "us-east-1";  // the global endpoint signs as us-east-1
        ProfileContext ctx = ctx_;
        const std::string desc = "profile " + name + ": role " + roleArn + " via " + base->describe();
        auto fetch = [=]() -> AwsCredentials {
            Params form = {{"Action", "AssumeRole"},
                           {"Version", "2011-06-15"},
                           {"RoleArn", roleArn},
                           {"RoleSessionName", session}};
            if (!duration.empty()) form.emplace_back("DurationSeconds", duration);
            if (!external.empty()) form.emplace_back("ExternalId", external);
            if (!mfa.empty()) {
                std::optional<std::string> code = ctx.askMfaCode ? ctx.askMfaCode(mfa, name) : std::nullopt;
                if (!code || code->empty()) throw CredentialsError("The MFA code for " + mfa + " was not given");
                form.emplace_back("SerialNumber", mfa);
                form.emplace_back("TokenCode", trim(*code));
            }
            return callSts(ctx, endpoint, region, base->get(false), formEncode(form), roleArn);
        };
        return refreshingCredentials(fetch, desc, ctx_.now);
    }

    static AwsCredentials callSts(const ProfileContext& ctx, const std::string& endpoint, const std::string& region,
                                  const AwsCredentials& source, const std::string& body, const std::string& roleArn) {
        if (!ctx.http) throw CredentialsError("No HTTP client for STS");
        sys::UrlParts u;
        if (!sys::splitUrl(endpoint, u)) throw CredentialsError("Invalid STS endpoint " + endpoint);
        SignInput in;
        in.method = "POST";
        in.host = u.host + (u.port && u.port != (u.scheme == "https" ? 443 : 80) ? ":" + std::to_string(u.port) : "");
        in.encodedPath = "/";
        in.service = "sts";
        in.region = region;
        in.now = ctx.now ? ctx.now() : realNow();
        in.headers = {{"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}};
        in.payloadHash = payloadHash(body);
        const Signed s = sign(in, source);
        sys::HttpRequest req;
        req.method = "POST";
        req.url = u.scheme + "://" + in.host + "/";
        req.headers = s.headers;
        req.body = std::make_shared<const std::string>(body);
        const sys::HttpResponse r = ctx.http->send(req);
        if (r.status == 0) throw CredentialsError("STS could not be reached: " + r.error);
        try {
            const XmlNode doc = parseXml(r.body);
            if (r.status != 200) {
                const XmlNode* err = doc.child("Error");
                const std::string code = err ? err->childText("Code") : std::string();
                const std::string msg = err ? err->childText("Message") : std::string();
                throw CredentialsError("Assuming the role " + roleArn + " failed: " + (msg.empty() ? "HTTP " + std::to_string(r.status) : msg) +
                                       (code.empty() ? "" : " (" + code + ")"));
            }
            const XmlNode* res = doc.child("AssumeRoleResult");
            const XmlNode* c = res ? res->child("Credentials") : nullptr;
            if (!c) throw CredentialsError("STS answered without credentials");
            AwsCredentials out;
            out.accessKeyId = c->childText("AccessKeyId");
            out.secretAccessKey = c->childText("SecretAccessKey");
            out.sessionToken = c->childText("SessionToken");
            out.expiresAt = parseIso8601(c->childText("Expiration")).value_or(0) / 1000;
            if (out.accessKeyId.empty() || out.secretAccessKey.empty()) throw CredentialsError("STS answered without credentials");
            if (ctx.log) ctx.log("S3: assumed role " + roleArn + " until " + formatIso8601(out.expiresAt));
            return out;
        } catch (const XmlError& e) {
            throw CredentialsError(std::string("STS sent an answer that could not be read: ") + e.what());
        }
    }

    std::shared_ptr<CredentialProvider> sso(const std::string& name) {
        std::string startUrl = value(name, "sso_start_url");
        std::string ssoRegion = value(name, "sso_region");
        std::string cacheKey = startUrl;
        if (const std::string sess = value(name, "sso_session"); !sess.empty()) {
            auto it = files_.ssoSessions.find(sess);
            if (it == files_.ssoSessions.end())
                throw CredentialsError("The profile \"" + name + "\" names sso_session \"" + sess + "\", which is not in " +
                                       sys::fromPath(files_.configPath));
            auto v = [&](const char* k) {
                auto f = it->second.find(k);
                return f == it->second.end() ? std::string() : f->second;
            };
            startUrl = v("sso_start_url");
            ssoRegion = v("sso_region");
            cacheKey = sess;
        }
        const std::string account = value(name, "sso_account_id");
        const std::string role = value(name, "sso_role_name");
        if (startUrl.empty() || ssoRegion.empty() || account.empty() || role.empty())
            throw CredentialsError("The SSO profile \"" + name + "\" needs sso_start_url, sso_region, sso_account_id and sso_role_name");
        const std::string dir = ctx_.ssoCacheDir.empty() ? sys::fromPath(sys::homeDirectory() / ".aws" / "sso" / "cache")
                                                         : ctx_.ssoCacheDir;
        const std::string cacheFile = dir + "/" + hexEncode(crypto::sha1(cacheKey)) + ".json";
        std::string portal = ctx_.ssoPortalEndpoint.empty() ? "https://portal.sso.%r.amazonaws.com" : ctx_.ssoPortalEndpoint;
        portal = replaceAll(portal, "%r", ssoRegion);
        ProfileContext ctx = ctx_;
        auto fetch = [=]() -> AwsCredentials {
            const std::string login = "Run `aws sso login --profile " + name + "` and connect again";
            const auto text = sys::readFile(sys::toPath(cacheFile));
            if (!text) throw CredentialsError("There is no SSO sign-in for the profile \"" + name + "\". " + login);
            Json tok;
            try {
                tok = Json::parse(*text);
            } catch (const std::exception&) {
                throw CredentialsError("The SSO sign-in file " + cacheFile + " could not be read. " + login);
            }
            const std::string token = tok.value("accessToken", "");
            const int64_t expires = parseIso8601(tok.value("expiresAt", "")).value_or(0) / 1000;
            const int64_t t = ctx.now ? ctx.now() : realNow();
            if (token.empty() || expires <= t) throw CredentialsError("The SSO sign-in has expired. " + login);
            if (!ctx.http) throw CredentialsError("No HTTP client for SSO");
            sys::HttpRequest req;
            req.url = portal + "/federation/credentials?account_id=" + uriEncode(account) + "&role_name=" + uriEncode(role);
            req.headers = {{"x-amz-sso_bearer_token", token}};
            const sys::HttpResponse r = ctx.http->send(req);
            if (r.status == 0) throw CredentialsError("The SSO portal could not be reached: " + r.error);
            if (r.status == 401 || r.status == 403) throw CredentialsError("The SSO sign-in is no longer valid. " + login);
            if (r.status != 200) throw CredentialsError("The SSO portal answered HTTP " + std::to_string(r.status) + ": " + r.body);
            try {
                const Json j = Json::parse(r.body).at("roleCredentials");
                AwsCredentials out;
                out.accessKeyId = j.value("accessKeyId", "");
                out.secretAccessKey = j.value("secretAccessKey", "");
                out.sessionToken = j.value("sessionToken", "");
                out.expiresAt = j.value("expiration", int64_t{0}) / 1000;
                if (out.accessKeyId.empty()) throw CredentialsError("The SSO portal answered without credentials");
                return out;
            } catch (const CredentialsError&) {
                throw;
            } catch (const std::exception&) {
                throw CredentialsError("The SSO portal sent an answer that could not be read");
            }
        };
        return refreshingCredentials(fetch, "profile " + name + ": SSO " + account + "/" + role, ctx_.now);
    }

    const AwsConfigFiles& files_;
    ProfileContext ctx_;
};

}  // namespace

std::shared_ptr<CredentialProvider> fixedCredentials(AwsCredentials creds, std::string description) {
    return std::make_shared<Fixed>(std::move(creds), std::move(description));
}

std::shared_ptr<CredentialProvider> refreshingCredentials(std::function<AwsCredentials()> fetch, std::string description,
                                                          std::function<int64_t()> now) {
    return std::make_shared<Refreshing>(std::move(fetch), std::move(description), std::move(now));
}

std::optional<AwsCredentials> credentialsFromEnvironment() {
    auto id = sys::getEnv("AWS_ACCESS_KEY_ID");
    auto secret = sys::getEnv("AWS_SECRET_ACCESS_KEY");
    if (!id || id->empty() || !secret || secret->empty()) return std::nullopt;
    AwsCredentials c;
    c.accessKeyId = *id;
    c.secretAccessKey = *secret;
    c.sessionToken = sys::getEnv("AWS_SESSION_TOKEN").value_or(std::string());
    return c;
}

std::string regionFromEnvironment() {
    if (auto v = sys::getEnv("AWS_REGION"); v && !v->empty()) return *v;
    return sys::getEnv("AWS_DEFAULT_REGION").value_or(std::string());
}

std::string endpointFromEnvironment() {
    if (auto v = sys::getEnv("AWS_ENDPOINT_URL_S3"); v && !v->empty()) return *v;
    return sys::getEnv("AWS_ENDPOINT_URL").value_or(std::string());
}

std::shared_ptr<CredentialProvider> profileCredentials(const AwsConfigFiles& files, const std::string& profile,
                                                        const ProfileContext& ctx) {
    return ProfileResolver(files, ctx).provider(profile);
}

AwsCredentials parseCredentialProcessOutput(const std::string& json) {
    Json j;
    try {
        j = Json::parse(json);
    } catch (const std::exception&) {
        throw CredentialsError("credential_process printed no valid JSON");
    }
    if (!j.is_object() || j.value("Version", 0) != 1)
        throw CredentialsError("credential_process printed an unknown format (\"Version\": 1 expected)");
    AwsCredentials c;
    c.accessKeyId = j.value("AccessKeyId", "");
    c.secretAccessKey = j.value("SecretAccessKey", "");
    c.sessionToken = j.value("SessionToken", "");
    if (j.contains("Expiration") && j["Expiration"].is_string()) {
        auto t = parseIso8601(j["Expiration"].get<std::string>());
        if (!t) throw CredentialsError("credential_process printed an Expiration that is not a date");
        c.expiresAt = *t / 1000;
    }
    if (c.accessKeyId.empty() || c.secretAccessKey.empty())
        throw CredentialsError("credential_process printed no AccessKeyId / SecretAccessKey");
    return c;
}

std::string runCredentialProcess(const std::string& command, const std::function<void(const std::string&)>& log,
                                 int timeoutMs) {
    std::string error;
    std::string stderrText;
    std::mutex m;
    sys::SocketHandle s = sys::startProxyCommand(
        command,
        [&](const std::string& line) {
            std::lock_guard lk(m);
            if (stderrText.size() < 4096) stderrText += line + "\n";
            if (log) log("credential_process: " + line);
        },
        error);
    if (s == sys::kInvalidSocket) throw CredentialsError("credential_process could not be started: " + error);
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[8192];
    bool timedOut = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        const int ready = sys::waitReadable(s, static_cast<int>(std::min<long long>(left, 500)));
        if (ready < 0) break;
        if (ready == 0) continue;
        const long n = sys::recvSome(s, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 1024 * 1024) break;
    }
    sys::detachProxyCommand(s, true);
    sys::closeProxyCommandSocket(s);
    if (timedOut) throw CredentialsError("credential_process did not finish within " + std::to_string(timeoutMs / 1000) + " s");
    if (trim(out).empty()) {
        std::lock_guard lk(m);
        throw CredentialsError("credential_process printed nothing" + (stderrText.empty() ? std::string() : ": " + trim(stderrText)));
    }
    return out;
}

}  // namespace st::s3
