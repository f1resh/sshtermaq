#pragma once
// An S3 session as a Transport: no terminal, but the connection life cycle the UI already knows (Connecting ->
// Authenticating -> Connected, prompts, errors, close). On start it opens the HTTP client (proxy, pinned
// certificates), asks about an untrusted certificate, signs in (keys, environment, ~/.aws profile, anonymous; the secret
// saved, from Credentials or asked), and checks access with HeadBucket / ListBuckets. The file manager then opens an
// S3Fs (native/src/sftp/S3Fs.h) on its S3Connection.
#include "transport/Transport.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace st::sys {
class HttpSession;
}

namespace st {

namespace s3 {
class S3Client;
}

/** What the file manager works with once the session is connected. */
struct S3Connection {
    std::shared_ptr<sys::HttpSession> http;
    std::shared_ptr<s3::S3Client> client;
    /** "" = the root lists the buckets. */
    std::string bucket;
    /** Folder inside the bucket to start in, without slashes at either end. */
    std::string startPath;
    uint64_t partSize = 16ull << 20;
    int parallelParts = 4;
    bool https = true;
    std::atomic<bool> closed{false};
    std::function<void(LogLevel, const std::string&)> log;
};

class S3Transport : public Transport {
public:
    /** The connection; null until Connected and after close. */
    virtual std::shared_ptr<S3Connection> connection() const = 0;
};

std::unique_ptr<Transport> makeS3Transport(TransportContext ctx);

/** The session's settings resolved against the environment and the ~/.aws profile (for tests and the editor). */
struct S3Resolved {
    std::string endpoint;  // "" = AWS
    std::string region;
    bool pathStyle = false;
};
S3Resolved resolveS3Settings(const SessionConfig& s);

}  // namespace st
