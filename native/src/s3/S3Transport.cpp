#include "s3/S3Transport.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "platform/Platform.h"
#include "platform/sys/Debug.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Http.h"
#include "platform/sys/Net.h"
#include "s3/AwsConfig.h"
#include "s3/Credentials.h"
#include "s3/S3Client.h"
#include "session/Prompter.h"
#include "store/Credentials.h"
#include "store/KnownHosts.h"
#include "store/Vault.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace st {

namespace {

constexpr const char* kCertKeyType = "x509-sha256";

CloseInfo closedByUser() {
    CloseInfo ci;
    ci.clean = true;
    ci.reason = "Connection closed by user";
    return ci;
}

CloseInfo failed(std::string reason) {
    CloseInfo ci;
    ci.error = true;
    ci.reason = std::move(reason);
    return ci;
}

void wipe(std::string& s) { crypto::cleanse(s); }

std::string stripSlashes(std::string s) {
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

/** The profile a session uses ('profile' method), or the default one. */
std::string profileName(const SessionConfig& s) {
    return trim(s.s3.auth.profile).empty() ? s3::defaultAwsProfile() : trim(s.s3.auth.profile);
}

struct Cancelled {};

class S3TransportImpl final : public S3Transport {
public:
    explicit S3TransportImpl(TransportContext ctx) : ctx_(std::move(ctx)) {}
    ~S3TransportImpl() override {
        close();
        if (thread_.joinable()) thread_.join();
    }

    Protocol protocol() const override { return Protocol::S3; }
    void start() override {
        if (started_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }
    void write(std::string) override {}
    void resize(int, int, int, int) override {}
    std::vector<SpecialCommandDef> specials() const override { return {}; }
    void sendSpecial(const std::string&) override {}
    LdiscModes defaultLdisc() const override { return {}; }
    S3Transport* asS3() override { return this; }

    void close() override {
        cancel_ = true;
        std::shared_ptr<S3Connection> c;
        std::shared_ptr<sys::HttpSession> http;
        {
            std::lock_guard lk(m_);
            c = conn_;
            http = http_;
        }
        if (c) c->closed = true;
        if (http) http->close();
        cv_.notify_all();
    }

    std::shared_ptr<S3Connection> connection() const override {
        std::lock_guard lk(m_);
        return connected_ ? conn_ : nullptr;
    }

private:
    void log(LogLevel level, const std::string& msg) {
        if (ctx_.listener) ctx_.listener->onLog(level, msg);
    }
    void status(ConnStatus st, const std::string& msg) {
        if (ctx_.listener) ctx_.listener->onStatus(st, msg);
    }
    void checkCancel() {
        if (cancel_) throw Cancelled{};
    }

    void run() {
        sys::setThreadName("st-s3");
        CloseInfo info;
        try {
            connect();
            // Connected: wait until closed. The file manager does the work on its own threads.
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return cancel_.load(); });
            info = closedByUser();
        } catch (const Cancelled&) {
            info = closedByUser();
        } catch (const s3::S3Error& e) {
            info = cancel_ ? closedByUser() : failed(e.what());
        } catch (const s3::CredentialsError& e) {
            info = cancel_ ? closedByUser() : failed(e.what());
        } catch (const std::exception& e) {
            info = cancel_ ? closedByUser() : failed(std::string("Internal error: ") + e.what());
        }
        {
            std::lock_guard lk(m_);
            connected_ = false;
            if (conn_) conn_->closed = true;
        }
        if (http_) http_->close();
        if (!info.error) log(LogLevel::Info, "S3 session closed");
        if (ctx_.listener) ctx_.listener->onClose(info);
    }

    void connect() {
        const SessionConfig& s = ctx_.session;
        const S3Resolved where = resolveS3Settings(s);
        const std::string endpointText = where.endpoint.empty() ? "AWS (" + where.region + ")" : where.endpoint;
        status(ConnStatus::Connecting, "Connecting to " + endpointText);
        log(LogLevel::Info, "S3: endpoint " + endpointText + ", region " + where.region +
                                (where.pathStyle ? ", path-style addressing" : ", virtual-hosted addressing"));
        const std::string baseUrl = where.endpoint.empty() ? "https://s3." + where.region + ".amazonaws.com" : where.endpoint;
        sys::UrlParts url;
        if (!sys::splitUrl(baseUrl, url))
            throw s3::S3Error("The endpoint \"" + where.endpoint + "\" is not an http:// or https:// address");
        if (url.scheme == "http")
            log(LogLevel::Warn, "S3: " + where.endpoint + " is plain HTTP: files and requests are not encrypted");

        sys::HttpSessionOptions ho;
        ho.userAgent = std::string("SSHTermaq/") + sys::osName();
        ho.connectTimeoutMs = std::max(1, s.connection.connectTimeoutSeconds) * 1000;
        if (s.s3.systemProxy) {
            ho.proxy.mode = sys::HttpProxy::Mode::System;
        } else if (s.proxy.type == ProxyType::Http) {
            ho.proxy.mode = sys::HttpProxy::Mode::Http;
            ho.proxy.host = s.proxy.host;
            ho.proxy.port = s.proxy.port;
            ho.proxy.username = s.proxy.username;
            ho.proxy.password = s.proxy.password;
            log(LogLevel::Info, "S3: through the HTTP proxy " + sys::formatHostPort(s.proxy.host, s.proxy.port));
        } else {
            if (s.proxy.type != ProxyType::None)
                log(LogLevel::Warn, "S3: only HTTP proxies are supported; connecting directly");
            ho.proxy.mode = sys::HttpProxy::Mode::None;
        }
        certHost_ = url.host;
        certPort_ = url.effectivePort();
        ho.pinnedSha256 = pinnedCertificates();
        auto http = sys::openHttpSession(ho);
        {
            std::lock_guard lk(m_);
            http_ = http;
        }
        checkCancel();

        // A self-hosted https:// endpoint: check its certificate before anything else (an unsigned HEAD), so an
        // untrusted one is asked about before the secret is.
        if (url.scheme == "https" && !where.endpoint.empty()) {
            const sys::HttpCertificate cert = http->probeCertificate(baseUrl + "/");
            checkCancel();
            const std::vector<std::string> pins = pinnedCertificates();
            if (cert.ok && !cert.trusted && std::find(pins.begin(), pins.end(), cert.sha256) == pins.end() &&
                !askCertificate(*http, baseUrl, &cert))
                throw Cancelled{};
        }

        status(ConnStatus::Authenticating, "Signing in");
        bool fromPrompt = false, remember = false;
        std::string secret;
        auto creds = credentials(http, fromPrompt, remember, secret, "");
        s3::S3Config cfg;
        cfg.endpoint = where.endpoint;
        cfg.region = where.region;
        cfg.pathStyle = where.pathStyle;
        cfg.log = [this](const std::string& l) { log(LogLevel::Info, l); };
        auto client = std::make_shared<s3::S3Client>(http, creds, cfg);

        // Check access; ask about an untrusted certificate, and again for a secret that was not accepted.
        const std::string bucket = stripSlashes(s.s3.bucket);
        for (int attempt = 0;; ++attempt) {
            checkCancel();
            try {
                if (!bucket.empty()) {
                    try {
                        const std::string region = client->bucketRegion(bucket, &cancel_);
                        log(LogLevel::Info, "S3: bucket " + bucket + " is in " + (region.empty() ? where.region : region));
                    } catch (const s3::S3Error& e) {
                        if (e.code != "AccessDenied") throw;
                        // A HEAD answer has no error document: list one key to learn why (a wrong secret, or really
                        // no permission for HeadBucket, which keys limited to some folders often lack).
                        try {
                            client->listObjects(bucket, stripSlashes(s.s3.startPath).empty() ? "" : stripSlashes(s.s3.startPath) + "/",
                                                "/", "", 1, &cancel_);
                        } catch (const s3::S3Error& l) {
                            if (l.code != "AccessDenied") throw;
                            log(LogLevel::Warn, std::string("S3: ") + l.what() + "; connecting anyway");
                        }
                    }
                } else if (s.s3.auth.method == "anonymous") {
                    throw s3::S3Error("Anonymous access needs the bucket's name: enter it in the session");
                } else {
                    const auto buckets = client->listBuckets(&cancel_);
                    log(LogLevel::Info, "S3: " + std::to_string(buckets.size()) + " bucket(s)");
                }
                break;
            } catch (const s3::S3Error& e) {
                if (e.cancelled) throw Cancelled{};
                if (e.certificate && attempt < 3) {
                    if (!askCertificate(*http, baseUrl)) throw Cancelled{};
                    continue;
                }
                const bool badSecret = e.code == "SignatureDoesNotMatch" || e.code == "InvalidAccessKeyId";
                if (badSecret && s.s3.auth.method == "keys" && attempt < 3 && ctx_.prompter) {
                    log(LogLevel::Info, std::string("S3: ") + e.what());
                    wipe(secret);
                    creds = credentials(http, fromPrompt, remember, secret,
                                        e.code == "InvalidAccessKeyId" ? "The access key ID was not accepted"
                                                                       : "The secret access key was not accepted");
                    client = std::make_shared<s3::S3Client>(http, creds, cfg);
                    continue;
                }
                throw;
            }
        }
        if (fromPrompt && remember && !secret.empty()) saveSecret(secret);
        wipe(secret);

        auto conn = std::make_shared<S3Connection>();
        conn->http = http;
        conn->client = client;
        conn->bucket = bucket;
        conn->startPath = stripSlashes(s.s3.startPath);
        conn->partSize = static_cast<uint64_t>(std::clamp(s.s3.partSizeMiB, 8, 64)) << 20;
        conn->parallelParts = std::clamp(s.s3.parallelParts, 1, 16);
        conn->https = url.scheme == "https";
        conn->log = [this](LogLevel l, const std::string& m) { log(l, m); };
        {
            std::lock_guard lk(m_);
            conn_ = conn;
            connected_ = true;
        }
        if (ctx_.listener) ctx_.listener->onServerInfo("S3 | " + client->endpointDescription() + " | " + creds->describe());
        log(LogLevel::Info, "S3: signed in (" + creds->describe() + ")");
        status(ConnStatus::Connected, "Connected");
    }

    /** Certificates the user accepted for this endpoint before (Known hosts). */
    std::vector<std::string> pinnedCertificates() const {
        std::vector<std::string> out;
        if (!ctx_.knownHosts) return out;
        for (const KnownHost& k : ctx_.knownHosts->find(certHost_, certPort_)) {
            if (k.keyType != kCertKeyType) continue;
            std::string der;
            if (crypto::base64Decode(k.key, der)) out.push_back(hexEncode(crypto::sha256(der)));
        }
        return out;
    }

    /** The certificate question; false = rejected. Pins the certificate for this session (and Known hosts). */
    bool askCertificate(sys::HttpSession& http, const std::string& url, const sys::HttpCertificate* probed = nullptr) {
        const sys::HttpCertificate cert = probed ? *probed : http.probeCertificate(url + "/");
        if (!cert.ok) throw s3::S3Error("The TLS connection failed: " + cert.error);
        log(LogLevel::Warn, "S3: the certificate of " + certHost_ + " is not trusted (" + cert.problem + "); SHA-256 " + cert.sha256);
        if (!ctx_.prompter) throw s3::S3Error("The server's certificate is not trusted: " + cert.problem);
        std::vector<std::string> previous;
        bool mismatch = false;
        if (ctx_.knownHosts) {
            for (const KnownHost& k : ctx_.knownHosts->find(certHost_, certPort_)) {
                if (k.keyType != kCertKeyType) continue;
                std::string der;
                if (crypto::base64Decode(k.key, der)) previous.push_back(hexEncode(crypto::sha256(der)));
                mismatch = true;
            }
        }
        prompts::CertificateInfo info{cert.sha256, cert.subject, cert.issuer, cert.validFrom, cert.validTo, cert.problem};
        const PromptResponse pr = ctx_.prompter->prompt(prompts::certificate(certHost_, certPort_, info, mismatch, previous));
        if (pr.cancelled || pr.choice.empty() || pr.choice == "reject") {
            log(LogLevel::Info, "S3: certificate not accepted");
            return false;
        }
        std::vector<std::string> pins = pinnedCertificates();
        pins.push_back(cert.sha256);
        http.setPinnedCertificates(pins);
        if (pr.choice == "accept-save" && ctx_.knownHosts) {
            KnownHost kh;
            kh.host = certHost_;
            kh.port = certPort_;
            kh.keyType = kCertKeyType;
            kh.key = crypto::base64Encode(cert.der);
            kh.fingerprint = "SHA256:" + crypto::base64Encode(crypto::sha256(cert.der), false);
            kh.source = "user";
            kh.comment = cert.subject;
            ctx_.knownHosts->add(kh);
            log(LogLevel::Info, "S3: certificate saved in Known hosts");
        } else {
            log(LogLevel::Info, "S3: certificate accepted for this session only");
        }
        return true;
    }

    std::optional<std::string> askText(const std::string& title, const std::string& label, bool echo) {
        if (!ctx_.prompter) return std::nullopt;
        const PromptResponse pr = ctx_.prompter->prompt(prompts::kbdint(title, "", "", {{label, echo}}, false));
        if (pr.cancelled || pr.values.empty()) return std::nullopt;
        return pr.values[0];
    }

    /** Unlocks Credentials when a vault secret needs it; false when cancelled. */
    bool ensureUnlocked() {
        if (!ctx_.vault || !ctx_.vault->locked()) return true;
        for (int attempt = 0; attempt < 20 && ctx_.prompter; ++attempt) {
            const PromptResponse pr = ctx_.prompter->prompt(prompts::unlock(
                "Unlock Credentials", attempt > 0 ? "Wrong master password, try again." : "Enter the master password of Credentials."));
            if (pr.cancelled || pr.values.empty()) return false;
            std::string pw = pr.values[0];
            const bool ok = ctx_.vault->unlock(pw);
            wipe(pw);
            if (ok) return true;
        }
        return false;
    }

    std::shared_ptr<s3::CredentialProvider> credentials(const std::shared_ptr<sys::HttpSession>& http, bool& fromPrompt,
                                                        bool& remember, std::string& secret, const std::string& retryMessage) {
        const SessionConfig& s = ctx_.session;
        const std::string method = s.s3.auth.method;
        fromPrompt = false;
        remember = false;
        if (method == "anonymous") {
            s3::AwsCredentials a;
            a.anonymous = true;
            log(LogLevel::Info, "S3: anonymous access (requests are not signed)");
            return s3::fixedCredentials(a, "anonymous");
        }
        if (method == "env") {
            auto env = s3::credentialsFromEnvironment();
            if (!env) {
                std::string msg = "AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY are not set in SSHTermaq's environment";
                if (std::string(sys::osName()) == "darwin")
                    msg += " (an app started from the Finder or the Dock does not see the variables of your shell)";
                throw s3::CredentialsError(msg);
            }
            return s3::fixedCredentials(*env, "environment: access key " + env->accessKeyId);
        }
        if (method == "profile") {
            const s3::AwsConfigFiles files = s3::loadAwsConfig();
            for (const auto& w : files.warnings) log(LogLevel::Warn, "S3: " + w);
            s3::ProfileContext pc;
            pc.http = http;
            pc.log = [this](const std::string& l) { log(LogLevel::Info, l); };
            pc.askMfaCode = [this](const std::string& serial, const std::string& profile) {
                return askText("MFA code for the profile " + profile, "Code from " + serial + ":", true);
            };
            const std::string name = profileName(s);
            if (trim(s.s3.auth.profile).empty() && !files.profiles.count(name)) {
                // No profile named and none to default to (s3:// from Quick connect): the environment's keys, if any.
                if (auto env = s3::credentialsFromEnvironment()) {
                    log(LogLevel::Info, "S3: no profile \"" + name + "\"; using AWS_ACCESS_KEY_ID from the environment");
                    return s3::fixedCredentials(*env, "environment: access key " + env->accessKeyId);
                }
            }
            log(LogLevel::Info, "S3: signing in with the profile \"" + name + "\" of " + sys::fromPath(files.configPath));
            return s3::profileCredentials(files, name, pc);
        }
        // 'keys': the session's own access key, or a login from Credentials.
        std::string keyId = trim(s.s3.auth.accessKeyId);
        std::string loginPassword;
        bool useLogin = !s.s3.auth.loginId.empty();
        if (useLogin) {
            auto login = ctx_.vault ? ctx_.vault->getLogin(s.s3.auth.loginId) : std::nullopt;
            if (!login) {
                // As SSH does: the session's own access key (saved, or asked), with a line in the event log.
                log(LogLevel::Warn, "S3: the login chosen from Credentials no longer exists; using this session's own access key");
                useLogin = false;
            } else {
                keyId = trim(login->username);
                loginPassword = login->password;
            }
        }
        if (keyId.empty()) {
            auto asked = askText("S3 access key", "Access key ID:", true);
            if (!asked || trim(*asked).empty()) throw Cancelled{};
            keyId = trim(*asked);
        }
        std::optional<std::string> found;
        if (retryMessage.empty()) {
            if (!ctx_.oneTimePassword.empty()) {
                found = ctx_.oneTimePassword;
            } else if (useLogin) {
                if (loginPassword == "stored" && ctx_.vault) {
                    if (ctx_.vault->needsUnlock(Vault::loginSecretKey(s.s3.auth.loginId)) && !ensureUnlocked()) throw Cancelled{};
                    try {
                        found = ctx_.vault->loginPassword(s.s3.auth.loginId);
                    } catch (const std::exception&) {
                    }
                }
            } else if (ctx_.credentials && !s.id.empty()) {
                found = ctx_.credentials->get(Credentials::passwordKey(s.id));
            }
        }
        if (found && !found->empty()) {
            secret = *found;
        } else {
            if (!ctx_.prompter) throw s3::CredentialsError("The secret access key is not saved");
            const bool canSave = !useLogin && !s.id.empty();
            const PromptResponse pr = ctx_.prompter->prompt(
                prompts::password("Secret access key for " + keyId, retryMessage, keyId, canSave));
            if (pr.cancelled || pr.values.empty()) throw Cancelled{};
            secret = pr.values[0];
            fromPrompt = true;
            remember = pr.remember && canSave;
        }
        s3::AwsCredentials c;
        c.accessKeyId = keyId;
        c.secretAccessKey = secret;
        const std::string shown = keyId.size() > 8 ? keyId.substr(0, 4) + "…" + keyId.substr(keyId.size() - 4) : keyId;
        return s3::fixedCredentials(c, "access key " + shown);
    }

    void saveSecret(const std::string& secret) {
        if (!ctx_.credentials || ctx_.session.id.empty()) return;
        try {
            ctx_.credentials->set(Credentials::passwordKey(ctx_.session.id), secret);
            log(LogLevel::Info, "S3: secret access key saved");
        } catch (const std::exception& e) {
            log(LogLevel::Warn, std::string("S3: the secret could not be saved: ") + e.what());
        }
    }

    TransportContext ctx_;
    std::atomic<bool> started_{false};
    std::atomic<bool> cancel_{false};
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<sys::HttpSession> http_;
    std::shared_ptr<S3Connection> conn_;
    bool connected_ = false;
    std::string certHost_;
    int certPort_ = 443;
};

}  // namespace

