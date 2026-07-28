#include "fast_local_codec.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

#include <sodium.h>

#include "crypto.h"
#include "encoder.h"
#include "performance_profiler.h"
#include "safe_output.h"
#include "video_decoder.h"
#include "video_encoder.h"

namespace {
class StreamingSha256 {
public:
    StreamingSha256() {
        if (sodium_init() < 0 ||
            crypto_hash_sha256_init(&state_) != 0) {
            throw std::runtime_error("SHA-256 initialization failed");
        }
    }

    void update(const std::span<const std::byte> bytes) {
        if (!bytes.empty() &&
            crypto_hash_sha256_update(
                &state_,
                reinterpret_cast<const unsigned char *>(bytes.data()),
                static_cast<unsigned long long>(bytes.size())) != 0) {
            throw std::runtime_error("SHA-256 update failed");
        }
    }

    [[nodiscard]] Sha256Digest finish() {
        Sha256Digest digest;
        if (crypto_hash_sha256_final(
                &state_,
                reinterpret_cast<unsigned char *>(
                    digest.bytes.data())) != 0) {
            throw std::runtime_error("SHA-256 finalization failed");
        }
        return digest;
    }

private:
    crypto_hash_sha256_state state_{};
};

Sha256Digest hash_file(const std::filesystem::path &path,
                       PerformanceProfiler *profiler) {
    ScopedTimer timer(profiler, PerformanceStage::InputRead);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open input file");
    StreamingSha256 hash;
    std::vector<std::byte> buffer(1024 * 1024);
    while (input) {
        input.read(
            reinterpret_cast<char *>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span<const std::byte>(
                buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) throw std::runtime_error("input file read failed");
    return hash.finish();
}

bool has_mkv_extension(const std::filesystem::path &path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return extension == ".mkv";
}

[[noreturn]] void corrupt(const std::string &message) {
    throw FastLocalError(FastLocalErrorCode::Corrupt, message);
}

FastLocalStatistics make_statistics(
    const uint64_t input_bytes,
    const uint64_t output_bytes,
    const uint64_t frames,
    const uint64_t payload_bytes) {
    FastLocalStatistics statistics;
    statistics.input_bytes = input_bytes;
    statistics.output_bytes = output_bytes;
    statistics.total_frames = frames;
    statistics.header_bytes =
        FAST_LOCAL_FILE_HEADER_SIZE +
        frames * FAST_LOCAL_FRAME_HEADER_SIZE;
    statistics.payload_bytes = payload_bytes;
    const uint64_t raw_bytes =
        frames * static_cast<uint64_t>(FAST_LOCAL_FRAME_BYTES);
    statistics.padding_bytes =
        raw_bytes - statistics.header_bytes - payload_bytes;
    return statistics;
}
}

bool fast_local_has_magic(const std::filesystem::path &video_path) {
    try {
        VideoDecoder decoder(video_path.string());
        std::vector<std::byte> pixels;
        if (!decoder.decode_next_gray8_frame(pixels) ||
            pixels.size() < FAST_LOCAL_MAGIC.size()) {
            return false;
        }
        return std::equal(
            FAST_LOCAL_MAGIC.begin(), FAST_LOCAL_MAGIC.end(),
            pixels.begin());
    } catch (...) {
        return false;
    }
}

FastLocalStatistics encode_fast_local(
    const std::filesystem::path &input_path,
    const std::filesystem::path &output_path,
    const bool encrypted,
    const std::span<const std::byte> password,
    PerformanceProfiler *profiler,
    const FastLocalProgressFn progress,
    void *progress_user) {
    if (!has_mkv_extension(output_path)) {
        throw std::invalid_argument(
            "Fast Local output must use the .mkv extension");
    }
    if (encrypted && password.empty()) {
        throw std::invalid_argument(
            "Fast Local encryption requires a password");
    }

    const uint64_t input_size = std::filesystem::file_size(input_path);
    const uint64_t total_frames =
        fast_local_frame_count(input_size, encrypted);
    const uint64_t plain_capacity =
        fast_local_plain_capacity(encrypted);
    const Sha256Digest digest = hash_file(input_path, profiler);

    FastLocalFileHeader file_header;
    file_header.flags = encrypted ? FastLocalEncrypted : 0;
    file_header.original_size = input_size;
    file_header.total_frames = total_frames;
    file_header.plain_frame_capacity = plain_capacity;
    if (encrypted) {
        file_header.file_id = make_encoding_file_id();
    }
    file_header.original_sha256 = digest;
    const auto serialized_file_header =
        serialize_fast_local_file_header(file_header);

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypted) {
        ScopedTimer timer(profiler, PerformanceStage::Preprocess);
        key = derive_key(password, file_header.file_id);
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        if (encrypted) secure_zero(key);
        throw std::runtime_error("could not open input file");
    }

    uint64_t payload_bytes = 0;
    uint64_t output_size = 0;
    try {
        SafeOutputFile safe_output(output_path);
        {
            VideoEncoder encoder(
                safe_output.partial_path().string(), profiler);
            std::vector<std::byte> plain(
                static_cast<std::size_t>(plain_capacity));
            std::vector<std::byte> frame(
                FAST_LOCAL_FRAME_BYTES, std::byte{0});

            uint64_t remaining = input_size;
            for (uint64_t index = 0; index < total_frames; ++index) {
                if (progress &&
                    progress(index, total_frames, progress_user) != 0) {
                    throw std::runtime_error(
                        "Fast Local encoding was cancelled");
                }
                const std::size_t plain_length =
                    static_cast<std::size_t>(
                        std::min<uint64_t>(remaining, plain_capacity));
                if (plain_length > 0) {
                    ScopedTimer timer(
                        profiler, PerformanceStage::InputRead);
                    input.read(
                        reinterpret_cast<char *>(plain.data()),
                        static_cast<std::streamsize>(plain_length));
                    if (input.gcount() !=
                        static_cast<std::streamsize>(plain_length)) {
                        throw std::runtime_error(
                            "input file changed or was truncated");
                    }
                }

                std::span<const std::byte> stored(
                    plain.data(), plain_length);
                std::vector<std::byte> encrypted_payload;
                if (encrypted) {
                    ScopedTimer timer(
                        profiler, PerformanceStage::Preprocess);
                    encrypted_payload = encrypt_chunk(
                        stored, key, file_header.file_id,
                        static_cast<uint32_t>(index));
                    stored = encrypted_payload;
                }
                if (stored.size() >
                    FAST_LOCAL_FRAME_PAYLOAD_CAPACITY) {
                    throw std::overflow_error(
                        "Fast Local frame payload overflow");
                }

                FastLocalFrameHeader frame_header;
                frame_header.flags =
                    encrypted ? FastLocalEncrypted : 0;
                frame_header.frame_index =
                    static_cast<uint32_t>(index);
                frame_header.total_frames =
                    static_cast<uint32_t>(total_frames);
                frame_header.payload_length =
                    static_cast<uint32_t>(stored.size());
                frame_header.plain_length =
                    static_cast<uint32_t>(plain_length);
                frame_header.payload_checksum = crc32c(stored);
                const auto serialized_frame_header =
                    serialize_fast_local_frame_header(frame_header);

                {
                    ScopedTimer timer(
                        profiler, PerformanceStage::PacketToFrame);
                    std::fill(frame.begin(), frame.end(), std::byte{0});
                    if (index == 0) {
                        std::copy(
                            serialized_file_header.begin(),
                            serialized_file_header.end(), frame.begin());
                    }
                    std::copy(
                        serialized_frame_header.begin(),
                        serialized_frame_header.end(),
                        frame.begin() + FAST_LOCAL_FILE_HEADER_SIZE);
                    std::copy(
                        stored.begin(), stored.end(),
                        frame.begin() + FAST_LOCAL_RESERVED_PREFIX);
                }
                encoder.encode_gray8_frame(frame);
                payload_bytes += stored.size();
                remaining -= plain_length;
            }
            encoder.finalize();
            output_size =
                std::filesystem::file_size(safe_output.partial_path());
        }
        {
            ScopedTimer timer(
                profiler, PerformanceStage::MuxDiskWrite);
            safe_output.commit();
        }
    } catch (...) {
        if (encrypted) secure_zero(key);
        throw;
    }
    if (encrypted) secure_zero(key);
    return make_statistics(
        input_size, output_size, total_frames, payload_bytes);
}

FastLocalStatistics decode_fast_local(
    const std::filesystem::path &video_path,
    const std::filesystem::path &output_path,
    const std::span<const std::byte> password,
    PerformanceProfiler *profiler,
    const FastLocalProgressFn progress,
    void *progress_user) {
    VideoDecoder decoder(video_path.string(), profiler);
    std::vector<std::byte> frame;
    if (!decoder.decode_next_gray8_frame(frame)) {
        throw FastLocalError(
            FastLocalErrorCode::Incomplete,
            "Fast Local video contains no frames");
    }
    if (frame.size() != FAST_LOCAL_FRAME_BYTES) {
        throw FastLocalError(
            FastLocalErrorCode::InvalidFormat,
            "Fast Local video has an incompatible pixel layout");
    }

    FastLocalFileHeader file_header;
    try {
        file_header = deserialize_fast_local_file_header(frame);
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        const auto code =
            message.find("unsupported") != std::string::npos
                ? FastLocalErrorCode::UnsupportedVersion
                : FastLocalErrorCode::Corrupt;
        throw FastLocalError(code, message);
    }
    const bool encrypted =
        (file_header.flags & FastLocalEncrypted) != 0;
    if (encrypted && password.empty()) {
        throw FastLocalError(
            FastLocalErrorCode::Crypto,
            "Fast Local video is encrypted; a password is required");
    }
    if (fast_local_frame_count(
            file_header.original_size, encrypted) !=
        file_header.total_frames) {
        corrupt("Fast Local total frame count does not match file size");
    }

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypted) {
        ScopedTimer timer(profiler, PerformanceStage::Postprocess);
        key = derive_key(password, file_header.file_id);
    }

