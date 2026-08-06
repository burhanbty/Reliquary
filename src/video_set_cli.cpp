#include "video_set_cli.h"

#include "media_storage.h"
#include "safe_output.h"
#include "video_set.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace video_set_cli {
namespace {

using namespace video_set;

constexpr int kExitUsage = 2;
constexpr int kExitIncomplete = 3;
constexpr int kExitCorrupt = 4;
constexpr int kExitHash = 5;

struct Options {
    std::string input;
    std::string output;
    std::string manifest;
    std::string videos_dir;
    std::string output_name;
    std::string profile = "resilient";
    std::string password;
    std::string container = "mkv";
    uint64_t target_seconds = kDefaultTargetDurationSeconds;
    uint64_t max_video_mib = kDefaultMaximumVideoSizeBytes / (1024 * 1024);
    double reserve_percent = kDefaultReservePercent;
    uint32_t upload_batch_size = 10;
    bool resume = false;
    bool restart_recovery = false;
    bool overwrite = false;
    bool plan_only = false;
};

struct Candidate {
    std::filesystem::path video;
    PartEnvelopeV1 envelope;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_video_path(const std::filesystem::path &path) {
    const auto ext = lower(path.extension().string());
    return ext == ".mkv" || ext == ".mp4" || ext == ".webm" ||
           ext == ".avi" || ext == ".mov";
}

void print_help(const char *program) {
    std::cout
        << "Video Set commands (file encode/decode only; streaming is unsupported):\n"
        << "  " << program << " set-plan <input-file> <output-root> [options]\n"
        << "  " << program << " set-encode <input-file> <output-root> [options]\n"
        << "  " << program << " set-status --manifest <set_manifest.json>\n"
        << "  " << program << " set-inspect <video-or-folder> [--password <pwd>]\n"
        << "  " << program << " set-recover <manifest-or-video-folder> <output-folder> [options]\n"
        << "\nEncode options:\n"
        << "  --reliability-profile <resilient|high-capacity|balanced|durable>\n"
        << "  --target-duration-seconds <seconds>   (project default: 600)\n"
        << "  --max-video-size-mib <MiB>           (project default: 1500; 0 disables)\n"
        << "  --reserve-percent <0..99>            (project default: 10)\n"
        << "  --output-container <mkv>             (current supported container)\n"
        << "  --resume  --overwrite  --plan-only  --upload-batch-size <count>\n"
        << "  --password <pwd>                     (never written to metadata)\n"
        << "\nRecovery options:\n"
        << "  --manifest <json> --videos-dir <folder> --output-dir <folder>\n"
        << "  --output-name <safe-name> --resume --restart-recovery --overwrite\n"
        << "\n" << kRealYoutubeValidation.cli_statement << "\n"
        << "Always verify successful recovery using the final full-file SHA-256 result.\n"
        << "\nResilient remains the default. High Capacity is explicit opt-in. The size defaults\n"
        << "are conservative VidStoreX project settings, not a YouTube guarantee. Upload and\n"
        << "download remain manual; filenames and playlist order are not identities.\n";
}

uint64_t parse_u64(const std::string &text, const char *name) {
    std::size_t used = 0;
    const uint64_t value = std::stoull(text, &used);
    if (used != text.size()) throw std::invalid_argument(std::string("invalid ") + name);
    return value;
}

double parse_double(const std::string &text, const char *name) {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value))
        throw std::invalid_argument(std::string("invalid ") + name);
    return value;
}

Options parse_options(const int argc, char *argv[], const std::string &command) {
    Options options;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require = [&](const char *name) -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--reliability-profile" || arg == "--profile") options.profile = require(arg.c_str());
        else if (arg == "--target-duration-seconds" || arg == "--target-seconds")
            options.target_seconds = parse_u64(require(arg.c_str()), "target duration");
        else if (arg == "--max-video-size-mib")
            options.max_video_mib = parse_u64(require(arg.c_str()), "maximum video size");
        else if (arg == "--reserve-percent")
            options.reserve_percent = parse_double(require(arg.c_str()), "reserve percentage");
        else if (arg == "--upload-batch-size")
            options.upload_batch_size = static_cast<uint32_t>(parse_u64(require(arg.c_str()), "upload batch size"));
        else if (arg == "--manifest") options.manifest = require("--manifest");
        else if (arg == "--videos-dir") options.videos_dir = require("--videos-dir");
        else if (arg == "--output-dir") options.output = require("--output-dir");
        else if (arg == "--output-name") options.output_name = require("--output-name");
        else if (arg == "--password") options.password = require("--password");
        else if (arg == "--output-container" || arg == "--container")
            options.container = lower(require(arg.c_str()));
        else if (arg == "--resume") options.resume = true;
        else if (arg == "--restart-recovery") options.restart_recovery = true;
        else if (arg == "--overwrite") options.overwrite = true;
        else if (arg == "--plan-only") options.plan_only = true;
        else if (arg == "--report-format") (void) require("--report-format");
        else if (arg == "--help" || arg == "-h") { print_help(argv[0]); throw std::runtime_error("__help__"); }
        else if (!arg.empty() && arg[0] == '-') throw std::invalid_argument("unknown Video Set option: " + arg);
        else positional.push_back(arg);
    }
    if (command == "set-status") {
        if (options.manifest.empty() && !positional.empty()) options.manifest = positional.front();
        if (options.manifest.empty()) throw std::invalid_argument("set-status requires --manifest");
    } else if (command == "set-inspect") {
        if (positional.empty()) throw std::invalid_argument("set-inspect requires a video or folder");
        options.input = positional[0];
    } else {
        if (!options.manifest.empty() && command == "set-recover") options.input = options.manifest;
        else if (!positional.empty()) options.input = positional[0];
        if (options.output.empty() && positional.size() > 1) options.output = positional[1];
        if (options.input.empty() || options.output.empty())
            throw std::invalid_argument(command + " requires input and output paths");
    }
    if (options.container != "mkv")
        throw std::invalid_argument("Video Set currently supports only the mkv output container");
    return options;
}

