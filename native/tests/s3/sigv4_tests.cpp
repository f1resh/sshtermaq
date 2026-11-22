// AWS Signature Version 4: the worked examples of the S3 documentation ("Signature Calculations for the
// Authorization Header: Transferring Payload in a Single Chunk" and "Authenticating Requests: Using Query
// Parameters"), with its example keys and the time 2013-05-24T00:00:00Z.
#include "core/Crypto.h"
#include "core/Strings.h"
#include "s3/SigV4.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace st;
using namespace st::s3;

namespace {

constexpr int64_t kExampleTime = 1369353600;  // 2013-05-24T00:00:00Z

AwsCredentials exampleKeys() {
    AwsCredentials c;
    c.accessKeyId = "AKIAIOSFODNN7EXAMPLE";
    c.secretAccessKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    return c;
}

SignInput exampleRequest(std::string method, std::string path) {
    SignInput in;
    in.method = std::move(method);
    in.host = "examplebucket.s3.amazonaws.com";
    in.encodedPath = std::move(path);
    in.region = "us-east-1";
    in.now = kExampleTime;
    return in;
}

std::string header(const Signed& s, std::string_view name) {
    for (const auto& [n, v] : s.headers)
        if (iequals(n, name)) return v;
    return {};
}

}  // namespace

TEST_CASE("crypto: HMAC-SHA-256 and HMAC-SHA-1 (RFC 4231 / RFC 2202 test case 2)") {
    CHECK(hexEncode(crypto::hmacSha256("Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(hexEncode(crypto::hmacSha1("Jefe", "what do ya want for nothing?")) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

TEST_CASE("sigv4: dates and encoding") {
    CHECK(amzDate(0) == "19700101T000000Z");
    CHECK(amzDate(kExampleTime) == "20130524T000000Z");
    CHECK(amzDate(951782400) == "20000229T000000Z");
    CHECK(amzDate(4102444799) == "20991231T235959Z");
    CHECK(uriEncode("a b/c~d_e.f-g+h*") == "a%20b%2Fc~d_e.f-g%2Bh%2A");
    CHECK(uriEncode("dir/file name.txt", false) == "dir/file%20name.txt");
    CHECK(uriEncode("\xC3\xA9") == "%C3%A9");
    CHECK(payloadHash("") == kEmptySha256);
    CHECK(canonicalQuery({{"prefix", "J"}, {"max-keys", "2"}}) == "max-keys=2&prefix=J");
    CHECK(canonicalQuery({{"lifecycle", ""}}) == "lifecycle=");
}

TEST_CASE("sigv4: GET object (the documentation's example)") {
    SignInput in = exampleRequest("GET", "/test.txt");
    in.headers = {{"Range", "bytes=0-9"}};
    const Signed s = sign(in, exampleKeys());
    CHECK(s.canonicalRequest ==
          "GET\n/test.txt\n\nhost:examplebucket.s3.amazonaws.com\nrange:bytes=0-9\n"
          "x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
          "x-amz-date:20130524T000000Z\n\nhost;range;x-amz-content-sha256;x-amz-date\n"
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(s.stringToSign ==
          "AWS4-HMAC-SHA256\n20130524T000000Z\n20130524/us-east-1/s3/aws4_request\n"
          "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");
    CHECK(s.signature == "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    CHECK(header(s, "Authorization") ==
          "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
          "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
          "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    CHECK(header(s, "Host") == "examplebucket.s3.amazonaws.com");
}

TEST_CASE("sigv4: PUT object with a signed payload") {
    SignInput in = exampleRequest("PUT", "/" + uriEncode("test$file.text"));
    in.headers = {{"Date", "Fri, 24 May 2013 00:00:00 GMT"}, {"x-amz-storage-class", "REDUCED_REDUNDANCY"}};
    in.payloadHash = payloadHash("Welcome to Amazon S3.");
    CHECK(in.payloadHash == "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");
    const Signed s = sign(in, exampleKeys());
    CHECK(s.signature == "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
}

TEST_CASE("sigv4: GET bucket lifecycle and list objects (query strings)") {
    SignInput lc = exampleRequest("GET", "/");
    lc.query = {{"lifecycle", ""}};
    CHECK(sign(lc, exampleKeys()).signature == "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");

    SignInput list = exampleRequest("GET", "/");
    list.query = {{"prefix", "J"}, {"max-keys", "2"}};
    const Signed s = sign(list, exampleKeys());
    CHECK(s.query == "max-keys=2&prefix=J");
    CHECK(s.signature == "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");
}

TEST_CASE("sigv4: presigned URL") {
    const Signed s = presign(exampleRequest("GET", "/test.txt"), exampleKeys(), 86400);
    CHECK(s.signature == "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
    CHECK(s.query ==
          "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
          "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host"
          "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
    // The expiry is clamped to S3's limit of 7 days.
    CHECK(presign(exampleRequest("GET", "/test.txt"), exampleKeys(), 10'000'000).query.find("X-Amz-Expires=604800") !=
          std::string::npos);
}

TEST_CASE("sigv4: session tokens, anonymous requests, other services") {
    AwsCredentials t = exampleKeys();
    t.sessionToken = "token/with+chars";
    const Signed s = sign(exampleRequest("GET", "/test.txt"), t);
    CHECK(header(s, "X-Amz-Security-Token") == "token/with+chars");
    CHECK(header(s, "Authorization").find("x-amz-security-token") != std::string::npos);
    CHECK(presign(exampleRequest("GET", "/test.txt"), t, 60).query.find("X-Amz-Security-Token=token%2Fwith%2Bchars") !=
          std::string::npos);

    AwsCredentials anon;
    anon.anonymous = true;
    const Signed a = sign(exampleRequest("GET", "/test.txt"), anon);
    CHECK(header(a, "Authorization").empty());
    CHECK(header(a, "X-Amz-Date").empty());
    CHECK(header(a, "Host") == "examplebucket.s3.amazonaws.com");

    SignInput sts = exampleRequest("POST", "/");
    sts.host = "sts.amazonaws.com";
    sts.service = "sts";
    sts.headers = {{"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}};
    sts.payloadHash = payloadHash("Action=AssumeRole&Version=2011-06-15");
    const Signed st = sign(sts, exampleKeys());
    CHECK(header(st, "X-Amz-Content-Sha256").empty());  // S3 only
    CHECK(header(st, "Authorization").find("/us-east-1/sts/aws4_request") != std::string::npos);
}