std::unique_ptr<Transport> makeS3Transport(TransportContext ctx) { return std::make_unique<S3TransportImpl>(std::move(ctx)); }

S3Resolved resolveS3Settings(const SessionConfig& s) {
    S3Resolved r;
    r.endpoint = trim(s.s3.endpoint);
    r.region = trim(s.s3.region);
    std::string style;  // the profile's s3.addressing_style
    if (s.s3.auth.method == "profile") {
        const s3::AwsConfigFiles files = s3::loadAwsConfig();
        const std::string name = profileName(s);
        if (r.endpoint.empty())
            r.endpoint = files.get(name, "s3.endpoint_url").value_or(files.get(name, "endpoint_url").value_or(""));
        if (r.region.empty()) r.region = files.get(name, "region").value_or("");
        style = files.get(name, "s3.addressing_style").value_or("");
    }
    if (s.s3.auth.method == "profile" || s.s3.auth.method == "env") {
        if (r.endpoint.empty()) r.endpoint = s3::endpointFromEnvironment();
        if (r.region.empty()) r.region = s3::regionFromEnvironment();
    }
    if (r.region.empty()) r.region = "us-east-1";
    while (!r.endpoint.empty() && r.endpoint.back() == '/') r.endpoint.pop_back();
    if (s.s3.addressing == "path")
        r.pathStyle = true;
    else if (s.s3.addressing == "virtual")
        r.pathStyle = false;
    else if (style == "path" || style == "virtual")
        r.pathStyle = style == "path";
    else
        r.pathStyle = !r.endpoint.empty();  // MinIO, Ceph and most self-hosted services need path-style
    return r;
}

}  // namespace st