PlanOptions plan_options(const Options &options) {
    PlanOptions result;
    result.profile = parse_reliability_profile(options.profile);
    result.target_duration_seconds = options.target_seconds;
    if (options.max_video_mib > std::numeric_limits<uint64_t>::max() / (1024ull * 1024ull))
        throw std::overflow_error("maximum video size overflow");
    result.maximum_actual_video_size_bytes = options.max_video_mib * 1024ull * 1024ull;
    result.reserve_percent = options.reserve_percent;
    return result;
}

void print_plan(const SetPlan &plan) {
    uint64_t total_estimated = 0;
    double total_duration = 0;
    std::cout << "Video Set plan\n"
              << "  Set ID: " << id_hex(plan.set_id) << "\n"
              << "  Source: " << plan.original_filename << " (" << plan.original_file_size << " bytes)\n"
              << "  Source SHA-256: " << plan.original_file_sha256.hexValue() << "\n"
              << "  Profile/config: " << plan.profile_name << " / " << plan.config_id << "\n"
              << "  Geometry: " << plan.block_size << "x" << plan.block_size << ", "
              << plan.bits_per_symbol << "-bit, signal " << plan.signal_strength
              << ", repair " << plan.repair_percent << "%\n"
              << "  Target duration: " << plan.target_duration_seconds << " s\n"
              << "  Maximum actual size: " << (plan.maximum_actual_video_size_bytes == 0 ?
                    std::string("disabled") : std::to_string(plan.maximum_actual_video_size_bytes) + " bytes") << "\n"
              << "  Reserve: " << plan.reserve_percent << "%\n"
              << "  Selected chunk payload: " << plan.selected_chunk_payload_bytes << " bytes\n"
              << "  Parts: " << plan.parts.size() << "\n";
    for (const auto &p : plan.parts) {
        total_estimated += p.estimated_output_bytes;
        total_duration += p.estimated_duration_seconds;
        std::cout << "    P" << std::setw(4) << std::setfill('0') << p.part_index + 1
                  << ": offset=" << p.chunk_offset << " bytes=" << p.chunk_size
                  << " frames=" << p.estimated_frames << " duration="
                  << std::fixed << std::setprecision(2) << p.estimated_duration_seconds
                  << "s estimated-video=" << p.estimated_output_bytes << " bytes\n";
    }
    std::cout << "  Estimated total video bytes: " << total_estimated
              << "\n  Estimated total video duration: " << total_duration
              << " s\n  Temporary disk: one logical part plus one video\n"
              << "  Recovery disk required: at least " << plan.original_file_size
              << " bytes plus filesystem overhead\n";
}

ms_encoding_mode_t mode_for(const ReliabilityProfile profile) {
    return profile == ReliabilityProfile::HighCapacity ?
        MS_ENCODING_MODE_HIGH_CAPACITY : MS_ENCODING_MODE_RESILIENT;
}

bool decode_video(const std::filesystem::path &video,
                  const std::filesystem::path &payload,
                  const std::string &password, ms_result_t *result = nullptr) {
    const std::string video_text = video.string();
    const std::string payload_text = payload.string();
    ms_decode_options_t options{};
    options.input_path = video_text.c_str();
    options.output_path = payload_text.c_str();
    options.password = password.c_str();
    options.password_len = password.size();
    return ms_decode(&options, result) == MS_OK;
}

bool envelope_matches(const PartEnvelopeV1 &actual, const SetPlan &plan,
                      const PartPlan &part) {
    return actual.set_id == plan.set_id && actual.part_index == part.part_index &&
        actual.part_count == plan.parts.size() && actual.part_id == part.part_id &&
        actual.original_file_size == plan.original_file_size &&
        actual.chunk_offset == part.chunk_offset && actual.chunk_size == part.chunk_size &&
        actual.original_file_sha256 == plan.original_file_sha256 &&
        actual.chunk_sha256 == part.chunk_sha256 &&
        actual.descriptor_hash == plan.descriptor_hash &&
        actual.encoder_config_id == plan.config_id;
}

