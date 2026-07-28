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

#include "media_storage.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <omp.h>
#include <span>
#include <sstream>
#include <string>
#include <thread>

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoding_reliability.h"
#include "encoder.h"
#include "performance_profiler.h"
#include "stream.h"
#include "video_decoder.h"
#include "video_encoder.h"

namespace {
    constexpr double bytes_per_mib = 1024.0 * 1024.0;

    constexpr const char *stage_names[MS_PERF_STAGE_COUNT] = {
        "Input file read/open",
        "Encryption / preprocessing",
        "Chunk creation",
        "FEC / repair packet generation",
        "Packets to frames",
        "FFmpeg video encoding",
        "Mux and disk write",
        "Video read and demux",
        "Frame decode",
        "Packet extraction",
        "FEC / chunk recovery",
        "Decryption / postprocessing",
        "Output file write",
    };

    constexpr const char *stage_json_names[MS_PERF_STAGE_COUNT] = {
        "input_read",
        "preprocess",
        "chunk_creation",
        "fec_repair_generation",
        "packet_to_frame",
        "ffmpeg_encode",
        "mux_disk_write",
        "video_read_demux",
        "frame_decode",
        "packet_extraction",
        "fec_chunk_recovery",
        "postprocess",
        "output_write",
    };

    const char *operation_name(const ms_operation_t operation) {
        switch (operation) {
            case MS_OPERATION_ENCODE: return "encode";
            case MS_OPERATION_DECODE: return "decode";
            case MS_OPERATION_STREAM_ENCODE: return "stream_encode";
            case MS_OPERATION_STREAM_DECODE: return "stream_decode";
            default: return "unknown";
        }
    }

    bool stage_applies(const ms_operation_t operation, const std::size_t index) {
        if (operation == MS_OPERATION_ENCODE ||
            operation == MS_OPERATION_STREAM_ENCODE) {
            return index <= MS_PERF_MUX_DISK_WRITE;
        }
        if (operation == MS_OPERATION_DECODE ||
            operation == MS_OPERATION_STREAM_DECODE) {
            return index >= MS_PERF_VIDEO_READ_DEMUX;
        }
        return true;
    }

    void fill_result(ms_result_t *result,
                     PerformanceProfiler &profiler,
                     const ms_operation_t operation,
                     const uint64_t input_size,
                     const uint64_t output_size,
                     const uint64_t chunks,
                     const uint64_t packets,
                     const uint64_t source_packets,
                     const uint64_t repair_packets,
                     const uint64_t frames,
                     const double selected_repair_ratio = 0.0) {
        if (!result) return;

        profiler.finish();
        *result = {};
        result->input_size = input_size;
        result->output_size = output_size;
        result->total_chunks = chunks;
        result->total_packets = packets;
        result->total_frames = frames;
        result->source_packets = source_packets;
        result->repair_packets = repair_packets;
        result->operation = operation;
        result->total_seconds = profiler.total_seconds();
        result->average_frames_per_second =
            result->total_seconds > 0.0
                ? static_cast<double>(frames) / result->total_seconds
                : 0.0;

        const uint64_t processed_bytes =
            operation == MS_OPERATION_DECODE ||
            operation == MS_OPERATION_STREAM_DECODE
                ? output_size
                : input_size;
        result->throughput_mib_per_second =
            result->total_seconds > 0.0
                ? static_cast<double>(processed_bytes) /
                      bytes_per_mib / result->total_seconds
                : 0.0;
        result->output_input_ratio =
            input_size > 0
                ? static_cast<double>(output_size) /
                      static_cast<double>(input_size)
                : 0.0;
        if (operation == MS_OPERATION_ENCODE ||
            operation == MS_OPERATION_STREAM_ENCODE) {
            result->selected_repair_ratio = selected_repair_ratio;
            result->selected_repair_percentage =
                repair_ratio_to_percentage(selected_repair_ratio);
        }
        result->repair_source_ratio =
            source_packets > 0
                ? static_cast<double>(repair_packets) /
                      static_cast<double>(source_packets)
                : 0.0;

        for (std::size_t i = 0; i < MS_PERF_STAGE_COUNT; ++i) {
            const auto stage = static_cast<PerformanceStage>(i);
            auto &timing = result->stage_timings[i];
            timing.seconds = profiler.stage_seconds(stage);
            timing.percent_of_total =
                result->total_seconds > 0.0
                    ? timing.seconds * 100.0 / result->total_seconds
                    : 0.0;
            timing.invocations = profiler.stage_invocations(stage);
        }
    }

