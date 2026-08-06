// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include "configuration.h"
#include "encoding_reliability.h"
#include "media_storage.h"
#include "safe_output.h"
#include "video_encoder.h"
#include "youtube_capacity_lab.h"
#include "youtube_test_lab.h"
#include "video_set.h"
#include "video_set_cli.h"

static std::string format_size(const uint64_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    auto size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 3) {
        size /= 1024;
        ++unit;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    return oss.str();
}

static bool print_performance_report(const ms_result_t &result,
                                     const std::string &json_path) {
    const size_t required = ms_format_performance_report(
        &result, nullptr, 0);
    std::vector<char> report(required);
    ms_format_performance_report(&result, report.data(), report.size());
    std::cout << report.data();

    if (!json_path.empty()) {
        if (const ms_status_t status =
                ms_write_benchmark_json(&result, json_path.c_str());
            status != MS_OK) {
            std::cerr << "Error writing benchmark JSON: "
                      << ms_status_string(status) << "\n";
            return false;
        }
        std::cout << "Benchmark JSON: " << json_path << "\n";
    }
    return true;
}

static void print_encoding_estimate(
    const ms_encoding_estimate_t &estimate,
    const std::string &reliability_profile) {
    std::cout << "\nPreflight estimate:\n"
              << "  Encoding mode: "
              << (estimate.encoding_mode == MS_ENCODING_MODE_FAST_LOCAL
                      ? "Fast Local"
                      : estimate.encoding_mode ==
                            MS_ENCODING_MODE_HIGH_CAPACITY
                      ? "High Capacity"
                      : "Resilient / Platform")
              << "\n"
              << "  Input size: "
              << format_size(estimate.input_size_bytes)
              << " (" << estimate.input_size_bytes << " bytes)\n";
    if (estimate.encoding_mode == MS_ENCODING_MODE_FAST_LOCAL) {
        std::cout << "  Header bytes: "
                  << estimate.header_bytes << "\n"
                  << "  Frame payload capacity: "
                  << estimate.frame_payload_capacity << "\n"
                  << "  Stored payload bytes: "
                  << estimate.payload_bytes << "\n"
                  << "  Raw-frame padding bytes: "
                  << estimate.padding_bytes << "\n";
    } else {
        std::cout << "  Reliability profile: "
                  << reliability_profile << "\n"
                  << std::fixed << std::setprecision(2)
                  << "  Repair percentage: "
                  << estimate.repair_percentage << "%\n"
                  << std::setprecision(6)
                  << "  Repair ratio: " << estimate.repair_ratio << "\n"
                  << "  Source packets: "
                  << estimate.source_packet_count << "\n"
                  << "  Repair packets: "
                  << estimate.repair_packet_count << "\n"
                  << "  Total packets: "
                  << estimate.total_packet_count << "\n";
        if (estimate.encoding_mode ==
                MS_ENCODING_MODE_HIGH_CAPACITY) {
            ResilientVideoConfig same_resolution_resilient;
            same_resolution_resilient.width = 1920;
            same_resolution_resilient.height = 1080;
            const uint64_t resilient_packets_per_frame =
                static_cast<uint64_t>(VideoEncoder::packets_per_frame(
                    same_resolution_resilient));
            const uint64_t resilient_frames =
                estimate.total_packet_count / resilient_packets_per_frame +
                (estimate.total_packet_count % resilient_packets_per_frame
                     != 0 ? 1 : 0);
            std::cout
                << "  Block geometry: 4x4\n"
                << "  Bits per symbol: 1\n"
                << "  Signal strength: 1.0\n"
                << "  Config ID: "
                << reliability_profile_config_id(
                       ReliabilityProfile::HighCapacity)
                << "\n"
                << "  Validation: Real YouTube tested, 6/6 exact\n"
                << "  Same-resolution Resilient estimate: "
                << resilient_frames << " frames, "
                << std::fixed << std::setprecision(2)
                << static_cast<double>(resilient_frames) / FRAME_FPS
                << " s\n"
                << "  Useful payload capacity gain: approximately 4x\n"
                << "  Expected video count: 1\n"
                << "  Note: actual encoded file size may vary.\n";
        }
    }
    std::cout << "  Estimated frames: "
              << estimate.estimated_frame_count << "\n"
              << std::setprecision(2)
              << "  Estimated video duration: "
              << estimate.estimated_video_duration_seconds << " s\n";
    if (estimate.output_size_estimate_available) {
        std::cout << "  Estimated likely output: "
                  << format_size(estimate.estimated_output_bytes)
                  << " (" << estimate.estimated_output_bytes
                  << " bytes)\n"
                  << "  Estimated output minimum: "
                  << format_size(estimate.estimated_output_min_bytes)
                  << "\n"
                  << "  Estimated output maximum: "
                  << format_size(estimate.estimated_output_max_bytes)
                  << "\n";
    } else {
        std::cout
            << "  Estimated likely output: unavailable\n"
            << "  Estimated output minimum: unavailable\n"
            << "  Estimated output maximum: unavailable\n";
    }
    std::cout << "  Probe frame count: "
              << estimate.probe_frame_count << "\n"
              << "  Probe duration: "
              << estimate.probe_duration_seconds << " s\n"
              << "  Estimation method: "
              << estimate.estimation_method << "\n";
    if (estimate.disk_space_known) {
        std::cout << "  Available disk: "
                  << format_size(estimate.available_disk_bytes)
                  << " (" << estimate.available_disk_bytes
                  << " bytes)\n";
    } else {
        std::cout << "  Available disk: unknown\n";
    }
    const char *disk_status =
        estimate.disk_space_sufficient < 0
            ? "unknown"
            : (estimate.disk_space_sufficient
                   ? "sufficient"
                   : "insufficient");
    std::cout << "  Disk space status: " << disk_status << "\n";
    if (estimate.required_disk_space_known) {
        std::cout << "  Safety margin: "
                  << format_size(estimate.safety_margin_bytes)
                  << " (" << estimate.safety_margin_bytes
                  << " bytes)\n"
                  << "  Required disk: "
                  << format_size(estimate.required_disk_bytes)
                  << " (" << estimate.required_disk_bytes
                  << " bytes)\n";
    } else {
        std::cout
            << "  Safety margin: unavailable\n"
            << "  Required disk: unavailable\n";
    }
    std::cout << "  Can start encoding: "
              << (estimate.can_start_encoding ? "yes" : "no")
              << "\n"
              << "  Preflight duration: "
              << estimate.preflight_duration_seconds << " s\n"
              << "  Warning: "
              << (estimate.warning[0] != '\0'
                      ? estimate.warning
                      : "none")
              << "\n"
              << "  Error: "
              << (estimate.error[0] != '\0'
                      ? estimate.error
                      : "none")
              << "\n";
}

static void print_insufficient_disk_details(
    const ms_encoding_estimate_t &estimate,
    const bool overridden) {
    const uint64_t missing =
        estimate.required_disk_bytes >
                estimate.available_disk_bytes
            ? estimate.required_disk_bytes -
                  estimate.available_disk_bytes
            : 0;
    std::ostream &out = std::cerr;
    out << (overridden
                ? "WARNING: --allow-low-disk explicitly overrides "
                  "a known insufficient-disk result.\n"
                : "Error: insufficient disk space; encoding will not "
                  "start.\n")
        << "  Available space: "
        << format_size(estimate.available_disk_bytes)
        << " (" << estimate.available_disk_bytes << " bytes)\n"
        << "  Required space: "
        << format_size(estimate.required_disk_bytes)
        << " (" << estimate.required_disk_bytes << " bytes)\n"
        << "  Missing space: " << format_size(missing)
        << " (" << missing << " bytes)\n"
        << "  Estimated output maximum: "
        << format_size(estimate.estimated_output_max_bytes)
        << " (" << estimate.estimated_output_max_bytes << " bytes)\n"
        << "  Safety margin: "
        << format_size(estimate.safety_margin_bytes)
        << " (" << estimate.safety_margin_bytes << " bytes)\n";
    if (overridden) {
        out << "Proceeding because the user consciously requested "
               "the low-disk override.\n";
    }
}

static int encode_progress(const uint64_t current, const uint64_t total, void *) {
    if (total > 0) {
        std::cout << "\rEncoding chunk " << (current + 1) << "/" << total << "..." << std::flush;
    }
    return 0;
}

static int decode_progress(const uint64_t current, const uint64_t total, void *) {
    if (total > 0) {
        std::cout << "\rDecoding frame " << current << "/" << total << "..." << std::flush;
    }
    return 0;
}

