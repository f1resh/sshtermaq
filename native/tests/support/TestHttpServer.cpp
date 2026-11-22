#include "TestHttpServer.h"

#include "TestNet.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace st::test {

namespace {

constexpr int kPollMs = 50;

bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 307: return "Temporary Redirect";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 407: return "Proxy Authentication Required";
        case 409: return "Conflict";
        case 412: return "Precondition Failed";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

/** A fresh P-256 key and a self-signed certificate for `name` (CN and subjectAltName). */
struct Cert {
    EVP_PKEY* key = nullptr;
    X509* x509 = nullptr;
    std::string sha256;

    explicit Cert(const std::string& name) {
        key = EVP_EC_gen("P-256");
        x509 = X509_new();
        if (!key || !x509) throw std::runtime_error("TestHttpServer: cannot make a key");
        X509_set_version(x509, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x509), static_cast<long>(std::chrono::system_clock::now().time_since_epoch().count() & 0x7fffffff));
        X509_gmtime_adj(X509_getm_notBefore(x509), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x509), 86400);
        X509_set_pubkey(x509, key);
        X509_NAME* n = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0);
        X509_set_issuer_name(x509, n);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
        const std::string san = "DNS:" + name + ",IP:127.0.0.1";
        if (X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san.c_str())) {
            X509_add_ext(x509, ext, -1);
            X509_EXTENSION_free(ext);
        }
        if (!X509_sign(x509, key, EVP_sha256())) throw std::runtime_error("TestHttpServer: cannot sign the certificate");
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        X509_digest(x509, EVP_sha256(), md, &len);
        static const char kHex[] = "0123456789abcdef";
        for (unsigned int i = 0; i < len; ++i) {
            sha256 += kHex[md[i] >> 4];
            sha256 += kHex[md[i] & 15];
        }
    }
    ~Cert() {
        X509_free(x509);
        EVP_PKEY_free(key);
    }
};

/** One accepted connection: plain or TLS. */
class Conn {
public:
    Conn(net::Socket s, SSL_CTX* ctx) : sock_(std::move(s)) {
        if (ctx) {
            ssl_ = SSL_new(ctx);
            SSL_set_fd(ssl_, static_cast<int>(sock_.get()));
        }
    }
    ~Conn() {
        if (ssl_) SSL_free(ssl_);
    }
    bool handshake() { return !ssl_ || SSL_accept(ssl_) == 1; }
    /** > 0 bytes, 0 closed, -1 error. Blocking. */
    long read(char* buf, size_t n) {
        if (ssl_) {
            const int r = SSL_read(ssl_, buf, static_cast<int>(n));
            return r > 0 ? r : (SSL_get_error(ssl_, r) == SSL_ERROR_ZERO_RETURN ? 0 : -1);
        }
        return sys::recvSome(sock_.get(), buf, n);
    }
    bool write(std::string_view data) {
        if (ssl_) {
            while (!data.empty()) {
                const int r = SSL_write(ssl_, data.data(), static_cast<int>(std::min<size_t>(data.size(), 1 << 20)));
                if (r <= 0) return false;
                data.remove_prefix(static_cast<size_t>(r));
            }
            return true;
        }
        return net::sendAll(sock_.get(), data);
    }
    bool readable(int timeoutMs) {
        if (ssl_ && SSL_pending(ssl_) > 0) return true;
        return net::waitReadable(sock_.get(), timeoutMs);
    }
    void closeNow() {
        if (ssl_) SSL_shutdown(ssl_);
        sock_.reset();
    }
    net::Socket& socket() { return sock_; }

private:
    net::Socket sock_;
    SSL* ssl_ = nullptr;
};

}  // namespace

struct TestHttpServer::Impl {
    TestHttpServerOptions opts;
    std::unique_ptr<Cert> cert;
    SSL_CTX* ctx = nullptr;
    net::Socket listener;
    int port = 0;
    std::atomic<bool> stopping{false};
    std::thread acceptor;

    mutable std::mutex mu;
    TestHttpHandler handler;
    std::list<std::thread> workers;
    std::vector<std::string> log;
    int requests = 0;
    int connections = 0;
    int connectionsWithData = 0;

    ~Impl() {
        if (ctx) SSL_CTX_free(ctx);
    }

