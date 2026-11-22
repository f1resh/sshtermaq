// Sign-in methods: the environment, ~/.aws profiles (static keys, credential_process, assume role with
// source_profile chains, loops and MFA, credential_source = Environment, SSO with the token cache), refreshing.
#include "core/Crypto.h"
#include "core/Strings.h"
#include "platform/Platform.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Http.h"
#include "s3/Credentials.h"
#include "s3/Dates.h"

#include "TestHttpServer.h"
#include "TestServers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>

using namespace st;
using namespace st::s3;
using namespace st::test;

namespace {

AwsConfigFiles parse(const std::string& config, const std::string& credentials = {}) {
    AwsConfigFiles f;
    parseAwsIni(config, true, f);
    AwsConfigFiles c;
    parseAwsIni(credentials, false, c);
    for (auto& [n, v] : c.profiles)
        for (auto& [k, x] : v) f.profiles[n][k] = x;
    return f;
}

sys::HttpSessionOptions httpOptions() {
    sys::HttpSessionOptions o;
    o.proxy.mode = sys::HttpProxy::Mode::None;
    return o;
}

/** A fake STS: AssumeRole answers keys named after the role and the caller's key. */
struct FakeSts {
    std::vector<TestHttpRequest> calls;
    std::mutex m;
    int64_t expires = 4102444800;  // 2100
    TestHttpServer server{[this](const TestHttpRequest& r) {
        std::lock_guard lk(m);
        calls.push_back(r);
        TestHttpResponse out;
        const std::string body = r.body;
        auto field = [&](const std::string& name) {
            for (const std::string& kv : split(body, '&')) {
                const size_t eq = kv.find('=');
                if (percentDecode(kv.substr(0, eq)) == name) return percentDecode(kv.substr(eq + 1));
            }
            return std::string();
        };
        if (field("RoleArn").find("denied") != std::string::npos) {
            out.status = 403;
            out.body = "<ErrorResponse><Error><Code>AccessDenied</Code><Message>not allowed to assume</Message></Error></ErrorResponse>";
            return out;
        }
        const std::string auth = r.header("Authorization");
        const size_t c = auth.find("Credential=");
        const std::string caller = auth.substr(c + 11, auth.find('/', c) - c - 11);
        out.body = "<AssumeRoleResponse><AssumeRoleResult><Credentials><AccessKeyId>ASIA-" + field("RoleArn").substr(field("RoleArn").rfind('/') + 1) +
                   "-from-" + caller + "</AccessKeyId><SecretAccessKey>s</SecretAccessKey><SessionToken>t</SessionToken><Expiration>" +
                   formatIso8601(expires) + "</Expiration></Credentials></AssumeRoleResult></AssumeRoleResponse>";
        return out;
    }};
};

ProfileContext context(FakeSts* sts) {
    ProfileContext ctx;
    ctx.http = sys::openHttpSession(httpOptions());
    if (sts) ctx.stsEndpoint = sts->server.url();
    return ctx;
}

}  // namespace

TEST_CASE("credentials: the environment") {
    ScopedEnv a("AWS_ACCESS_KEY_ID", "AKIDENV");
    ScopedEnv b("AWS_SECRET_ACCESS_KEY", "secret");
    ScopedEnv c("AWS_SESSION_TOKEN", "tok");
    ScopedEnv d("AWS_REGION", "eu-north-1");
    ScopedEnv e("AWS_ENDPOINT_URL", "http://generic");
    ScopedEnv g("AWS_ENDPOINT_URL_S3", "http://s3-only");
    const auto creds = credentialsFromEnvironment();
    REQUIRE(creds);
    CHECK(creds->accessKeyId == "AKIDENV");
    CHECK(creds->sessionToken == "tok");
    CHECK(regionFromEnvironment() == "eu-north-1");
    CHECK(endpointFromEnvironment() == "http://s3-only");
    {
        ScopedEnv none("AWS_SECRET_ACCESS_KEY", "");
        CHECK_FALSE(credentialsFromEnvironment());
    }
}