std::filesystem::path staging_path_for(const std::filesystem::path &output_root,
                                       const SetPlan &plan) {
    std::string base = sanitize_filename(plan.original_filename);
    if (const auto dot = base.find_last_of('.'); dot != std::string::npos)
        base.resize(dot);
    return output_root / std::filesystem::u8path(
        "." + base + "_" + plan.original_file_sha256.hexValue().substr(0, 8) + ".vsx-staging");
}

std::filesystem::path final_path_for(const std::filesystem::path &output_root,
                                     const SetPlan &plan) {
    std::string base = sanitize_filename(plan.original_filename);
    if (const auto dot = base.find_last_of('.'); dot != std::string::npos)
        base.resize(dot);
    return output_root / std::filesystem::u8path(
        base + "_" + id_hex(plan.set_id).substr(0, 8));
}

void remove_scoped(const std::filesystem::path &target,
                   const std::filesystem::path &root) {
    const auto absolute_target = std::filesystem::absolute(target).lexically_normal();
    const auto absolute_root = std::filesystem::absolute(root).lexically_normal();
    const auto relative = absolute_target.lexically_relative(absolute_root);
    if (relative.empty() || relative.native().starts_with(L"..") || absolute_target == absolute_root)
        throw std::runtime_error("refusing to remove a path outside the requested output root");
    std::filesystem::remove_all(absolute_target);
}

void atomic_publish_with_optional_replace(
    const std::filesystem::path &staged,
    const std::filesystem::path &target,
    const std::filesystem::path &scope_root,
    const bool overwrite) {
    if (!std::filesystem::exists(target)) {
        std::filesystem::rename(staged, target);
        return;
    }
    if (!overwrite)
        throw std::runtime_error("output exists; use --overwrite");
    auto backup = target;
    backup += ".vsx-replace-backup";
    if (std::filesystem::exists(backup))
        throw std::runtime_error(
            "safe overwrite backup already exists; refusing replacement");
    std::filesystem::rename(target, backup);
    try {
        std::filesystem::rename(staged, target);
    } catch (...) {
        std::error_code restore_error;
        std::filesystem::rename(backup, target, restore_error);
        if (restore_error)
            throw std::runtime_error(
                "new publish and automatic restoration both failed; "
                "the prior output remains at " + backup.string());
        throw;
    }
    remove_scoped(backup, scope_root);
}

void write_recovery_state(const std::filesystem::path &path, const SetPlan &plan,
                          const std::filesystem::path &partial,
                          const std::set<uint32_t> &completed,
                          const std::string &state) {
    SafeOutputFile safe(path);
    std::ofstream out(safe.partial_path());
    out << "{\n  \"schema\": \"vidstorex.video_set.recovery\",\n  \"version\": 1,\n"
        << "  \"set_id\": \"" << id_hex(plan.set_id) << "\",\n"
        << "  \"descriptor_hash\": \"" << plan.descriptor_hash.hexValue() << "\",\n"
        << "  \"temporary_path\": \"" << partial.filename().generic_string() << "\",\n"
        << "  \"original_size\": " << plan.original_file_size << ",\n"
        << "  \"original_sha256\": \"" << plan.original_file_sha256.hexValue() << "\",\n"
        << "  \"state\": \"" << state << "\",\n  \"completed\": [";
    bool first = true;
    for (const auto index : completed) {
        if (!first) out << ','; first = false;
        const auto &p = plan.parts[index];
        out << "{\"part_index\":" << index << ",\"offset\":" << p.chunk_offset
            << ",\"length\":" << p.chunk_size << ",\"chunk_sha256\":\""
            << p.chunk_sha256.hexValue() << "\"}";
    }
    out << "]\n}\n";
    out.close(); safe.commit();
}

std::set<uint32_t> read_completed_state(const std::filesystem::path &state,
                                        const SetPlan &plan,
                                        const std::filesystem::path &partial) {
    std::set<uint32_t> completed;
    if (!std::filesystem::exists(state) || !std::filesystem::exists(partial)) return completed;
    std::ifstream in(state);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    if (text.find(id_hex(plan.set_id)) == std::string::npos ||
        text.find(plan.descriptor_hash.hexValue()) == std::string::npos ||
        std::filesystem::file_size(partial) != plan.original_file_size) return completed;
    for (const auto &part : plan.parts) {
        const std::string marker = "\"part_index\":" + std::to_string(part.part_index);
        if (text.find(marker) != std::string::npos &&
            sha256_file_range(partial, part.chunk_offset, part.chunk_size) == part.chunk_sha256)
            completed.insert(part.part_index);
    }
    return completed;
}

