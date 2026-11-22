// platform/sys/Http.h on Windows: WinHTTP in synchronous mode, one request per calling thread. A watchdog thread
// closes the handle of a request whose cancel flag is set, which makes its blocked call return.
//
// Pinned certificates: with pins, a request accepts any certificate in the TLS handshake (SECURITY_FLAG_IGNORE_*) and
// checks it itself in the WINHTTP_CALLBACK_STATUS_SENDING_REQUEST callback, which comes after the handshake and before
// the request is written: trusted by the system for the host name, or pinned, else the request handle is closed there
// and nothing is sent.
#include "platform/sys/Http.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace st::sys {

namespace {

constexpr DWORD kIgnoreCertFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
constexpr size_t kWriteChunk = 256 * 1024;
constexpr size_t kReadChunk = 256 * 1024;

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h) WinHttpCloseHandle(h);
        h = nullptr;
    }
    explicit operator bool() const { return h != nullptr; }
};

std::string hexOf(const BYTE* p, DWORD n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (DWORD i = 0; i < n; ++i) {
        out += kHex[p[i] >> 4];
        out += kHex[p[i] & 15];
    }
    return out;
}

std::string sha256Of(PCCERT_CONTEXT cert) {
    BYTE hash[32];
    DWORD n = sizeof hash;
    if (!CertGetCertificateContextProperty(cert, CERT_SHA256_HASH_PROP_ID, hash, &n)) return {};
    return hexOf(hash, n);
}

std::string certName(PCCERT_CONTEXT cert, DWORD flags) {
    wchar_t buf[512];
    const DWORD n = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buf, 512);
    return n > 1 ? narrow(std::wstring_view(buf, n - 1)) : std::string();
}

std::string dateOf(const FILETIME& ft) {
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return {};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

/** Whether the system trusts `cert` for `host` (an SSL server policy check); `problem` says why not. */
bool systemTrusts(PCCERT_CONTEXT cert, const std::wstring& host, std::string& problem) {
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof para;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &para, 0, nullptr, &chain)) {
        problem = "the certificate chain could not be checked: " + lastErrorMessage();
        return false;
    }
    HTTPSPolicyCallbackData ssl{};
    ssl.cbStruct = sizeof ssl;
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = const_cast<wchar_t*>(host.c_str());
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof policy;
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof status;
    const BOOL ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);
    CertFreeCertificateChain(chain);
    if (!ok) {
        problem = "the certificate could not be checked";
        return false;
    }
    switch (static_cast<HRESULT>(status.dwError)) {
        case S_OK: return true;
        case CERT_E_UNTRUSTEDROOT: problem = "it is not issued by an authority this computer trusts (self-signed?)"; break;
        case CERT_E_CHAINING: problem = "its issuer is not known to this computer"; break;
        case CERT_E_EXPIRED: problem = "it has expired or is not valid yet"; break;
        case CERT_E_CN_NO_MATCH: problem = "it is issued for another name"; break;
        case CERT_E_WRONG_USAGE: problem = "it is not meant for servers"; break;
        case CRYPT_E_REVOKED: problem = "it has been revoked"; break;
        default: problem = errorMessage(status.dwError); break;
    }
    return false;
}

void describe(PCCERT_CONTEXT cert, const std::wstring& host, HttpCertificate& out) {
    out.ok = true;
    out.der.assign(reinterpret_cast<const char*>(cert->pbCertEncoded), cert->cbCertEncoded);
    out.sha256 = sha256Of(cert);
    out.subject = certName(cert, 0);
    out.issuer = certName(cert, CERT_NAME_ISSUER_FLAG);
    out.validFrom = dateOf(cert->pCertInfo->NotBefore);
    out.validTo = dateOf(cert->pCertInfo->NotAfter);
    out.trusted = systemTrusts(cert, host, out.problem);
}

std::string winHttpError(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not connect to the server";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "The connection was reset";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "The server name could not be resolved";
        case ERROR_WINHTTP_TIMEOUT: return "The server did not answer in time";
        case ERROR_WINHTTP_SECURE_FAILURE: return "The server's certificate is not trusted";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "The server sent an invalid answer";
        case ERROR_WINHTTP_OPERATION_CANCELLED: return "Cancelled";
        case ERROR_WINHTTP_LOGIN_FAILURE: return "The proxy refused the sign-in";
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return "The server asks for a client certificate";
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR: return "The TLS connection failed";
        case ERROR_WINHTTP_INVALID_URL:
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "Invalid URL";
        default: break;
    }
    std::string m = errorMessage(code);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == '.')) m.pop_back();
    return m;
}