static int stream_encode_progress(const uint64_t current, const uint64_t total, void *) {
    if (total > 0) {
        std::cout << "\rStreaming chunk " << (current + 1) << "/" << total << "..." << std::flush;
    }
    return 0;
}

static int stream_decode_progress(const uint64_t current, const uint64_t total, void *) {
    if (total > 0) {
        std::cout << "\rReceiving frame " << current << "/" << total << "..." << std::flush;
    } else {
        std::cout << "\rReceiving frame " << current << "..." << std::flush;
    }
    return 0;
}

static void print_usage(const char *program) {
    std::cerr << "Usage:\n"
            << "  " << program <<
            " encode --input <file> --output <video> [--encrypt --password <pwd>] [--hash <crc32|xxhash>]\n"
            << "    [--mode <resilient|fast-local>]\n"
            << "    [--reliability-profile <high-capacity|resilient|local|balanced|durable>] [--repair-percent <0..500>]\n"
            << "    [--estimate-only] [--estimate-json <estimate.json>] [--no-probe] [--allow-low-disk]\n"
            << "    [--benchmark-json <report.json>]\n"
            << "  " << program << " decode --input <video> --output <file> [--password <pwd>]"
            << " [--benchmark-json <report.json>]\n"
            << "  " << program <<
            " stream-encode --input <file> --url <rtmp://...> [--bitrate <kbps>] [--width <w> --height <h>] [--encrypt --password <pwd>] [--reliability-profile <resilient|local|balanced|durable>] [--repair-percent <0..500>] [--benchmark-json <report.json>]\n"
            "\n  high-capacity: 4x4, 1-bit, signal 1.0, repair 5%; real YouTube stress-tested\n"
            << "  " << program << " stream-decode --url <stream_url> --output <file> [--password <pwd>] [--benchmark-json <report.json>]\n";
    std::cerr
        << "  " << program << " set-plan <input-file> <output-root> [Video Set options]\n"
        << "  " << program << " set-encode <input-file> <output-root> [Video Set options]\n"
        << "  " << program << " set-status --manifest <set_manifest.json>\n"
        << "  " << program << " set-inspect <video-or-folder>\n"
        << "  " << program << " set-recover <manifest-or-video-folder> <output-folder> [Video Set options]\n"
        << "  " << program << " set-help\n";
    std::cerr
        << "  " << program
        << " testlab generate --preset <quick|full> --output <folder>\n"
        << "    [--repair-percent <0..500>] [--input-size <64KiB|256KiB|1MiB>]\n"
        << "    [--data-type <random|compressible>] [--resolution <1080p|1440p|2160p>]\n"
        << "    [--minimum-upload-duration <seconds>=2] "
           "[--allow-low-disk]\n"
        << "  " << program
        << " testlab simulate --suite <manifest.json> --profile <yt-sim-profile>\n"
        << "  " << program
        << " testlab resume --suite <manifest.json> [--allow-low-disk]\n"
        << "  " << program
        << " testlab analyze --suite <manifest.json> [--case <case-id>] --video <downloaded-video>\n"
        << "    [--session-label <label>] [--record-new-observation]\n"
        << "  " << program
        << " testlab analyze-folder --suite <manifest.json> --folder <download-folder>\n"
        << "    [--session-label <label>] [--map <filename=case-id>] [--dry-run]\n"
        << "    [--record-new-observation]\n"
        << "  " << program
        << " testlab deduplicate --suite <manifest.json> [--dry-run|--apply]\n"
        << "  " << program
        << " testlab report --suite <manifest.json> --format <json|csv|markdown>\n"
        << "  " << program
        << " capacitylab estimate --preset <smoke|staged|boundary-1080p|onebit-verification-1080p|custom> --output <folder>\n"
        << "  " << program
        << " capacitylab run --preset <smoke|staged|boundary-1080p|onebit-verification-1080p|custom> --output <folder>\n"
        << "    [--block-size 8,6,4] [--bits-per-block 1,2] "
           "[--signal 0.75,1.0,1.25,1.5]\n"
        << "    [--repair-percent 0,1,2,5] "
           "[--resolution 1080p,2160p]\n"
        << "    [--simulation h264-medium,h264-heavy] "
           "[--max-cases 64] [--max-disk-gib 20]\n"
        << "  " << program
        << " capacitylab resume --manifest <manifest.json>\n"
        << "  " << program
        << " capacitylab shortlist --manifest <manifest.json> "
           "[--max-videos 8]\n"
        << " capacitylab validate --manifest <manifest.json> "
           "[--repair]\n"
        << "  " << program
        << " capacitylab analyze-folder --manifest <manifest.json> "
           "--folder <downloads> [--session-label <label>]\n"
        << "  " << program
        << " capacitylab report --manifest <manifest.json> "
           "--format <json|csv|markdown>\n"
        << "  " << program
        << " capacitylab boundary-report --manifest <manifest.json> "
           "--format <json|csv|markdown>\n"
        << "  " << program
        << " capacitylab boundary-status --manifest <manifest.json>\n"
        << " capacitylab onebit-report --manifest <manifest.json> --format markdown\n"
        << " capacitylab onebit-status --manifest <manifest.json>\n"
        << " capacitylab stress-plan --output <folder> --source-manifest <onebit-manifest.json>\n"
        << " capacitylab stress-stage --output <folder> --source-manifest <onebit-manifest.json>\n"
        << " capacitylab stress-status --manifest <manifest.json>\n"
        << " capacitylab stress-report --manifest <manifest.json> --format markdown\n"
        << " capacitylab stress-analyze --manifest <manifest.json>\n"
        << " capacitylab verify-source-payloads --manifest <manifest.json>\n"
        << "\nTest Lab only supports Resilient mode. Local simulation is not "
           "a guaranteed copy of YouTube processing.\n";
}

static uint64_t parse_testlab_size(const std::string &value) {
    std::size_t used = 0;
    const uint64_t number = std::stoull(value, &used);
    std::string suffix = value.substr(used);
    for (char &c : suffix)
        c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
    uint64_t multiplier = 1;
    if (suffix == "kib" || suffix == "kb") multiplier = 1024;
    else if (suffix == "mib" || suffix == "mb")
        multiplier = 1024 * 1024;
    else if (!suffix.empty() && suffix != "b")
        throw std::invalid_argument("invalid size suffix");
    if (number > std::numeric_limits<uint64_t>::max() / multiplier)
        throw std::overflow_error("input size overflow");
    return number * multiplier;
}

static std::pair<int, int> parse_testlab_resolution(
    const std::string &value) {
    if (value == "1080p") return {1920, 1080};
    if (value == "1440p") return {2560, 1440};
    if (value == "2160p" || value == "4k") return {3840, 2160};
    const auto x = value.find('x');
    if (x == std::string::npos)
        throw std::invalid_argument("invalid resolution");
    const int width = std::stoi(value.substr(0, x));
    const int height = std::stoi(value.substr(x + 1));
    if (width <= 0 || height <= 0 ||
        width % 8 != 0 || height % 8 != 0)
        throw std::invalid_argument("invalid resolution");
    return {width, height};
}

