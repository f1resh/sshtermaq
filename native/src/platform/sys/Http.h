#pragma once
// HTTP/1.1 and HTTPS through the OS: WinHTTP (Windows) and NSURLSession (macOS). The OS does TLS, checks certificates
// against the system's trust store (company TLS-inspection roots work), applies the system proxy and keeps
// connections alive. Used by the S3 client (native/src/s3).
//
// Bodies: a request body is in memory (S3 parts are at most 64 MB, and every part is sent whole, so a resend after a
// proxy sign-in or a retry needs no rewinding); a response body is streamed to a callback, or collected.
//
// Certificates: a server whose certificate the system does not trust is accepted only when the SHA-256 of its
// certificate (DER, lower-case hex) is in HttpSessionOptions::pinnedSha256, checked on every new connection before the
// request goes out. probeCertificate() fetches the certificate of a server that failed, for the "do you trust it"
// question.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::sys {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpProxy {
    enum class Mode { None, System, Http };
    Mode mode = Mode::System;
    /** Mode::Http: the proxy's host and port. */
    std::string host;
    int port = 8080;
    /** Sign-in at the proxy: with a user name, Negotiate / NTLM / Basic with these; without one, the OS user's own
     *  sign-in (Windows: Negotiate / NTLM, no password sent) where the proxy offers it. */
    std::string username;
    std::string password;
};

struct HttpSessionOptions {
    std::string userAgent = "SSHTermaq";
    HttpProxy proxy;
    /** Certificates accepted although the system does not trust them (hex SHA-256 of the DER). */
    std::vector<std::string> pinnedSha256;
    int connectTimeoutMs = 20000;
    /** Longest wait for any data sent or received. WinHTTP rounds it up to its timer of about 4 seconds (60 s ends
     *  after about 60 s, 700 ms after about 4 s). */
    int ioTimeoutMs = 60000;
};

struct HttpRequest {
    std::string method = "GET";
    /** "https://host[:port]/path?query", already percent-encoded. */
    std::string url;
    HttpHeaders headers;
    std::shared_ptr<const std::string> body;
    /** Response body pieces as they arrive; return false to stop (the request fails, cancelled). Only for a 2xx
     *  answer: the body of any other status (an error document) is collected in HttpResponse::body. Unset: collected
     *  in HttpResponse::body. */
    std::function<bool(std::string_view chunk)> onBody;
    /** Body bytes sent so far (uploads). */
    std::function<void(uint64_t sent)> onSent;
    /** Set from any thread to abort the request; it then fails with `cancelled`. */
    const std::atomic<bool>* cancel = nullptr;
};

struct HttpResponse {
    /** 0 when no answer came (see error). */
    int status = 0;
    HttpHeaders headers;
    std::string body;
    /** Set when status is 0: a readable reason ("Connection refused", "The certificate is not trusted", ...). */
    std::string error;
    bool cancelled = false;
    bool timedOut = false;
    /** The TLS certificate was refused (not trusted and not pinned); probeCertificate() tells which one. */
    bool certificateError = false;

    /** First header named `name` (any case), or "". */
    std::string header(std::string_view name) const;
};

struct HttpCertificate {
    /** False when no TLS connection could be made at all (see error). */
    bool ok = false;
    std::string error;
    std::string der;
    /** Lower-case hex SHA-256 of `der`. */
    std::string sha256;
    /** The system trusts it for this host name. */
    bool trusted = false;
    /** Why the system does not ("self-signed", "expired", "issued for another name", ...). */
    std::string problem;
    /** Short description: subject, issuer and validity, for the question. */
    std::string subject;
    std::string issuer;
    std::string validFrom;  // "2026-01-31"
    std::string validTo;
};

/** One client: its connections are kept alive between requests. Thread-safe; requests may run in parallel. */
class HttpSession {
public:
    virtual ~HttpSession() = default;
    /** Blocking. Does not follow redirects (the caller sees 301 / 307). */
    virtual HttpResponse send(const HttpRequest& request) = 0;
    /** The certificate a TLS server at `url` presents (nothing is sent but the TLS handshake). */
    virtual HttpCertificate probeCertificate(const std::string& url) = 0;
    /** Replaces the pinned certificates for later connections. */
    virtual void setPinnedCertificates(std::vector<std::string> sha256) = 0;
    /** Aborts every running request and refuses new ones. */
    virtual void close() = 0;
};

std::shared_ptr<HttpSession> openHttpSession(const HttpSessionOptions& options);

/** Splits "scheme://host:port/path?query". False on anything else. Port 0 = the scheme's default. */
struct UrlParts {
    std::string scheme;  // "http" | "https" (lower case)
    std::string host;    // without brackets for IPv6
    int port = 0;
    std::string pathAndQuery = "/";
    int effectivePort() const { return port ? port : (scheme == "https" ? 443 : 80); }
};
bool splitUrl(std::string_view url, UrlParts& out);

}  // namespace st::sys
