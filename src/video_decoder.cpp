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

#include "video_decoder.h"
#include "video_encoder.h"
#include "configuration.h"
#include "dct_common.h"
#include "decoder.h"
#include "performance_profiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>

VideoDecoder::VideoDecoder(const std::string &input_path,
                           PerformanceProfiler *profiler)
    : profiler_(profiler) {
    try {
        init_decoder(input_path);
    } catch (...) {
        cleanup();
        throw;
    }
}

VideoDecoder::~VideoDecoder() {
    cleanup();
}

void VideoDecoder::cleanup() noexcept {
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (av_packet_) av_packet_free(&av_packet_);
    if (gray_frame_) av_frame_free(&gray_frame_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (format_ctx_) avformat_close_input(&format_ctx_);
    sws_ctx_ = nullptr;
    video_stream_index_ = -1;
}

void VideoDecoder::init_decoder(const std::string &input_path) {
    int ret = 0;
    {
        ScopedTimer timer(profiler_, PerformanceStage::VideoReadDemux);
        ret = avformat_open_input(&format_ctx_, input_path.c_str(), nullptr, nullptr);
    }
    if (ret < 0) {
        throw std::runtime_error("Failed to open input file");
    }

    {
        ScopedTimer timer(profiler_, PerformanceStage::VideoReadDemux);
        ret = avformat_find_stream_info(format_ctx_, nullptr);
    }
    if (ret < 0) {
        throw std::runtime_error("Failed to find stream info");
    }

    for (unsigned i = 0; i < format_ctx_->nb_streams; ++i) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_index_ < 0) {
        throw std::runtime_error("No video stream found");
    }

    const AVStream *stream = format_ctx_->streams[video_stream_index_];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        throw std::runtime_error("Failed to find decoder");
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        throw std::runtime_error("Failed to allocate codec context");
    }

    ret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    codec_ctx_->thread_count = 0;
    codec_ctx_->thread_type = FF_THREAD_SLICE;

    {
        ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
        ret = avcodec_open2(codec_ctx_, codec, nullptr);
    }
    if (ret < 0) {
        throw std::runtime_error("Failed to open codec");
    }

    frame_ = av_frame_alloc();
    av_packet_ = av_packet_alloc();

    if (!frame_ || !av_packet_) {
        throw std::runtime_error("Failed to allocate frame/packet");
    }
    is_gray8_ = (codec_ctx_->pix_fmt == AV_PIX_FMT_GRAY8);

    if (!is_gray8_) {
        if (codec_ctx_->pix_fmt == AV_PIX_FMT_NONE ||
            codec_ctx_->width <= 0 || codec_ctx_->height <= 0) {
            throw std::runtime_error(
                "Video stream has incomplete pixel format metadata");
        }
        gray_frame_ = av_frame_alloc();
        if (!gray_frame_) {
            throw std::runtime_error("Failed to allocate gray frame");
        }

        gray_frame_->format = AV_PIX_FMT_GRAY8;
        gray_frame_->width = codec_ctx_->width;
        gray_frame_->height = codec_ctx_->height;

        ret = av_frame_get_buffer(gray_frame_, 0);
        if (ret < 0) {
            throw std::runtime_error("Failed to allocate gray frame buffer");
        }

        sws_ctx_ = sws_getContext(
            codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
            codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_GRAY8,
            SWS_POINT, nullptr, nullptr, nullptr
        );

        if (!sws_ctx_) {
            throw std::runtime_error("Failed to create swscale context");
        }
    }

    layout_ = compute_frame_layout(codec_ctx_->width, codec_ctx_->height);
    extract_buffer_.reserve(static_cast<std::size_t>(layout_.bytes_per_frame) * 2);
}

int64_t VideoDecoder::total_frames() const {
    if (video_stream_index_ >= 0) {
        const AVStream *stream = format_ctx_->streams[video_stream_index_];
        if (stream->nb_frames > 0) {
            return stream->nb_frames;
        }
        if (stream->duration > 0 && stream->time_base.den > 0) {
            const double duration_sec = static_cast<double>(stream->duration) *
                                        av_q2d(stream->time_base);
            return static_cast<int64_t>(duration_sec * FRAME_FPS);
        }
    }
    return -1;
}