static int do_testlab(const int argc, char *argv[]) {
    using namespace youtube_test_lab;
    if (argc < 3) {
        std::cerr << "Error: missing testlab subcommand\n";
        print_usage(argv[0]);
        return 1;
    }
    const std::string subcommand = argv[2];
    std::string preset = "quick";
    std::string output;
    std::string suite;
    std::string profile = "yt-sim-1080p-medium";
    std::string case_id;
    std::string video;
    std::string folder;
    std::string session_label;
    std::string format = "markdown";
    std::string cancel_file;
    std::map<std::string, std::string> manual_mappings;
    bool allow_low_disk = false;
    bool estimate_only = false;
    bool dry_run = false;
    bool apply = false;
    bool record_new_observation = false;
    bool custom_repair = false;
    bool custom_size = false;
    bool custom_type = false;
    bool custom_resolution = false;
    double minimum_upload_duration =
        kMinimumUploadDurationSeconds;
    MatrixOptions matrix;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char *name) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(
                    std::string("missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--preset") preset = require_value("--preset");
        else if (arg == "--output") output = require_value("--output");
        else if (arg == "--suite") suite = require_value("--suite");
        else if (arg == "--profile" ||
                 arg == "--simulation-profile")
            profile = require_value(arg.c_str());
        else if (arg == "--case") case_id = require_value("--case");
        else if (arg == "--video") video = require_value("--video");
        else if (arg == "--folder") folder = require_value("--folder");
        else if (arg == "--session-label")
            session_label = require_value("--session-label");
        else if (arg == "--map") {
            const auto mapping = require_value("--map");
            const auto equals = mapping.find('=');
            if (equals == std::string::npos || equals == 0 ||
                equals + 1 == mapping.size())
                throw std::invalid_argument(
                    "--map must use filename=case-id");
            manual_mappings[mapping.substr(0, equals)] =
                mapping.substr(equals + 1);
        }
        else if (arg == "--format") format = require_value("--format");
        else if (arg == "--cancel-file")
            cancel_file = require_value("--cancel-file");
        else if (arg == "--allow-low-disk") allow_low_disk = true;
        else if (arg == "--estimate-only") estimate_only = true;
        else if (arg == "--dry-run") dry_run = true;
        else if (arg == "--apply") apply = true;
        else if (arg == "--record-new-observation")
            record_new_observation = true;
        else if (arg == "--minimum-upload-duration") {
            const std::string value =
                require_value("--minimum-upload-duration");
            std::size_t used = 0;
            minimum_upload_duration = std::stod(value, &used);
            if (used != value.size() ||
                !std::isfinite(minimum_upload_duration) ||
                minimum_upload_duration <
                    kMinimumUploadDurationSeconds)
                throw std::invalid_argument(
                    "minimum upload duration must be at least 2 seconds");
        }
        else if (arg == "--mode") {
            const auto mode = require_value("--mode");
            if (mode == "fast-local")
                throw std::invalid_argument(
                    "Fast Local is not designed for lossy YouTube processing.");
            if (mode != "resilient")
                throw std::invalid_argument("invalid Test Lab mode");
        } else if (arg == "--repair-percent") {
            if (!custom_repair) matrix.repair_percentages.clear();
            matrix.repair_percentages.push_back(
                parse_repair_percentage(require_value("--repair-percent")));
            custom_repair = true;
        } else if (arg == "--reliability-profile") {
            if (!custom_repair) matrix.repair_percentages.clear();
            const auto resolved = reliability_options_for_profile(
                parse_reliability_profile(
                    require_value("--reliability-profile")));
            matrix.repair_percentages.push_back(
                repair_ratio_to_percentage(resolved.repair_ratio));
            custom_repair = true;
        } else if (arg == "--input-size") {
            if (!custom_size) matrix.input_sizes.clear();
            matrix.input_sizes.push_back(
                parse_testlab_size(require_value("--input-size")));
            custom_size = true;
        } else if (arg == "--data-type") {
            if (!custom_type) matrix.data_types.clear();
            const auto value = require_value("--data-type");
            if (value == "random")
                matrix.data_types.push_back(DataType::Random);
            else if (value == "compressible")
                matrix.data_types.push_back(DataType::Compressible);
            else
                throw std::invalid_argument("invalid data type");
            custom_type = true;
        } else if (arg == "--resolution") {
            if (!custom_resolution) matrix.resolutions.clear();
            matrix.resolutions.push_back(
                parse_testlab_resolution(
                    require_value("--resolution")));
            custom_resolution = true;
        } else {
            throw std::invalid_argument(
                "unknown testlab option: " + arg);
        }
    }

    try {
        const auto continue_running = [&] {
            return cancel_file.empty() ||
                !std::filesystem::exists(cancel_file);
        };
        if (subcommand == "generate") {
            if (output.empty())
                throw std::invalid_argument(
                    "--output is required for testlab generate");
            if (preset != "quick" && preset != "full")
                throw std::invalid_argument(
                    "invalid preset; expected quick or full");
            MatrixOptions defaults =
                preset == "quick" ? quick_matrix() : full_matrix();
            if (!custom_repair)
                matrix.repair_percentages =
                    defaults.repair_percentages;
            if (!custom_size)
                matrix.input_sizes = defaults.input_sizes;
            if (!custom_type)
                matrix.data_types = defaults.data_types;
            if (!custom_resolution)
                matrix.resolutions = defaults.resolutions;
            if (!custom_size && !custom_type)
                matrix.input_variants = defaults.input_variants;
            else
                matrix.input_variants.clear();
            matrix.fps = defaults.fps;
            matrix.minimum_upload_duration_seconds =
                minimum_upload_duration;
            const auto preview_id = create_suite_id();
            const auto cases = build_matrix(matrix, preview_id);
            const auto estimate = estimate_suite(cases, output);
            std::cout
                << "YouTube Test Lab preflight:\n"
                << "  Cases: " << estimate.case_count << "\n"
                << "  Estimated frames: "
                << estimate.estimated_total_frames << "\n"
                << "  Estimated duration: "
                << estimate.estimated_total_duration_seconds << " s\n"
                << "  Minimum per candidate: "
                << minimum_upload_duration << " s / "
                << minimum_frames_for_duration(
                       minimum_upload_duration, matrix.fps)
                << " frames\n"
                << "  Estimated output: "
                << format_size(estimate.estimated_output_bytes) << "\n"
                << "  Safety margin: "
                << format_size(estimate.safety_margin_bytes) << "\n"
                << "  Required disk: "
                << format_size(estimate.required_disk_bytes) << "\n";
            if (estimate.available_disk_bytes)
                std::cout << "  Available disk: "
                          << format_size(*estimate.available_disk_bytes)
                          << "\n";
            if (!allow_low_disk && !estimate.disk_space_sufficient)
                throw std::runtime_error(
                    "insufficient disk space (use --allow-low-disk "
                    "only after reviewing the estimate)");
            if (estimate_only) {
                std::cout << "Estimate only: no suite files were created.\n";
                return 0;
            }
            const auto manifest = create_suite(
                output, preset, matrix, allow_low_disk,
                [&](const Progress &p) {
                    std::cout << "\rCase " << p.completed_cases << "/"
                              << p.total_cases << " " << p.active_case
                              << " " << static_cast<int>(
                                     p.case_progress * 100.0)
                              << "%   " << std::flush;
                    return continue_running();
                });
            std::cout << "\nSuite generated: "
                      << (std::filesystem::path(output) /
                          "youtube_test_lab" / manifest.suite_id /
                          "manifest.json").string()
                      << "\n";
            for (const auto &item : manifest.cases) {
                std::cout
                    << "Case " << item.test_case_id << ":\n"
                    << "  Requested input bytes: "
                    << item.requested_input_size << "\n"
                    << "  Effective input bytes: "
                    << item.effective_input_size << "\n"
                    << "  Minimum frames: "
                    << item.minimum_required_frames << "\n"
                    << "  Expected frames: "
                    << item.expected_encoded_frames << "\n"
                    << "  Actual frames: "
                    << item.actual_candidate_frames << "\n"
                    << "  Candidate duration: "
                    << item.candidate_duration_seconds << " s\n"
                    << "  Candidate validation: "
                    << (item.candidate_validation_error.empty()
                            ? "passed"
                            : item.candidate_validation_error)
                    << "\n"
                    << "  YouTube ready: "
                    << (item.candidate_ready_for_youtube
                            ? "Yes" : "No")
                    << "\n";
            }
            if (!continue_running()) {
                std::cerr << "Test Lab generation cancelled; completed "
                             "cases were preserved for resume.\n";
                return 130;
            }
            const bool any_failed = std::any_of(
                manifest.cases.begin(), manifest.cases.end(),
                [](const TestCase &item) {
                    return item.state == CaseState::Failed;
                });
            if (any_failed)
                std::cerr
                    << "One or more cases failed; inspect manifest "
                       "notes and use testlab resume after correcting "
                       "the cause.\n";
            return any_failed ? 2 : 0;
        }
        if (subcommand == "simulate") {
            if (suite.empty())
                throw std::invalid_argument("--suite is required");
            const auto selected = find_simulation_profile(profile);
            if (!selected)
                throw std::invalid_argument(
                    "invalid simulation profile: " + profile);
            std::cout
                << "WARNING: Local simulation is not a guaranteed "
                   "copy of YouTube processing.\n";
            simulate_suite(suite, *selected,
                [&](const Progress &p) {
                    std::cout << "\rSimulating " << p.completed_cases
                              << "/" << p.total_cases << " "
                              << p.active_case << "   " << std::flush;
                    return continue_running();
                });
            std::cout << "\nSimulation complete.\n";
            if (!continue_running()) {
                std::cerr << "Test Lab simulation cancelled.\n";
                return 130;
            }
            const auto updated = read_manifest(suite);
            const bool profile_failed = std::any_of(
                updated.cases.begin(), updated.cases.end(),
                [&](const TestCase &item) {
                    return !item.results.empty() &&
                        item.results.back().simulation_profile ==
                            selected->name &&
                        item.results.back().final_status !=
                            FinalStatus::Pass;
                });
            return profile_failed ? 2 : 0;
        }
        if (subcommand == "resume") {
            if (suite.empty())
                throw std::invalid_argument("--suite is required");
            resume_suite(suite, allow_low_disk,
                [&](const Progress &p) {
                    std::cout << "\rResuming " << p.completed_cases
                              << "/" << p.total_cases << " "
                              << p.active_case << "   " << std::flush;
                    return continue_running();
                });
            std::cout << "\nResume complete.\n";
            if (!continue_running()) {
                std::cerr << "Test Lab resume cancelled.\n";
                return 130;
            }
            const auto updated = read_manifest(suite);
            return std::any_of(
                updated.cases.begin(), updated.cases.end(),
                [](const TestCase &item) {
                    return item.state == CaseState::Failed;
                }) ? 2 : 0;
        }
        if (subcommand == "analyze") {
            if (suite.empty() || video.empty())
                throw std::invalid_argument(
                    "--suite and --video are required");
            const auto manifest = read_manifest(suite);
            if (case_id.empty()) {
                const auto detected =
                    case_id_from_filename(manifest, video);
                if (!detected)
                    throw std::invalid_argument(
                        "case ID was not uniquely found in the filename; "
                        "use --case");
                case_id = *detected;
            }
            const auto it = std::find_if(
                manifest.cases.begin(), manifest.cases.end(),
                [&](const TestCase &c) {
                    return c.test_case_id == case_id;
                });
            if (it == manifest.cases.end())
                throw std::invalid_argument("unknown case ID");
            AnalysisOptions options;
            options.session_label = session_label;
            options.record_new_observation =
                record_new_observation;
            const auto outcome = analyze_real_video(
                suite, case_id, video, options);
            const auto &result = outcome.result;
            if (outcome.duplicate) {
                std::cout
                    << "This video has already been analyzed for this case.\n"
                    << "Observation ID: " << result.observation_id << "\n"
                    << "Analyzed at: " << result.analyzed_at_utc << "\n"
                    << "Status: " << to_string(result.final_status) << "\n";
                return 0;
            }
            std::cout << "Source: Real YouTube roundtrip\n"
                      << "Case: " << case_id << "\n"
                      << "Observation ID: " << result.observation_id << "\n"
                      << "Session: " << result.analysis_session_id << "\n"
                      << "Packet recovery: "
                      << result.packet_recovery_percentage << "%\n"
                      << "SHA-256: "
                      << (result.sha256_match ? "match" : "mismatch")
                      << "\nStatus: "
                      << to_string(result.final_status) << "\n";
            return result.final_status == FinalStatus::Pass ? 0 : 2;
        }
        if (subcommand == "analyze-folder") {
            if (suite.empty() || folder.empty())
                throw std::invalid_argument(
                    "--suite and --folder are required");
            const auto manifest = read_manifest(suite);
            const auto preview = preview_analysis_folder(
                manifest, folder, manual_mappings);
            std::cout
                << "Batch analysis preview:\n"
                << "Filename\tCase\tResolution\tCodec\tSize\tStatus\tDuplicate\n";
            bool needs_mapping = false;
            for (const auto &item : preview) {
                const auto matched_case =
                    !item.user_case_id.empty()
                        ? item.user_case_id
                        : item.detected_case_id.value_or("-");
                std::cout
                    << item.filename << "\t" << matched_case << "\t"
                    << item.video.width << "x" << item.video.height << "\t"
                    << (item.video.codec.empty()
                            ? "unavailable" : item.video.codec)
                    << "\t" << item.file_size << "\t"
                    << to_string(item.status) << "\t"
                    << (item.duplicate_observation ? "yes" : "no")
                    << "\n";
                needs_mapping =
                    needs_mapping ||
                    item.status == BatchMatchStatus::NeedsMapping ||
                    item.status ==
                        BatchMatchStatus::DuplicateCaseConflict;
            }
            if (dry_run) {
                std::cout << "Dry run: manifest was not changed.\n";
                return needs_mapping ? 2 : 0;
            }
            if (needs_mapping)
                throw std::invalid_argument(
                    "one or more files need an explicit --map "
                    "filename=case-id selection");
            AnalysisOptions options;
            options.session_label = session_label;
            options.source_folder =
                std::filesystem::absolute(folder).string();
            options.record_new_observation =
                record_new_observation;
            const auto summary = analyze_folder(
                suite, folder, manual_mappings, options,
                [&](const Progress &p) {
                    std::cout
                        << "\rAnalyzing " << p.completed_cases << "/"
                        << p.total_cases << " " << p.active_case
                        << "   " << std::flush;
                    return continue_running();
                });
            std::cout
                << "\nSession: " << summary.analysis_session_id
                << "\nDiscovered: " << summary.discovered
                << "\nAnalyzed: " << summary.analyzed
                << "\nDuplicates skipped: "
                << summary.duplicates_skipped
                << "\nNeeds mapping: " << summary.needs_mapping
                << "\nUnsupported: " << summary.unsupported << "\n";
            if (summary.cancelled) {
                std::cerr
                    << "Batch analysis cancelled; completed observations "
                       "were preserved.\n";
                return 130;
            }
            return summary.needs_mapping || summary.unsupported ? 2 : 0;
        }
        if (subcommand == "deduplicate") {
            if (suite.empty())
                throw std::invalid_argument("--suite is required");
            if (dry_run && apply)
                throw std::invalid_argument(
                    "--dry-run and --apply are mutually exclusive");
            const auto summary = deduplicate_results(suite, apply);
            std::cout
                << (summary.applied ? "Deduplicate applied" :
                                      "Deduplicate dry run")
                << "\nObservations scanned: "
                << summary.observations_scanned
                << "\nDuplicate groups: " << summary.duplicate_groups
                << "\nDuplicate observations: "
                << summary.duplicate_observations
                << "\nObservations after apply: "
                << summary.observations_after_apply << "\n";
            if (!summary.backup_path.empty())
                std::cout << "Backup: "
                          << summary.backup_path.string() << "\n";
            for (const auto &message : summary.messages)
                std::cout << "  " << message << "\n";
            if (!apply)
                std::cout
                    << "No changes made. Re-run with --apply after review.\n";
            return 0;
        }
        if (subcommand == "report") {
            if (suite.empty())
                throw std::invalid_argument("--suite is required");
            if (format != "json" && format != "csv" &&
                format != "markdown" && format != "md")
                throw std::invalid_argument(
                    "invalid report format");
            const auto manifest = read_manifest(suite);
            const auto reports =
                std::filesystem::absolute(suite).parent_path() /
                "reports";
            write_reports(manifest, reports);
            const std::string extension =
                format == "markdown" || format == "md"
                    ? "md" : format;
            std::cout << "Report: "
                      << (reports / ("report." + extension)).string()
                      << "\n";
            return 0;
        }
        throw std::invalid_argument(
            "unknown testlab subcommand: " + subcommand);
    } catch (const std::exception &error) {
        std::cerr << "Test Lab error: " << error.what() << "\n";
        return 1;
    }
}

