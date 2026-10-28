#include <gtest/gtest.h>

#include "media_storage.h"
#include "integrity.h"
#include "crypto.h"
#include "encoder.h"
#include "decoder.h"
#include "safe_output.h"
#include "video_encoder.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
#include <tuple>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;

void put(const fs::path &path, const std::string &contents) {
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    ASSERT_TRUE(out);
}

std::string get(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string digest(const fs::path &path) {
    const auto bytes = get(path);
    return sha256(std::as_bytes(std::span(bytes))).hexValue();
}

class RecoveryOutputSafety : public testing::Test {
protected:
    fs::path root;
    fs::path input, video, output;
    const std::string plain = std::string(4096, 'R') + "synthetic recovery payload";
    const std::string sentinel = "existing-sentinel\n";
    const std::string password = "synthetic-correct-password";
    bool owns_root = false;

    void SetUp() override {
        root = fs::temp_directory_path() /
            ("reliquary-recovery-" + std::to_string(std::random_device{}()));
        owns_root = fs::create_directory(root);
        ASSERT_TRUE(owns_root);
        input = root / "source.bin";
        video = root / "archive.mkv";
        output = root / "recovered.bin";
        put(input, plain);
    }

    void TearDown() override {
        if (!owns_root) return;
        std::error_code ec;
        const auto resolved = fs::weakly_canonical(root, ec);
        ASSERT_FALSE(ec);
        const auto temp = fs::weakly_canonical(fs::temp_directory_path());
        ASSERT_EQ(resolved.parent_path(), temp);
        ASSERT_TRUE(resolved.filename().string().starts_with("reliquary-recovery-"));
        fs::remove_all(resolved, ec);
        EXPECT_FALSE(ec);
    }

    void encode(bool encrypted = false,
                ms_encoding_mode_t mode = MS_ENCODING_MODE_RESILIENT) {
        const auto in = input.string(), out = video.string();
        ms_encode_options_t options{};
        options.input_path = in.c_str();
        options.output_path = out.c_str();
        options.encoding_mode = mode;
        options.encrypt = encrypted;
        options.password = password.c_str();
        options.password_len = password.size();
        ASSERT_EQ(ms_encode(&options, nullptr), MS_OK);
    }

    ms_status_t decode(const fs::path &target, const std::string &pw = {},
                       ms_progress_fn progress = nullptr, void *user = nullptr) {
        const auto in = video.string(), out = target.string();
        ms_decode_options_t options{};
        options.input_path = in.c_str();
        options.output_path = out.c_str();
        options.password = pw.c_str();
        options.password_len = pw.size();
        options.progress = progress;
        options.progress_user = user;
        return ms_decode(&options, nullptr);
    }

    void no_partials() {
        for (const auto &entry : fs::directory_iterator(root)) {
            EXPECT_EQ(entry.path().filename().string().find(".vidstorex-part-"),
                      std::string::npos) << entry.path();
        }
    }
};

TEST_F(RecoveryOutputSafety, WrongPasswordPreservesExistingTarget) {
    encode(true);
    put(output, sentinel);
    const auto before = digest(output), source_before = digest(video);
    EXPECT_EQ(decode(output, "wrong-password"), MS_ERR_DECODE_FAILED);
    EXPECT_EQ(digest(output), before);
    EXPECT_EQ(digest(video), source_before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, SamePathRejectedBeforeProgressAndPreservesVideo) {
    encode();
    const auto before = digest(video);
    int calls = 0;
    EXPECT_EQ(decode(video, {}, [](uint64_t, uint64_t, void *p) {
        ++*static_cast<int *>(p);
        return 0;
    }, &calls), MS_ERR_INVALID_ARGS);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(digest(video), before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, WrongPasswordDoesNotCreateNewTarget) {
    encode(true);
    const auto before = digest(video);
    EXPECT_EQ(decode(output, "wrong-password"), MS_ERR_DECODE_FAILED);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(digest(video), before);
    no_partials();
}

class RecoverySuccess : public RecoveryOutputSafety,
                        public testing::WithParamInterface<int> {};

TEST_P(RecoverySuccess, ValidatedReplacementPreservesSource) {
    const ms_encoding_mode_t modes[] = {MS_ENCODING_MODE_RESILIENT,
        MS_ENCODING_MODE_HIGH_CAPACITY, MS_ENCODING_MODE_FAST_LOCAL};
    encode(GetParam() >= 3, modes[GetParam() % 3]);
    put(output, sentinel);
    const auto before = digest(video);
    struct Observer { fs::path target; std::string hash; int calls = 0; };
    Observer observer{output, digest(output)};
    EXPECT_EQ(decode(output, password, [](uint64_t, uint64_t, void *p) {
        auto &o = *static_cast<Observer *>(p);
        EXPECT_EQ(digest(o.target), o.hash);
        ++o.calls;
        return 0;
    }, &observer), MS_OK);
    EXPECT_GT(observer.calls, 0);
    EXPECT_EQ(get(output), plain);
    EXPECT_EQ(digest(output), digest(input));
    EXPECT_EQ(digest(video), before);
    no_partials();
}

TEST_P(RecoverySuccess, NewOutputHasCorrectContent) {
    const ms_encoding_mode_t modes[] = {MS_ENCODING_MODE_RESILIENT,
        MS_ENCODING_MODE_HIGH_CAPACITY, MS_ENCODING_MODE_FAST_LOCAL};
    encode(GetParam() >= 3, modes[GetParam() % 3]);
    const auto before = digest(video);
    ASSERT_EQ(decode(output, password), MS_OK);
    EXPECT_EQ(get(output), plain);
    EXPECT_EQ(digest(output), digest(input));
    EXPECT_EQ(digest(video), before);
    no_partials();
}

TEST_P(RecoverySuccess, SameSourceRejectedInEveryProfile) {
    const ms_encoding_mode_t modes[] = {MS_ENCODING_MODE_RESILIENT,
        MS_ENCODING_MODE_HIGH_CAPACITY, MS_ENCODING_MODE_FAST_LOCAL};
    encode(GetParam() >= 3, modes[GetParam() % 3]);
    const auto before = digest(video);
    EXPECT_EQ(decode(video, password), MS_ERR_INVALID_ARGS);
    EXPECT_EQ(digest(video), before);
    no_partials();
}
INSTANTIATE_TEST_SUITE_P(Profiles, RecoverySuccess, testing::Range(0, 6));

class RecoveryAlias : public RecoveryOutputSafety,
                      public testing::WithParamInterface<std::string> {};

TEST_P(RecoveryAlias, RejectsPhysicalSourceBeforeProgress) {
    encode();
    auto target = video;
    const auto kind = GetParam();
    if (kind == "relative") target = fs::relative(video, fs::current_path());
    if (kind == "dotdot") {
        fs::create_directory(root / "child");
        target = root / "child" / ".." / "." / "archive.mkv";
    }
    if (kind == "hardlink") {
        target = root / "hardlink.mkv";
        fs::create_hard_link(video, target);
    }
    if (kind == "symlink" || kind == "input_symlink") {
        target = root / "symlink.mkv";
        std::error_code ec;
        fs::create_symlink(video, target, ec);
        if (ec) GTEST_SKIP() << "Symlink unavailable: " << ec.message();
        if (kind == "input_symlink") std::swap(video, target);
    }
#ifdef _WIN32
    if (kind == "case") {
        auto name = video.string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        target = name;
    }
    if (kind == "trailing_dot") target = video.string() + ".";
    if (kind == "short_path") {
        wchar_t name[32768]{};
        ASSERT_NE(GetShortPathNameW(video.c_str(), name, 32768), 0u);
        target = name;
    }
#endif
    ASSERT_TRUE(fs::equivalent(video, target));
    const auto before = digest(video);
    int calls = 0;
    EXPECT_EQ(decode(target, {}, [](uint64_t, uint64_t, void *p) {
        ++*static_cast<int *>(p);
        return 0;
    }, &calls), MS_ERR_INVALID_ARGS);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(digest(video), before);
    EXPECT_EQ(digest(target), before);
    no_partials();
}
const std::vector<std::string> alias_cases = {
    "relative", "dotdot", "hardlink", "symlink", "input_symlink"
#ifdef _WIN32
    , "case", "trailing_dot", "short_path"
#endif
};
INSTANTIATE_TEST_SUITE_P(Paths, RecoveryAlias, testing::ValuesIn(alias_cases));

class RecoveryIoFailure : public RecoveryOutputSafety,
                          public testing::WithParamInterface<std::tuple<std::string, bool>> {};

class InjectFailure {
public:
    explicit InjectFailure(const std::string &stage) {
        const char *previous = std::getenv("VIDSTOREX_TEST_RECOVERY_IO_FAILURE");
        previous_ = previous ? previous : "";
        set(stage);
    }
    ~InjectFailure() { set(previous_); }
private:
    std::string previous_;
    static void set(const std::string &value) {
#ifdef _WIN32
        _putenv_s("VIDSTOREX_TEST_RECOVERY_IO_FAILURE", value.c_str());
#else
        if (value.empty()) unsetenv("VIDSTOREX_TEST_RECOVERY_IO_FAILURE");
        else setenv("VIDSTOREX_TEST_RECOVERY_IO_FAILURE", value.c_str(), 1);
#endif
    }
};

TEST_P(RecoveryIoFailure, PreservesTargetAndCleansPartial) {
    encode();
    const auto &[stage, existing] = GetParam();
    std::string target_before;
    if (existing) { put(output, sentinel); target_before = digest(output); }
    const auto before = digest(video);
    {
        const InjectFailure failure(stage);
        EXPECT_EQ(decode(output), MS_ERR_DECODE_FAILED);
    }
    if (existing) EXPECT_EQ(digest(output), target_before);
    else EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(digest(video), before);
    no_partials();
}
INSTANTIATE_TEST_SUITE_P(Stages, RecoveryIoFailure, testing::Combine(
    testing::Values("create", "write", "flush", "close", "publish"), testing::Bool()));

class RecoveryCancellation : public RecoveryOutputSafety,
                             public testing::WithParamInterface<std::tuple<bool, bool>> {};
TEST_P(RecoveryCancellation, PreservesTargetAndSource) {
    encode(true);
    const auto [late, existing] = GetParam();
    std::string target_before;
    if (existing) { put(output, sentinel); target_before = digest(output); }
    const auto source_before = digest(video);
    struct Cancel { fs::path root; bool late; bool cancelled = false; };
    Cancel state{root, late};
    EXPECT_EQ(decode(output, password, [](uint64_t, uint64_t, void *p) {
        auto &s = *static_cast<Cancel *>(p);
        bool partial_exists = false;
        for (const auto &entry : fs::directory_iterator(s.root))
            partial_exists |= entry.path().string().find(".vidstorex-part-") != std::string::npos;
        s.cancelled = !s.late || partial_exists;
        return s.cancelled ? 1 : 0;
    }, &state), MS_ERR_DECODE_FAILED);
    EXPECT_TRUE(state.cancelled);
    if (existing) EXPECT_EQ(digest(output), target_before);
    else EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(digest(video), source_before);
    no_partials();
}
INSTANTIATE_TEST_SUITE_P(Timing, RecoveryCancellation,
                       testing::Combine(testing::Bool(), testing::Bool()));

class RecoveryCorruption : public RecoveryOutputSafety,
                          public testing::WithParamInterface<std::tuple<std::string, bool>> {};
TEST_P(RecoveryCorruption, FailurePreservesTargetAndSource) {
    const auto &[kind, existing] = GetParam();
    const Encoder::FileId id{};
    const Encoder encoder(id);
    auto data = std::as_bytes(std::span(plain));
    const bool encrypted = kind == "encrypted_crc" || kind == "authentication";
    std::vector<std::byte> stored(data.begin(), data.end());
    if (encrypted) {
        auto key = derive_key(std::as_bytes(std::span(password)), id);
        stored = encrypt_chunk(data, key, id, 0);
        secure_zero(key);
        if (kind == "authentication") stored.back() ^= std::byte{1};
    }
    const bool missing = kind == "missing_chunk";
    auto [packets, manifest] = encoder.encode_chunk(missing ? 1 : 0, stored, true, encrypted);
    if (kind == "plain_crc" || kind == "encrypted_crc")
        for (auto &packet : packets) packet.bytes[HEADER_SIZE] ^= std::byte{1};
    {
        VideoEncoder writer(video.string());
        writer.encode_packets(packets);
        writer.finalize();
    }
    const auto source_before = digest(video);
    std::string target_before;
    if (existing) { put(output, sentinel); target_before = digest(output); }
    const auto result = decode(output, password);
    EXPECT_NE(result, MS_OK);
    if (missing) EXPECT_EQ(result, MS_ERR_INCOMPLETE);
    if (kind == "authentication") EXPECT_EQ(result, MS_ERR_DECODE_FAILED);
    if (existing) EXPECT_EQ(digest(output), target_before);
    else EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(digest(video), source_before);
    no_partials();
}
INSTANTIATE_TEST_SUITE_P(Data, RecoveryCorruption, testing::Combine(
    testing::Values("plain_crc", "encrypted_crc", "authentication", "missing_chunk"),
    testing::Bool()));

TEST_F(RecoveryOutputSafety, NativeTemporaryCreationFailurePreservesFiles) {
    encode();
    put(output, sentinel);
    const auto before = digest(output), source_before = digest(video);
    // A file cannot be used as the parent directory of a partial file.
    EXPECT_NE(decode(output / "uncreatable.bin"), MS_OK);
    EXPECT_EQ(digest(output), before);
    EXPECT_EQ(digest(video), source_before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, ReplacementDoesNotTruncateOtherHardlinks) {
    encode();
    put(output, sentinel);
    auto other = root / "sentinel-hardlink.bin";
    fs::create_hard_link(output, other);
    const auto target_before = digest(other), source_before = digest(video);
    ASSERT_EQ(decode(output), MS_OK);
    EXPECT_EQ(get(output), plain);
    EXPECT_EQ(digest(other), target_before);
    EXPECT_EQ(digest(video), source_before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, SourceAliasIntroducedDuringRecoveryIsRejected) {
    encode();
    const auto before = digest(video);
    struct Alias { fs::path source, target; bool created = false; } state{video, output};
    EXPECT_EQ(decode(output, {}, [](uint64_t, uint64_t, void *p) {
        auto &s = *static_cast<Alias *>(p);
        if (!s.created) {
            fs::create_hard_link(s.source, s.target);
            s.created = true;
        }
        return 0;
    }, &state), MS_ERR_DECODE_FAILED);
    EXPECT_TRUE(state.created);
    EXPECT_EQ(digest(video), before);
    EXPECT_EQ(digest(output), before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, ExclusiveStreamCannotPublishBeforeClose) {
    put(output, sentinel);
    const auto before = digest(output);
    {
        SafeOutputFile safe(output, SafeOutputFile::Mode::ExclusiveWrite);
        EXPECT_TRUE(fs::exists(safe.partial_path()));
        safe.write(std::as_bytes(std::span(plain)));
        EXPECT_THROW(safe.commit(), std::logic_error);
        EXPECT_EQ(digest(output), before);
    }
    EXPECT_EQ(digest(output), before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, MalformedEncryptedChunkPreservesExistingTarget) {
    put(output, sentinel);
    const auto before = digest(output);
    Decoder decoder;
    const Encoder encoder(Encoder::FileId{});
    // Valid packet checksum around an invalid encrypted-chunk header.
    const std::vector<std::byte> invalid(1, std::byte{0});
    auto [packets, manifest] = encoder.encode_chunk(0, invalid, true, true);
    for (const auto &packet : packets) (void)decoder.process_packet(packet.bytes);
    ASSERT_EQ(decoder.chunks_completed(), 1u);
    std::array<std::byte, 32> key{};
    decoder.set_decrypt_key(key);
    EXPECT_FALSE(decoder.write_assembled_file(output.string(), 1));
    decoder.clear_decrypt_key();
    EXPECT_EQ(digest(output), before);
    no_partials();
}

TEST_F(RecoveryOutputSafety, DistinctExclusiveTemporaryFilesAreCleaned) {
    put(output, sentinel);
    const auto before = digest(output);
    {
        SafeOutputFile first(output, SafeOutputFile::Mode::ExclusiveWrite);
        SafeOutputFile second(output, SafeOutputFile::Mode::ExclusiveWrite);
        EXPECT_NE(first.partial_path(), second.partial_path());
        first.write(std::as_bytes(std::span(plain)));
        second.write(std::as_bytes(std::span(sentinel)));
        first.finish_writing();
        second.finish_writing();
        EXPECT_EQ(get(first.partial_path()), plain);
        EXPECT_EQ(get(second.partial_path()), sentinel);
    }
    EXPECT_EQ(digest(output), before);
    no_partials();
}

#ifdef _WIN32
TEST_F(RecoveryOutputSafety, NativePublishSharingViolationPreservesExistingTarget) {
    encode();
    put(output, sentinel);
    const auto before = digest(output), source_before = digest(video);
    HANDLE locked = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    EXPECT_EQ(decode(output), MS_ERR_DECODE_FAILED);
    EXPECT_TRUE(CloseHandle(locked));
    EXPECT_EQ(digest(output), before);
    EXPECT_EQ(digest(video), source_before);
    no_partials();
}
#endif
} // namespace