std::vector<std::filesystem::path> collect_videos(const std::filesystem::path &input) {
    std::vector<std::filesystem::path> videos;
    if (std::filesystem::is_regular_file(input) && is_video_path(input)) videos.push_back(input);
    else if (std::filesystem::is_directory(input)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(input,
                 std::filesystem::directory_options::skip_permission_denied))
            if (entry.is_regular_file() && is_video_path(entry.path())) videos.push_back(entry.path());
    }
    std::sort(videos.begin(), videos.end());
    return videos;
}

std::filesystem::path unique_scan_directory(const std::filesystem::path &parent) {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = parent / (".vsx-scan-" + std::to_string(token));
    std::filesystem::create_directories(path);
    return path;
}

std::vector<Candidate> scan_candidates(const std::vector<std::filesystem::path> &videos,
                                       const std::filesystem::path &temporary,
                                       const std::string &password,
                                       std::vector<std::filesystem::path> &corrupt_videos) {
    std::vector<Candidate> candidates;
    std::size_t ordinal = 0;
    for (const auto &video : videos) {
        const auto payload = temporary / ("scan-" + std::to_string(ordinal++) + ".payload");
        if (!decode_video(video, payload, password)) {
            corrupt_videos.push_back(video); continue;
        }
        PartEnvelopeV1 envelope;
        std::string error;
        if (!verify_logical_payload(payload, &envelope, &error)) {
            if (parse_envelope_file(payload).kind != ParseKind::NotVideoSet)
                corrupt_videos.push_back(video);
            std::error_code ignored; std::filesystem::remove(payload, ignored);
            continue;
        }
        candidates.push_back({video, envelope});
        std::error_code ignored; std::filesystem::remove(payload, ignored);
    }
    return candidates;
}

SetPlan plan_from_envelopes(const std::vector<Candidate> &candidates,
                            const std::string &set_id) {
    const auto first = std::find_if(candidates.begin(), candidates.end(),
        [&](const Candidate &c) { return id_hex(c.envelope.set_id) == set_id; });
    if (first == candidates.end()) throw std::runtime_error("no usable Video Set parts found");
    const auto &e = first->envelope;
    SetPlan plan;
    plan.set_id = e.set_id;
    plan.original_filename = sanitize_filename(e.original_filename);
    plan.original_file_size = e.original_file_size;
    plan.original_file_sha256 = e.original_file_sha256;
    plan.profile = reliability_profile_from_id(e.profile_stable_id);
    plan.profile_name = std::string(reliability_profile_definition(plan.profile).cli_name);
    plan.config_id = e.encoder_config_id;
    plan.block_size = e.block_size;
    plan.bits_per_symbol = e.bits_per_symbol;
    plan.signal_strength = e.signal_milli / 1000.0;
    plan.repair_percent = e.repair_basis_points / 100.0;
    plan.width = e.width; plan.height = e.height; plan.fps = e.fps;
    plan.descriptor_hash = e.descriptor_hash;
    plan.parts.resize(e.part_count);
    std::vector<bool> seen(e.part_count);
    for (const auto &candidate : candidates) {
        const auto &x = candidate.envelope;
        if (id_hex(x.set_id) != set_id || x.part_index >= plan.parts.size()) continue;
        if (x.original_file_size != plan.original_file_size ||
            x.original_file_sha256 != plan.original_file_sha256 ||
            x.part_count != plan.parts.size() || x.descriptor_hash != plan.descriptor_hash)
            continue;
        if (!seen[x.part_index]) {
            auto &p = plan.parts[x.part_index];
            p.part_index = x.part_index; p.part_id = x.part_id;
            p.chunk_offset = x.chunk_offset; p.chunk_size = x.chunk_size;
            p.chunk_sha256 = x.chunk_sha256; seen[x.part_index] = true;
        }
    }
    return plan;
}

int do_plan(const Options &options) {
    auto plan = plan_file(options.input, plan_options(options));
    populate_chunk_hashes(options.input, plan);
    print_plan(plan);
    return 0;
}

