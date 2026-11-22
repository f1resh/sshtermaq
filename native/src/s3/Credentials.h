#pragma once
// Where S3 credentials come from: keys given directly, the environment (AWS_ACCESS_KEY_ID, ...), a profile of the
// ~/.aws files (static keys, credential_process, assume role, IAM Identity Center / SSO), or none (anonymous).
// Temporary credentials are refreshed shortly before they expire, under a lock, so parallel transfers keep going.
#include "s3/AwsConfig.h"
#include "s3/SigV4.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace st::sys {
class HttpSession;
}

namespace st::s3 {

/** Sign-in failed; the message says what to do ("run `aws sso login --profile dev`", ...). */
struct CredentialsError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class CredentialProvider {
public:
    virtual ~CredentialProvider() = default;
    /** Current credentials. `forceRefresh`: the service said they have expired. Thread-safe; throws CredentialsError. */
    virtual AwsCredentials get(bool forceRefresh = false) = 0;
    /** For the event log: "access key AKIA…MPLE", "profile dev: role arn:… via profile default", ... */
    virtual std::string describe() const = 0;
};

/** Keys that never change (session keys, environment, static profile keys), or anonymous. */
std::shared_ptr<CredentialProvider> fixedCredentials(AwsCredentials creds, std::string description);

/**
 * Credentials fetched by `fetch` and cached until 5 minutes before they expire (expiresAt), then fetched again.
 * fetch throws CredentialsError on failure.
 */
std::shared_ptr<CredentialProvider> refreshingCredentials(std::function<AwsCredentials()> fetch, std::string description,
                                                          std::function<int64_t()> now = {});

/** AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN; nullopt when the key ID or secret is missing. */
std::optional<AwsCredentials> credentialsFromEnvironment();

/** Region and endpoint from the environment (AWS_REGION, else AWS_DEFAULT_REGION; AWS_ENDPOINT_URL_S3, else
 *  AWS_ENDPOINT_URL); "" when unset. */
std::string regionFromEnvironment();
std::string endpointFromEnvironment();

/** What a profile needs from the outside world. */
struct ProfileContext {
    /** HTTP for STS and the SSO portal. */
    std::shared_ptr<sys::HttpSession> http;
    /** Asks for an MFA code (mfa_serial); nullopt = cancelled. */
    std::function<std::optional<std::string>(const std::string& mfaSerial, const std::string& profile)> askMfaCode;
    /** Runs a credential_process command and returns its standard output; throws CredentialsError on failure. */
    std::function<std::string(const std::string& command)> runProcess;
    /** Unix seconds (tests set a fixed time). */
    std::function<int64_t()> now;
    /** Overrides for tests: the STS endpoint ("https://sts.amazonaws.com") and the SSO portal endpoint
     *  ("https://portal.sso.<region>.amazonaws.com", %r = region). */
    std::string stsEndpoint;
    std::string ssoPortalEndpoint;
    /** The SSO token cache folder (default ~/.aws/sso/cache). */
    std::string ssoCacheDir;
    /** Event log lines. */
    std::function<void(const std::string&)> log;
};

/** The provider for a profile; throws CredentialsError when it does not exist or uses something not supported. */
std::shared_ptr<CredentialProvider> profileCredentials(const AwsConfigFiles& files, const std::string& profile,
                                                        const ProfileContext& ctx);

/** Parses the JSON a credential_process prints (Version 1). */
AwsCredentials parseCredentialProcessOutput(const std::string& json);

/** Runs `command` through the OS shell and returns what it wrote to stdout (stderr lines go to `log`). */
std::string runCredentialProcess(const std::string& command, const std::function<void(const std::string&)>& log,
                                 int timeoutMs = 60000);

}  // namespace st::s3
