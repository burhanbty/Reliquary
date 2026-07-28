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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "configuration.h"
#include "encoder.h"

class PerformanceProfiler;

struct VideoEncoderStatistics {
    std::vector<uint64_t> encoded_packet_bytes;
    uint64_t container_header_bytes = 0;
    uint64_t container_trailer_bytes = 0;
    uint64_t output_bytes = 0;
};

FrameLayout compute_frame_layout();

FrameLayout compute_frame_layout(int width, int height);

std::size_t max_packet_bytes_per_frame();

class VideoEncoder {
public:
    explicit VideoEncoder(const std::string &output_path,
                          PerformanceProfiler *profiler = nullptr);

    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;

    VideoEncoder &operator=(const VideoEncoder &) = delete;

    VideoEncoder(VideoEncoder &&) = delete;

    VideoEncoder &operator=(VideoEncoder &&) = delete;

    void add_packet(const Packet &packet);

    void encode_packets(const std::vector<Packet> &packets);

    void encode_gray8_frame(std::span<const std::byte> pixels);

    void finalize();

    [[nodiscard]] int64_t frames_written() const { return frame_index; }

    [[nodiscard]] static int packets_per_frame();

    [[nodiscard]] static constexpr std::size_t gray8_frame_bytes() {
        return static_cast<std::size_t>(FRAME_WIDTH) * FRAME_HEIGHT;
    }

    [[nodiscard]] const VideoEncoderStatistics &statistics() const {
        return statistics_;
    }

private:
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVStream *stream = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *av_packet = nullptr;

    std::vector<std::byte> frame_data_buffer;
    FrameLayout layout_{};
    int64_t frame_index = 0;
    bool finalized = false;
    PerformanceProfiler *profiler_ = nullptr;
    VideoEncoderStatistics statistics_;

    void init_encoder(const std::string &output_path);

    void cleanup() noexcept;

    void embed_data_in_frame(const std::vector<std::byte> &data) const;

    void encode_frame();

    void flush_encoder();

    void write_encoded_packet();

    void flush_frame_buffer();
};