int do_encode(const Options &options) {
    const std::filesystem::path source = options.input;
    const std::filesystem::path output_root = options.output;
    std::filesystem::create_directories(output_root);
    const auto source_before = sha256_file(source);
    auto plan = plan_file(source, plan_options(options));
    populate_chunk_hashes(source, plan);
    if (plan.original_file_sha256 != source_before) throw std::runtime_error("source changed while hashing");
    auto staging = staging_path_for(output_root, plan);
    if (options.resume && std::filesystem::exists(staging / "set_manifest.json")) {
        auto resumed = read_manifest(staging / "set_manifest.json");
        if (resumed.original_file_sha256 != plan.original_file_sha256 ||
            resumed.original_file_size != plan.original_file_size ||
            resumed.target_duration_seconds != plan.target_duration_seconds ||
            resumed.maximum_actual_video_size_bytes != plan.maximum_actual_video_size_bytes ||
            resumed.reserve_percent != plan.reserve_percent || resumed.profile != plan.profile)
            throw std::runtime_error("staging manifest does not match source or split policy; refusing mixed resume");
        plan = std::move(resumed);
    } else if (std::filesystem::exists(staging)) {
        if (!options.overwrite) throw std::runtime_error("staging set already exists; use --resume or --overwrite");
        remove_scoped(staging, output_root);
    }
    std::filesystem::create_directories(staging / "videos");
    std::filesystem::create_directories(staging / "returned");
    std::filesystem::create_directories(staging / "recovery");
    write_manifest_atomic(staging / "set_manifest.json", plan);
    print_plan(plan);
    if (options.plan_only) return 0;

    for (std::size_t index = 0; index < plan.parts.size(); ++index) {
        auto &part = plan.parts[index];
        const auto video = staging / "videos" /
            std::filesystem::u8path(part.expected_video_filename);
        if (options.resume && part.local_decode_state == "Exact" &&
            std::filesystem::exists(video) && std::filesystem::file_size(video) == part.actual_output_bytes &&
            lower(sha256_file(video).hexValue()) == lower(part.video_sha256)) {
            std::cout << "Resume: verified part " << index + 1 << " already complete\n";
            continue;
        }
        const auto payload = staging / (".part-" + std::to_string(index) + ".payload");
        const auto decoded = staging / (".part-" + std::to_string(index) + ".decoded");
        part.local_encode_state = "Encoding";
        part.local_decode_state = "Pending";
        write_manifest_atomic(staging / "set_manifest.json", plan);
        write_logical_payload(source, plan, part, payload);
        const std::string payload_text = payload.string();
        const std::string video_text = video.string();
        ms_encode_options_t encode{};
        encode.input_path = payload_text.c_str(); encode.output_path = video_text.c_str();
        encode.password = options.password.c_str(); encode.password_len = options.password.size();
        encode.encrypt = options.password.empty() ? 0 : 1;
        encode.hash_algorithm = MS_HASH_CRC32;
        encode.encoding_mode = mode_for(plan.profile);
        encode.repair_ratio = plan.repair_percent / 100.0;
        encode.repair_ratio_is_set = 1;
        ms_result_t result{};
        const auto status = ms_encode(&encode, &result);
        if (status != MS_OK) {
            part.local_encode_state = "Failed";
            part.notes = ms_status_string(status);
            write_manifest_atomic(staging / "set_manifest.json", plan);
            throw std::runtime_error("part encode failed: " + std::string(ms_status_string(status)));
        }
        part.local_encode_state = "Encoded";
        part.actual_frames = result.total_frames;
        part.actual_duration_seconds = plan.fps > 0 ? static_cast<double>(result.total_frames) / plan.fps : 0;
        part.actual_output_bytes = std::filesystem::file_size(video);
        part.video_sha256 = lower(sha256_file(video).hexValue());
        if (plan.maximum_actual_video_size_bytes != 0 &&
            part.actual_output_bytes > plan.maximum_actual_video_size_bytes) {
            if (index == 0 && plan.adaptive_retry_count < 3) {
                const long double scaled = static_cast<long double>(plan.selected_chunk_payload_bytes) *
                    plan.maximum_actual_video_size_bytes / part.actual_output_bytes * 0.95L;
                const uint64_t reduced = static_cast<uint64_t>((std::max)(1.0L, std::floor(scaled)));
                std::error_code ignored;
                std::filesystem::remove(video, ignored); std::filesystem::remove(payload, ignored);
                PlanOptions replanned = plan_options(options);
                replanned.deterministic_set_id = plan.set_id;
                replanned.forced_chunk_payload_bytes = reduced;
                const uint32_t retries = plan.adaptive_retry_count + 1;
                plan = plan_file(source, replanned);
                plan.adaptive_retry_count = retries;
                populate_chunk_hashes(source, plan);
                write_manifest_atomic(staging / "set_manifest.json", plan);
                index = static_cast<std::size_t>(-1);
                continue;
            }
            part.local_encode_state = "Failed size cap";
            part.notes = "actual video exceeds hard size cap";
            write_manifest_atomic(staging / "set_manifest.json", plan);
            throw std::runtime_error("actual video exceeds hard size cap; set remains in staging");
        }
        ms_result_t decoded_result{};
        if (!decode_video(video, decoded, options.password, &decoded_result)) {
            part.local_decode_state = "Failed";
            write_manifest_atomic(staging / "set_manifest.json", plan);
            throw std::runtime_error("local verification decode failed");
        }
        PartEnvelopeV1 actual;
        std::string verification_error;
        if (!verify_logical_payload(decoded, &actual, &verification_error) ||
            !envelope_matches(actual, plan, part)) {
            part.local_decode_state = "Failed";
            part.notes = verification_error.empty() ? "embedded metadata mismatch" : verification_error;
            write_manifest_atomic(staging / "set_manifest.json", plan);
            throw std::runtime_error("local part verification failed: " + part.notes);
        }
        part.local_decode_state = "Exact";
        part.local_encode_state = "Locally verified";
        part.notes.clear();
        std::error_code ignored;
        std::filesystem::remove(payload, ignored); std::filesystem::remove(decoded, ignored);
        write_manifest_atomic(staging / "set_manifest.json", plan);
        std::cout << "Part " << index + 1 << '/' << plan.parts.size() << " locally verified exact\n";
    }
    if (sha256_file(source) != source_before) throw std::runtime_error("source file changed during encode");
    plan.aggregate_state = infer_status(plan);
    write_reports(staging, plan);
    write_manual_workflow_files(staging, plan, options.upload_batch_size);
    write_manifest_atomic(staging / "set_manifest.json", plan);
    const auto final = final_path_for(output_root, plan);
    atomic_publish_with_optional_replace(
        staging, final, output_root, options.overwrite);
    std::cout << "Video Set locally verified and atomically published: " << final.string() << "\n";
    return 0;
}