    void classify_packet(const std::span<const std::byte> packet,
                         uint64_t &source_packets,
                         uint64_t &repair_packets) {
        if (packet.size() <= FLAGS_OFF) return;
        if ((static_cast<uint8_t>(packet[FLAGS_OFF]) & IsRepairSymbol) != 0) {
            ++repair_packets;
        } else {
            ++source_packets;
        }
    }

    EncodingReliabilityOptions reliability_from_encode_options(
        const double repair_ratio,
        const int repair_ratio_is_set) {
        const EncodingReliabilityOptions reliability{
            repair_ratio_is_set == 1
                ? repair_ratio
                : DEFAULT_REPAIR_RATIO
        };
        if (!is_valid_repair_ratio(reliability.repair_ratio)) {
            throw std::invalid_argument("invalid repair ratio");
        }
        return reliability;
    }
}

static std::array<std::byte, 16> make_file_id() {
    std::array<std::byte, 16> id{};
    for (int i = 0; i < 16; ++i) {
        id[i] = static_cast<std::byte>(i);
    }
    return id;
}

static HashAlgorithm to_internal_hash(const ms_hash_algorithm_t algo) {
    switch (algo) {
        case MS_HASH_XXHASH32: return HashAlgorithm::XXHash32;
        default: return HashAlgorithm::CRC32;
    }
}