TEST_CASE("credentials: static profiles, errors for what is missing or unsupported") {
    const AwsConfigFiles f = parse("[profile ec2]\nrole_arn = arn:aws:iam::1:role/r\ncredential_source = Ec2InstanceMetadata\n"
                                   "[profile nothing]\nregion = x\n"
                                   "[profile web]\nweb_identity_token_file = /t\n",
                                   "[default]\naws_access_key_id = AKIDDEFAULT\naws_secret_access_key = s\naws_session_token = st\n");
    const ProfileContext ctx = context(nullptr);
    const AwsCredentials c = profileCredentials(f, "default", ctx)->get();
    CHECK(c.accessKeyId == "AKIDDEFAULT");
    CHECK(c.sessionToken == "st");
    CHECK_THROWS_WITH(profileCredentials(f, "missing", ctx), Catch::Matchers::ContainsSubstring("is not in"));
    CHECK_THROWS_WITH(profileCredentials(f, "ec2", ctx), Catch::Matchers::ContainsSubstring("not supported"));
    CHECK_THROWS_WITH(profileCredentials(f, "nothing", ctx), Catch::Matchers::ContainsSubstring("has no credentials"));
    CHECK_THROWS_WITH(profileCredentials(f, "web", ctx), Catch::Matchers::ContainsSubstring("not supported"));
}

TEST_CASE("credentials: assume role through source_profile chains, loops, external id, MFA") {
    FakeSts sts;
    const AwsConfigFiles f = parse(
        "[profile base]\nregion = eu-west-1\n"
        "[profile dev]\nrole_arn = arn:aws:iam::111:role/dev\nsource_profile = base\nexternal_id = ext-1\nrole_session_name = me\n"
        "duration_seconds = 900\n"
        "[profile prod]\nrole_arn = arn:aws:iam::222:role/prod\nsource_profile = dev\nmfa_serial = arn:aws:iam::111:mfa/me\n"
        "[profile self]\nrole_arn = arn:aws:iam::333:role/self\nsource_profile = self\naws_access_key_id = AKIDSELF\naws_secret_access_key = s\n"
        "[profile loop1]\nrole_arn = arn:aws:iam::1:role/a\nsource_profile = loop2\n"
        "[profile loop2]\nrole_arn = arn:aws:iam::1:role/b\nsource_profile = loop1\n"
        "[profile denied]\nrole_arn = arn:aws:iam::1:role/denied\nsource_profile = base\n"
        "[profile fromenv]\nrole_arn = arn:aws:iam::1:role/envrole\ncredential_source = Environment\n",
        "[base]\naws_access_key_id = AKIDBASE\naws_secret_access_key = s\n");
    ProfileContext ctx = context(&sts);
    std::vector<std::string> asked;
    ctx.askMfaCode = [&](const std::string& serial, const std::string& profile) -> std::optional<std::string> {
        asked.push_back(serial + "@" + profile);
        return std::string(" 123456 ");
    };

    CHECK(profileCredentials(f, "dev", ctx)->get().accessKeyId == "ASIA-dev-from-AKIDBASE");
    REQUIRE(sts.calls.size() == 1);
    const std::string body = sts.calls[0].body;
    CHECK(body.find("Action=AssumeRole") != std::string::npos);
    CHECK(body.find("ExternalId=ext-1") != std::string::npos);
    CHECK(body.find("RoleSessionName=me") != std::string::npos);
    CHECK(body.find("DurationSeconds=900") != std::string::npos);
    CHECK(sts.calls[0].header("Authorization").find("/eu-west-1/sts/aws4_request") == std::string::npos);  // dev has no region

    // prod: dev's role credentials assume prod's role, with an MFA code.
    CHECK(profileCredentials(f, "prod", ctx)->get().accessKeyId == "ASIA-prod-from-ASIA-dev-from-AKIDBASE");
    CHECK(asked == std::vector<std::string>{"arn:aws:iam::111:mfa/me@prod"});
    CHECK(sts.calls.back().body.find("TokenCode=123456") != std::string::npos);
    CHECK(sts.calls.back().body.find("SerialNumber=arn%3Aaws%3Aiam%3A%3A111%3Amfa%2Fme") != std::string::npos);

    CHECK(profileCredentials(f, "self", ctx)->get().accessKeyId == "ASIA-self-from-AKIDSELF");
    CHECK_THROWS_WITH(profileCredentials(f, "loop1", ctx), Catch::Matchers::ContainsSubstring("refers back to itself"));
    CHECK_THROWS_WITH(profileCredentials(f, "denied", ctx)->get(), Catch::Matchers::ContainsSubstring("not allowed to assume"));

    ctx.askMfaCode = [](const std::string&, const std::string&) -> std::optional<std::string> { return std::nullopt; };
    CHECK_THROWS_WITH(profileCredentials(f, "prod", ctx)->get(), Catch::Matchers::ContainsSubstring("MFA code"));

    {
        ScopedEnv a("AWS_ACCESS_KEY_ID", "AKIDENV");
        ScopedEnv b("AWS_SECRET_ACCESS_KEY", "s");
        CHECK(profileCredentials(f, "fromenv", ctx)->get().accessKeyId == "ASIA-envrole-from-AKIDENV");
    }
}