static std::vector<std::string> split_csv(
    const std::string &value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const std::string token = value.substr(
            begin, comma == std::string::npos
                       ? std::string::npos : comma - begin);
        if (token.empty())
            throw std::invalid_argument(
                "empty value in comma-separated option");
        result.push_back(token);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return result;
}

static int do_capacitylab(const int argc, char *argv[]) {
    using namespace youtube_capacity_lab;
    if (argc < 3)
        throw std::invalid_argument(
            "missing capacitylab subcommand");
    RunOptions options;
    std::string subcommand = argv[2];
    if (subcommand == "stress-plan") {
        subcommand = "estimate";
        options.preset = Preset::OneBitStressValidation1080p;
    } else if (subcommand == "stress-stage") {
        subcommand = "run";
        options.preset = Preset::OneBitStressValidation1080p;
    }
    std::string manifest_path;
    std::string returned_folder;
    std::string session_label;
    std::string report_format = "markdown";
    std::string cancel_file;
    bool custom_matrix = false;
    bool block_set = false;
    bool bits_set = false;
    bool signal_set = false;
    bool repair_set = false;
    bool resolution_set = false;
    bool simulation_set = false;
    bool repair_manifest = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char *name) {
            if (i + 1 >= argc)
                throw std::invalid_argument(
                    std::string("missing value for ") + name);
            return std::string(argv[++i]);
        };
        if (arg == "--preset") {
            const std::string value = require_value("--preset");
            if (value == "smoke") options.preset = Preset::Smoke;
            else if (value == "staged")
                options.preset = Preset::Staged;
            else if (value == "boundary-1080p")
                options.preset = Preset::Boundary1080p;
            else if (value == "onebit-verification-1080p")
                options.preset = Preset::OneBitVerification1080p;
            else if (value == "onebit-stress-1080p")
                options.preset = Preset::OneBitStressValidation1080p;
            else if (value == "custom")
                options.preset = Preset::Custom;
            else
                throw std::invalid_argument(
                    "capacity preset must be smoke, staged, "
                    "boundary-1080p, onebit-verification-1080p, "
                    "onebit-stress-1080p, or custom");
        } else if (arg == "--output") {
            options.output_root = require_value("--output");
        } else if (arg == "--manifest" || arg == "--suite") {
            manifest_path = require_value(arg.c_str());
        } else if (arg == "--source-manifest") {
            options.source_manifest = require_value("--source-manifest");
        } else if (arg == "--folder") {
            returned_folder = require_value("--folder");
        } else if (arg == "--session-label") {
            session_label = require_value("--session-label");
        } else if (arg == "--format") {
            report_format = require_value("--format");
        } else if (arg == "--cancel-file") {
            cancel_file = require_value("--cancel-file");
        } else if (arg == "--max-cases") {
            options.maximum_cases = std::stoull(
                require_value("--max-cases"));
        } else if (arg == "--max-disk-gib") {
            const double gib = std::stod(
                require_value("--max-disk-gib"));
            if (!std::isfinite(gib) || gib < 0.5 ||
                gib > 1024.0)
                throw std::invalid_argument(
                    "max disk must be between 0.5 and 1024 GiB");
            options.maximum_disk_bytes =
                static_cast<uint64_t>(
                    gib * 1024.0 * 1024.0 * 1024.0);
        } else if (arg == "--max-shortlist-videos" ||
                   arg == "--max-videos") {
            options.maximum_shortlist_videos =
                std::stoull(require_value(arg.c_str()));
            if (options.maximum_shortlist_videos == 0 ||
                options.maximum_shortlist_videos > 12)
                throw std::invalid_argument(
                    "shortlist limit must be between 1 and 12");
        } else if (arg == "--block-size") {
            if (!block_set) options.block_sizes.clear();
            for (const auto &value :
                 split_csv(require_value("--block-size")))
                options.block_sizes.push_back(std::stoi(value));
            block_set = true;
            custom_matrix = true;
        } else if (arg == "--bits-per-block") {
            if (!bits_set) options.bits_per_block.clear();
            for (const auto &value :
                 split_csv(require_value("--bits-per-block")))
                options.bits_per_block.push_back(std::stoi(value));
            bits_set = true;
            custom_matrix = true;
        } else if (arg == "--signal") {
            if (!signal_set) options.signal_milli.clear();
            for (const auto &value :
                 split_csv(require_value("--signal"))) {
                const double signal = std::stod(value);
                options.signal_milli.push_back(
                    static_cast<int>(std::llround(signal * 1000.0)));
            }
            signal_set = true;
            custom_matrix = true;
        } else if (arg == "--repair-percent") {
            if (!repair_set)
                options.repair_basis_points.clear();
            for (const auto &value :
                 split_csv(require_value("--repair-percent"))) {
                const double repair = std::stod(value);
                options.repair_basis_points.push_back(
                    static_cast<int>(std::llround(repair * 100.0)));
            }
            repair_set = true;
            custom_matrix = true;
        } else if (arg == "--resolution") {
            if (!resolution_set) options.resolutions.clear();
            for (const auto &value :
                 split_csv(require_value("--resolution"))) {
                if (value == "1080p")
                    options.resolutions.emplace_back(1920, 1080);
                else if (value == "2160p" || value == "4k")
                    options.resolutions.emplace_back(3840, 2160);
                else
                    throw std::invalid_argument(
                        "Capacity Lab resolution must be 1080p or 2160p");
            }
            resolution_set = true;
            custom_matrix = true;
        } else if (arg == "--simulation") {
            if (!simulation_set) options.simulations.clear();
            for (const auto &value :
                 split_csv(require_value("--simulation")))
                options.simulations.push_back(value);
            simulation_set = true;
        } else if (arg == "--estimate-only") {
            options.estimate_only = true;
        } else if (arg == "--allow-low-disk") {
            options.allow_low_disk = true;
        } else if (arg ==
                   "--include-simulation-failures") {
            options.include_simulation_failures = true;
        } else if (arg == "--repair") {
            repair_manifest = true;
        } else {
            throw std::invalid_argument(
                "unknown capacitylab option: " + arg);
        }
    }
    if (custom_matrix && options.preset != Preset::Custom)
        options.preset = Preset::Custom;
    auto print_preflight = [](const Preflight &value) {
        std::cout
            << "YouTube Capacity Lab preflight:\n"
            << "  Raw combinations: "
            << value.raw_combination_count << "\n"
            << "  Staged maximum cases: "
            << value.staged_maximum_cases << "\n"
            << "  Estimated transcodes: "
            << value.estimated_transcodes << "\n"
            << "  Estimated total frames: "
            << value.estimated_total_frames << "\n"
            << "  Estimated output: "
            << format_size(value.estimated_output_bytes) << "\n"
            << "  Required with safety margin: "
            << format_size(value.required_disk_bytes) << "\n"
            << "  Estimated seconds: "
            << std::fixed << std::setprecision(1)
            << value.estimated_seconds << "\n"
            << "  Disk sufficient: "
            << (value.disk_space_sufficient ? "yes" : "no")
            << "\n";
    };
    if (subcommand == "estimate") {
        if (options.output_root.empty())
            throw std::invalid_argument(
                "--output is required for capacitylab estimate");
        options.estimate_only = true;
        print_preflight(estimate(options));
        return 0;
    }
    if (subcommand == "run") {
        if (options.output_root.empty())
            throw std::invalid_argument(
                "--output is required for capacitylab run");
        print_preflight(estimate(options));
        const auto result = run(options, [&](const Progress &value) {
            std::cout << "CAPACITY_PROGRESS "
                << value.completed_cases << "/"
                << value.total_cases << " stage="
                << value.stage << " config="
                << value.active_config_id << " disk="
                << value.disk_used_bytes << "\n";
            return cancel_file.empty() ||
                !std::filesystem::exists(cancel_file);
        });
        const auto path =
            options.output_root /
            (options.preset == Preset::Boundary1080p
                 ? "youtube_boundary_lab"
                 : options.preset == Preset::OneBitVerification1080p
                     ? "youtube_1bit_lab"
                 : options.preset == Preset::OneBitStressValidation1080p
                     ? "youtube_1bit_stress" : "youtube_capacity_lab") /
            result.experiment_id / "manifest.json";
        std::cout << "CAPACITY_MANIFEST "
                  << std::filesystem::absolute(path).string()
                  << "\n";
        return 0;
    }
    if (manifest_path.empty())
        throw std::invalid_argument(
            "--manifest is required for this capacitylab command");
    if (subcommand == "resume") {
        resume(manifest_path, [&](const Progress &value) {
            std::cout << "CAPACITY_PROGRESS "
                << value.completed_cases << "/"
                << value.total_cases << " stage="
                << value.stage << " config="
                << value.active_config_id << "\n";
            return cancel_file.empty() ||
                !std::filesystem::exists(cancel_file);
        });
        return 0;
    }
    if (subcommand == "shortlist") {
        const auto result = generate_shortlist(
            manifest_path, options.maximum_shortlist_videos);
        std::cout
            << "CAPACITY_ELIGIBLE "
            << result.eligible_configs << "\n"
            << "CAPACITY_SHORTLIST_SELECTED "
            << result.selected_configs << "\n";
        for (const auto &rejected : result.rejected_configs)
            std::cout << "CAPACITY_REJECTED "
                      << rejected << "\n";
        for (const auto &removed : result.removed_files)
            std::cout << "CAPACITY_SHORTLIST_REMOVED "
                      << removed << "\n";
        for (const auto &created : result.created_files)
            std::cout << "CAPACITY_SHORTLIST_CREATED "
                      << created << "\n";
        std::cout << "CAPACITY_MANIFEST_BACKUP "
                  << result.manifest_backup.string() << "\n";
        if (!result.previous_shortlist_archive.empty())
            std::cout << "CAPACITY_SHORTLIST_ARCHIVE "
                      << result.previous_shortlist_archive.string()
                      << "\n";
        return 0;
    }
    if (subcommand == "validate") {
        auto validation =
            validate_experiment(manifest_path);
        auto print_validation =
            [](const ValidationReport &value) {
                std::cout
                    << "CAPACITY_VALIDATE configs="
                    << value.total_configs
                    << " eligible=" << value.eligible_configs
                    << " rejected=" << value.rejected_configs
                    << " incomplete=" << value.incomplete_configs
                    << " pareto=" << value.pareto_configs
                    << " shortlisted="
                    << value.shortlisted_configs
                    << " errors=" << value.issues.size()
                    << "\n";
                for (const auto &issue : value.issues)
                    std::cout
                        << "CAPACITY_CONSISTENCY_ERROR "
                        << issue.code << " config="
                        << (issue.config_id.empty()
                                ? "-" : issue.config_id)
                        << " detail=" << issue.detail << "\n";
            };
        print_validation(validation);
        if (!repair_manifest)
            return validation.issues.empty() ? 0 : 2;
        const auto result = generate_shortlist(
            manifest_path, options.maximum_shortlist_videos);
        std::cout << "CAPACITY_MANIFEST_BACKUP "
                  << result.manifest_backup.string() << "\n";
        validation = validate_experiment(manifest_path);
        print_validation(validation);
        return validation.issues.empty() ? 0 : 2;
    }
    if (subcommand == "analyze-folder") {
        if (returned_folder.empty())
            throw std::invalid_argument(
                "--folder is required for analyze-folder");
        analyze_folder(
            manifest_path, returned_folder, session_label);
        return 0;
    }
    if (subcommand == "boundary-status") {
        const auto manifest = read_manifest(manifest_path);
        if (manifest.preset != Preset::Boundary1080p)
            throw std::invalid_argument(
                "boundary-status requires a boundary-1080p manifest");
        const auto status = infer_boundary(manifest);
        std::cout
            << "BOUNDARY_BASELINE "
            << status.baseline_status << "\n";
        for (const auto &density : status.densities) {
            const char *evidence =
                density.evidence == DensityEvidence::Pass
                    ? "pass"
                    : density.evidence == DensityEvidence::Fail
                        ? "fail" : "untested";
            std::cout << "BOUNDARY_DENSITY "
                << std::fixed << std::setprecision(2)
                << density.gain << "x " << evidence
                << " profiles=" << density.profiles_tested
                << " exact=" << density.exact_passes
                << " failures=" << density.failures << "\n";
        }
        std::cout << "BOUNDARY_HIGHEST_EXACT "
            << (status.highest_exact_pass
                    ? std::to_string(
                        *status.highest_exact_pass) + "x"
                    : "none") << "\n"
            << "BOUNDARY_LOWEST_FAILURE "
            << (status.lowest_failure_above
                    ? std::to_string(
                        *status.lowest_failure_above) + "x"
                    : "none") << "\n"
            << "BOUNDARY_BRACKET " << status.bracket << "\n"
            << "BOUNDARY_NON_MONOTONIC "
            << (status.non_monotonic ? "yes" : "no") << "\n"
            << "BOUNDARY_NEXT " << status.next_experiment << "\n";
        return 0;
    }
    if (subcommand == "verify-source-payloads") {
        verify_source_payloads(manifest_path);
        std::cout << "ONEBIT_SOURCE_PAYLOADS exact\n";
        return 0;
    }
    if (subcommand == "onebit-status") {
        const auto manifest = read_manifest(manifest_path);
        if (manifest.preset != Preset::OneBitVerification1080p)
            throw std::invalid_argument(
                "onebit-status requires a onebit-verification-1080p manifest");
        const auto status = infer_onebit_geometry(manifest);
        std::cout << "ONEBIT_PRODUCTION_CONTROL " << status.production_control << "\n"
                  << "ONEBIT_SOURCE_VALIDATION " << manifest.source_payload_validation << "\n"
                  << "ONEBIT_HISTORICAL_EVIDENCE " << manifest.historical_evidence.size() << "\n";
        for (const auto &c : manifest.cases)
            std::cout << "ONEBIT_CASE " << c.case_id << " "
                      << real_youtube_status(c) << "\n";
        for (const auto &d : status.densities)
            std::cout << "ONEBIT_GEOMETRY " << d.block_size << "x" << d.block_size
                      << " gain=" << d.gain << " status="
                      << static_cast<int>(d.evidence) << "\n";
        std::cout << "ONEBIT_4X " << status.four_x_state << "\n"
                  << "ONEBIT_BOUNDARY " << status.boundary_bracket << "\n"
                  << "ONEBIT_SAFE " << (status.safe_candidate.empty() ? "none" : status.safe_candidate) << "\n"
                  << "ONEBIT_BALANCED " << (status.balanced_candidate.empty() ? "none" : status.balanced_candidate) << "\n"
                  << "ONEBIT_EXPERIMENTAL " << (status.experimental_candidate.empty() ? "none" : status.experimental_candidate) << "\n"
                  << "ONEBIT_NON_MONOTONIC " << (status.non_monotonic ? "yes" : "no") << "\n"
                  << "ONEBIT_RETEST_REQUIRED " << (status.retest_required ? "yes" : "no") << "\n"
                  << "ONEBIT_NEXT " << status.recommended_next_experiment << "\n";
        return 0;
    }
    if (subcommand == "stress-status") {
        const auto manifest = read_manifest(manifest_path);
        if (manifest.preset != Preset::OneBitStressValidation1080p)
            throw std::invalid_argument(
                "stress-status requires a onebit-stress-1080p manifest");
        const auto status = infer_stress_validation(manifest);
        std::cout << "STRESS_PRODUCTION "
                  << status.production_recommendation << "\n"
                  << "STRESS_4X_EXACT " << status.four_x_exact_case_count
                  << "/" << status.four_x_case_count << "\n"
                  << "STRESS_4X_FAILURES "
                  << status.four_x_failure_case_count << "\n";
        for (std::size_t i = 0; i < manifest.cases.size(); ++i)
            std::cout << "STRESS_CASE " << manifest.cases[i].case_id
                      << " local="
                      << (manifest.cases[i].upload_eligible
                              ? "pass" : "failed")
                      << " real=" << status.cases[i].current_status << "\n";
        for (const auto &candidate : status.repair_upload_shortlist)
            std::cout << "STRESS_REPAIR_UPLOAD " << candidate << "\n";
        return 0;
    }
    if (subcommand == "stress-analyze") {
        const auto manifest = read_manifest(manifest_path);
        if (manifest.preset != Preset::OneBitStressValidation1080p)
            throw std::invalid_argument(
                "stress-analyze requires a onebit-stress-1080p manifest");
        const auto root = std::filesystem::absolute(manifest_path).parent_path();
        analyze_folder(manifest_path, root / "returned");
        return 0;
    }
    if (subcommand == "report" ||
        subcommand == "boundary-report" ||
        subcommand == "onebit-report" ||
        subcommand == "stress-report") {
        if (report_format != "markdown" &&
            report_format != "json" &&
            report_format != "csv")
            throw std::invalid_argument(
                "report format must be markdown, json, or csv");
        const auto manifest = read_manifest(manifest_path);
        if (subcommand == "boundary-report" &&
            manifest.preset != Preset::Boundary1080p)
            throw std::invalid_argument(
                "boundary-report requires a boundary-1080p manifest");
        if (subcommand == "onebit-report" &&
            manifest.preset != Preset::OneBitVerification1080p)
            throw std::invalid_argument(
                "onebit-report requires a onebit-verification-1080p manifest");
        if (subcommand == "stress-report" &&
            manifest.preset != Preset::OneBitStressValidation1080p)
            throw std::invalid_argument(
                "stress-report requires a onebit-stress-1080p manifest");
        write_reports(
            manifest,
            std::filesystem::absolute(manifest_path)
                .parent_path() / "reports");
        return 0;
    }
    throw std::invalid_argument(
        "unknown capacitylab subcommand: " + subcommand);
}