ms_status_t ms_encode(const ms_encode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }
    if (options->encrypt && (!options->password || options->password_len == 0)) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);
    PerformanceProfiler profiler;
    EncodingReliabilityOptions reliability;
    try {
        reliability = reliability_from_encode_options(
            options->repair_ratio, options->repair_ratio_is_set);
    } catch (...) {
        return MS_ERR_INVALID_ARGS;
    }

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    const bool encrypt = options->encrypt != 0;
    const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
    uint64_t input_size = 0;
    std::unique_ptr<FileChunkReader> reader_storage;
    try {
        ScopedTimer timer(&profiler, PerformanceStage::InputRead);
        input_size = std::filesystem::file_size(input_path);
        reader_storage = std::make_unique<FileChunkReader>(
            input_path.c_str(), chunk_size);
    } catch (...) {
        return MS_ERR_IO;
    }
    const FileChunkReader &reader = *reader_storage;
    std::size_t num_chunks = 0;
    {
        ScopedTimer timer(&profiler, PerformanceStage::ChunkCreation);
        num_chunks = reader.num_chunks();
    }

    const auto file_id = make_file_id();
    const Encoder encoder(
        file_id, to_internal_hash(options->hash_algorithm), reliability);

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypt) {
        ScopedTimer timer(&profiler, PerformanceStage::Preprocess);
        const std::span pw(
            reinterpret_cast<const std::byte *>(options->password),
            options->password_len);
        key = derive_key(pw, file_id);
    }

    std::size_t total_packets = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    int64_t total_frames = 0;

    try {
        VideoEncoder video_encoder(output_path, &profiler);

        const int batch_size = std::max(1, omp_get_max_threads());

        using BatchResults = std::vector<std::pair<std::vector<Packet>, ChunkManifestEntry>>;

        auto fec_encode_batch = [&](const std::size_t batch_start, const int batch_count) -> BatchResults {
            BatchResults results(batch_count);
            bool batch_error = false;

#pragma omp parallel for schedule(dynamic)
            for (int j = 0; j < batch_count; ++j) {
                if (batch_error) continue;
                try {
                    const std::size_t i = batch_start + j;
                    std::span<const std::byte> data_to_encode;
                    {
                        ScopedTimer timer(&profiler, PerformanceStage::ChunkCreation);
                        data_to_encode = reader.chunk_view(i);
                    }
                    std::vector<std::byte> encrypted_buf;
                    if (encrypt) {
                        ScopedTimer timer(&profiler, PerformanceStage::Preprocess);
                        encrypted_buf = encrypt_chunk(
                            data_to_encode, key, file_id,
                            static_cast<uint32_t>(i));
                        data_to_encode = encrypted_buf;
                    }
                    const bool is_last = (i == num_chunks - 1);
                    {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::FecRepairGeneration);
                        results[j] = encoder.encode_chunk(
                            static_cast<uint32_t>(i), data_to_encode,
                            is_last, encrypt);
                    }
                } catch (...) {
                    batch_error = true;
                }
            }

            if (batch_error) throw std::runtime_error("batch FEC encoding failed");
            return results;
        };

        const auto first_end = std::min(static_cast<std::size_t>(batch_size), num_chunks);
        std::future<BatchResults> pending =
            std::async(std::launch::async, fec_encode_batch,
                       static_cast<std::size_t>(0), static_cast<int>(first_end));

        for (std::size_t batch_start = 0; batch_start < num_chunks;
             batch_start += batch_size) {
            const std::size_t batch_end =
                std::min(batch_start + static_cast<std::size_t>(batch_size),
                         num_chunks);
            const int batch_count = static_cast<int>(batch_end - batch_start);

            if (options->progress) {
                if (options->progress(static_cast<uint64_t>(batch_start),
                                      static_cast<uint64_t>(num_chunks),
                                      options->progress_user) != 0) {
                    if (pending.valid()) pending.wait();
                    if (encrypt) secure_zero(std::span<std::byte>(key));
                    return MS_ERR_ENCODE_FAILED;
                }
            }

            auto results = pending.get();

            if (const std::size_t next_start = batch_start + batch_size; next_start < num_chunks) {
                const auto next_end = std::min(
                    next_start + static_cast<std::size_t>(batch_size), num_chunks);
                const int next_count = static_cast<int>(next_end - next_start);
                pending = std::async(std::launch::async,
                                     fec_encode_batch, next_start, next_count);
            }

            for (int j = 0; j < batch_count; ++j) {
                total_packets += results[j].first.size();
                for (const auto &packet: results[j].first) {
                    classify_packet(std::span(packet.bytes), source_packets,
                                    repair_packets);
                }
                video_encoder.encode_packets(results[j].first);
            }
        }

        video_encoder.finalize();
        total_frames = video_encoder.frames_written();
    } catch (...) {
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    }

    if (encrypt) secure_zero(std::span<std::byte>(key));

    const auto output_size = std::filesystem::file_size(output_path);
    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_ENCODE,
                input_size, output_size, num_chunks, total_packets,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames),
                reliability.repair_ratio);

    return MS_OK;
}

