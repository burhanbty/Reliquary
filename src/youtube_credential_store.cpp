#include "youtube_auth.h"

#include "safe_output.h"

#include <fstream>
#include <stdexcept>
#include <cctype>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>
#endif

namespace youtube_sync {
namespace {

class PlatformCredentialStore final : public CredentialStore {
public:
    explicit PlatformCredentialStore(std::filesystem::path directory)
        : directory_(std::move(directory)) {}

    void save(const std::string_view account, const std::string_view secret) override {
#ifdef _WIN32
        DATA_BLOB input{static_cast<DWORD>(secret.size()),
            reinterpret_cast<BYTE *>(const_cast<char *>(secret.data()))};
        DATA_BLOB output{};
        if (!CryptProtectData(&input, L"VidStoreX YouTube OAuth", nullptr,
                              nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &output))
            throw std::runtime_error("Windows could not protect OAuth credentials");
        write(account, output.pbData, output.cbData);
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
#else
        (void) account; (void) secret;
        throw std::runtime_error("secure credential storage is unavailable on this platform");
#endif
    }

    std::optional<std::string> load(const std::string_view account) const override {
        const auto bytes = read(account);
        if (bytes.empty()) return std::nullopt;
#ifdef _WIN32
        DATA_BLOB input{static_cast<DWORD>(bytes.size()),
            const_cast<BYTE *>(reinterpret_cast<const BYTE *>(bytes.data()))};
        DATA_BLOB output{};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &output))
            throw std::runtime_error("Windows could not unlock OAuth credentials");
        std::string value(reinterpret_cast<char *>(output.pbData), output.cbData);
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return value;
#else
        return std::nullopt;
#endif
    }

    void remove(const std::string_view account) override {
        std::error_code ignored;
        std::filesystem::remove(path(account), ignored);
    }

private:
    std::filesystem::path path(const std::string_view account) const {
        std::string safe;
        for (const unsigned char c : account)
            safe += std::isalnum(c) || c == '-' || c == '_' ? static_cast<char>(c) : '_';
        return directory_ / (safe + ".dpapi");
    }
    void write(const std::string_view account, const void *data, const std::size_t size) {
        std::filesystem::create_directories(directory_);
        SafeOutputFile safe(path(account));
        std::ofstream out(safe.partial_path(), std::ios::binary);
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        out.close();
        if (!out) throw std::runtime_error("could not store protected OAuth credentials");
        safe.commit();
    }
    std::vector<unsigned char> read(const std::string_view account) const {
        std::ifstream in(path(account), std::ios::binary);
        if (!in) return {};
        return {std::istreambuf_iterator<char>(in), {}};
    }
    std::filesystem::path directory_;
};

} // namespace

std::unique_ptr<CredentialStore> make_platform_credential_store(
    std::filesystem::path fallback_directory) {
    if (fallback_directory.empty()) {
#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
        if (const char *testRoot = std::getenv("VIDSTOREX_CREDENTIALS_ROOT");
            testRoot && *testRoot)
            fallback_directory = std::filesystem::u8path(testRoot);
#endif
#ifdef _WIN32
        if (fallback_directory.empty()) {
            if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
                fallback_directory = std::filesystem::u8path(local) /
                    "VidStoreX" / "Credentials";
        }
#endif
        if (fallback_directory.empty())
            fallback_directory = std::filesystem::temp_directory_path() /
                "VidStoreX" / "Credentials";
    }
    return std::make_unique<PlatformCredentialStore>(
        std::move(fallback_directory));
}

} // namespace youtube_sync