static int do_encode(const std::string &input_path, const std::string &output_path,
                     const bool encrypt, const std::string &password,
                     const ms_hash_algorithm_t hash_algo,
                     const EncodingReliabilityOptions &reliability,
                     const std::string &benchmark_json,
                     const std::string &estimate_json,
                     const bool estimate_only,
                     const bool enable_probe,
                     const bool allow_low_disk,
                     const std::string &reliability_profile,
                     const ms_encoding_mode_t encoding_mode) {
    std::cout << "Input: " << input_path << "\n";
    std::cout << "Output: " << output_path << "\n";

    ms_encode_options_t opts{};
    opts.input_path = input_path.c_str();
    opts.output_path = output_path.c_str();
    opts.encrypt = encrypt ? 1 : 0;
    opts.password = password.c_str();
    opts.password_len = password.size();
    opts.hash_algorithm = hash_algo;
    opts.progress = encode_progress;
    opts.progress_user = nullptr;
    opts.repair_ratio = reliability.repair_ratio;
    opts.repair_ratio_is_set = 1;
    opts.encoding_mode = encoding_mode;

    ms_encoding_estimate_t estimate{};
    if (const ms_status_t status =
            ms_estimate_encode(
                &opts, enable_probe ? 1 : 0, &estimate);
        status != MS_OK) {
        std::cerr << "Preflight failed: "
                  << ms_status_string(status) << "\n";
        return 1;
    }
    print_encoding_estimate(estimate, reliability_profile);
    if (!estimate_json.empty()) {
        if (const ms_status_t status =
                ms_write_encoding_estimate_json(
                    &estimate, estimate_json.c_str());
            status != MS_OK) {
            std::cerr << "Error writing estimate JSON: "
                      << ms_status_string(status) << "\n";
            return 1;
        }
        std::cout << "Estimate JSON: " << estimate_json << "\n";
    }
    if (estimate_only && !benchmark_json.empty() &&
        benchmark_json != estimate_json) {
        if (const ms_status_t status =
                ms_write_encoding_estimate_json(
                    &estimate, benchmark_json.c_str());
            status != MS_OK) {
            std::cerr << "Error writing estimate JSON through "
                         "--benchmark-json: "
                      << ms_status_string(status) << "\n";
            return 1;
        }
        std::cout
            << "Estimate JSON (--benchmark-json alias): "
            << benchmark_json << "\n";
    }

    const bool low_disk_only =
        estimate.low_disk_override_permitted != 0;
    if (low_disk_only) {
        print_insufficient_disk_details(
            estimate, allow_low_disk);
    }
    if (!estimate.can_start_encoding &&
        !(low_disk_only && allow_low_disk)) {
        if (estimate_only && low_disk_only) return 0;
        return 1;
    }
    if (estimate_only) return 0;
    opts.preflight_estimate = &estimate;
    opts.preflight_duration_seconds =
        estimate.preflight_duration_seconds;
    opts.allow_low_disk = allow_low_disk ? 1 : 0;

    ms_result_t result{};
    if (const ms_status_t status = ms_encode(&opts, &result); status != MS_OK) {
        std::cout << "\n";
        std::cerr << "Error: " << ms_status_string(status) << "\n";
        if (status == MS_ERR_PREFLIGHT_STALE) {
            std::cerr
                << "The input changed after estimation; rerun the "
                   "command to obtain a fresh preflight estimate.\n";
        } else if (status == MS_ERR_INSUFFICIENT_DISK) {
            print_insufficient_disk_details(estimate, false);
        }
        return 1;
    }

    std::cout << "\n\nEncode complete: " << format_size(result.input_size) << " -> "
            << format_size(result.output_size) << "\n";
    std::cout << "Chunks: " << result.total_chunks
            << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";
    std::cout << "Written to: " << output_path << "\n";

    return print_performance_report(result, benchmark_json) ? 0 : 1;
}