ms_status_t ms_decode(const ms_decode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);
    PerformanceProfiler profiler;

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    uint64_t video_size = 0;
    {
        ScopedTimer timer(&profiler, PerformanceStage::VideoReadDemux);
        video_size = std::filesystem::file_size(input_path);
    }

    Decoder decoder;
    std::size_t total_extracted = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    std::size_t decoded_chunks = 0;
    uint32_t max_chunk_index = 0;
    bool found_last_chunk = false;
    uint32_t last_chunk_index = 0;
    int64_t total_frames_read = 0;

    try {
        VideoDecoder video_decoder(input_path, &profiler);
        const int64_t total = video_decoder.total_frames();

        while (!video_decoder.is_eof()) {
            if (found_last_chunk && decoded_chunks >= last_chunk_index + 1)
                break;

            if (options->progress) {
                const auto cur = static_cast<uint64_t>(video_decoder.frames_read());
                if (const uint64_t tot = total >= 0 ? static_cast<uint64_t>(total) : 0; options->progress(cur, tot, options->progress_user) != 0) {
                    return MS_ERR_DECODE_FAILED;
                }
            }

            auto frame_packets = video_decoder.decode_next_frame();
            if (frame_packets.empty()) continue;

            for (auto &pkt_data : frame_packets) {
                ++total_extracted;
                classify_packet(std::span<const std::byte>(
                                    pkt_data.data(), pkt_data.size()),
                                source_packets, repair_packets);

                {
                    ScopedTimer timer(
                        &profiler, PerformanceStage::FecChunkRecovery);
                    if (pkt_data.size() >= HEADER_SIZE &&
                        Decoder::validate_raw_packet_crc(
                            std::span<const std::byte>(
                                pkt_data.data(), pkt_data.size()))) {
                        const auto flags = static_cast<uint8_t>(
                            pkt_data[FLAGS_OFF]);
                        uint32_t chunk_idx = 0;
                        std::memcpy(
                            &chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF,
                            sizeof(chunk_idx));
                        if (chunk_idx > max_chunk_index) {
                            max_chunk_index = chunk_idx;
                        }
                        if (flags & LastChunk) {
                            found_last_chunk = true;
                            last_chunk_index = chunk_idx;
                        }
                    }

                    const std::span<const std::byte> data(
                        pkt_data.data(), pkt_data.size());
                    if (auto res = decoder.process_packet(data, false);
                        res && res->success) {
                        ++decoded_chunks;
                    }
                }
            }
        }

        total_frames_read = video_decoder.frames_read();
    } catch (...) {
        return MS_ERR_DECODE_FAILED;
    }

    if (total_extracted == 0) {
        return MS_ERR_DECODE_FAILED;
    }

    const uint32_t expected_chunks = found_last_chunk
        ? last_chunk_index + 1
        : max_chunk_index + 1;

    if (decoded_chunks < expected_chunks) {
        return MS_ERR_INCOMPLETE;
    }

    if (decoder.is_encrypted()) {
        if (!options->password || options->password_len == 0) {
            return MS_ERR_CRYPTO;
        }
        {
            ScopedTimer timer(&profiler, PerformanceStage::Postprocess);
            const std::span<const std::byte> pw(
                reinterpret_cast<const std::byte *>(options->password),
                options->password_len);
            auto key = derive_key(pw, *decoder.file_id());
            decoder.set_decrypt_key(key);
            secure_zero(std::span<std::byte>(key));
        }
    }

    if (!decoder.write_assembled_file(
            output_path, expected_chunks, &profiler)) {
        if (decoder.is_encrypted()) decoder.clear_decrypt_key();
        return MS_ERR_DECODE_FAILED;
    }

    if (decoder.is_encrypted()) decoder.clear_decrypt_key();

    const auto output_size = std::filesystem::file_size(output_path);
    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_DECODE,
                video_size, output_size, expected_chunks, total_extracted,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames_read));

    return MS_OK;
}

