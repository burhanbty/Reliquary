/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MEDIA_STORAGE_H
#define MEDIA_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#   ifdef MEDIA_STORAGE_BUILD_SHARED
#       define MS_API __declspec(dllexport)
#   elif defined(MEDIA_STORAGE_SHARED)
#       define MS_API __declspec(dllimport)
#   else
#       define MS_API
#   endif
#else
#   if defined(MEDIA_STORAGE_BUILD_SHARED) && defined(__GNUC__)
#       define MS_API __attribute__((visibility("default")))
#   else
#       define MS_API
#   endif
#endif

typedef enum {
    MS_OK = 0,
    MS_ERR_INVALID_ARGS = 1,
    MS_ERR_FILE_NOT_FOUND = 2,
    MS_ERR_IO = 3,
    MS_ERR_ENCODE_FAILED = 4,
    MS_ERR_DECODE_FAILED = 5,
    MS_ERR_CRYPTO = 6,
    MS_ERR_INCOMPLETE = 7,
    MS_ERR_INSUFFICIENT_DISK = 8,
    MS_ERR_PREFLIGHT_STALE = 9,
} ms_status_t;

typedef enum {
    MS_HASH_CRC32 = 0,
    MS_HASH_XXHASH32 = 1,
} ms_hash_algorithm_t;

typedef enum {
    MS_OPERATION_UNKNOWN = 0,
    MS_OPERATION_ENCODE = 1,
    MS_OPERATION_DECODE = 2,
    MS_OPERATION_STREAM_ENCODE = 3,
    MS_OPERATION_STREAM_DECODE = 4,
} ms_operation_t;

typedef enum {
    MS_PERF_INPUT_READ = 0,
    MS_PERF_PREPROCESS = 1,
    MS_PERF_CHUNK_CREATION = 2,
    MS_PERF_FEC_REPAIR_GENERATION = 3,
    MS_PERF_PACKET_TO_FRAME = 4,
    MS_PERF_FFMPEG_ENCODE = 5,
    MS_PERF_MUX_DISK_WRITE = 6,
    MS_PERF_VIDEO_READ_DEMUX = 7,
    MS_PERF_FRAME_DECODE = 8,
    MS_PERF_PACKET_EXTRACTION = 9,
    MS_PERF_FEC_CHUNK_RECOVERY = 10,
    MS_PERF_POSTPROCESS = 11,
    MS_PERF_OUTPUT_WRITE = 12,
    MS_PERF_STAGE_COUNT = 13,
} ms_performance_stage_t;

typedef struct {
    double seconds;
    double percent_of_total;
    uint64_t invocations;
} ms_stage_timing_t;

/**
 * Progress callback invoked during encode/decode.
 *
 * @param current  Current step (e.g. chunk index or frame index).
 * @param total    Total steps (0 if unknown).
 * @param user     User-supplied context pointer.
 * @return         0 to continue, non-zero to cancel.
 */
typedef int (*ms_progress_fn)(uint64_t current, uint64_t total, void *user);

#define MS_ESTIMATION_METHOD_CAPACITY 256
#define MS_ESTIMATION_MESSAGE_CAPACITY 512
#define MS_ENCODING_ESTIMATE_VERSION 1

typedef struct {
    uint32_t struct_size;
    uint32_t struct_version;
    uint64_t input_size_bytes;
    int64_t input_last_write_time_ticks;
    uint64_t input_path_fingerprint;
    double repair_percentage;
    double repair_ratio;
    uint64_t chunk_count;
    uint64_t source_packet_count;
    uint64_t repair_packet_count;
    uint64_t total_packet_count;
    uint64_t estimated_frame_count;
    double estimated_video_duration_seconds;

    uint64_t estimated_output_bytes;
    uint64_t estimated_output_min_bytes;
    uint64_t estimated_output_max_bytes;
    int output_size_estimate_available;

    uint64_t available_disk_bytes;
    int disk_space_known;
    uint64_t safety_margin_bytes;
    uint64_t required_disk_bytes;
    int required_disk_space_known;
    /**
     * -1 means unknown, 0 means insufficient, and 1 means sufficient.
     */
    int disk_space_sufficient;
    int can_start_encoding;
    int low_disk_override_permitted;

    char estimation_method[MS_ESTIMATION_METHOD_CAPACITY];
    uint64_t probe_frame_count;
    double probe_duration_seconds;
    double preflight_duration_seconds;
    char warning[MS_ESTIMATION_MESSAGE_CAPACITY];
    char error[MS_ESTIMATION_MESSAGE_CAPACITY];
} ms_encoding_estimate_t;

typedef struct {
    const char *input_path;
    const char *output_path;

    int encrypt;
    const char *password;
    size_t password_len;

    ms_hash_algorithm_t hash_algorithm;

    ms_progress_fn progress;
    void *progress_user;

    /**
     * Repair packets divided by source packets (0.05 means 5%).
     * Set repair_ratio_is_set to 1 to use this field. Otherwise the
     * backward-compatible default of 0.05 is used.
     */
    double repair_ratio;
    int repair_ratio_is_set;

    /**
     * Optional estimate returned by ms_estimate_encode for the same paths
     * and options. ms_encode validates its metadata and disk state again
     * immediately before encoding. NULL requests an internal preflight.
     */
    const ms_encoding_estimate_t *preflight_estimate;
    double preflight_duration_seconds;

    /**
     * Override only a known insufficient-disk result. It does not override
     * stale metadata, invalid paths, overflow, or invalid options.
     */
    int allow_low_disk;
} ms_encode_options_t;