TEST_CASE("credentials: temporary credentials are refreshed before they expire") {
    FakeSts sts;
    sts.expires = 1000 + 3600;
    const AwsConfigFiles f = parse("[profile dev]\nrole_arn = arn:aws:iam::1:role/dev\nsource_profile = base\n",
                                   "[base]\naws_access_key_id = AKIDBASE\naws_secret_access_key = s\n");
    ProfileContext ctx = context(&sts);
    std::atomic<int64_t> now{1000};
    ctx.now = [&] { return now.load(); };
    auto p = profileCredentials(f, "dev", ctx);
    p->get();
    p->get();
    CHECK(sts.calls.size() == 1);
    now = 1000 + 3600 - 299;  // within 5 minutes of the expiry
    p->get();
    CHECK(sts.calls.size() == 2);
    p->get(true);  // the service said they expired
    CHECK(sts.calls.size() == 3);
}

TEST_CASE("credentials: credential_process") {
    CHECK(parseCredentialProcessOutput(R"({"Version":1,"AccessKeyId":"AK","SecretAccessKey":"S","SessionToken":"T","Expiration":"2030-01-01T00:00:00Z"})")
              .expiresAt == 1893456000);
    CHECK(parseCredentialProcessOutput(R"({"Version":1,"AccessKeyId":"AK","SecretAccessKey":"S"})").expiresAt == 0);
    CHECK_THROWS_AS(parseCredentialProcessOutput("not json"), CredentialsError);
    CHECK_THROWS_AS(parseCredentialProcessOutput(R"({"Version":2,"AccessKeyId":"AK","SecretAccessKey":"S"})"), CredentialsError);
    CHECK_THROWS_AS(parseCredentialProcessOutput(R"({"Version":1,"AccessKeyId":"AK"})"), CredentialsError);

    // Through a real shell: the command prints a JSON file.
    TempDir dir;
    const auto json = dir.path() / "creds.json";
    sys::writeFileAtomic(json, R"({"Version": 1, "AccessKeyId": "AKIDPROC", "SecretAccessKey": "s"})");
    const bool windows = std::string(sys::osName()) == "win32";
    const std::string cmd = (windows ? "type \"" : "cat '") + sys::fromPath(json) + (windows ? "\"" : "'");
    const AwsConfigFiles f = parse("[profile proc]\ncredential_process = " + cmd + "\n");
    ProfileContext ctx = context(nullptr);
    CHECK(profileCredentials(f, "proc", ctx)->get().accessKeyId == "AKIDPROC");

    const std::string failing = windows ? "echo nope 1>&2" : "echo nope 1>&2";
    CHECK_THROWS_WITH(runCredentialProcess(failing, {}), Catch::Matchers::ContainsSubstring("printed nothing"));
    CHECK_THROWS_WITH(runCredentialProcess(windows ? "ping -n 30 127.0.0.1 >nul" : "sleep 30", {}, 500),
                      Catch::Matchers::ContainsSubstring("did not finish"));

    // A fake command for the provider: called again when its credentials are about to expire.
    int runs = 0;
    ctx.runProcess = [&](const std::string&) {
        ++runs;
        return std::string(R"({"Version":1,"AccessKeyId":"AK)") + std::to_string(runs) +
               R"(","SecretAccessKey":"S","Expiration":"2000-01-01T00:00:00Z"})";
    };
    auto p = profileCredentials(f, "proc", ctx);
    CHECK(p->get().accessKeyId == "AK1");
    CHECK(p->get().accessKeyId == "AK2");  // already expired: fetched again
}

