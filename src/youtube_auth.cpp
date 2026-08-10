#include "youtube_auth.h"

#include "safe_output.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace youtube_sync {
namespace {

std::string base64url(const unsigned char *data, const std::size_t size) {
    std::string out(sodium_base64_ENCODED_LEN(
        size, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(out.data(), out.size(), data, size,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

std::string url_decode(std::string_view value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+' ) out += ' ';
        else if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = [](const char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            const int high = hex(value[i + 1]), low = hex(value[i + 2]);
            if (high < 0 || low < 0) throw std::invalid_argument("invalid callback encoding");
            out += static_cast<char>((high << 4) | low);
            i += 2;
        } else out += value[i];
    }
    return out;
}

std::string url_encode(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << static_cast<char>(c);
        else out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return out.str();
}

std::string json_string(const std::string &json, const std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    auto pos = json.find(marker);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string result;
    bool escape = false;
    for (++pos; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escape) { result += c; escape = false; }
        else if (c == '\\') escape = true;
        else if (c == '"') return result;
        else result += c;
    }
    throw std::runtime_error("unterminated JSON string");
}

int64_t json_integer(const std::string &json, const std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    auto pos = json.find(marker);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return 0;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    std::size_t end = pos;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) ++end;
    return end == pos ? 0 : std::stoll(json.substr(pos, end - pos));
}

std::string escape(std::string_view value) {
    std::string out;
    for (const char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

} // namespace

OAuthClientConfig read_oauth_client_config(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    OAuthClientConfig config;
    // Google desktop client downloads wrap values in an "installed" object;
    // key lookup is deliberately tolerant of that wrapper.
    config.client_id = json_string(json, "client_id");
    config.client_secret = json_string(json, "client_secret");
    const auto auth = json_string(json, "auth_uri");
    const auto token = json_string(json, "token_uri");
    if (!auth.empty()) config.authorization_endpoint = auth;
    if (!token.empty()) config.token_endpoint = token;
    if (!config.client_id.empty() && config.client_id.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("invalid OAuth client ID");
    return config;
}

PkceAttempt create_pkce_attempt() {
    if (sodium_init() < 0) throw std::runtime_error("cryptographic initialization failed");
    std::array<unsigned char, 64> verifier_bytes{};
    std::array<unsigned char, 32> state_bytes{};
    randombytes_buf(verifier_bytes.data(), verifier_bytes.size());
    randombytes_buf(state_bytes.data(), state_bytes.size());
    PkceAttempt result;
    result.verifier = base64url(verifier_bytes.data(), verifier_bytes.size());
    result.challenge = s256_challenge(result.verifier);
    result.state = base64url(state_bytes.data(), state_bytes.size());
    return result;
}

std::string s256_challenge(const std::string_view verifier) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(),
        reinterpret_cast<const unsigned char *>(verifier.data()),
        static_cast<unsigned long long>(verifier.size()));
    return base64url(hash.data(), hash.size());
}

OAuthCallback parse_oauth_callback(const std::string_view request_target,
                                   const std::string_view expected_state) {
    OAuthCallback result;
    const auto query_at = request_target.find('?');
    if (query_at == std::string_view::npos) {
        result.error = "authorization callback is missing parameters";
        return result;
    }
    std::map<std::string, std::string> parameters;
    auto query = request_target.substr(query_at + 1);
    if (const auto hash = query.find('#'); hash != std::string_view::npos)
        query = query.substr(0, hash);
    for (std::size_t at = 0; at <= query.size();) {
        const auto amp = query.find('&', at);
        const auto pair = query.substr(at, amp == std::string_view::npos ? query.size() - at : amp - at);
        const auto equals = pair.find('=');
        if (equals != std::string_view::npos)
            parameters[url_decode(pair.substr(0, equals))] = url_decode(pair.substr(equals + 1));
        if (amp == std::string_view::npos) break;
        at = amp + 1;
    }
    const auto state = parameters.find("state");
    if (state == parameters.end() || state->second != expected_state) {
        result.error = "authorization state did not match";
        return result;
    }
    if (const auto error = parameters.find("error"); error != parameters.end()) {
        result.error = error->second;
        return result;
    }
    const auto code = parameters.find("code");
    if (code == parameters.end() || code->second.empty()) {
        result.error = "authorization callback did not contain a code";
        return result;
    }
    result.accepted = true;
    result.code = code->second;
    return result;
}

std::string authorization_url(const OAuthClientConfig &config,
                              const PkceAttempt &attempt,
                              const std::string_view redirect_uri) {
    if (!config.configured() || attempt.verifier.size() < 43 ||
        attempt.challenge.empty() || attempt.state.empty())
        throw std::invalid_argument("OAuth authorization is not configured");
    return config.authorization_endpoint + "?client_id=" + url_encode(config.client_id) +
        "&redirect_uri=" + url_encode(redirect_uri) +
        "&response_type=code&scope=" + url_encode(kYoutubeScope) +
        "&code_challenge=" + url_encode(attempt.challenge) +
        "&code_challenge_method=S256&state=" + url_encode(attempt.state) +
        "&access_type=offline&prompt=consent";
}

std::string serialize_token_record(const TokenRecord &record) {
    return "{\"access_token\":\"" + escape(record.access_token) +
        "\",\"refresh_token\":\"" + escape(record.refresh_token) +
        "\",\"token_type\":\"" + escape(record.token_type) +
        "\",\"expires_at\":" + std::to_string(record.expires_at_epoch_seconds) + "}";
}

TokenRecord parse_token_record(const std::string_view text) {
    const std::string json(text);
    TokenRecord record;
    record.access_token = json_string(json, "access_token");
    record.refresh_token = json_string(json, "refresh_token");
    record.token_type = json_string(json, "token_type");
    record.expires_at_epoch_seconds = json_integer(json, "expires_at");
    if (record.expires_at_epoch_seconds == 0) {
        const auto expiresIn = json_integer(json, "expires_in");
        if (expiresIn > 0)
            record.expires_at_epoch_seconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() +
                expiresIn;
    }
    if (record.access_token.empty() && record.refresh_token.empty())
        throw std::runtime_error("OAuth token response did not contain usable credentials");
    if (record.token_type.empty()) record.token_type = "Bearer";
    return record;
}

std::string redact_sensitive(const std::string_view value) {
    std::string text(value);
    for (const std::string marker : {"Authorization: Bearer ", "access_token=", "refresh_token=", "code_verifier="}) {
        std::size_t at = 0;
        while ((at = text.find(marker, at)) != std::string::npos) {
            const auto start = at + marker.size();
            auto end = text.find_first_of("&\r\n \"", start);
            if (end == std::string::npos) end = text.size();
            text.replace(start, end - start, "[REDACTED]");
            at = start + 10;
        }
    }
    return text;
}

} // namespace youtube_sync