typedef struct {
    const char *input_path;
    const char *output_path;

    const char *password;
    size_t password_len;

    ms_progress_fn progress;
    void *progress_user;
} ms_decode_options_t;

typedef struct {
    const char *input_path;
    const char *stream_url;

    int encrypt;
    const char *password;
    size_t password_len;

    ms_hash_algorithm_t hash_algorithm;
    int bitrate_kbps;
    int width;
    int height;
    int fps;

    ms_progress_fn progress;
    void *progress_user;

    double repair_ratio;
    int repair_ratio_is_set;
} ms_stream_encode_options_t;

typedef struct {
    const char *stream_url;
    const char *output_path;

    const char *password;
    size_t password_len;

    int timeout_sec;

    ms_progress_fn progress;
    void *progress_user;
} ms_stream_decode_options_t;

typedef struct {
    uint64_t input_size;
    uint64_t output_size;
    uint64_t total_chunks;
    uint64_t total_packets;
    uint64_t total_frames;
    uint64_t source_packets;
    uint64_t repair_packets;

    ms_operation_t operation;
    double total_seconds;
    double average_frames_per_second;
    double throughput_mib_per_second;
    double output_input_ratio;
    double selected_repair_percentage;
    double selected_repair_ratio;
    double repair_source_ratio;

    int estimate_validation_available;
    int estimate_relative_error_available;
    uint64_t estimated_output_bytes;
    uint64_t estimated_output_min_bytes;
    uint64_t estimated_output_max_bytes;
    uint64_t actual_output_bytes;
    uint64_t estimate_absolute_error_bytes;
    double estimate_relative_error_percent;
    /**
     * -1 means unavailable, 0 means outside, and 1 means inside.
     */
    int actual_inside_estimated_range;
    double preflight_duration_seconds;
    double actual_encode_duration_seconds;

    ms_stage_timing_t stage_timings[MS_PERF_STAGE_COUNT];
} ms_result_t;

/**
 * Encode a file into a lossless video.
 *
 * @param options  Encoding parameters (input/output paths, encryption, etc.).
 * @param result   Optional pointer to receive statistics about the operation.
 * @return         MS_OK on success, or an error code.
 */
MS_API ms_status_t ms_encode(const ms_encode_options_t *options, ms_result_t *result);

/**
 * Estimate an encode and inspect target-disk safety without touching the
 * requested output file. A failed FFV1 probe still returns MS_OK when the
 * deterministic counts are available; inspect the availability flags.
 */
MS_API ms_status_t ms_estimate_encode(
    const ms_encode_options_t *options,
    int enable_probe,
    ms_encoding_estimate_t *estimate);

/**
 * Decode a video back into the original file.
 *
 * @param options  Decoding parameters (input/output paths, password, etc.).
 * @param result   Optional pointer to receive statistics about the operation.
 * @return         MS_OK on success, or an error code.
 */
MS_API ms_status_t ms_decode(const ms_decode_options_t *options, ms_result_t *result);

/**
 * Encode a file and stream it via RTMP to Twitch/YouTube/etc.
 *
 * @param options  Stream encoding parameters (input path, RTMP URL, bitrate, etc.).
 * @param result   Optional pointer to receive statistics about the operation.
 * @return         MS_OK on success, or an error code.
 */
MS_API ms_status_t ms_stream_encode(const ms_stream_encode_options_t *options, ms_result_t *result);

/**
 * Decode a live stream back into the original file.
 *
 * @param options  Stream decoding parameters (stream URL, output path, etc.).
 * @param result   Optional pointer to receive statistics about the operation.
 * @return         MS_OK on success, or an error code.
 */
MS_API ms_status_t ms_stream_decode(const ms_stream_decode_options_t *options, ms_result_t *result);

/**
 * Return a human-readable string for the given status code.
 * The returned pointer is valid for the lifetime of the program.
 */
MS_API const char *ms_status_string(ms_status_t status);

/**
 * Return the library version string (e.g. "1.0.0").
 */
MS_API const char *ms_version(void);

/**
 * Format a performance result as a human-readable report.
 *
 * If buffer is NULL or buffer_size is zero, returns the required buffer size,
 * including the terminating null byte.
 */
MS_API size_t ms_format_performance_report(
    const ms_result_t *result, char *buffer, size_t buffer_size);

/**
 * Write a machine-readable JSON benchmark report.
 */
MS_API ms_status_t ms_write_benchmark_json(
    const ms_result_t *result, const char *output_path);

/**
 * Write a machine-readable preflight estimate JSON document.
 */
MS_API ms_status_t ms_write_encoding_estimate_json(
    const ms_encoding_estimate_t *estimate, const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_STORAGE_H */
