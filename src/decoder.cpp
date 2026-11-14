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

#include "decoder.h"
#include "safe_output.h"
#include <atomic>
#include "performance_profiler.h"

#include "configuration.h"
#include "crypto.h"
#include "libs/wirehair/wirehair.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>
#include <fstream>
#include <mutex>
#include <stdexcept>

static std::once_flag ensure_init;

static void ensureWirehairInit() {
    std::call_once(ensure_init, [] {
        if (const WirehairResult result = wirehair_init(); result != Wirehair_Success) {
            throw std::runtime_error("wirehair_init failed");
        }
    });
}

static uint8_t readByte(std::span<const std::byte> buffer, const std::size_t offset) {
    return static_cast<uint8_t>(buffer[offset]);
}

static uint16_t readU16LE(const std::span<const std::byte> buffer, const std::size_t offset) {
    uint16_t value;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

static uint32_t readU32LE(const std::span<const std::byte> buffer, const std::size_t offset) {
    uint32_t value;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

ChunkDecoder::ChunkDecoder(const uint32_t chunk_index, const uint32_t chunk_size, const uint32_t k,
                           const uint16_t symbol_size)
    : chunk_index_(chunk_index)
      , chunk_size_(chunk_size)
      , k_(k)
      , symbol_size_(symbol_size) {
    ensureWirehairInit();
    codec_ = wirehair_decoder_create(nullptr, chunk_size_, symbol_size_);
    if (!codec_) {
        throw std::runtime_error("wirehair_decoder_create failed");
    }
}

ChunkDecoder::~ChunkDecoder() {
    if (codec_) {
        wirehair_free(static_cast<WirehairCodec>(codec_));
        codec_ = nullptr;
    }
}

ChunkDecoder::ChunkDecoder(ChunkDecoder &&other) noexcept
    : chunk_index_(other.chunk_index_)
      , chunk_size_(other.chunk_size_)
      , k_(other.k_)
      , symbol_size_(other.symbol_size_)
      , codec_(other.codec_)
      , decoded_(other.decoded_)
      , packets_received_(other.packets_received_)
      , decoded_data_(std::move(other.decoded_data_)) {
    other.codec_ = nullptr;
}

ChunkDecoder &ChunkDecoder::operator=(ChunkDecoder &&other) noexcept {
    if (this != &other) {
        if (codec_) {
            wirehair_free(static_cast<WirehairCodec>(codec_));
        }
        chunk_index_ = other.chunk_index_;
        chunk_size_ = other.chunk_size_;
        k_ = other.k_;
        symbol_size_ = other.symbol_size_;
        codec_ = other.codec_;
        decoded_ = other.decoded_;
        packets_received_ = other.packets_received_;
        decoded_data_ = std::move(other.decoded_data_);
        other.codec_ = nullptr;
    }
    return *this;
}

bool ChunkDecoder::add_packet(const uint32_t esi, const std::span<const std::byte> payload) {
    if (decoded_) {
        return true;
    }

    if (!codec_) {
        throw std::runtime_error("codec is null");
    }

    ++packets_received_;

    const auto *payloadData = reinterpret_cast<const uint8_t *>(payload.data());
    const auto payloadSize = static_cast<uint32_t>(payload.size());

    WirehairResult result = wirehair_decode(
        static_cast<WirehairCodec>(codec_),
        esi,
        payloadData,
        payloadSize
    );

    if (result == Wirehair_Success) {
        decoded_data_.resize(chunk_size_);
        result = wirehair_recover(
            static_cast<WirehairCodec>(codec_),
            decoded_data_.data(),
            chunk_size_
        );

        if (result != Wirehair_Success) {
            throw std::runtime_error("wirehair_recover failed");
        }

        decoded_ = true;
        return true;
    }
    if (result == Wirehair_NeedMore) {
        return false;
    }
    throw std::runtime_error("wirehair_decode failed with error");
}

std::vector<std::byte> ChunkDecoder::get_decoded_data() const {
    if (!decoded_) {
        throw std::runtime_error("data not yet decoded");
    }
    return decoded_data_;
}

std::vector<std::byte> ChunkDecoder::consume_decoded_data() {
    if (!decoded_) {
        throw std::runtime_error("data not yet decoded");
    }
    return std::move(decoded_data_);
}

Decoder::Decoder() = default;

std::optional<DecodedPacket> Decoder::parse_packet(const std::span<const std::byte> packet_data) {
    if (packet_data.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    const uint8_t version = readByte(packet_data, VERSION_OFF);
    const size_t header_size = (version == VERSION_ID_V2) ? HEADER_SIZE_V2 : HEADER_SIZE;
    if (version != VERSION_ID && version != VERSION_ID_V2) {
        return std::nullopt;
    }
    if (packet_data.size() < header_size) {
        return std::nullopt;
    }

    DecodedPacket result;
    auto &[magic, v, flags, file_id, chunk_index, chunk_size, original_size, symbol_size, k, esi, payload_len, crc] =
            result.header;

    magic = readU32LE(packet_data, MAGIC_OFF);
    if (magic != MAGIC_ID) {
        return std::nullopt;
    }

    v = version;
    flags = readByte(packet_data, FLAGS_OFF);

    std::memcpy(file_id.data(), packet_data.data() + FILE_ID_OFF, FILE_ID_SIZE);
    chunk_index = readU32LE(packet_data, CHUNK_INDEX_OFF);
    chunk_size = readU32LE(packet_data, CHUNK_SIZE_OFF);
    if (version == VERSION_ID_V2) {
        original_size = readU32LE(packet_data, ORIGINAL_SIZE_OFF);
    } else {
        original_size = chunk_size;
    }
    symbol_size = readU16LE(packet_data, SYMBOL_SIZE_OFF);
    k = readU32LE(packet_data, K_OFF);
    esi = readU32LE(packet_data, ESI_OFF);
    payload_len = readU16LE(packet_data, PAYLOAD_LEN_OFF);
    crc = readU32LE(packet_data, (version == VERSION_ID_V2) ? CRC_OFF_V2 : CRC_OFF);

    if (const size_t expected_total = header_size + symbol_size; packet_data.size() < expected_total) {
        return std::nullopt;
    }
    std::memcpy(result.raw_header.data(), packet_data.data(), header_size);

    result.payload.resize(symbol_size);
    std::memcpy(result.payload.data(), packet_data.data() + header_size, symbol_size);

    return result;
}

bool Decoder::validate_raw_packet_crc(const std::span<const std::byte> packet_data) {
    if (packet_data.size() < HEADER_SIZE) {
        return false;
    }

    const uint8_t version = readByte(packet_data, VERSION_OFF);
    const size_t header_size = (version == VERSION_ID_V2) ? HEADER_SIZE_V2 : HEADER_SIZE;
    const size_t crc_offset = (version == VERSION_ID_V2) ? CRC_OFF_V2 : CRC_OFF;
    if (version != VERSION_ID && version != VERSION_ID_V2) {
        return false;
    }
    if (packet_data.size() < header_size) {
        return false;
    }

    const uint16_t symbol_size = readU16LE(packet_data, SYMBOL_SIZE_OFF);
    if (packet_data.size() < header_size + symbol_size) {
        return false;
    }

    const uint32_t stored_crc = readU32LE(packet_data, crc_offset);
    const uint8_t flags = readByte(packet_data, FLAGS_OFF);
    const HashAlgorithm algo = (flags & UseXXHash) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;

    const auto header_span = packet_data.subspan(0, header_size);
    const auto payload_span = packet_data.subspan(header_size, symbol_size);
    const uint32_t computed_crc = packet_checksum(header_span, payload_span, crc_offset, algo, CRC_SIZE);

    return stored_crc == computed_crc;
}

bool Decoder::validate_packet_crc(const DecodedPacket &packet) {
    const bool is_v2 = (packet.header.version == VERSION_ID_V2);
    const size_t header_size = is_v2 ? HEADER_SIZE_V2 : HEADER_SIZE;
    const size_t crc_offset = is_v2 ? CRC_OFF_V2 : CRC_OFF;

    auto header = packet.raw_header;
    constexpr uint32_t zero_crc = 0;
    std::memcpy(header.data() + crc_offset, &zero_crc, sizeof(zero_crc));

    const std::span<const std::byte> headerSpan(header.data(), header_size);
    const std::span payloadSpan(packet.payload.data(), packet.payload.size());
    const HashAlgorithm algo = (packet.header.flags & UseXXHash) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;
    const uint32_t computed_crc = packet_checksum(headerSpan, payloadSpan, crc_offset, algo, CRC_SIZE);

    return computed_crc == packet.header.crc;
}

namespace {
    struct ParsedPacketView {
        PacketHeader header;
        std::span<const std::byte> payload;
    };
}

static std::optional<ParsedPacketView> parse_and_validate_packet(const std::span<const std::byte> packet_data) {
    if (packet_data.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    ParsedPacketView result;
    auto &[magic, v, flags, file_id, chunk_index, chunk_size, original_size, symbol_size, k, esi, payload_len, crc] =
            result.header;

    v = readByte(packet_data, VERSION_OFF);
    if (v != VERSION_ID && v != VERSION_ID_V2) {
        return std::nullopt;
    }
    const size_t header_size = (v == VERSION_ID_V2) ? HEADER_SIZE_V2 : HEADER_SIZE;
    const size_t crc_offset = (v == VERSION_ID_V2) ? CRC_OFF_V2 : CRC_OFF;
    if (packet_data.size() < header_size) {
        return std::nullopt;
    }

    magic = readU32LE(packet_data, MAGIC_OFF);
    if (magic != MAGIC_ID) {
        return std::nullopt;
    }

    flags = readByte(packet_data, FLAGS_OFF);
    std::memcpy(file_id.data(), packet_data.data() + FILE_ID_OFF, FILE_ID_SIZE);
    chunk_index = readU32LE(packet_data, CHUNK_INDEX_OFF);
    chunk_size = readU32LE(packet_data, CHUNK_SIZE_OFF);
    original_size = (v == VERSION_ID_V2) ? readU32LE(packet_data, ORIGINAL_SIZE_OFF) : chunk_size;
    symbol_size = readU16LE(packet_data, SYMBOL_SIZE_OFF);
    k = readU32LE(packet_data, K_OFF);
    esi = readU32LE(packet_data, ESI_OFF);
    payload_len = readU16LE(packet_data, PAYLOAD_LEN_OFF);

    if (packet_data.size() < header_size + symbol_size) {
        return std::nullopt;
    }

    crc = readU32LE(packet_data, crc_offset);
    const auto header_span = packet_data.subspan(0, header_size);
    const auto payload_span = packet_data.subspan(header_size, symbol_size);
    if (const HashAlgorithm algo = (flags & UseXXHash) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32; packet_checksum(header_span, payload_span, crc_offset, algo, CRC_SIZE) != crc) {
        return std::nullopt;
    }

    result.payload = payload_span;
    return result;
}

bool Decoder::header_fields_in_bounds(const PacketHeader &header) {
    // The encoder always emits fixed-size symbols and pads every chunk to at
    // least two symbols, never exceeding CHUNK_SIZE_BYTES (encrypted chunks
    // included). Wirehair additionally needs at least two source symbols.
    if (header.symbol_size != SYMBOL_SIZE_BYTES) {
        return false;
    }
    if (header.chunk_size <= header.symbol_size || header.chunk_size > CHUNK_SIZE_BYTES) {
        return false;
    }
    const uint32_t expected_k =
        (header.chunk_size + header.symbol_size - 1u) / header.symbol_size;
    if (header.k != expected_k) {
        return false;
    }
    if (header.original_size > header.chunk_size) {
        return false;
    }
    if (header.payload_len > header.symbol_size) {
        return false;
    }
    return true;
}

std::optional<ChunkDecodeResult> Decoder::process_packet(const std::span<const std::byte> packet_data, const bool compute_sha256) {
    ++total_packets_;

    const auto parsed = parse_and_validate_packet(packet_data);
    if (!parsed) {
        return std::nullopt;
    }
    return accept_packet(parsed->header, parsed->payload, compute_sha256);
}

std::optional<ChunkDecodeResult> Decoder::process_packet(const DecodedPacket &packet, const bool compute_sha256) {
    ++total_packets_;
    if (!validate_packet_crc(packet)) {
        return std::nullopt;
    }
    return accept_packet(
        packet.header,
        std::span<const std::byte>(packet.payload.data(), packet.payload.size()),
        compute_sha256);
}

std::optional<ChunkDecodeResult> Decoder::accept_packet(
    const PacketHeader &hdr, const std::span<const std::byte> payload, const bool compute_sha256) {
    if (!header_fields_in_bounds(hdr) || payload.size() != hdr.symbol_size) {
        ++rejected_packets_;
        return std::nullopt;
    }

    // The first accepted packet locks the file identity and encryption mode.
    // Packets from any other file are refused instead of being mixed in; if
    // the lock landed on a stray packet, recovery fails rather than
    // producing a spliced file.
    const bool packet_encrypted = (hdr.flags & Encrypted) != 0;
    if (!id) {
        id = hdr.file_id;
        encrypted_ = packet_encrypted;
    } else if (hdr.file_id != *id || packet_encrypted != encrypted_) {
        ++rejected_packets_;
        return std::nullopt;
    }

    if (completed_chunks.contains(hdr.chunk_index)) {
        return std::nullopt;
    }

    auto active = active_decoders.find(hdr.chunk_index);
    if (active == active_decoders.end()) {
        active = active_decoders.emplace(
            hdr.chunk_index,
            ActiveChunk{ChunkDecoder(hdr.chunk_index, hdr.chunk_size, hdr.k, hdr.symbol_size), hdr.original_size}).first;
    } else {
        const ChunkDecoder &existing = active->second.decoder;
        if (existing.chunk_size() != hdr.chunk_size ||
            existing.source_symbol_count() != hdr.k ||
            existing.symbol_size() != hdr.symbol_size ||
            active->second.original_size != hdr.original_size) {
            ++rejected_packets_;
            return std::nullopt;
        }
    }

    // Only record the end-of-file marker once the packet has passed every
    // identity and consistency check above.
    if ((hdr.flags & LastChunk) != 0) {
        if (!last_chunk_index_) {
            last_chunk_index_ = hdr.chunk_index;
        } else if (*last_chunk_index_ != hdr.chunk_index) {
            last_chunk_conflict_ = true;
        }
    }

    if (ChunkDecoder &decoder = active->second.decoder; decoder.add_packet(hdr.esi, payload)) {
        ChunkDecodeResult result;
        result.chunk_index = hdr.chunk_index;
        result.data = decoder.consume_decoded_data();
        const uint32_t copy_len = std::min(static_cast<uint32_t>(result.data.size()), active->second.original_size);
        result.data.resize(copy_len);
        if (compute_sha256) {
            result.sha256 = sha256(std::span<const std::byte>(result.data.data(), result.data.size()));
        }
        result.success = true;
        // Preserves the historical contract: the chunk bytes live in the
        // decoder, the returned result carries index/hash/success.
        completed_chunks.emplace(hdr.chunk_index, std::move(result.data));
        ++completed_count_;
        active_decoders.erase(active);

        return result;
    }

    return std::nullopt;
}

bool Decoder::is_chunk_complete(const uint32_t chunk_index) const {
    return completed_chunks.contains(chunk_index);
}

std::optional<std::vector<std::byte> > Decoder::get_chunk_data(const uint32_t chunk_index) const {
    if (const auto it = completed_chunks.find(chunk_index); it != completed_chunks.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<uint32_t> Decoder::completed_chunk_indices() const {
    std::vector<uint32_t> indices;
    indices.reserve(completed_count_);
    for (const auto &index: completed_chunks | std::views::keys) {
        indices.push_back(index);
    }
    return indices;
}

std::optional<uint32_t> Decoder::expected_chunk_count() const {
    if (!last_chunk_index_ || last_chunk_conflict_ ||
        *last_chunk_index_ == std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return *last_chunk_index_ + 1u;
}

bool Decoder::has_complete_contiguous_chunks(const uint32_t expected_chunks) const {
    if (expected_chunks == 0 || last_chunk_conflict_) {
        return false;
    }
    if (last_chunk_index_ && *last_chunk_index_ + 1ull != expected_chunks) {
        return false;
    }
    if (completed_count_ != expected_chunks || completed_chunks.size() != expected_chunks) {
        return false;
    }
    // std::map is ordered: exactly expected_chunks keys whose largest is
    // expected_chunks - 1 means every index in [0, expected_chunks) exists.
    return completed_chunks.rbegin()->first == expected_chunks - 1u;
}

void Decoder::set_decrypt_key(const std::span<const std::byte, 32> key) {
    std::memcpy(decrypt_key_.data(), key.data(), 32);
    decrypt_key_set_ = true;
}

void Decoder::clear_decrypt_key() {
    if (decrypt_key_set_) {
        secure_zero(decrypt_key_);
        decrypt_key_set_ = false;
    }
}

namespace {
    using ChunkPointers = std::vector<const std::vector<std::byte> *>;

    // Caller must have verified that every index in [0, expected) exists.
    ChunkPointers ordered_chunks(
        const std::map<uint32_t, std::vector<std::byte> > &chunks,
        const uint32_t expected_chunks) {
        ChunkPointers ordered;
        ordered.reserve(expected_chunks);
        for (const auto &chunk: chunks | std::views::values) {
            ordered.push_back(&chunk);
        }
        return ordered;
    }

    std::optional<std::vector<std::size_t> > compute_chunk_sizes(
        const ChunkPointers &chunks,
        const uint32_t expected_chunks,
        const bool encrypted,
        const bool decrypt_key_set) {
        std::vector<std::size_t> sizes(expected_chunks);
        for (uint32_t idx = 0; idx < expected_chunks; ++idx) {
            const auto &chunk = *chunks[idx];
            if (encrypted && decrypt_key_set) {
                if (chunk.size() < CRYPTO_PLAIN_SIZE_HEADER) {
                    return std::nullopt;
                }
                sizes[idx] = read_plain_size_from_header(chunk);
                if (sizes[idx] > CHUNK_SIZE_BYTES) {
                    return std::nullopt;
                }
            } else {
                sizes[idx] = chunk.size();
            }
        }
        return sizes;
    }

    std::vector<std::size_t> compute_prefix_offsets(const std::vector<std::size_t> &sizes) {
        std::vector<std::size_t> offsets(sizes.size() + 1);
        offsets[0] = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            offsets[i + 1] = offsets[i] + sizes[i];
        }
        return offsets;
    }

    void decrypt_and_copy_into(
        std::vector<std::byte> &result,
        const ChunkPointers &chunks,
        const uint32_t expected_chunks,
        const std::vector<std::size_t> &offsets,
        const std::vector<std::size_t> &sizes,
        const bool encrypted,
        const bool decrypt_key_set,
        const std::array<std::byte, 32> &decrypt_key,
        const std::array<std::byte, 16> &file_id) {
#pragma omp parallel for schedule(static)
        for (int i = 0; i < static_cast<int>(expected_chunks); ++i) {
            const auto &chunk = *chunks[i];
            const std::size_t copy_size = sizes[i];
            if (encrypted && decrypt_key_set) {
                decrypt_chunk_into(
                    std::span<std::byte>(result.data() + offsets[i], copy_size),
                    chunk, decrypt_key, file_id, static_cast<uint32_t>(i));
            } else {
                std::memcpy(result.data() + offsets[i], chunk.data(), copy_size);
            }
        }
    }
}

std::optional<std::vector<std::byte> > Decoder::assemble_file(const uint32_t expected_chunks) const {
    if (!has_complete_contiguous_chunks(expected_chunks)) {
        return std::nullopt;
    }
    if (encrypted_ && !decrypt_key_set_) {
        return std::nullopt;
    }
    if (!id) {
        return std::nullopt;
    }

    const ChunkPointers chunks = ordered_chunks(completed_chunks, expected_chunks);
    const auto chunk_sizes = compute_chunk_sizes(
        chunks, expected_chunks, encrypted_, decrypt_key_set_);
    if (!chunk_sizes) {
        return std::nullopt;
    }
    const auto offsets = compute_prefix_offsets(*chunk_sizes);
    std::vector<std::byte> result(offsets[expected_chunks]);
    decrypt_and_copy_into(result, chunks, expected_chunks, offsets,
                          *chunk_sizes, encrypted_, decrypt_key_set_, decrypt_key_, *id);
    return result;
}

bool Decoder::write_assembled_file(const std::string &output_path,
                                   const uint32_t expected_chunks,
                                   PerformanceProfiler *profiler,
                                   const std::function<bool()> &publish_allowed,
                                   uint64_t *output_size) const try {
    if (!has_complete_contiguous_chunks(expected_chunks)) {
        return false;
    }
    if (encrypted_ && !decrypt_key_set_) {
        return false;
    }
    if (!id) {
        return false;
    }

    const ChunkPointers chunks = ordered_chunks(completed_chunks, expected_chunks);
    SafeOutputFile output(output_path, SafeOutputFile::Mode::ExclusiveWrite);

    if (encrypted_ && decrypt_key_set_) {
        std::vector<std::size_t> sizes(expected_chunks);
        {
            ScopedTimer timer(profiler, PerformanceStage::Postprocess);
            for (uint32_t i = 0; i < expected_chunks; ++i) {
                const auto &chunk = *chunks[i];
                if (chunk.size() < CRYPTO_PLAIN_SIZE_HEADER) {
                    return false;
                }
                sizes[i] = read_plain_size_from_header(chunk);
                if (sizes[i] > CHUNK_SIZE_BYTES) {
                    return false;
                }
            }
        }

        std::vector<std::vector<std::byte>> decrypted_chunks(expected_chunks);
        std::atomic_bool decrypt_error{false};

        {
            ScopedTimer timer(profiler, PerformanceStage::Postprocess);
#pragma omp parallel for schedule(static)
            for (int i = 0; i < static_cast<int>(expected_chunks); ++i) {
                if (decrypt_error) continue;
                try {
                    decrypted_chunks[i].resize(sizes[i]);
                    decrypt_chunk_into(
                        std::span<std::byte>(decrypted_chunks[i].data(), sizes[i]),
                        *chunks[i],
                        decrypt_key_, *id, static_cast<uint32_t>(i));
                } catch (...) {
                    decrypt_error = true;
                }
            }
        }

        if (decrypt_error) return false;

        {
            ScopedTimer timer(profiler, PerformanceStage::OutputWrite);
            for (uint32_t i = 0; i < expected_chunks; ++i) {
                output.write(decrypted_chunks[i]);
            }
        }
    } else {
        ScopedTimer timer(profiler, PerformanceStage::OutputWrite);
        for (uint32_t i = 0; i < expected_chunks; ++i) {
            const auto &chunk = *chunks[i];
            output.write(chunk);
        }
    }

    {
        ScopedTimer timer(profiler, PerformanceStage::OutputWrite);
        output.finish_writing();
        // Read all fallible metadata before publication. Nothing after a
        // successful rename should turn the operation into a reported failure.
        const auto size = std::filesystem::file_size(output.partial_path());
        if (publish_allowed && !publish_allowed()) return false;
        output.commit();
        if (output_size) *output_size = size;
    }
    return true;
} catch (...) {
    return false;
}
