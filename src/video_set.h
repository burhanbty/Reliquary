#pragma once

#include "encoding_reliability.h"
#include "integrity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace video_set {

inline constexpr std::array<std::byte, 8> kMagic{
    std::byte{'V'}, std::byte{'S'}, std::byte{'X'}, std::byte{'S'},
    std::byte{'E'}, std::byte{'T'}, std::byte{'0'}, std::byte{'1'}};
inline constexpr uint16_t kSchemaVersion = 1;
inline constexpr std::size_t kMaximumHeaderBytes = 4096;
inline constexpr std::size_t kMaximumFilenameBytes = 255;
inline constexpr uint64_t kDefaultTargetDurationSeconds = 600;
inline constexpr uint64_t kDefaultMaximumVideoSizeBytes = 1500ull * 1024ull * 1024ull;
inline constexpr double kDefaultReservePercent = 10.0;

using Id128 = std::array<std::byte, 16>;

struct PartEnvelopeV1 {
    uint32_t flags = 0;
    Id128 set_id{};
    uint32_t part_index = 0;
    uint32_t part_count = 0;
    Id128 part_id{};
    uint64_t original_file_size = 0;
    uint64_t chunk_offset = 0;
    uint64_t chunk_size = 0;
    Sha256Digest original_file_sha256{};
    Sha256Digest chunk_sha256{};
    uint16_t profile_stable_id = 0;
    uint16_t block_size = 8;
    uint16_t bits_per_symbol = 1;
    uint32_t signal_milli = 1000;
    uint32_t repair_basis_points = 500;
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint16_t fps = 30;
    std::string encoder_config_id;
    std::string original_filename;
    Sha256Digest descriptor_hash{};
    uint32_t header_checksum = 0;
    uint16_t header_length = 0;
};

enum class ParseKind { NotVideoSet, Valid, Invalid };

struct ParseResult {
    ParseKind kind = ParseKind::NotVideoSet;
    PartEnvelopeV1 envelope;
    std::size_t payload_offset = 0;
    std::string error;
};

struct PlanOptions {
    ReliabilityProfile profile = ReliabilityProfile::Local;
    uint64_t target_duration_seconds = kDefaultTargetDurationSeconds;
    uint64_t maximum_actual_video_size_bytes = kDefaultMaximumVideoSizeBytes;
    double reserve_percent = kDefaultReservePercent;
    uint32_t fps = 30;
    std::optional<Id128> deterministic_set_id;
    std::optional<uint64_t> forced_chunk_payload_bytes;
};

struct PartPlan {
    uint32_t part_index = 0;
    uint64_t chunk_offset = 0;
    uint64_t chunk_size = 0;
    Sha256Digest chunk_sha256{};
    Id128 part_id{};
    uint64_t estimated_frames = 0;
    double estimated_duration_seconds = 0.0;
    uint64_t estimated_output_bytes = 0;
    std::string expected_video_filename;
    uint64_t actual_frames = 0;
    double actual_duration_seconds = 0.0;
    uint64_t actual_output_bytes = 0;
    std::string video_sha256;
    std::string local_encode_state = "Pending";
    std::string local_decode_state = "Pending";
    std::string upload_state = "Awaiting upload";
    std::string returned_video_path;
    std::string recovered_state = "Pending";
    std::string notes;
};

struct SetPlan {
    Id128 set_id{};
    std::string original_filename;
    uint64_t original_file_size = 0;
    Sha256Digest original_file_sha256{};
    ReliabilityProfile profile = ReliabilityProfile::Local;
    std::string profile_name;
    std::string config_id;
    int block_size = 8;
    int bits_per_symbol = 1;
    double signal_strength = 1.0;
    double repair_percent = 5.0;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    uint64_t target_duration_seconds = kDefaultTargetDurationSeconds;
    uint64_t maximum_actual_video_size_bytes = kDefaultMaximumVideoSizeBytes;
    double reserve_percent = kDefaultReservePercent;
    uint64_t selected_chunk_payload_bytes = 0;
    uint32_t adaptive_retry_count = 0;
    Sha256Digest descriptor_hash{};
    std::vector<PartPlan> parts;
    std::string aggregate_state = "Planned";
    std::string final_output_path;
};

[[nodiscard]] std::vector<std::byte> serialize_envelope(
    const PartEnvelopeV1 &envelope);
[[nodiscard]] ParseResult parse_envelope(std::span<const std::byte> bytes);
[[nodiscard]] ParseResult parse_envelope_file(
    const std::filesystem::path &path);
[[nodiscard]] bool has_video_set_magic(const std::filesystem::path &path);

[[nodiscard]] Id128 generate_set_id();
[[nodiscard]] std::string id_hex(const Id128 &id);
[[nodiscard]] Id128 id_from_hex(const std::string &hex);
[[nodiscard]] std::string sanitize_filename(std::string value);
[[nodiscard]] Sha256Digest sha256_file(const std::filesystem::path &path);
[[nodiscard]] Sha256Digest sha256_file_range(
    const std::filesystem::path &path, uint64_t offset, uint64_t size);

[[nodiscard]] SetPlan plan_file(const std::filesystem::path &source,
                                const PlanOptions &options);
void populate_chunk_hashes(const std::filesystem::path &source,
                           SetPlan &plan);
void recompute_descriptor_and_part_ids(SetPlan &plan);
[[nodiscard]] PartEnvelopeV1 envelope_for_part(
    const SetPlan &plan, const PartPlan &part);
void write_logical_payload(const std::filesystem::path &source,
                           const SetPlan &plan, const PartPlan &part,
                           const std::filesystem::path &output);
[[nodiscard]] bool verify_logical_payload(
    const std::filesystem::path &payload,
    PartEnvelopeV1 *envelope,
    std::string *error);

[[nodiscard]] std::string manifest_json(const SetPlan &plan);
void write_manifest_atomic(const std::filesystem::path &path,
                           const SetPlan &plan);
[[nodiscard]] SetPlan read_manifest(const std::filesystem::path &path);
void write_reports(const std::filesystem::path &set_root,
                   const SetPlan &plan,
                   const std::vector<uint32_t> &missing = {},
                   const std::vector<uint32_t> &duplicates = {},
                   const std::vector<uint32_t> &conflicts = {},
                   const std::vector<uint32_t> &corrupt = {});
void write_manual_workflow_files(const std::filesystem::path &set_root,
                                 const SetPlan &plan,
                                 uint32_t upload_batch_size);

[[nodiscard]] std::string infer_status(
    const SetPlan &plan,
    std::span<const uint32_t> missing = {},
    std::span<const uint32_t> conflicts = {},
    std::span<const uint32_t> corrupt = {},
    bool recovering = false,
    bool recovered_exact = false,
    bool global_hash_failed = false);

} // namespace video_set