HttpHeaders parseRawHeaders(const std::wstring& raw) {
    HttpHeaders out;
    const std::string s = narrow(raw);
    size_t pos = s.find("\r\n");  // skip the status line
    while (pos != std::string::npos) {
        const size_t start = pos + 2;
        const size_t end = s.find("\r\n", start);
        const std::string line = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        pos = end;
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string v = line.substr(colon + 1);
        const size_t first = v.find_first_not_of(" \t");
        v = first == std::string::npos ? std::string() : v.substr(first);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        out.emplace_back(line.substr(0, colon), std::move(v));
    }
    return out;
}

/** State shared with the status callback of one request (its context value). */
struct CallbackState {
    std::wstring host;
    std::vector<std::string> pins;
    bool checkCertificate = false;
    bool rejected = false;
    std::string rejectReason;
    DWORD secureFailureFlags = 0;
};

void CALLBACK statusCallback(HINTERNET h, DWORD_PTR context, DWORD status, LPVOID info, DWORD infoLen) {
    auto* st = reinterpret_cast<CallbackState*>(context);
    if (!st) return;
    if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && info && infoLen >= sizeof(DWORD)) {
        st->secureFailureFlags = *static_cast<DWORD*>(info);
        return;
    }
    if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || !st->checkCertificate) return;
    PCCERT_CONTEXT cert = nullptr;
    DWORD n = sizeof cert;
    if (!WinHttpQueryOption(h, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &n) || !cert) return;  // plain HTTP
    std::string problem;
    bool ok = systemTrusts(cert, st->host, problem);
    if (!ok) {
        const std::string sha = sha256Of(cert);
        ok = std::find(st->pins.begin(), st->pins.end(), sha) != st->pins.end();
        if (!ok) st->rejectReason = "The server's certificate is not trusted: " + problem;
    }
    CertFreeCertificateContext(cert);
    if (!ok) {
        st->rejected = true;
        WinHttpCloseHandle(h);  // nothing has been written yet; the send fails with ERROR_WINHTTP_OPERATION_CANCELLED
    }
}