    uint64_t written_bytes = 0;
    uint64_t payload_bytes = 0;
    try {
        SafeOutputFile safe_output(output_path);
        std::ofstream output(
            safe_output.partial_path(),
            std::ios::binary | std::ios::trunc);
        if (!output) {
            throw FastLocalError(
                FastLocalErrorCode::Io,
                "could not create partial output file");
        }
        StreamingSha256 hash;

        for (uint64_t index = 0;
             index < file_header.total_frames; ++index) {
            if (index > 0 &&
                !decoder.decode_next_gray8_frame(frame)) {
                throw FastLocalError(
                    FastLocalErrorCode::Incomplete,
                    "Fast Local video is missing a payload frame");
            }
            if (progress &&
                progress(index, file_header.total_frames,
                         progress_user) != 0) {
                throw FastLocalError(
                    FastLocalErrorCode::Io,
                    "Fast Local decoding was cancelled");
            }

            FastLocalFrameHeader frame_header;
            std::span<const std::byte> stored;
            {
                ScopedTimer timer(
                    profiler, PerformanceStage::PacketExtraction);
                try {
                    frame_header = deserialize_fast_local_frame_header(
                        std::span<const std::byte>(frame).subspan(
                            FAST_LOCAL_FILE_HEADER_SIZE,
                            FAST_LOCAL_FRAME_HEADER_SIZE));
                } catch (const std::runtime_error &error) {
                    corrupt(error.what());
                }
                if (frame_header.frame_index != index ||
                    frame_header.total_frames !=
                        file_header.total_frames) {
                    corrupt(
                        "Fast Local frame index or total count mismatch");
                }
                if (((frame_header.flags & FastLocalEncrypted) != 0) !=
                    encrypted) {
                    corrupt("Fast Local frame encryption flag mismatch");
                }
                stored = std::span<const std::byte>(frame).subspan(
                    FAST_LOCAL_RESERVED_PREFIX,
                    frame_header.payload_length);
                if (crc32c(stored) !=
                    frame_header.payload_checksum) {
                    corrupt("Fast Local frame payload checksum mismatch");
                }
            }

            std::span<const std::byte> plain = stored;
            std::vector<std::byte> decrypted;
            if (encrypted) {
                try {
                    ScopedTimer timer(
                        profiler, PerformanceStage::Postprocess);
                    decrypted = decrypt_chunk_up_to(
                        stored, key, file_header.file_id,
                        static_cast<uint32_t>(index),
                        static_cast<std::size_t>(
                            file_header.plain_frame_capacity));
                    plain = decrypted;
                } catch (const std::exception &) {
                    throw FastLocalError(
                        FastLocalErrorCode::Crypto,
                        "Fast Local decryption failed: wrong password "
                        "or corrupted data");
                }
            }
            if (plain.size() != frame_header.plain_length ||
                plain.size() >
                    file_header.original_size - written_bytes) {
                corrupt("Fast Local frame plain length mismatch");
            }
            {
                ScopedTimer timer(
                    profiler, PerformanceStage::Postprocess);
                hash.update(plain);
            }
            {
                ScopedTimer timer(
                    profiler, PerformanceStage::OutputWrite);
                output.write(
                    reinterpret_cast<const char *>(plain.data()),
                    static_cast<std::streamsize>(plain.size()));
                if (!output) {
                    throw FastLocalError(
                        FastLocalErrorCode::Io,
                        "Fast Local output write failed");
                }
            }
            written_bytes += plain.size();
            payload_bytes += stored.size();
        }

        if (written_bytes != file_header.original_size) {
            corrupt("Fast Local total payload size mismatch");
        }
        if (decoder.decode_next_gray8_frame(frame)) {
            corrupt("Fast Local video contains unexpected extra frames");
        }
        Sha256Digest actual;
        {
            ScopedTimer timer(
                profiler, PerformanceStage::Postprocess);
            actual = hash.finish();
        }
        if (!(actual == file_header.original_sha256)) {
            corrupt("Fast Local SHA-256 mismatch");
        }
        output.close();
        if (!output) {
            throw FastLocalError(
                FastLocalErrorCode::Io,
                "Fast Local output flush failed");
        }
        {
            ScopedTimer timer(
                profiler, PerformanceStage::OutputWrite);
            safe_output.commit();
        }
    } catch (...) {
        if (encrypted) secure_zero(key);
        throw;
    }
    if (encrypted) secure_zero(key);

    return make_statistics(
        std::filesystem::file_size(video_path),
        written_bytes, file_header.total_frames, payload_bytes);
}