int do_status(const Options &options) {
    const auto plan = read_manifest(options.manifest);
    std::vector<uint32_t> missing;
    const auto root = std::filesystem::path(options.manifest).parent_path();
    for (const auto &p : plan.parts) {
        const auto video = root / "videos" /
            std::filesystem::u8path(p.expected_video_filename);
        if (!std::filesystem::exists(video)) missing.push_back(p.part_index);
    }
    std::cout << "Set ID: " << id_hex(plan.set_id) << "\nSource: " << plan.original_filename
              << "\nProfile/config: " << plan.profile_name << " / " << plan.config_id
              << "\nParts: " << plan.parts.size() - missing.size() << '/' << plan.parts.size()
              << "\nStatus: " << infer_status(plan, missing) << "\n";
    if (!missing.empty()) {
        std::cout << "Missing parts:";
        for (const auto index : missing) std::cout << ' ' << index + 1;
        std::cout << '\n';
        return kExitIncomplete;
    }
    return 0;
}

int do_inspect(const Options &options) {
    const auto videos = collect_videos(options.input);
    if (videos.empty()) throw std::runtime_error("no supported video files found");
    const auto temporary = unique_scan_directory(std::filesystem::temp_directory_path());
    std::vector<std::filesystem::path> corrupt;
    const auto candidates = scan_candidates(videos, temporary, options.password, corrupt);
    std::error_code ignored; std::filesystem::remove_all(temporary, ignored);
    std::map<std::string, std::map<uint32_t, std::vector<Candidate>>> sets;
    for (const auto &c : candidates)
        sets[id_hex(c.envelope.set_id)][c.envelope.part_index].push_back(c);
    bool conflict_found = false;
    for (const auto &[set_id, parts] : sets) {
        const auto first = std::find_if(candidates.begin(), candidates.end(),
            [&](const Candidate &c) { return id_hex(c.envelope.set_id) == set_id; });
        std::cout << "Set " << set_id << ": " << first->envelope.original_filename
                  << ", available " << parts.size() << '/' << first->envelope.part_count << " parts";
        std::size_t duplicate_count = 0;
        std::size_t conflict_count = 0;
        for (const auto &[index, group] : parts) {
            if (group.size() < 2) continue;
            const auto &expected = group.front().envelope;
            const bool conflict = std::any_of(
                group.begin() + 1, group.end(), [&](const Candidate &candidate) {
                    const auto &actual = candidate.envelope;
                    return actual.part_id != expected.part_id ||
                           actual.chunk_sha256 != expected.chunk_sha256 ||
                           actual.chunk_offset != expected.chunk_offset ||
                           actual.chunk_size != expected.chunk_size ||
                           actual.descriptor_hash != expected.descriptor_hash;
                });
            if (conflict) ++conflict_count;
            else duplicate_count += group.size() - 1;
        }
        conflict_found = conflict_found || conflict_count != 0;
        const auto missing_count =
            first->envelope.part_count > parts.size()
                ? first->envelope.part_count - parts.size() : 0;
        std::cout << ", duplicates " << duplicate_count
                  << ", conflicts " << conflict_count
                  << ", missing " << missing_count << "\n";
    }
    if (!corrupt.empty()) std::cout << "Corrupt/unreadable videos: " << corrupt.size() << "\n";
    if (sets.empty()) return kExitCorrupt;
    return corrupt.empty() && !conflict_found ? 0 : kExitCorrupt;
}

