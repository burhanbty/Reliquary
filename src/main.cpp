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

#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "configuration.h"
#include "encoding_reliability.h"
#include "media_storage.h"

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
                      ? "Fast Local" : "Resilient / Platform")
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
            << "    [--reliability-profile <local|balanced|durable>] [--repair-percent <0..500>]\n"
            << "    [--estimate-only] [--estimate-json <estimate.json>] [--no-probe] [--allow-low-disk]\n"
            << "    [--benchmark-json <report.json>]\n"
            << "  " << program << " decode --input <video> --output <file> [--password <pwd>]"
            << " [--benchmark-json <report.json>]\n"
            << "  " << program <<
            " stream-encode --input <file> --url <rtmp://...> [--bitrate <kbps>] [--width <w> --height <h>] [--encrypt --password <pwd>] [--reliability-profile <local|balanced|durable>] [--repair-percent <0..500>] [--benchmark-json <report.json>]\n"
            << "  " << program << " stream-decode --url <stream_url> --output <file> [--password <pwd>] [--benchmark-json <report.json>]\n";
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
    std::string reliability_label = "Local / Fast";
    if (repair_percentage.has_value()) {
        reliability_label = "Custom";
    } else if (reliability_profile.has_value()) {
        switch (*reliability_profile) {
            case ReliabilityProfile::Local:
                reliability_label = "Local / Fast";
                break;
            case ReliabilityProfile::Balanced:
                reliability_label = "Balanced";
                break;
            case ReliabilityProfile::Durable:
                reliability_label = "Durable";
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
