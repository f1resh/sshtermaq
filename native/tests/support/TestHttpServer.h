#pragma once
// An HTTP/1.1 server for tests (owner: testinfra): HTTP or HTTPS with a fresh self-signed certificate, several
// connections at once (one thread each), keep-alive, and fault injection per answer. The S3 fake (TestS3Server.h)
// runs on it.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::test {

using TestHttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct TestHttpRequest {
    std::string method;
    /** As sent: "/path?query" (still percent-encoded). */
    std::string target;
    std::string path;   // the part before '?', still encoded
    std::string query;  // the part after '?'
    TestHttpHeaders headers;
    std::string body;
    /** Which connection (1, 2, ...) it came on. */
    int connection = 0;

    std::string header(std::string_view name) const;
    /** A query parameter, percent-decoded ("" when missing); has() tells missing from empty. */
    std::string param(std::string_view name) const;
    bool has(std::string_view name) const;
};

struct TestHttpResponse {
    int status = 200;
    TestHttpHeaders headers;
    std::string body;
    /** A big body made on the fly instead of `body`: generatedSize bytes, filled by generate(offset, out, n). */
    uint64_t generatedSize = 0;
    std::function<void(uint64_t offset, char* out, size_t n)> generate;

    // ---- faults ----
    /** Waits this long before answering. */
    int delayMs = 0;
    /** Closes the connection without any answer. */
    bool closeWithoutAnswer = false;
    /** Closes the connection after this many bytes of the answer (headers included); -1 = no. */
    int64_t dropAfterBytes = -1;
    /** Sends the headers and this many bytes of the body, then stalls until the client gives up; -1 = no. */
    int64_t stallAfterBodyBytes = -1;
    /** Closes the connection after the answer (Connection: close). */
    bool close = false;
};

using TestHttpHandler = std::function<TestHttpResponse(const TestHttpRequest&)>;

struct TestHttpServerOptions {
    bool tls = false;
    /** The certificate's common name (also its subjectAltName DNS name). */
    std::string certificateName = "st-test";
    /** Fixed port on 127.0.0.1 (0 = any free one). */
    int port = 0;
};

class TestHttpServer {
public:
    TestHttpServer(TestHttpHandler handler, TestHttpServerOptions options = {});
    ~TestHttpServer();
    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    int port() const;
    /** "http://127.0.0.1:<port>" or "https://..." */
    std::string url() const;
    /** HTTPS: lower-case hex SHA-256 of the certificate's DER. */
    std::string certificateSha256() const;
    /** Requests answered (or faulted) so far, and connections accepted. */
    int requests() const;
    int connections() const;
    /** Connections that sent anything after the TLS handshake (or anything at all, without TLS). */
    int connectionsWithData() const;
    /** "METHOD target" of every request, in order. */
    std::vector<std::string> log() const;
    /** Replaces the handler (thread-safe). */
    void setHandler(TestHttpHandler handler);
    void stop();

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

/** Percent-decoding ("a%20b" -> "a b"; '+' stays). */
std::string percentDecode(std::string_view s);

}  // namespace st::test