    void acceptLoop() {
        while (!stopping.load()) {
            if (!net::waitReadable(listener.get(), kPollMs)) continue;
            net::Socket s(sys::acceptConnection(listener.get(), true));
            if (!s) continue;
            net::setNonBlocking(s.get(), false);
            int id = 0;
            {
                std::lock_guard lk(mu);
                id = ++connections;
            }
            auto c = std::make_shared<Conn>(std::move(s), ctx);
            std::lock_guard lk(mu);
            workers.emplace_back([this, c, id] { serve(*c, id); });
        }
    }

    void serve(Conn& c, int id) {
        if (!c.handshake()) return;
        std::string buf;
        bool counted = false;
        char chunk[65536];
        while (!stopping.load()) {
            // Headers.
            size_t end = buf.find("\r\n\r\n");
            while (end == std::string::npos) {
                if (stopping.load()) return;
                if (!c.readable(kPollMs)) continue;
                const long n = c.read(chunk, sizeof chunk);
                if (n <= 0) return;
                if (!counted) {
                    counted = true;
                    std::lock_guard lk(mu);
                    ++connectionsWithData;
                }
                buf.append(chunk, static_cast<size_t>(n));
                end = buf.find("\r\n\r\n");
            }
            TestHttpRequest req;
            req.connection = id;
            const std::string head = buf.substr(0, end);
            buf.erase(0, end + 4);
            size_t lineEnd = head.find("\r\n");
            const std::string requestLine = head.substr(0, lineEnd);
            const size_t sp1 = requestLine.find(' ');
            const size_t sp2 = requestLine.rfind(' ');
            if (sp1 == std::string::npos || sp2 == sp1) return;
            req.method = requestLine.substr(0, sp1);
            req.target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
            const size_t q = req.target.find('?');
            req.path = req.target.substr(0, q);
            req.query = q == std::string::npos ? std::string() : req.target.substr(q + 1);
            while (lineEnd != std::string::npos) {
                const size_t start = lineEnd + 2;
                lineEnd = head.find("\r\n", start);
                const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string v = line.substr(colon + 1);
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                req.headers.emplace_back(line.substr(0, colon), v);
            }
            // Body.
            size_t length = 0;
            if (const std::string cl = req.header("Content-Length"); !cl.empty()) length = std::stoull(cl);
            while (buf.size() < length) {
                if (stopping.load()) return;
                if (!c.readable(kPollMs)) continue;
                const long n = c.read(chunk, sizeof chunk);
                if (n <= 0) return;
                buf.append(chunk, static_cast<size_t>(n));
            }
            req.body = buf.substr(0, length);
            buf.erase(0, length);

            TestHttpHandler h;
            {
                std::lock_guard lk(mu);
                h = handler;
                log.push_back(req.method + " " + req.target);
            }
            TestHttpResponse r = h ? h(req) : TestHttpResponse{404};
            {
                std::lock_guard lk(mu);
                ++requests;
            }
            if (r.delayMs > 0) {
                for (int waited = 0; waited < r.delayMs && !stopping.load(); waited += 10)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (r.closeWithoutAnswer) {
                c.closeNow();
                return;
            }
            if (!answer(c, req, r)) return;
            if (r.close || sameText(req.header("Connection"), "close")) {
                c.closeNow();
                return;
            }
        }
    }

    /** False when the connection is done (faults, write errors). */
    bool answer(Conn& c, const TestHttpRequest& req, const TestHttpResponse& r) {
        const bool head = req.method == "HEAD";
        const uint64_t size = r.generate ? r.generatedSize : r.body.size();
        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
        bool hasLength = false;
        for (const auto& [n, v] : r.headers) {
            if (sameText(n, "Content-Length")) hasLength = true;
            out += n + ": " + v + "\r\n";
        }
        if (!hasLength && r.status != 204 && r.status != 304) out += "Content-Length: " + std::to_string(size) + "\r\n";
        if (r.close) out += "Connection: close\r\n";
        out += "\r\n";
        int64_t budget = r.dropAfterBytes;
        auto send = [&](std::string_view data) {
            if (budget >= 0) {
                if (static_cast<int64_t>(data.size()) >= budget) {
                    c.write(data.substr(0, static_cast<size_t>(budget)));
                    c.closeNow();
                    return false;
                }
                budget -= static_cast<int64_t>(data.size());
            }
            return c.write(data);
        };
        if (!send(out)) return false;
        if (head) return true;
        int64_t stall = r.stallAfterBodyBytes;
        auto stallNow = [&] {
            while (!stopping.load()) {
                if (c.readable(kPollMs)) {
                    char b[256];
                    if (c.read(b, sizeof b) <= 0) break;
                }
            }
            return false;
        };
        if (r.generate) {
            std::string piece;
            for (uint64_t off = 0; off < size;) {
                size_t n = static_cast<size_t>(std::min<uint64_t>(size - off, 1 << 20));
                if (stall >= 0) {
                    if (static_cast<int64_t>(off) >= stall) return stallNow();
                    n = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(n), stall - static_cast<int64_t>(off)));
                }
                piece.resize(n);
                r.generate(off, piece.data(), n);
                if (!send(piece)) return false;
                off += n;
            }
            if (stall >= 0 && static_cast<int64_t>(size) >= stall) return stallNow();
            return true;
        }
        if (stall >= 0) {
            send(std::string_view(r.body).substr(0, static_cast<size_t>(std::min<int64_t>(stall, static_cast<int64_t>(r.body.size())))));
            return stallNow();
        }
        return send(r.body);
    }
};