static int do_decode(const std::string &input_path, const std::string &output_path,
                     const std::string &password,
                     const std::string &benchmark_json) {
    std::cout << "Input: " << input_path << "\n";
    std::cout << "Output: " << output_path << "\n";

    ms_decode_options_t opts{};
    opts.input_path = input_path.c_str();
    opts.output_path = output_path.c_str();
    opts.password = password.c_str();
    opts.password_len = password.size();
    opts.progress = decode_progress;
    opts.progress_user = nullptr;

    ms_result_t result{};
    if (const ms_status_t status = ms_decode(&opts, &result); status != MS_OK) {
        std::cout << "\n";
        std::cerr << "Error: " << ms_status_string(status) << "\n";
        return 1;
    }

    const auto decoded_kind =
        video_set::parse_envelope_file(output_path).kind;
    if (decoded_kind != video_set::ParseKind::NotVideoSet) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        std::cerr
            << "\nThis video contains a Video Set part. The decoded logical "
               "payload was not kept. Use set-inspect or set-recover so "
               "embedded part metadata and SHA-256 are validated.\n";
        return decoded_kind == video_set::ParseKind::Valid ? 3 : 4;
    }

    std::cout << "\n\nDecode complete: " << format_size(result.input_size) << " -> "
            << format_size(result.output_size) << "\n";
    std::cout << "Chunks: " << result.total_chunks
            << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";
    std::cout << "Written to: " << output_path << "\n";

    return print_performance_report(result, benchmark_json) ? 0 : 1;
}