void VideoDecoder::extract_data_into(std::vector<std::byte> &dest,
                                     const int block_size) const {
    const auto layout = compute_frame_layout(
        codec_ctx_->width, codec_ctx_->height, block_size, 1);
    const int blocks_per_row = layout.blocks_per_row;
    const int total_blocks = layout.total_blocks;
    constexpr int blocks_per_byte = 8 / BITS_PER_BLOCK;

    const int total_bytes = total_blocks / blocks_per_byte;
    const uint8_t *src_base;
    int src_stride;
    if (is_gray8_) {
        src_base = frame_->data[0];
        src_stride = frame_->linesize[0];
    } else {
        src_base = gray_frame_->data[0];
        src_stride = gray_frame_->linesize[0];
    }

    const std::size_t base = dest.size();
    dest.resize(base + total_bytes);
    auto *out = reinterpret_cast<uint8_t *>(dest.data() + base);
    std::memset(out, 0, total_bytes);

    if (block_size == 8) {
#pragma omp parallel for schedule(static)
        for (int byte_idx = 0; byte_idx < total_bytes; ++byte_idx) {
            uint8_t current_byte = 0;
            for (int sub = 0; sub < 8; ++sub) {
                constexpr int32_t C0 = 8035; // round(cos(1*pi/16) * 8192)
                constexpr int32_t C1 = 6811; // round(cos(3*pi/16) * 8192)
                constexpr int32_t C2 = 4551; // round(cos(5*pi/16) * 8192)
                constexpr int32_t C3 = 1598; // round(cos(7*pi/16) * 8192)

                const int block_idx = byte_idx * 8 + sub;
                const int block_row = block_idx / blocks_per_row;
                const int block_col = block_idx % blocks_per_row;
                const int base_x = block_col * block_size;
                const int base_y = block_row * block_size;

                int col[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (int y = 0; y < 8; ++y) {
                    const uint8_t *row = src_base + (base_y + y) * src_stride + base_x;
                    col[0] += row[0];
                    col[1] += row[1];
                    col[2] += row[2];
                    col[3] += row[3];
                    col[4] += row[4];
                    col[5] += row[5];
                    col[6] += row[6];
                    col[7] += row[7];
                }

                const int32_t s =
                        C0 * (col[0] - col[7]) +
                        C1 * (col[1] - col[6]) +
                        C2 * (col[2] - col[5]) +
                        C3 * (col[3] - col[4]);
                current_byte = static_cast<uint8_t>((current_byte << 1) | (s > 0 ? 1 : 0));
            }
            out[byte_idx] = current_byte;
        }
    } else {
        constexpr double pi = 3.14159265358979323846;
#pragma omp parallel for schedule(static)
        for (int byte_idx = 0; byte_idx < total_bytes; ++byte_idx) {
            uint8_t current_byte = 0;
            for (int sub = 0; sub < 8; ++sub) {
                const int block_idx = byte_idx * 8 + sub;
                const int block_row = block_idx / blocks_per_row;
                const int block_col = block_idx % blocks_per_row;
                const int base_x = block_col * block_size;
                const int base_y = block_row * block_size;
                double projection = 0.0;
                for (int y = 0; y < block_size; ++y) {
                    const uint8_t *row = src_base +
                        (base_y + y) * src_stride + base_x;
                    for (int x = 0; x < block_size; ++x) {
                        projection += row[x] * std::cos(
                            (2.0 * x + 1.0) * pi /
                            (2.0 * block_size));
                    }
                }
                current_byte = static_cast<uint8_t>(
                    (current_byte << 1) |
                    (projection > 0.0 ? 1 : 0));
            }
            out[byte_idx] = current_byte;
        }
    }
}

bool VideoDecoder::detect_geometry() {
    for (const int candidate : {8, 4}) {
        if (codec_ctx_->width % candidate != 0 ||
            codec_ctx_->height % candidate != 0)
            continue;
        std::vector<std::byte> raw;
        extract_data_into(raw, candidate);
        for (std::size_t offset = 0; offset + HEADER_SIZE <= raw.size();
             ++offset) {
            uint32_t magic = 0;
            std::memcpy(&magic, raw.data() + offset, sizeof(magic));
            if (magic == MAGIC_ID &&
                Decoder::validate_raw_packet_crc(
                    std::span(raw).subspan(offset))) {
                block_size_ = candidate;
                layout_ = compute_frame_layout(
                    codec_ctx_->width, codec_ctx_->height,
                    block_size_, 1);
                extract_buffer_.reserve(
                    static_cast<std::size_t>(layout_.bytes_per_frame) * 2);
                geometry_detected_ = true;
                return true;
            }
        }
    }
    return false;
}

std::vector<std::byte> VideoDecoder::extract_data_from_frame() const {
    std::vector<std::byte> data;
    extract_data_into(data, block_size_);
    return data;
}

std::size_t get_packet_size(const std::span<const std::byte> data) {
    if (data.size() < 5) {
        return HEADER_SIZE + SYMBOL_SIZE_BYTES;
    }
    const auto version = static_cast<uint8_t>(data[4]);
    return (version == VERSION_ID_V2)
               ? (HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES)
               : (HEADER_SIZE + SYMBOL_SIZE_BYTES);
}

void VideoDecoder::extract_packets_from_buffer(std::vector<std::byte> &accumulated,
                                               std::vector<std::vector<std::byte> > &out_packets) {
    std::size_t offset = 0;

    while (offset + 4 <= accumulated.size()) {
        uint32_t magic = 0;
        std::memcpy(&magic, accumulated.data() + offset, sizeof(magic));
        if (magic == MAGIC_ID) {
            const std::size_t pkt_size = get_packet_size(
                std::span<const std::byte>(accumulated.data() + offset,
                                           accumulated.size() - offset));
            if (offset + pkt_size > accumulated.size()) break;
            out_packets.emplace_back(
                accumulated.begin() + static_cast<std::ptrdiff_t>(offset),
                accumulated.begin() + static_cast<std::ptrdiff_t>(offset + pkt_size));
            offset += pkt_size;
        } else {
            ++offset;
        }
    }

    accumulated.erase(accumulated.begin(),
                      accumulated.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<std::vector<std::byte> > VideoDecoder::extract_packets_from_frame() const {
    const auto raw_data = extract_data_from_frame();
    std::vector<std::vector<std::byte> > packets;
    const std::size_t packet_size = get_packet_size(std::span(raw_data));
    packets.reserve(raw_data.size() / packet_size);
    std::size_t offset = 0;
    while (offset + packet_size <= raw_data.size()) {
        if (offset + 4 <= raw_data.size()) {
            uint32_t magic = 0;
            std::memcpy(&magic, raw_data.data() + offset, sizeof(magic));
            if (magic != MAGIC_ID) {
                break;
            }
        }
        std::vector<std::byte> packet(
            raw_data.begin() + static_cast<std::ptrdiff_t>(offset),
            raw_data.begin() + static_cast<std::ptrdiff_t>(offset + packet_size));
        packets.push_back(std::move(packet));
        offset += packet_size;
    }
    return packets;
}

void VideoDecoder::prepare_frame_for_extraction() {
    {
        ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
        if (!is_gray8_) {
            sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height,
                      gray_frame_->data, gray_frame_->linesize);
        }
    }
    ++frame_index_;
}

void VideoDecoder::copy_current_gray8_frame(
    std::vector<std::byte> &pixels) const {
    const uint8_t *source = is_gray8_
        ? frame_->data[0] : gray_frame_->data[0];
    const int stride = is_gray8_
        ? frame_->linesize[0] : gray_frame_->linesize[0];
    pixels.resize(
        static_cast<std::size_t>(codec_ctx_->width) * codec_ctx_->height);
    for (int y = 0; y < codec_ctx_->height; ++y) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(y) * codec_ctx_->width,
            source + y * stride,
            static_cast<std::size_t>(codec_ctx_->width));
    }
}

