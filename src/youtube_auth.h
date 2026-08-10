#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace youtube_sync {

inline constexpr std::string_view kYoutubeScope =
    "https://www.googleapis.com/auth/youtube";

struct OAuthClientConfig {
    std::string client_id;
    std::string client_secret;
    std::string authorization_endpoint =
        "https://accounts.google.com/o/oauth2/v2/auth";
    std::string token_endpoint = "https://oauth2.googleapis.com/token";
    std::string revoke_endpoint = "https://oauth2.googleapis.com/revoke";
    [[nodiscard]] bool configured() const noexcept { return !client_id.empty(); }
};

struct PkceAttempt {
    std::string verifier;
    std::string challenge;
    std::string state;
};

struct OAuthCallback {
    bool accepted = false;
    std::string code;
    std::string error;
};

struct TokenRecord {
    std::string access_token;
    std::string refresh_token;
    std::string token_type = "Bearer";
    int64_t expires_at_epoch_seconds = 0;
};

class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual void save(std::string_view account, std::string_view secret) = 0;
    [[nodiscard]] virtual std::optional<std::string> load(
        std::string_view account) const = 0;
    virtual void remove(std::string_view account) = 0;
};

[[nodiscard]] OAuthClientConfig read_oauth_client_config(
    const std::filesystem::path &path);
[[nodiscard]] PkceAttempt create_pkce_attempt();
[[nodiscard]] std::string s256_challenge(std::string_view verifier);
[[nodiscard]] OAuthCallback parse_oauth_callback(
    std::string_view request_target, std::string_view expected_state);
[[nodiscard]] std::string authorization_url(
    const OAuthClientConfig &config, const PkceAttempt &attempt,
    std::string_view redirect_uri);
[[nodiscard]] std::string serialize_token_record(const TokenRecord &record);
[[nodiscard]] TokenRecord parse_token_record(std::string_view json);
[[nodiscard]] std::string redact_sensitive(std::string_view text);
[[nodiscard]] std::unique_ptr<CredentialStore> make_platform_credential_store(
    std::filesystem::path fallback_directory = {});

} // namespace youtube_sync
