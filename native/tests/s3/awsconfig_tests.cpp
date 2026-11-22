#include "platform/sys/Fs.h"
#include "s3/AwsConfig.h"

#include "TestServers.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;
using namespace st::s3;

TEST_CASE("aws config: sections, comments, nested settings, warnings") {
    AwsConfigFiles f;
    parseAwsIni("# comment\r\n"
                "[default]\r\n"
                "region = eu-west-1\r\n"
                "output=json\r\n"
                "s3 =\r\n"
                "  max_concurrent_requests = 20\r\n"
                "  addressing_style = path\r\n"
                "endpoint_url = http://localhost:9000\r\n"
                "\r\n"
                "[profile dev]\n"
                "; another comment\n"
                "role_arn = arn:aws:iam::123456789012:role/dev\n"
                "source_profile = default\n"
                "mfa_serial = arn:aws:iam::123456789012:mfa/me\n"
                "garbage line\n"
                "[sso-session corp]\n"
                "sso_start_url = https://corp.awsapps.com/start\n"
                "sso_region = us-east-1\n"
                "[services local]\n"
                "s3 =\n"
                "  endpoint_url = http://ignored\n"
                "[plugins]\n"
                "x = y\n",
                true, f);
    CHECK(f.get("default", "region") == "eu-west-1");
    CHECK(f.get("default", "output") == "json");
    CHECK(f.get("default", "s3.max_concurrent_requests") == "20");
    CHECK(f.get("default", "s3.addressing_style") == "path");
    CHECK(f.get("default", "endpoint_url") == "http://localhost:9000");
    CHECK(f.get("dev", "role_arn") == "arn:aws:iam::123456789012:role/dev");
    CHECK(f.get("dev", "source_profile") == "default");
    CHECK(f.ssoSessions["corp"]["sso_start_url"] == "https://corp.awsapps.com/start");
    CHECK(f.profiles.size() == 2);
    CHECK(f.warnings.size() == 1);
    CHECK(!f.get("dev", "missing"));
    CHECK(!f.get("nobody", "region"));
}

TEST_CASE("aws config: the credentials file wins over config, key by key") {
    test::TempDir dir;
    const auto config = dir.path() / "config";
    const auto creds = dir.path() / "credentials";
    sys::writeFileAtomic(config,
                         "[default]\nregion = us-west-2\naws_access_key_id = FROMCONFIG\n"
                         "[profile only-config]\nregion = ap-south-1\n");
    sys::writeFileAtomic(creds,
                         "[default]\naws_access_key_id = AKIDEXAMPLE\naws_secret_access_key = secret\n"
                         "[only-creds]\naws_access_key_id = X\naws_secret_access_key = Y\n"
                         "[profile not-a-prefix-here]\naws_access_key_id = Z\n");
    const AwsConfigFiles f = loadAwsConfig(config, creds);
    CHECK(f.get("default", "aws_access_key_id") == "AKIDEXAMPLE");
    CHECK(f.get("default", "region") == "us-west-2");
    CHECK(f.get("only-config", "region") == "ap-south-1");
    CHECK(f.get("only-creds", "aws_secret_access_key") == "Y");
    // In the credentials file "[profile x]" is a profile literally named "profile x", as in the AWS CLI.
    CHECK(f.get("profile not-a-prefix-here", "aws_access_key_id") == "Z");
    CHECK(f.profileNames() ==
          std::vector<std::string>{"default", "only-config", "only-creds", "profile not-a-prefix-here"});

    const AwsConfigFiles none = loadAwsConfig(dir.path() / "nope", dir.path() / "nope2");
    CHECK(none.profiles.empty());
}