class WinHttpSession final : public HttpSession {
public:
    explicit WinHttpSession(const HttpSessionOptions& o) : opts_(o) {
        pins_ = o.pinnedSha256;
        DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
        std::wstring proxyName;
        if (o.proxy.mode == HttpProxy::Mode::None) access = WINHTTP_ACCESS_TYPE_NO_PROXY;
        if (o.proxy.mode == HttpProxy::Mode::Http) {
            access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            proxyName = widen(o.proxy.host + ":" + std::to_string(o.proxy.port));
        }
        session_.h = WinHttpOpen(widen(o.userAgent).c_str(), access,
                                 proxyName.empty() ? WINHTTP_NO_PROXY_NAME : proxyName.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) {
            WinHttpSetTimeouts(session_.h, o.connectTimeoutMs, o.connectTimeoutMs, o.ioTimeoutMs, o.ioTimeoutMs);
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            if (!WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // before Windows 11: no TLS 1.3 flag
                WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
            }
            DWORD decompress = 0;
            WinHttpSetOption(session_.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
        } else {
            openError_ = winHttpError(GetLastError());
        }
    }

    ~WinHttpSession() override { close(); }

    HttpResponse send(const HttpRequest& req) override { return run(req, false, nullptr); }

    HttpCertificate probeCertificate(const std::string& url) override {
        HttpCertificate cert;
        HttpRequest req;
        req.method = "HEAD";
        req.url = url;
        HttpResponse r = run(req, true, &cert);
        if (!cert.ok) cert.error = r.error.empty() ? "No TLS certificate (not an https:// address?)" : r.error;
        return cert;
    }

    void setPinnedCertificates(std::vector<std::string> sha256) override {
        std::lock_guard lk(m_);
        pins_ = std::move(sha256);
    }

    void close() override {
        std::thread watchdog;
        {
            std::lock_guard lk(m_);
            if (closed_) return;
            closed_ = true;
            for (Active* a : active_) closeActive(*a);
            cv_.notify_all();
            watchdog = std::move(watchdog_);
        }
        if (watchdog.joinable()) watchdog.join();
        session_.reset();
    }

private:
    struct Active {
        HINTERNET request = nullptr;
        const std::atomic<bool>* cancel = nullptr;
        bool closedByOthers = false;
        bool cancelled = false;
    };

    /** Under m_. */
    void closeActive(Active& a) {
        if (a.closedByOthers || !a.request) return;
        a.closedByOthers = true;
        a.cancelled = true;
        WinHttpCloseHandle(a.request);
    }

    void watch() {
        std::unique_lock lk(m_);
        while (!closed_) {
            if (active_.empty()) {
                cv_.wait(lk);
                continue;
            }
            cv_.wait_for(lk, std::chrono::milliseconds(50));
            for (Active* a : active_)
                if (a->cancel && a->cancel->load()) closeActive(*a);
        }
    }

    bool registerActive(Active& a) {
        std::lock_guard lk(m_);
        if (closed_) return false;
        active_.push_back(&a);
        if (!watchdog_.joinable()) watchdog_ = std::thread([this] { watch(); });
        cv_.notify_all();
        return true;
    }

    /** Takes `a` out of the watch list; true when the handle is still ours to close. */
    bool unregisterActive(Active& a) {
        std::lock_guard lk(m_);
        active_.erase(std::remove(active_.begin(), active_.end(), &a), active_.end());
        return !a.closedByOthers;
    }

    HttpResponse fail(HttpResponse r, DWORD code, const Active& a, const CallbackState& cb) {
        if (cb.rejected) {
            r.certificateError = true;
            r.error = cb.rejectReason;
        } else if (a.cancelled) {
            r.cancelled = true;
            r.error = "Cancelled";
        } else {
            r.timedOut = code == ERROR_WINHTTP_TIMEOUT;
            r.certificateError = code == ERROR_WINHTTP_SECURE_FAILURE;
            r.error = winHttpError(code);
        }
        return r;
    }

    HttpResponse run(const HttpRequest& req, bool probe, HttpCertificate* certOut) {
        HttpResponse r;
        UrlParts u;
        if (!splitUrl(req.url, u)) {
            r.error = "Invalid URL: " + req.url;
            return r;
        }
        if (!session_) {
            r.error = openError_.empty() ? "The HTTP client is closed" : openError_;
            return r;
        }
        const bool https = u.scheme == "https";
        const std::wstring whost = widen(u.host);
        Handle connect(WinHttpConnect(session_.h, whost.c_str(), static_cast<INTERNET_PORT>(u.effectivePort()), 0));
        if (!connect) {
            r.error = winHttpError(GetLastError());
            return r;
        }
        CallbackState cb;
        cb.host = whost;
        {
            std::lock_guard lk(m_);
            cb.pins = pins_;
        }
        cb.checkCertificate = https && !probe && !cb.pins.empty();

        Active a;
        a.cancel = req.cancel;
        int authRounds = 0;
        DWORD proxyScheme = 0;
        for (;;) {
            DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY | (https ? WINHTTP_FLAG_SECURE : 0);
            HINTERNET hreq = WinHttpOpenRequest(connect.h, widen(req.method).c_str(), widen(u.pathAndQuery).c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
            if (!hreq) {
                r.error = winHttpError(GetLastError());
                return r;
            }
            a.request = hreq;
            a.closedByOthers = false;
            if (!registerActive(a)) {
                WinHttpCloseHandle(hreq);
                r.cancelled = true;
                r.error = "The HTTP client is closed";
                return r;
            }
            auto release = [&] {
                if (unregisterActive(a) && !cb.rejected) WinHttpCloseHandle(hreq);
                a.request = nullptr;
            };

            DWORD feature = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(hreq, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof feature);
            if (https && (probe || cb.checkCertificate)) {
                DWORD sec = kIgnoreCertFlags;
                WinHttpSetOption(hreq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof sec);
            }
            DWORD_PTR ctx = reinterpret_cast<DWORD_PTR>(&cb);
            WinHttpSetOption(hreq, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof ctx);
            WinHttpSetStatusCallback(hreq, statusCallback,
                                     WINHTTP_CALLBACK_FLAG_SEND_REQUEST | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0);
            if (opts_.proxy.mode != HttpProxy::Mode::None && opts_.proxy.username.empty()) {
                DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;  // the OS user's sign-in at the proxy
                WinHttpSetOption(hreq, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof policy);
            }
            if (authRounds > 0) applyProxyCredentials(hreq, proxyScheme);

            std::wstring headers;
            for (const auto& [n, v] : req.headers) {
                if (_stricmp(n.c_str(), "host") == 0) continue;  // WinHTTP writes it from the connection
                headers += widen(n) + L": " + widen(v) + L"\r\n";
            }
            const DWORD total = req.body ? static_cast<DWORD>(req.body->size()) : 0;
            if (!WinHttpSendRequest(hreq, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                    static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, total, 0)) {
                const DWORD code = GetLastError();
                release();
                return fail(std::move(r), code, a, cb);
            }
            uint64_t sent = 0;
            bool writeFailed = false;
            DWORD writeError = 0;
            while (req.body && sent < req.body->size()) {
                const DWORD n = static_cast<DWORD>(std::min<uint64_t>(kWriteChunk, req.body->size() - sent));
                DWORD wrote = 0;
                if (!WinHttpWriteData(hreq, req.body->data() + sent, n, &wrote)) {
                    writeFailed = true;
                    writeError = GetLastError();
                    break;
                }
                sent += wrote;
                if (req.onSent) {
                    try {
                        req.onSent(sent);
                    } catch (...) {
                        release();  // the handle and the watch list entry must not outlive this frame
                        throw;
                    }
                }
            }
            if (writeFailed || !WinHttpReceiveResponse(hreq, nullptr)) {
                const DWORD code = writeFailed ? writeError : GetLastError();
                release();
                return fail(std::move(r), code, a, cb);
            }
            DWORD status = 0, len = sizeof status;
            WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &len, WINHTTP_NO_HEADER_INDEX);
            if (probe && certOut) {
                PCCERT_CONTEXT cert = nullptr;
                DWORD n = sizeof cert;
                if (WinHttpQueryOption(hreq, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &n) && cert) {
                    describe(cert, whost, *certOut);
                    CertFreeCertificateContext(cert);
                }
            }
            if (status == 407 && authRounds < 2 && (proxyScheme = proxySchemeFor(hreq)) != 0) {
                ++authRounds;
                drain(hreq);
                release();
                continue;
            }
            r.status = static_cast<int>(status);
            DWORD size = 0;
            WinHttpQueryHeaders(hreq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                                &size, WINHTTP_NO_HEADER_INDEX);
            if (size) {
                std::wstring raw(size / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(hreq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    raw.resize(size / sizeof(wchar_t));
                    r.headers = parseRawHeaders(raw);
                }
            }
            if (req.method != "HEAD" && status != 204 && status != 304) {
                // WinHTTP ends a body that the server cut short without an error: compare with Content-Length.
                uint64_t expected = UINT64_MAX, received = 0;
                if (const std::string cl = r.header("Content-Length"); !cl.empty()) {
                    try {
                        expected = std::stoull(cl);
                    } catch (...) {
                    }
                }
                std::string buf(kReadChunk, '\0');
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(hreq, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                        const DWORD code = GetLastError();
                        release();
                        HttpResponse e = fail(HttpResponse{}, code, a, cb);
                        e.status = 0;
                        return e;
                    }
                    if (got == 0) {
                        if (expected != UINT64_MAX && received != expected) {
                            release();
                            HttpResponse e;
                            e.error = a.cancelled ? "Cancelled" : "The connection was closed before the whole answer arrived";
                            e.cancelled = a.cancelled;
                            return e;
                        }
                        break;
                    }
                    received += got;
                    if (req.onBody && status >= 200 && status < 300) {
                        bool more = false;
                        try {
                            more = req.onBody(std::string_view(buf.data(), got));
                        } catch (...) {
                            release();
                            throw;
                        }
                        if (!more) {
                            release();
                            HttpResponse e;
                            e.cancelled = true;
                            e.error = "Cancelled";
                            return e;
                        }
                    } else {
                        r.body.append(buf.data(), got);
                    }
                }
            }
            release();
            if (a.cancelled) {
                HttpResponse e;
                e.cancelled = true;
                e.error = "Cancelled";
                return e;
            }
            return r;
        }
    }

    static void drain(HINTERNET hreq) {
        char buf[4096];
        DWORD got = 0;
        while (WinHttpReadData(hreq, buf, sizeof buf, &got) && got) {
        }
    }

    /** A 407: the proxy's best sign-in scheme; 0 when there is nothing to sign in with. */
    DWORD proxySchemeFor(HINTERNET hreq) const {
        DWORD supported = 0, first = 0, target = 0;
        if (!WinHttpQueryAuthSchemes(hreq, &supported, &first, &target)) return 0;
        const bool withUser = !opts_.proxy.username.empty();
        for (DWORD s : {WINHTTP_AUTH_SCHEME_NEGOTIATE, WINHTTP_AUTH_SCHEME_NTLM, WINHTTP_AUTH_SCHEME_BASIC}) {
            if (!(supported & s)) continue;
            if (s == WINHTTP_AUTH_SCHEME_BASIC && !withUser) continue;  // Basic needs a user name and password
            return s;
        }
        return 0;
    }

    void applyProxyCredentials(HINTERNET hreq, DWORD scheme) const {
        if (!scheme) return;
        if (opts_.proxy.username.empty()) {
            WinHttpSetCredentials(hreq, WINHTTP_AUTH_TARGET_PROXY, scheme, nullptr, nullptr, nullptr);
        } else {
            WinHttpSetCredentials(hreq, WINHTTP_AUTH_TARGET_PROXY, scheme, widen(opts_.proxy.username).c_str(),
                                  widen(opts_.proxy.password).c_str(), nullptr);
        }
    }

    HttpSessionOptions opts_;
    Handle session_;
    std::string openError_;

    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Active*> active_;
    std::vector<std::string> pins_;
    std::thread watchdog_;
    bool closed_ = false;
};

}  // namespace

std::shared_ptr<HttpSession> openHttpSession(const HttpSessionOptions& options) {
    return std::make_shared<WinHttpSession>(options);
}

}  // namespace st::sys