bool VideoDecoder::decode_next_gray8_frame(
    std::vector<std::byte> &pixels) {
    if (eof_) return false;
    while (true) {
        int receive_result = 0;
        {
            ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
            receive_result = avcodec_receive_frame(codec_ctx_, frame_);
        }
        if (receive_result == 0) {
            prepare_frame_for_extraction();
            copy_current_gray8_frame(pixels);
            return true;
        }
        if (receive_result != AVERROR(EAGAIN) &&
            receive_result != AVERROR_EOF) {
            throw std::runtime_error("Error receiving frame");
        }

        if (raw_drain_sent_) {
            eof_ = true;
            return false;
        }

        int read_result = 0;
        do {
            {
                ScopedTimer timer(
                    profiler_, PerformanceStage::VideoReadDemux);
                read_result = av_read_frame(format_ctx_, av_packet_);
            }
            if (read_result < 0) {
                ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
                const int send_result =
                    avcodec_send_packet(codec_ctx_, nullptr);
                raw_drain_sent_ = true;
                if (send_result < 0 && send_result != AVERROR_EOF) {
                    throw std::runtime_error("Error draining decoder");
                }
                break;
            }
            if (av_packet_->stream_index != video_stream_index_) {
                av_packet_unref(av_packet_);
            }
        } while (read_result >= 0 &&
                 av_packet_->stream_index != video_stream_index_);

        if (read_result >= 0) {
            int send_result = 0;
            {
                ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
                send_result =
                    avcodec_send_packet(codec_ctx_, av_packet_);
            }
            av_packet_unref(av_packet_);
            if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
                throw std::runtime_error("Error sending video packet");
            }
        }
    }
}

