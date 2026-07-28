#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>

#include "fast_local_format.h"

class PerformanceProfiler;

using FastLocalProgressFn = int (*)(uint64_t, uint64_t, void *);

enum class FastLocalErrorCode {
    InvalidFormat,
    UnsupportedVersion,
    Corrupt,
    Incomplete,
    Crypto,
    Io,
};

class FastLocalError final : public std::runtime_error {
public:
    FastLocalError(FastLocalErrorCode code, const std::string &message)
        : std::runtime_error(message), code_(code) {}

    [[nodiscard]] FastLocalErrorCode code() const noexcept {
        return code_;
    }

private:
    FastLocalErrorCode code_;
};

struct FastLocalStatistics {
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t total_frames = 0;
    uint64_t header_bytes = 0;
    uint64_t payload_bytes = 0;
    uint64_t padding_bytes = 0;
    uint64_t frame_payload_capacity = FAST_LOCAL_FRAME_PAYLOAD_CAPACITY;
};

[[nodiscard]] bool fast_local_has_magic(
    const std::filesystem::path &video_path);

[[nodiscard]] FastLocalStatistics encode_fast_local(
    const std::filesystem::path &input_path,
    const std::filesystem::path &output_path,
    bool encrypted,
    std::span<const std::byte> password,
    PerformanceProfiler *profiler,
    FastLocalProgressFn progress = nullptr,
    void *progress_user = nullptr);

[[nodiscard]] FastLocalStatistics decode_fast_local(
    const std::filesystem::path &video_path,
    const std::filesystem::path &output_path,
    std::span<const std::byte> password,
    PerformanceProfiler *profiler,
    FastLocalProgressFn progress = nullptr,
    void *progress_user = nullptr);
