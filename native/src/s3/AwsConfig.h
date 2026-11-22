#pragma once
// The AWS CLI's shared files: ~/.aws/config and ~/.aws/credentials (or AWS_CONFIG_FILE /
// AWS_SHARED_CREDENTIALS_FILE). INI with "[default]", "[profile name]" and "[sso-session name]" in config, "[name]" in
// credentials; values from credentials win over config. Nested settings ("s3 =" followed by indented lines) are kept
// as "s3.key".
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::s3 {

using AwsSection = std::map<std::string, std::string>;

struct AwsConfigFiles {
    /** Profile name -> settings (config and credentials merged). */
    std::map<std::string, AwsSection> profiles;
    /** sso-session name -> settings. */
    std::map<std::string, AwsSection> ssoSessions;
    std::filesystem::path configPath;
    std::filesystem::path credentialsPath;
    /** Problems while reading ("line 7: no '='"), for the event log. */
    std::vector<std::string> warnings;

    /** A profile's setting; nullopt when the profile or the key is not there. */
    std::optional<std::string> get(const std::string& profile, const std::string& key) const;
    /** Sorted profile names (for the editor's picker). */
    std::vector<std::string> profileNames() const;
};

/** Parses the text of a config (`isConfig`) or credentials file into `into`. */
void parseAwsIni(std::string_view text, bool isConfig, AwsConfigFiles& into);

/** The file locations: AWS_CONFIG_FILE / AWS_SHARED_CREDENTIALS_FILE, else ~/.aws/config and ~/.aws/credentials. */
std::filesystem::path awsConfigPath();
std::filesystem::path awsCredentialsPath();

/** Reads both files (missing files are empty). */
AwsConfigFiles loadAwsConfig();
AwsConfigFiles loadAwsConfig(const std::filesystem::path& config, const std::filesystem::path& credentials);

/** The profile used when a session names none: AWS_PROFILE, else AWS_DEFAULT_PROFILE, else "default". */
std::string defaultAwsProfile();

}  // namespace st::s3