TEST_CASE("credentials: IAM Identity Center (SSO) with the token cache") {
    std::vector<TestHttpRequest> calls;
    std::mutex m;
    TestHttpServer portal([&](const TestHttpRequest& r) {
        std::lock_guard lk(m);
        calls.push_back(r);
        TestHttpResponse out;
        if (r.header("x-amz-sso_bearer_token") != "good-token") {
            out.status = 401;
            return out;
        }
        out.body = R"({"roleCredentials":{"accessKeyId":"ASIASSO-)" + r.param("account_id") + "-" + r.param("role_name") +
                   R"(","secretAccessKey":"s","sessionToken":"t","expiration":4102444800000}})";
        return out;
    });
    TempDir cache;
    auto writeToken = [&](const std::string& key, const std::string& token, const std::string& expires) {
        sys::writeFileAtomic(cache.path() / (hexEncode(crypto::sha1(key)) + ".json"),
                             R"({"startUrl":"https://corp.awsapps.com/start","region":"us-east-1","accessToken":")" + token +
                                 R"(","expiresAt":")" + expires + R"("})");
    };
    const AwsConfigFiles f = parse("[profile modern]\nsso_session = corp\nsso_account_id = 123456789012\nsso_role_name = Admin\n"
                                   "[sso-session corp]\nsso_start_url = https://corp.awsapps.com/start\nsso_region = eu-central-1\n"
                                   "[profile legacy]\nsso_start_url = https://old.awsapps.com/start\nsso_region = us-east-1\n"
                                   "sso_account_id = 999\nsso_role_name = Read\n"
                                   "[profile broken]\nsso_session = nope\nsso_account_id = 1\nsso_role_name = R\n");
    ProfileContext ctx = context(nullptr);
    ctx.ssoPortalEndpoint = portal.url() + "/%r";
    ctx.ssoCacheDir = sys::fromPath(cache.path());

    CHECK_THROWS_WITH(profileCredentials(f, "modern", ctx)->get(),
                      Catch::Matchers::ContainsSubstring("aws sso login --profile modern"));
    writeToken("corp", "good-token", "2099-01-01T00:00:00Z");
    CHECK(profileCredentials(f, "modern", ctx)->get().accessKeyId == "ASIASSO-123456789012-Admin");
    REQUIRE(!calls.empty());
    CHECK(calls.back().path == "/eu-central-1/federation/credentials");

    writeToken("https://old.awsapps.com/start", "good-token", "2099-01-01T00:00:00Z");
    CHECK(profileCredentials(f, "legacy", ctx)->get().accessKeyId == "ASIASSO-999-Read");

    writeToken("corp", "good-token", "2001-01-01T00:00:00Z");
    CHECK_THROWS_WITH(profileCredentials(f, "modern", ctx)->get(), Catch::Matchers::ContainsSubstring("has expired"));
    writeToken("corp", "revoked", "2099-01-01T00:00:00Z");
    CHECK_THROWS_WITH(profileCredentials(f, "modern", ctx)->get(), Catch::Matchers::ContainsSubstring("no longer valid"));
    CHECK_THROWS_WITH(profileCredentials(f, "broken", ctx), Catch::Matchers::ContainsSubstring("sso_session \"nope\""));
}