int do_recover(const Options &options) {
    std::filesystem::path input = options.input;
    std::filesystem::path output = options.output;
    std::filesystem::create_directories(output);
    std::optional<SetPlan> manifest_plan;
    std::filesystem::path manifest_path;
    std::filesystem::path video_source = input;
    if (std::filesystem::is_regular_file(input) && input.filename() == "set_manifest.json") {
        manifest_path = input;
        manifest_plan = read_manifest(input);
        if (!options.videos_dir.empty()) video_source = options.videos_dir;
        else if (std::filesystem::exists(input.parent_path() / "returned") &&
                 !collect_videos(input.parent_path() / "returned").empty())
            video_source = input.parent_path() / "returned";
        else video_source = input.parent_path() / "videos";
    } else if (!options.manifest.empty() && std::filesystem::exists(options.manifest)) {
        manifest_path = options.manifest; manifest_plan = read_manifest(manifest_path);
        if (!options.videos_dir.empty()) video_source = options.videos_dir;
    }
    const auto videos = collect_videos(video_source);
    if (videos.empty()) throw std::runtime_error("no supported video files found for recovery");
    const auto temporary = unique_scan_directory(output);
    std::vector<std::filesystem::path> unreadable;
    const auto candidates = scan_candidates(videos, temporary, options.password, unreadable);
    std::map<std::string, std::vector<Candidate>> groups;
    for (const auto &c : candidates) groups[id_hex(c.envelope.set_id)].push_back(c);
    if (manifest_plan) {
        const auto wanted = id_hex(manifest_plan->set_id);
        for (auto it = groups.begin(); it != groups.end();) {
            if (it->first != wanted) it = groups.erase(it); else ++it;
        }
        if (!groups.contains(wanted)) groups[wanted] = {};
    }
    if (groups.empty()) {
        std::error_code ignored; std::filesystem::remove_all(temporary, ignored);
        std::cerr << "No valid embedded Video Set envelopes were recovered; corrupt videos: " << unreadable.size() << "\n";
        return kExitCorrupt;
    }
    int overall = 0;
    for (auto &[set_id, group] : groups) {
        SetPlan plan = manifest_plan && id_hex(manifest_plan->set_id) == set_id ?
            *manifest_plan : plan_from_envelopes(candidates, set_id);
        std::map<uint32_t, std::vector<Candidate>> by_index;
        for (const auto &c : group) by_index[c.envelope.part_index].push_back(c);
        std::vector<uint32_t> missing, duplicates, conflicts, corrupt_indices;
        std::map<uint32_t, Candidate> chosen;
        for (uint32_t index = 0; index < plan.parts.size(); ++index) {
            auto found = by_index.find(index);
            if (found == by_index.end() || found->second.empty()) { missing.push_back(index); continue; }
            const auto &first = found->second.front();
            bool conflict = false;
            for (const auto &candidate : found->second) {
                if (candidate.envelope.part_id != first.envelope.part_id ||
                    candidate.envelope.chunk_sha256 != first.envelope.chunk_sha256 ||
                    candidate.envelope.chunk_offset != first.envelope.chunk_offset ||
                    candidate.envelope.chunk_size != first.envelope.chunk_size ||
                    candidate.envelope.descriptor_hash != plan.descriptor_hash) conflict = true;
            }
            if (conflict || !envelope_matches(first.envelope, plan, plan.parts[index])) {
                conflicts.push_back(index); continue;
            }
            if (found->second.size() > 1) duplicates.push_back(index);
            chosen.emplace(index, first);
        }
        if (!unreadable.empty()) {
            std::cout << "Unreadable/corrupt video candidates: " << unreadable.size() << "\n";
            if (manifest_plan) {
                const auto count = (std::min)(missing.size(), unreadable.size());
                corrupt_indices.insert(corrupt_indices.end(), missing.begin(),
                                       missing.begin() + static_cast<std::ptrdiff_t>(count));
            }
        }
        if (manifest_plan || missing.empty()) {
            uint64_t covered = 0;
            for (const auto &part : plan.parts) {
                if (part.chunk_offset != covered || covered > plan.original_file_size ||
                    part.chunk_size > plan.original_file_size - covered) {
                    conflicts.push_back(part.part_index);
                    break;
                }
                covered += part.chunk_size;
            }
            if (covered != plan.original_file_size && conflicts.empty())
                conflicts.push_back(0);
        }
        const auto report_root = manifest_path.empty() ? output / ("recovery_" + set_id.substr(0, 8)) : manifest_path.parent_path();
        std::filesystem::create_directories(report_root / "reports");
        if (!missing.empty() || !conflicts.empty() || !corrupt_indices.empty()) {
            plan.aggregate_state = infer_status(plan, missing, conflicts, corrupt_indices);
            write_reports(report_root, plan, missing, duplicates, conflicts, corrupt_indices);
            std::cout << "Set " << set_id << ": " << plan.aggregate_state << "\n";
            overall = (conflicts.empty() && corrupt_indices.empty()) ?
                (std::max)(overall, kExitIncomplete) : (std::max)(overall, kExitCorrupt);
            continue;
        }
        const std::string final_name = sanitize_filename(options.output_name.empty() ?
            plan.original_filename : options.output_name);
        const auto final = output / std::filesystem::u8path(final_name);
        const auto recovery_dir = report_root / "recovery";
        std::filesystem::create_directories(recovery_dir);
        const auto partial = recovery_dir / (final_name + ".vsx.partial");
        const auto state = recovery_dir / "recovery_state.json";
        if (options.restart_recovery) {
            std::error_code ignored; std::filesystem::remove(partial, ignored); std::filesystem::remove(state, ignored);
        }
        if (std::filesystem::exists(final) && !options.overwrite)
            throw std::runtime_error("recovery output exists; use --overwrite");
        std::set<uint32_t> completed = options.resume ? read_completed_state(state, plan, partial) : std::set<uint32_t>{};
        if (!std::filesystem::exists(partial)) {
            std::ofstream create(partial, std::ios::binary | std::ios::trunc);
            if (plan.original_file_size > 0) {
                create.seekp(static_cast<std::streamoff>(plan.original_file_size - 1)); create.put('\0');
            }
        }
        std::fstream assembled(partial, std::ios::binary | std::ios::in | std::ios::out);
        if (!assembled) throw std::runtime_error("could not open recovery partial output");
        for (uint32_t index = 0; index < plan.parts.size(); ++index) {
            if (completed.contains(index)) { std::cout << "Resume: part " << index + 1 << " already verified\n"; continue; }
            const auto payload = temporary / ("recover-" + set_id.substr(0, 8) + '-' + std::to_string(index) + ".payload");
            if (!decode_video(chosen.at(index).video, payload, options.password)) {
                corrupt_indices.push_back(index); break;
            }
            PartEnvelopeV1 actual; std::string error;
            if (!verify_logical_payload(payload, &actual, &error) ||
                !envelope_matches(actual, plan, plan.parts[index])) {
                corrupt_indices.push_back(index); break;
            }
            std::ifstream chunk(payload, std::ios::binary);
            chunk.seekg(actual.header_length);
            assembled.seekp(static_cast<std::streamoff>(actual.chunk_offset));
            std::vector<char> buffer(1024 * 1024);
            uint64_t remaining = actual.chunk_size;
            while (remaining) {
                const auto count = static_cast<std::streamsize>((std::min)(remaining, static_cast<uint64_t>(buffer.size())));
                chunk.read(buffer.data(), count);
                if (chunk.gcount() != count) { corrupt_indices.push_back(index); break; }
                assembled.write(buffer.data(), count);
                remaining -= static_cast<uint64_t>(count);
            }
            assembled.flush();
            if (!corrupt_indices.empty()) break;
            if (sha256_file_range(partial, actual.chunk_offset, actual.chunk_size) != actual.chunk_sha256) {
                corrupt_indices.push_back(index); break;
            }
            completed.insert(index);
            plan.parts[index].recovered_state = "Exact";
            write_recovery_state(state, plan, partial, completed, "Recovering");
            std::error_code ignored; std::filesystem::remove(payload, ignored);
        }
        assembled.close();
        if (!corrupt_indices.empty()) {
            plan.aggregate_state = "Corrupt parts detected";
            write_reports(report_root, plan, {}, duplicates, {}, corrupt_indices);
            overall = (std::max)(overall, kExitCorrupt); continue;
        }
        if (sha256_file(partial) != plan.original_file_sha256) {
            plan.aggregate_state = "Failed global SHA validation";
            write_recovery_state(state, plan, partial, completed, plan.aggregate_state);
            write_reports(report_root, plan);
            overall = (std::max)(overall, kExitHash); continue;
        }
        atomic_publish_with_optional_replace(
            partial, final, output, options.overwrite);
        plan.aggregate_state = "Recovered exact";
        plan.final_output_path = final.string();
        write_recovery_state(state, plan, final, completed, plan.aggregate_state);
        write_reports(report_root, plan, {}, duplicates);
        std::cout << "Set " << set_id << ": Recovered exact\nFinal: " << final.string()
                  << "\nSHA-256: " << plan.original_file_sha256.hexValue() << "\n";
    }
    std::error_code ignored; std::filesystem::remove_all(temporary, ignored);
    return overall;
}

} // namespace

bool is_command(const char *command) {
    if (!command) return false;
    const std::string value(command);
    return value == "set-plan" || value == "set-encode" || value == "set-status" ||
           value == "set-recover" || value == "set-inspect" || value == "set-help";
}

int run(const int argc, char *argv[]) {
    const std::string command = argv[1];
    if (command == "set-help") { print_help(argv[0]); return 0; }
    try {
        const auto options = parse_options(argc, argv, command);
        if (command == "set-plan") return do_plan(options);
        if (command == "set-encode") return do_encode(options);
        if (command == "set-status") return do_status(options);
        if (command == "set-inspect") return do_inspect(options);
        return do_recover(options);
    } catch (const std::runtime_error &error) {
        if (std::string(error.what()) == "__help__") return 0;
        std::cerr << "Video Set error: " << error.what() << "\n";
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "Video Set argument error: " << error.what() << "\n";
        print_help(argv[0]);
        return kExitUsage;
    }
}

} // namespace video_set_cli