ms_status_t ms_stream_encode(const ms_stream_encode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->stream_url) {
        return MS_ERR_INVALID_ARGS;
    }
    if (options->encrypt && (!options->password || options->password_len == 0)) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string stream_url(options->stream_url);
    PerformanceProfiler profiler;
    EncodingReliabilityOptions reliability;
    try {
        reliability = reliability_from_encode_options(
            options->repair_ratio, options->repair_ratio_is_set);
    } catch (...) {
        return MS_ERR_INVALID_ARGS;
    }

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    const bool encrypt = options->encrypt != 0;
    const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
    uint64_t input_size = 0;
    std::unique_ptr<FileChunkReader> reader_storage;
    try {
        ScopedTimer timer(&profiler, PerformanceStage::InputRead);
        input_size = std::filesystem::file_size(input_path);
        reader_storage = std::make_unique<FileChunkReader>(
            input_path.c_str(), chunk_size);
    } catch (...) {
        return MS_ERR_IO;
    }
    const FileChunkReader &reader = *reader_storage;
    std::size_t num_chunks = 0;
    {
        ScopedTimer timer(&profiler, PerformanceStage::ChunkCreation);
        num_chunks = reader.num_chunks();
    }

    const auto file_id = make_file_id();
    const Encoder encoder(
        file_id, to_internal_hash(options->hash_algorithm), reliability);

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypt) {
        ScopedTimer timer(&profiler, PerformanceStage::Preprocess);
        const std::span pw(reinterpret_cast<const std::byte *>(options->password),
                           options->password_len);
        key = derive_key(pw, file_id);
    }

    std::size_t total_packets = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    int64_t total_frames = 0;
    const int bitrate = options->bitrate_kbps > 0 ? options->bitrate_kbps : FRAME_BITRATE;
    const int width = options->width > 0 ? options->width : FRAME_WIDTH_STREAM;
    const int height = options->height > 0 ? options->height : FRAME_HEIGHT_STREAM;
    const int fps = options->fps > 0 ? options->fps : FRAME_FPS;

    try {
        StreamEncoder stream_encoder(
            stream_url, bitrate, width, height, fps, &profiler);

        const int batch_size = std::max(1, omp_get_max_threads());

        using BatchResults = std::vector<std::pair<std::vector<Packet>, ChunkManifestEntry>>;

        auto fec_encode_batch = [&](const std::size_t batch_start, const int batch_count) -> BatchResults {
            BatchResults results(batch_count);
            bool batch_error = false;

#pragma omp parallel for schedule(dynamic)
            for (int j = 0; j < batch_count; ++j) {
                if (batch_error) continue;
                try {
                    const std::size_t i = batch_start + j;
                    std::span<const std::byte> data_to_encode;
                    {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::ChunkCreation);
                        data_to_encode = reader.chunk_view(i);
                    }
                    std::vector<std::byte> encrypted_buf;
                    if (encrypt) {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::Preprocess);
                        encrypted_buf = encrypt_chunk(
                            data_to_encode, key, file_id,
                            static_cast<uint32_t>(i));
                        data_to_encode = encrypted_buf;
                    }
                    const bool is_last = (i == num_chunks - 1);
                    {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::FecRepairGeneration);
                        results[j] = encoder.encode_chunk(
                            static_cast<uint32_t>(i), data_to_encode,
                            is_last, encrypt);
                    }
                } catch (...) {
                    batch_error = true;
                }
            }

            if (batch_error) throw std::runtime_error("batch FEC encoding failed");
            return results;
        };

        const auto first_end = std::min(static_cast<std::size_t>(batch_size), num_chunks);
        std::future<BatchResults> pending =
            std::async(std::launch::async, fec_encode_batch,
                       static_cast<std::size_t>(0), static_cast<int>(first_end));

        for (std::size_t batch_start = 0; batch_start < num_chunks;
             batch_start += batch_size) {
            const std::size_t batch_end =
                std::min(batch_start + static_cast<std::size_t>(batch_size),
                         num_chunks);
            const int batch_count = static_cast<int>(batch_end - batch_start);

            if (options->progress) {
                if (options->progress(static_cast<uint64_t>(batch_start),
                                      static_cast<uint64_t>(num_chunks),
                                      options->progress_user) != 0) {
                    if (pending.valid()) pending.wait();
                    if (encrypt) secure_zero(std::span<std::byte>(key));
                    return MS_ERR_ENCODE_FAILED;
                }
            }

            auto results = pending.get();

            if (const std::size_t next_start = batch_start + batch_size; next_start < num_chunks) {
                const auto next_end = std::min(
                    next_start + static_cast<std::size_t>(batch_size), num_chunks);
                const int next_count = static_cast<int>(next_end - next_start);
                pending = std::async(std::launch::async,
                                     fec_encode_batch, next_start, next_count);
            }

            for (int j = 0; j < batch_count; ++j) {
                total_packets += results[j].first.size();
                for (const auto &packet: results[j].first) {
                    classify_packet(std::span(packet.bytes), source_packets,
                                    repair_packets);
                }
                stream_encoder.encode_packets(results[j].first);
            }
        }

        stream_encoder.finalize();
        total_frames = stream_encoder.frames_written();
    } catch (const std::exception &e) {
        fprintf(stderr, "Stream encode error: %s\n", e.what());
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    } catch (...) {
        fprintf(stderr, "Stream encode error: unknown exception\n");
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    }

    if (encrypt) secure_zero(std::span<std::byte>(key));

    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_STREAM_ENCODE,
                input_size, 0, num_chunks, total_packets,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames),
                reliability.repair_ratio);

    return MS_OK;
}