std::vector<std::vector<std::byte> > VideoDecoder::accumulate_frame_and_extract_packets() {
    ScopedTimer timer(profiler_, PerformanceStage::PacketExtraction);
    if (!geometry_detected_ && !detect_geometry()) return {};
    extract_data_into(extract_buffer_, block_size_);
    std::vector<std::vector<std::byte> > packets;
    packets.reserve(extract_buffer_.size() / (HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES));
    extract_packets_from_buffer(extract_buffer_, packets);
    return packets;
}

std::vector<std::vector<std::byte> > VideoDecoder::flush_decoder_and_collect_packets() {
    {
        ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
        avcodec_send_packet(codec_ctx_, nullptr);
    }
    std::vector<std::vector<std::byte> > collected;
    while (true) {
        int ret = 0;
        {
            ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
            ret = avcodec_receive_frame(codec_ctx_, frame_);
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving frame");
        }
        prepare_frame_for_extraction();
        for (auto packets = accumulate_frame_and_extract_packets(); auto &p: packets) {
            collected.push_back(std::move(p));
        }
    }
    return collected;
}

std::vector<std::vector<std::byte> > VideoDecoder::decode_next_frame() {
    if (eof_) {
        return {};
    }

    while (true) {
        int read_ret = 0;
        {
            ScopedTimer timer(profiler_, PerformanceStage::VideoReadDemux);
            read_ret = av_read_frame(format_ctx_, av_packet_);
        }
        if (read_ret < 0) {
            eof_ = true;
            if (auto flushed = flush_decoder_and_collect_packets(); !flushed.empty()) {
                return flushed;
            }
            if (!extract_buffer_.empty()) {
                std::vector<std::vector<std::byte> > packets;
                extract_packets_from_buffer(extract_buffer_, packets);
                return packets;
            }
            return {};
        }

        if (av_packet_->stream_index != video_stream_index_) {
            av_packet_unref(av_packet_);
            continue;
        }

        int send_ret = 0;
        {
            ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
            send_ret = avcodec_send_packet(codec_ctx_, av_packet_);
        }
        av_packet_unref(av_packet_);
        if (send_ret < 0) {
            continue;
        }

        int recv_ret = 0;
        {
            ScopedTimer timer(profiler_, PerformanceStage::FrameDecode);
            recv_ret = avcodec_receive_frame(codec_ctx_, frame_);
        }
        if (recv_ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (recv_ret == AVERROR_EOF) {
            eof_ = true;
            return {};
        }
        if (recv_ret < 0) {
            throw std::runtime_error("Error receiving frame");
        }

        break;
    }
    prepare_frame_for_extraction();
    return accumulate_frame_and_extract_packets();
}

std::vector<std::vector<std::byte> > VideoDecoder::decode_all_frames() {
    std::vector<std::vector<std::byte> > results;
    while (!eof_) {
        for (auto packets = decode_next_frame(); auto &pkt: packets) {
            results.push_back(std::move(pkt));
        }
    }
    return results;
}