static int do_stream_encode(const std::string &input_path, const std::string &stream_url,
                            const bool encrypt, const std::string &password,
                            const ms_hash_algorithm_t hash_algo, const int bitrate_kbps,
                            const int width, const int height, const int fps,
                            const EncodingReliabilityOptions &reliability,
                            const std::string &benchmark_json) {
    std::cout << "Input: " << input_path << "\n";
    std::cout << "Stream URL: " << stream_url << "\n";
    std::cout << "Resolution: " << width << "x" << height << "(" << fps << " fps)\n";
    std::cout << "Bitrate: " << bitrate_kbps << " kbps\n";

    ms_stream_encode_options_t opts{};
    opts.input_path = input_path.c_str();
    opts.stream_url = stream_url.c_str();
    opts.encrypt = encrypt ? 1 : 0;
    opts.password = password.c_str();
    opts.password_len = password.size();
    opts.hash_algorithm = hash_algo;
    opts.bitrate_kbps = bitrate_kbps;
    opts.width = width;
    opts.height = height;
    opts.fps = fps;
    opts.progress = stream_encode_progress;
    opts.progress_user = nullptr;
    opts.repair_ratio = reliability.repair_ratio;
    opts.repair_ratio_is_set = 1;

    ms_result_t result{};
    if (const ms_status_t status = ms_stream_encode(&opts, &result); status != MS_OK) {
        std::cout << "\n";
        std::cerr << "Error: " << ms_status_string(status) << "\n";
        return 1;
    }

    std::cout << "\n\nStream encode complete: " << format_size(result.input_size) << "\n";
    std::cout << "Chunks: " << result.total_chunks
            << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";

    return print_performance_report(result, benchmark_json) ? 0 : 1;
}

static int do_stream_decode(const std::string &stream_url, const std::string &output_path,
                            const std::string &password,
                            const std::string &benchmark_json) {
    std::cout << "Stream URL: " << stream_url << "\n";
    std::cout << "Output: " << output_path << "\n";
    std::cout << "Waiting for stream...\n";

    ms_stream_decode_options_t opts{};
    opts.stream_url = stream_url.c_str();
    opts.output_path = output_path.c_str();
    opts.password = password.c_str();
    opts.password_len = password.size();
    opts.timeout_sec = 30;
    opts.progress = stream_decode_progress;
    opts.progress_user = nullptr;

    ms_result_t result{};
    if (const ms_status_t status = ms_stream_decode(&opts, &result); status != MS_OK) {
        std::cout << "\n";
        std::cerr << "Error: " << ms_status_string(status) << "\n";
        return 1;
    }

    std::cout << "\n\nStream decode complete: -> " << format_size(result.output_size) << "\n";
    std::cout << "Chunks: " << result.total_chunks
            << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";
    std::cout << "Written to: " << output_path << "\n";

    return print_performance_report(result, benchmark_json) ? 0 : 1;
}

