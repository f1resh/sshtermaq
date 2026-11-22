#pragma once
// AWS Signature Version 4 (https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv-create-signed-request.html)
// for S3, STS and the SSO portal: signed headers and presigned URLs.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::s3 {

/** Signing credentials. Anonymous = no signing at all (public buckets). */
struct AwsCredentials {
    std::string accessKeyId;
    std::string secretAccessKey;
    std::string sessionToken;
    /** Unix seconds when temporary credentials expire; 0 = they don't. */
    int64_t expiresAt = 0;
    bool anonymous = false;
};

using Params = std::vector<std::pair<std::string, std::string>>;

/** The hash of an empty body, and the value for bodies that are not hashed (HTTPS only). */
inline constexpr std::string_view kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
inline constexpr std::string_view kUnsignedPayload = "UNSIGNED-PAYLOAD";

/** RFC 3986 percent-encoding as SigV4 wants it: unreserved characters stay, everything else is %XX (upper case). */
std::string uriEncode(std::string_view s, bool encodeSlash = true);
/** "20130524T000000Z" for Unix seconds. */
std::string amzDate(int64_t unixSeconds);
/** Hex SHA-256 of a body. */
std::string payloadHash(std::string_view body);

struct SignInput {
    std::string method;  // "GET", "PUT", ...
    /** The Host header value, with ":port" when it is not the scheme's default. */
    std::string host;
    /** The path as sent: already percent-encoded ("/bucket/my%20file"). S3 does not normalise it further. */
    std::string encodedPath = "/";
    /** Query parameters, not encoded. */
    Params query;
    /** Headers to send and sign (any case); host and the x-amz-* headers below are added by sign(). */
    Params headers;
    /** Hex SHA-256 of the body, or kUnsignedPayload. */
    std::string payloadHash = std::string(kEmptySha256);
    std::string region = "us-east-1";
    std::string service = "s3";
    int64_t now = 0;  // Unix seconds
};

struct Signed {
    /** Every header to send, including Host, X-Amz-Date, X-Amz-Content-Sha256 (S3), X-Amz-Security-Token and
     *  Authorization. */
    Params headers;
    /** The encoded query string ("a=1&b=2", no "?"), sorted as signed. */
    std::string query;
    // For tests and the event log.
    std::string canonicalRequest;
    std::string stringToSign;
    std::string signature;
};

/** Signs with an Authorization header. Anonymous credentials: the headers and query, unsigned. */
Signed sign(const SignInput& in, const AwsCredentials& creds);

/**
 * A presigned URL's query (X-Amz-Algorithm ... X-Amz-Signature), valid for `expiresSeconds` (1 .. 604800) from
 * in.now. Signs the host header and in.headers (the app passes none: a link must work from a browser); the payload
 * is UNSIGNED-PAYLOAD for S3 and in.payloadHash for other services.
 */
Signed presign(const SignInput& in, const AwsCredentials& creds, int64_t expiresSeconds);

/** The canonical query string of SigV4 (sorted by encoded name, then value). */
std::string canonicalQuery(const Params& query);

}  // namespace st::s3