std::string TestHttpRequest::header(std::string_view name) const {
    for (const auto& [n, v] : headers)
        if (sameText(n, name)) return v;
    return {};
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool TestHttpRequest::has(std::string_view name) const {
    for (std::string_view rest = query; !rest.empty();) {
        const size_t amp = rest.find('&');
        const std::string_view part = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        if (percentDecode(part.substr(0, part.find('='))) == name) return true;
    }
    return false;
}

std::string TestHttpRequest::param(std::string_view name) const {
    for (std::string_view rest = query; !rest.empty();) {
        const size_t amp = rest.find('&');
        const std::string_view part = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        const size_t eq = part.find('=');
        if (percentDecode(part.substr(0, eq)) == name)
            return eq == std::string_view::npos ? std::string() : percentDecode(part.substr(eq + 1));
    }
    return {};
}

TestHttpServer::TestHttpServer(TestHttpHandler handler, TestHttpServerOptions options) : impl_(std::make_shared<Impl>()) {
    net::ensureNetInit();
    impl_->opts = std::move(options);
    impl_->handler = std::move(handler);
    if (impl_->opts.tls) {
        impl_->cert = std::make_unique<Cert>(impl_->opts.certificateName);
        impl_->ctx = SSL_CTX_new(TLS_server_method());
        if (!impl_->ctx || SSL_CTX_use_certificate(impl_->ctx, impl_->cert->x509) != 1 ||
            SSL_CTX_use_PrivateKey(impl_->ctx, impl_->cert->key) != 1)
            throw std::runtime_error("TestHttpServer: TLS setup failed");
    }
    impl_->listener = net::listenLoopback(impl_->opts.port, impl_->port);
    net::setNonBlocking(impl_->listener.get(), true);
    impl_->acceptor = std::thread([i = impl_.get()] { i->acceptLoop(); });
}

TestHttpServer::~TestHttpServer() { stop(); }

int TestHttpServer::port() const { return impl_->port; }

std::string TestHttpServer::url() const {
    return std::string(impl_->opts.tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(impl_->port);
}

std::string TestHttpServer::certificateSha256() const { return impl_->cert ? impl_->cert->sha256 : std::string(); }

int TestHttpServer::requests() const {
    std::lock_guard lk(impl_->mu);
    return impl_->requests;
}

int TestHttpServer::connections() const {
    std::lock_guard lk(impl_->mu);
    return impl_->connections;
}

int TestHttpServer::connectionsWithData() const {
    std::lock_guard lk(impl_->mu);
    return impl_->connectionsWithData;
}

std::vector<std::string> TestHttpServer::log() const {
    std::lock_guard lk(impl_->mu);
    return impl_->log;
}

void TestHttpServer::setHandler(TestHttpHandler handler) {
    std::lock_guard lk(impl_->mu);
    impl_->handler = std::move(handler);
}

void TestHttpServer::stop() {
    if (impl_->stopping.exchange(true)) return;
    if (impl_->acceptor.joinable()) impl_->acceptor.join();
    std::list<std::thread> workers;
    {
        std::lock_guard lk(impl_->mu);
        workers.swap(impl_->workers);
    }
    for (auto& t : workers)
        if (t.joinable()) t.join();
    impl_->listener.reset();
}

}  // namespace st::test