int main(const int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];

    if (video_set_cli::is_command(argv[1]))
        return video_set_cli::run(argc, argv);

    if (command == "testlab") {
        try {
            return do_testlab(argc, argv);
        } catch (const std::exception &error) {
            std::cerr << "Test Lab error: " << error.what() << "\n";
            return 1;
        }
    }
    if (command == "capacitylab") {
        try {
            return do_capacitylab(argc, argv);
        } catch (const std::exception &error) {
            std::cerr << "Capacity Lab error: "
                      << error.what() << "\n";
            return 1;
        }
    }

    if (command != "encode" && command != "decode" &&
        command != "stream-encode" && command != "stream-decode") {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path;
    std::string output_path;
    std::string stream_url;
    bool encrypt = false;
    std::string password;
    auto hash_algo = MS_HASH_CRC32;
    int bitrate_kbps = FRAME_BITRATE;
    int stream_width = FRAME_WIDTH_STREAM;
    int stream_height = FRAME_HEIGHT_STREAM;
    int stream_fps = FRAME_FPS;
    std::string benchmark_json;
    std::string estimate_json;
    bool estimate_only = false;
    bool enable_probe = true;
    bool allow_low_disk = false;
    std::optional<ReliabilityProfile> reliability_profile;
    std::optional<double> repair_percentage;
    ms_encoding_mode_t encoding_mode = MS_ENCODING_MODE_RESILIENT;
    bool mode_was_set = false;
    bool video_set_mode_requested = false;

    for (int i = 2; i < argc; ++i) {
        if (const std::string arg = argv[i]; (arg == "--input" || arg == "-i") && i + 1 < argc) {
            input_path = argv[++i];
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            output_path = argv[++i];
        } else if ((arg == "--url" || arg == "-u") && i + 1 < argc) {
            stream_url = argv[++i];
        } else if ((arg == "--bitrate" || arg == "-b") && i + 1 < argc) {
            bitrate_kbps = std::stoi(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            stream_width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            stream_height = std::stoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            stream_fps = std::stoi(argv[++i]);
        } else if ((arg == "--encrypt" || arg == "-e")) {
            encrypt = true;
        } else if ((arg == "--password" || arg == "-p") && i + 1 < argc) {
            password = argv[++i];
        } else if ((arg == "--hash" || arg == "-H") && i + 1 < argc) {
            if (const std::string algo_str = argv[++i]; algo_str == "xxhash") {
                hash_algo = MS_HASH_XXHASH32;
            } else if (algo_str == "crc32") {
                hash_algo = MS_HASH_CRC32;
            } else {
                std::cerr << "Error: unknown hash algorithm '" << algo_str << "' (use crc32 or xxhash)\n";
                return 1;
            }
        } else if (arg == "--benchmark-json" && i + 1 < argc) {
            benchmark_json = argv[++i];
        } else if (arg == "--estimate-json" && i + 1 < argc) {
            estimate_json = argv[++i];
        } else if (arg == "--estimate-only") {
            estimate_only = true;
        } else if (arg == "--no-probe") {
            enable_probe = false;
        } else if (arg == "--allow-low-disk") {
            allow_low_disk = true;
        } else if (arg == "--video-set" || arg == "--set-mode") {
            video_set_mode_requested = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            const std::string value = argv[++i];
            mode_was_set = true;
            if (value == "resilient") {
                encoding_mode = MS_ENCODING_MODE_RESILIENT;
            } else if (value == "fast-local") {
                encoding_mode = MS_ENCODING_MODE_FAST_LOCAL;
            } else {
                std::cerr << "Error: invalid --mode '" << value
                          << "' (use resilient or fast-local)\n";
                return 1;
            }
        } else if (arg == "--repair-percent" && i + 1 < argc) {
            try {
                repair_percentage = parse_repair_percentage(argv[++i]);
            } catch (const std::exception &error) {
                std::cerr << "Error: invalid --repair-percent: "
                          << error.what() << "\n";
                return 1;
            }
        } else if (arg == "--reliability-profile" && i + 1 < argc) {
            try {
                reliability_profile =
                    parse_reliability_profile(argv[++i]);
            } catch (const std::exception &error) {
                std::cerr << "Error: invalid --reliability-profile: "
                          << error.what() << "\n";
                return 1;
            }
        } else if (!arg.empty() && arg[0] != '-' &&
                   (command == "encode" || command == "decode")) {
            if (input_path.empty()) {
                input_path = arg;
            } else if (output_path.empty()) {
                output_path = arg;
            } else {
                std::cerr << "Error: unexpected positional argument '"
                          << arg << "'\n";
                return 1;
            }
        } else {
            std::cerr << "Error: unknown or incomplete argument '" << arg << "'\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (video_set_mode_requested) {
        if (command == "stream-encode" || command == "stream-decode") {
            std::cerr
                << "Error: Video Set mode is unsupported for streams; "
                   "use the file-only set-plan/set-encode/set-recover workflow\n";
        } else {
            std::cerr
                << "Error: use the dedicated set-plan or set-encode command "
                   "for Video Set file operations\n";
        }
        return 2;
    }

    if ((command == "decode" || command == "stream-decode") &&
        (reliability_profile.has_value() ||
         repair_percentage.has_value())) {
        std::cerr << "Error: repair options apply only to encode operations\n";
        return 1;
    }
    if (command != "encode" && mode_was_set) {
        std::cerr << "Error: --mode applies only to file encode; "
                     "decode detects the format automatically\n";
        return 1;
    }
    if (encoding_mode == MS_ENCODING_MODE_FAST_LOCAL &&
        (reliability_profile.has_value() ||
         repair_percentage.has_value())) {
        std::cerr
            << "Error: reliability and repair options are not applicable "
               "in Fast Local mode\n";
        return 1;
    }
    if (reliability_profile == ReliabilityProfile::HighCapacity) {
        if (command != "encode") {
            std::cerr
                << "Error: high-capacity currently applies to file encode; "
                   "stream encoding keeps its existing geometry\n";
            return 1;
        }
        if (repair_percentage.has_value()) {
            std::cerr
                << "Error: high-capacity has a fixed 5% repair setting; "
                   "do not combine it with --repair-percent\n";
            return 1;
        }
        encoding_mode = MS_ENCODING_MODE_HIGH_CAPACITY;
    }
    if (command != "encode" &&
        (estimate_only || !estimate_json.empty() || !enable_probe ||
         allow_low_disk)) {
        std::cerr
            << "Error: estimate options apply only to file encode\n";
        return 1;
    }
    const EncodingReliabilityOptions reliability =
        resolve_reliability_options(
            reliability_profile, repair_percentage);
    std::string reliability_label = "Resilient";
    if (repair_percentage.has_value()) {
        reliability_label = "Custom";
    } else if (reliability_profile.has_value()) {
        switch (*reliability_profile) {
            case ReliabilityProfile::Local:
                reliability_label = "Resilient";
                break;
            case ReliabilityProfile::Balanced:
                reliability_label = "Balanced";
                break;
            case ReliabilityProfile::Durable:
                reliability_label = "Durable";
                break;
            case ReliabilityProfile::HighCapacity:
                reliability_label = "High Capacity";
                break;
        }
    }

    if (command == "encode") {
        if (input_path.empty() || output_path.empty()) {
            std::cerr << "Error: both --input and --output must be specified\n";
            print_usage(argv[0]);
            return 1;
        }
        if (encrypt && password.empty()) {
            std::cerr << "Error: --encrypt requires --password\n";
            return 1;
        }
        return do_encode(input_path, output_path, encrypt, password, hash_algo,
                         reliability,
                         benchmark_json, estimate_json, estimate_only,
                         enable_probe, allow_low_disk,
                         reliability_label, encoding_mode);
    } else if (command == "decode") {
        if (input_path.empty() || output_path.empty()) {
            std::cerr << "Error: both --input and --output must be specified\n";
            print_usage(argv[0]);
            return 1;
        }
        return do_decode(input_path, output_path, password, benchmark_json);
    } else if (command == "stream-encode") {
        if (input_path.empty() || stream_url.empty()) {
            std::cerr << "Error: --input and --url must be specified for stream-encode\n";
            print_usage(argv[0]);
            return 1;
        }
        if (encrypt && password.empty()) {
            std::cerr << "Error: --encrypt requires --password\n";
            return 1;
        }
        return do_stream_encode(input_path, stream_url, encrypt, password, hash_algo, bitrate_kbps,
                                stream_width, stream_height, stream_fps,
                                reliability,
                                benchmark_json);
    } else {
        if (stream_url.empty() || output_path.empty()) {
            std::cerr << "Error: --url and --output must be specified for stream-decode\n";
            print_usage(argv[0]);
            return 1;
        }
        return do_stream_decode(stream_url, output_path, password,
                                benchmark_json);
    }
}