ms_status_t ms_stream_decode(const ms_stream_decode_options_t *options, ms_result_t *result) {
    if (!options || !options->stream_url || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string stream_url(options->stream_url);
    const std::string output_path(options->output_path);
    PerformanceProfiler profiler;

    Decoder decoder;
    std::size_t total_extracted = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    std::size_t decoded_chunks = 0;
    uint32_t max_chunk_index = 0;
    bool found_last_chunk = false;
    uint32_t last_chunk_index = 0;
    int64_t total_frames_read = 0;

    try {
        const int max_retries = options->timeout_sec > 0 ? options->timeout_sec : 30;
        std::unique_ptr<VideoDecoder> vdec;
        for (int attempt = 0; attempt < max_retries; ++attempt) {
            try {
                vdec = std::make_unique<VideoDecoder>(
                    stream_url, &profiler);
                break;
            } catch (...) {
                if (attempt + 1 >= max_retries) throw;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        const int64_t total = vdec->total_frames();

        while (!vdec->is_eof()) {
            if (found_last_chunk && decoded_chunks >= last_chunk_index + 1)
                break;

            if (options->progress) {
                const auto cur = static_cast<uint64_t>(vdec->frames_read());
                if (const uint64_t tot = total >= 0 ? static_cast<uint64_t>(total) : 0; options->progress(cur, tot, options->progress_user) != 0) {
                    return MS_ERR_DECODE_FAILED;
                }
            }

            auto frame_packets = vdec->decode_next_frame();
            if (frame_packets.empty()) continue;

            for (auto &pkt_data : frame_packets) {
                ++total_extracted;
                classify_packet(std::span<const std::byte>(
                                    pkt_data.data(), pkt_data.size()),
                                source_packets, repair_packets);

                {
                    ScopedTimer timer(
                        &profiler, PerformanceStage::FecChunkRecovery);
                    if (pkt_data.size() >= HEADER_SIZE &&
                        Decoder::validate_raw_packet_crc(
                            std::span<const std::byte>(
                                pkt_data.data(), pkt_data.size()))) {
                        const auto flags = static_cast<uint8_t>(
                            pkt_data[FLAGS_OFF]);
                        uint32_t chunk_idx = 0;
                        std::memcpy(
                            &chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF,
                            sizeof(chunk_idx));
                        if (chunk_idx > max_chunk_index) {
                            max_chunk_index = chunk_idx;
                        }
                        if (flags & LastChunk) {
                            found_last_chunk = true;
                            last_chunk_index = chunk_idx;
                        }
                    }

                    const std::span<const std::byte> data(
                        pkt_data.data(), pkt_data.size());
                    if (const auto res = decoder.process_packet(data, false);
                        res && res->success) {
                        ++decoded_chunks;
                    }
                }
            }
        }

        total_frames_read = vdec->frames_read();
    } catch (const std::exception &e) {
        fprintf(stderr, "Stream decode error: %s\n", e.what());
        return MS_ERR_DECODE_FAILED;
    } catch (...) {
        fprintf(stderr, "Stream decode error: unknown exception\n");
        return MS_ERR_DECODE_FAILED;
    }

    if (total_extracted == 0) {
        return MS_ERR_DECODE_FAILED;
    }

    const uint32_t expected_chunks = found_last_chunk
        ? last_chunk_index + 1
        : max_chunk_index + 1;

    if (decoded_chunks < expected_chunks) {
        return MS_ERR_INCOMPLETE;
    }

    if (decoder.is_encrypted()) {
        if (!options->password || options->password_len == 0) {
            return MS_ERR_CRYPTO;
        }
        {
            ScopedTimer timer(&profiler, PerformanceStage::Postprocess);
            const std::span<const std::byte> pw(
                reinterpret_cast<const std::byte *>(options->password),
                options->password_len);
            auto key = derive_key(pw, *decoder.file_id());
            decoder.set_decrypt_key(key);
            secure_zero(std::span<std::byte>(key));
        }
    }

    if (!decoder.write_assembled_file(
            output_path, expected_chunks, &profiler)) {
        if (decoder.is_encrypted()) decoder.clear_decrypt_key();
        return MS_ERR_DECODE_FAILED;
    }

    if (decoder.is_encrypted()) decoder.clear_decrypt_key();

    const auto output_size = std::filesystem::file_size(output_path);
    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_STREAM_DECODE,
                0, output_size, expected_chunks, total_extracted,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames_read));

    return MS_OK;
}

const char *ms_status_string(const ms_status_t status) {
    switch (status) {
        case MS_OK:              return "success";
        case MS_ERR_INVALID_ARGS: return "invalid arguments";
        case MS_ERR_FILE_NOT_FOUND: return "file not found";
        case MS_ERR_IO:          return "I/O error";
        case MS_ERR_ENCODE_FAILED: return "encoding failed";
        case MS_ERR_DECODE_FAILED: return "decoding failed";
        case MS_ERR_CRYPTO:      return "encryption/decryption error";
        case MS_ERR_INCOMPLETE:  return "incomplete data";
        default:                 return "unknown error";
    }
}

const char *ms_version(void) {
    return "1.0.0";
}

size_t ms_format_performance_report(const ms_result_t *result,
                                    char *buffer,
                                    const size_t buffer_size) {
    if (!result) return 0;

    std::ostringstream out;
    out << "\n=== Performance report (" << operation_name(result->operation)
        << ") ===\n"
        << "Input / output: " << result->input_size << " B -> "
        << result->output_size << " B";
    if (result->input_size > 0) {
        out << "  (ratio " << std::fixed << std::setprecision(3)
            << result->output_input_ratio << "x)";
    }
    out << "\n";
    if (result->operation == MS_OPERATION_ENCODE ||
        result->operation == MS_OPERATION_STREAM_ENCODE) {
        out << "Reliability:\n"
            << std::fixed << std::setprecision(2)
            << "  Repair percentage: "
            << result->selected_repair_percentage << "%\n"
            << std::setprecision(4)
            << "  Repair ratio:      "
            << result->selected_repair_ratio << "\n"
            << "  Source packets:    " << result->source_packets << "\n"
            << "  Repair packets:    " << result->repair_packets << "\n"
            << "  Repair/source:     "
            << result->repair_source_ratio << "\n"
            << "  Total packets:     " << result->total_packets << "\n";
    }
    out << "Chunks: " << result->total_chunks
        << "  Packets: " << result->total_packets
        << " (source " << result->source_packets
        << ", repair " << result->repair_packets << ")"
        << "  Frames: " << result->total_frames << "\n"
        << std::fixed << std::setprecision(3)
        << "Rates: " << result->average_frames_per_second << " frames/s, "
        << result->throughput_mib_per_second << " MiB/s\n"
        << "Stage timings:\n";

    for (std::size_t i = 0; i < MS_PERF_STAGE_COUNT; ++i) {
        if (!stage_applies(result->operation, i)) continue;
        const auto &timing = result->stage_timings[i];
        out << "  " << std::left << std::setw(34) << stage_names[i]
            << std::right << std::setw(10) << std::setprecision(6)
            << timing.seconds << " s  "
            << std::setw(8) << std::setprecision(2)
            << timing.percent_of_total << "%  "
            << "(" << timing.invocations << " calls)\n";
    }
    out << "  " << std::left << std::setw(34) << "Total wall time"
        << std::right << std::setw(10) << std::setprecision(6)
        << result->total_seconds << " s    100.00%\n"
        << "Note: timings from parallel stages are accumulated work time; "
           "their percentages may overlap.\n";

    const std::string report = out.str();
    const size_t required = report.size() + 1;
    if (buffer && buffer_size > 0) {
        const size_t copied = (std::min)(report.size(), buffer_size - 1);
        std::memcpy(buffer, report.data(), copied);
        buffer[copied] = '\0';
    }
    return required;
}

ms_status_t ms_write_benchmark_json(const ms_result_t *result,
                                    const char *output_path) {
    if (!result || !output_path || output_path[0] == '\0') {
        return MS_ERR_INVALID_ARGS;
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) return MS_ERR_IO;

    out << std::fixed << std::setprecision(9)
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"operation\": \"" << operation_name(result->operation) << "\",\n"
        << "  \"input_size_bytes\": " << result->input_size << ",\n"
        << "  \"output_size_bytes\": " << result->output_size << ",\n"
        << "  \"output_input_ratio\": " << result->output_input_ratio << ",\n";
    if (result->operation == MS_OPERATION_ENCODE ||
        result->operation == MS_OPERATION_STREAM_ENCODE) {
        out << "  \"reliability\": {\n"
            << "    \"repair_percentage\": "
            << result->selected_repair_percentage << ",\n"
            << "    \"repair_ratio\": "
            << result->selected_repair_ratio << ",\n"
            << "    \"source_packet_count\": "
            << result->source_packets << ",\n"
            << "    \"repair_packet_count\": "
            << result->repair_packets << ",\n"
            << "    \"repair_source_ratio\": "
            << result->repair_source_ratio << ",\n"
            << "    \"total_packet_count\": "
            << result->total_packets << "\n"
            << "  },\n";
    }
    out << "  \"chunks\": " << result->total_chunks << ",\n"
        << "  \"packets\": {\n"
        << "    \"total\": " << result->total_packets << ",\n"
        << "    \"source\": " << result->source_packets << ",\n"
        << "    \"repair\": " << result->repair_packets << "\n"
        << "  },\n"
        << "  \"frames\": " << result->total_frames << ",\n"
        << "  \"average_frames_per_second\": "
        << result->average_frames_per_second << ",\n"
        << "  \"throughput_mib_per_second\": "
        << result->throughput_mib_per_second << ",\n"
        << "  \"total_seconds\": " << result->total_seconds << ",\n"
        << "  \"parallel_stage_time_note\": "
           "\"Stage durations are accumulated work time and may overlap.\",\n"
        << "  \"stages\": {\n";

    bool first = true;
    for (std::size_t i = 0; i < MS_PERF_STAGE_COUNT; ++i) {
        if (!stage_applies(result->operation, i)) continue;
        if (!first) out << ",\n";
        first = false;
        const auto &timing = result->stage_timings[i];
        out << "    \"" << stage_json_names[i] << "\": {"
            << "\"seconds\": " << timing.seconds
            << ", \"percent_of_total\": " << timing.percent_of_total
            << ", \"invocations\": " << timing.invocations << "}";
    }
    out << "\n  }\n}\n";

    return out.good() ? MS_OK : MS_ERR_IO;
}
