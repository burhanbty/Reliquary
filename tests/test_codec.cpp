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

#include <gtest/gtest.h>

#include "configuration.h"
#include "decoder.h"
#include "encoder.h"
#include "integrity.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace {
    Encoder::FileId make_test_file_id() {
        Encoder::FileId file_id{};
        for (std::size_t i = 0; i < file_id.size(); ++i) {
            file_id[i] = std::byte{static_cast<uint8_t>(i)};
        }
        return file_id;
    }

    std::vector<std::byte> make_test_data(const std::size_t byte_count) {
        std::vector<std::byte> data(byte_count);
        for (std::size_t i = 0; i < byte_count; ++i) {
            data[i] = std::byte{static_cast<uint8_t>(i % 251)};
        }
        return data;
    }

    uint32_t read_u32_le(const std::byte *source) {
        uint32_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }

    std::span<const std::byte> packet_span(const Packet &packet) {
        return {packet.bytes.data(), packet.bytes.size()};
    }

    struct EncodeResult {
        std::vector<Packet> packets;
        ChunkManifestEntry manifest;
    };

    EncodeResult encode_test_data(const Encoder &encoder,
                                  const std::vector<std::byte> &data,
                                  const uint32_t chunk_index = 0,
                                  const bool is_last_chunk = false,
                                  const bool encrypted = false) {
        auto [packets, manifest] = encoder.encode_chunk(chunk_index, data, is_last_chunk, encrypted);
        return {std::move(packets), manifest};
    }

    bool feed_all_packets_to_decoder(Decoder &decoder, const std::vector<Packet> &packets) {
        bool any_decoded = false;
        for (const Packet &packet: packets) {
            if (std::optional<ChunkDecodeResult> result = decoder.process_packet(packet_span(packet));
                result.has_value() && result->success) {
                any_decoded = true;
            }
        }
        return any_decoded;
    }
} // namespace

TEST(Codec, Encoder_ProducesCorrectPacketCount) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);
    const uint64_t source_packet_count =
        calculate_source_packet_count(input_data.size());
    const uint64_t repair_packet_count =
        calculate_repair_packet_count(
            source_packet_count, DEFAULT_REPAIR_RATIO);
    EXPECT_EQ(
        packets.size(), source_packet_count + repair_packet_count);
}

TEST(Codec, Encoder_PacketHasCorrectMagic) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);
    for (const auto &[bytes]: packets) {
        const uint32_t magic = read_u32_le(bytes.data());
        EXPECT_EQ(magic, MAGIC_ID);
    }
}

TEST(Codec, Encoder_PacketHasV2Version) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);
    for (const auto &[bytes]: packets) {
        const auto version = static_cast<uint8_t>(bytes[VERSION_OFF]);
        EXPECT_EQ(version, VERSION_ID_V2);
    }
}

TEST(Codec, Encoder_ManifestMatchesInput) {
    constexpr uint32_t chunk_index = 7;
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, chunk_index);
    EXPECT_EQ(manifest.chunk_index, chunk_index);
    EXPECT_EQ(manifest.original_size, static_cast<uint32_t>(input_data.size()));
}

TEST(Codec, Encoder_LastChunkFlagIsSet) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, true);
    for (const auto &[bytes]: packets) {
        const auto flags = static_cast<uint8_t>(bytes[FLAGS_OFF]);
        EXPECT_NE(flags & LastChunk, 0);
    }
}

TEST(Codec, Encoder_LastChunkFlagNotSetForMiddleChunk) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, false);
    for (const auto &[bytes]: packets) {
        const auto flags = static_cast<uint8_t>(bytes[FLAGS_OFF]);
        EXPECT_EQ(flags & LastChunk, 0);
    }
}

TEST(Codec, Encoder_SmallDataIsPadded) {
    const std::vector<std::byte> small_data = make_test_data(100);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, small_data);

    constexpr auto min_padded_size = static_cast<uint32_t>(SYMBOL_SIZE_BYTES * 2);
    EXPECT_GE(manifest.chunk_size, min_padded_size);
}

TEST(Codec, Decoder_ParsesValidPacket) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(packet_span(packets[0]));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->header.magic, MAGIC_ID);
    EXPECT_EQ(parsed->header.version, VERSION_ID_V2);
}

TEST(Codec, Decoder_RejectsInvalidMagic) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    std::vector corrupted_packet(packets[0].bytes.begin(),
                                 packets[0].bytes.end());
    corrupted_packet[0] = std::byte{0};
    corrupted_packet[1] = std::byte{0};
    corrupted_packet[2] = std::byte{0};
    corrupted_packet[3] = std::byte{0};

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(corrupted_packet);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Codec, Decoder_RejectsTruncatedPacket) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const auto &source_bytes = packets[0].bytes;
    const std::vector truncated_packet(source_bytes.begin(), source_bytes.begin() + 100);

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(truncated_packet);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Codec, Decoder_ValidatesCrcOnParsedPacket) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(packet_span(packets[0]));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(Decoder::validate_packet_crc(*parsed));
}

TEST(Codec, Decoder_ValidatesCrcOnRawBytes) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const bool crc_valid = Decoder::validate_raw_packet_crc(packet_span(packets[0]));
    EXPECT_TRUE(crc_valid);
}

TEST(Codec, Decoder_DetectsCorruptedPayload) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    std::vector corrupted_packet(packets[0].bytes.begin(),
                                 packets[0].bytes.end());
    corrupted_packet[HEADER_SIZE_V2] ^= std::byte{0xFF};

    const bool crc_valid = Decoder::validate_raw_packet_crc(corrupted_packet);
    EXPECT_FALSE(crc_valid);
}

TEST(Codec, Decoder_ChunkRoundtrip) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    Decoder decoder;
    const bool decoded = feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoded);
    EXPECT_TRUE(decoder.is_chunk_complete(0));

    const std::optional<std::vector<std::byte> > recovered_chunk = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered_chunk.has_value());
    EXPECT_EQ(*recovered_chunk, original_data);
}

TEST(Codec, Decoder_ChunkRoundtripVerifiesSha256) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    Decoder decoder;
    std::optional<ChunkDecodeResult> decode_result;
    for (const Packet &packet: packets) {
        if (std::optional<ChunkDecodeResult> result = decoder.process_packet(packet_span(packet), true);
            result.has_value() && result->success) {
            decode_result = std::move(*result);
        }
    }

    ASSERT_TRUE(decode_result.has_value());

    const std::span original_span(original_data.data(), original_data.size());
    const Sha256Digest expected_digest = sha256(original_span);
    EXPECT_EQ(decode_result->sha256, expected_digest);
}

TEST(Codec, Decoder_XXHashAlgorithmRoundtrip) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id(), HashAlgorithm::XXHash32);
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    Decoder decoder;
    const bool decoded = feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoded);

    const std::optional<std::vector<std::byte> > recovered_chunk = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered_chunk.has_value());
    EXPECT_EQ(*recovered_chunk, original_data);
}

TEST(Codec, Encoder_EncryptedFlagIsSet) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, false, true);
    for (const auto &[bytes]: packets) {
        const auto flags = static_cast<uint8_t>(bytes[FLAGS_OFF]);
        EXPECT_NE(flags & Encrypted, 0);
    }
}

TEST(Codec, Decoder_CompletedChunkReturnsNullopt) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets);
    EXPECT_TRUE(decoder.is_chunk_complete(0));

    for (const Packet &packet: packets) {
        auto result = decoder.process_packet(packet_span(packet));
        EXPECT_FALSE(result.has_value());
    }
}

TEST(Codec, Decoder_MultipleChunks) {
    const auto chunk0_data = make_test_data(2048);
    const auto chunk1_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());

    auto [packets0, m0] = encoder.encode_chunk(0, chunk0_data, false);
    auto [packets1, m1] = encoder.encode_chunk(1, chunk1_data, true);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets0);
    feed_all_packets_to_decoder(decoder, packets1);

    EXPECT_EQ(decoder.chunks_completed(), 2u);
    EXPECT_TRUE(decoder.is_chunk_complete(0));
    EXPECT_TRUE(decoder.is_chunk_complete(1));

    const auto data0 = decoder.get_chunk_data(0);
    const auto data1 = decoder.get_chunk_data(1);
    ASSERT_TRUE(data0.has_value());
    ASSERT_TRUE(data1.has_value());
    EXPECT_EQ(*data0, chunk0_data);
    EXPECT_EQ(*data1, chunk1_data);
}

TEST(Codec, Decoder_StateTracking) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    EXPECT_EQ(decoder.total_packets_received(), 0u);
    EXPECT_EQ(decoder.chunks_completed(), 0u);
    EXPECT_FALSE(decoder.is_chunk_complete(0));

    feed_all_packets_to_decoder(decoder, packets);

    EXPECT_EQ(decoder.total_packets_received(), packets.size());
    EXPECT_EQ(decoder.chunks_completed(), 1u);
    EXPECT_TRUE(decoder.is_chunk_complete(0));

    auto indices = decoder.completed_chunk_indices();
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], 0u);
}

TEST(Codec, Decoder_GetChunkDataIsCopy) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoder.get_chunk_data(0).has_value());
    EXPECT_TRUE(decoder.get_chunk_data(0).has_value());
}

TEST(Codec, Decoder_PacketLossResilience) {
    const std::vector<std::byte> original_data = make_test_data(SYMBOL_SIZE_BYTES * 4);
    const Encoder encoder(
        make_test_file_id(), HashAlgorithm::CRC32, {1.0});
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    const std::size_t half = packets.size() / 2;

    Decoder decoder;
    bool decoded = false;
    for (std::size_t i = 0; i < half; ++i) {
        if (auto result = decoder.process_packet(packet_span(packets[i]));
            result.has_value() && result->success) {
            decoded = true;
        }
    }

    EXPECT_TRUE(decoded);
    const auto recovered = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, original_data);
}

TEST(Codec, Decoder_OutOfOrderDelivery) {
    const std::vector<std::byte> original_data = make_test_data(SYMBOL_SIZE_BYTES * 4);
    const Encoder encoder(make_test_file_id());
    auto [packets, manifest] = encode_test_data(encoder, original_data);

    std::ranges::reverse(packets);

    Decoder decoder;
    const bool decoded = feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoded);
    const auto recovered = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, original_data);
}

TEST(Codec, Decoder_IncompleteChunkNotAccessible) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    (void) decoder.process_packet(packet_span(packets[0]));

    EXPECT_FALSE(decoder.is_chunk_complete(0));
    EXPECT_FALSE(decoder.get_chunk_data(0).has_value());
}

namespace {
    Encoder::FileId make_other_file_id() {
        Encoder::FileId file_id{};
        for (std::size_t i = 0; i < file_id.size(); ++i) {
            file_id[i] = std::byte{static_cast<uint8_t>(0xA0 + i)};
        }
        return file_id;
    }

    void write_u32_le(std::byte *dest, const uint32_t value) {
        std::memcpy(dest, &value, sizeof(value));
    }

    // Rewrites the stored checksum so a deliberately edited header still
    // passes the CRC gate. This models a crafted or foreign packet, which
    // a CRC alone cannot reject.
    void reseal_packet(Packet &packet) {
        write_u32_le(packet.bytes.data() + CRC_OFF_V2, 0);
        const std::span<const std::byte> header(packet.bytes.data(), HEADER_SIZE_V2);
        const std::span<const std::byte> payload(
            packet.bytes.data() + HEADER_SIZE_V2, SYMBOL_SIZE_BYTES);
        write_u32_le(
            packet.bytes.data() + CRC_OFF_V2,
            packet_checksum(header, payload, CRC_OFF_V2, HashAlgorithm::CRC32, CRC_SIZE));
    }

    std::vector<std::byte> concat(const std::vector<std::byte> &a, const std::vector<std::byte> &b) {
        std::vector<std::byte> out(a);
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }
} // namespace

TEST(Codec, Decoder_AssemblesFileFromAcceptedLastChunk) {
    const auto chunk0 = make_test_data(2048);
    const auto chunk1 = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    auto [packets0, m0] = encoder.encode_chunk(0, chunk0, false);
    auto [packets1, m1] = encoder.encode_chunk(1, chunk1, true);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets0);
    feed_all_packets_to_decoder(decoder, packets1);

    ASSERT_EQ(decoder.expected_chunk_count(), std::optional<uint32_t>(2u));
    EXPECT_EQ(decoder.rejected_packets(), 0u);
    const auto assembled = decoder.assemble_file(2);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, concat(chunk0, chunk1));
}

TEST(Codec, Decoder_RejectsPacketsFromAnotherFile) {
    const auto chunk0 = make_test_data(2048);
    const auto chunk1 = make_test_data(4096);
    const Encoder encoder_a(make_test_file_id());
    const Encoder encoder_b(make_other_file_id());
    auto [packets0, m0] = encoder_a.encode_chunk(0, chunk0, false);
    auto [packets1, m1] = encoder_b.encode_chunk(1, chunk1, true);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets0);
    feed_all_packets_to_decoder(decoder, packets1);

    EXPECT_TRUE(decoder.is_chunk_complete(0));
    EXPECT_FALSE(decoder.is_chunk_complete(1));
    EXPECT_EQ(decoder.rejected_packets(), packets1.size());
    // The foreign file's LastChunk marker must not define where this file ends.
    EXPECT_FALSE(decoder.expected_chunk_count().has_value());
    EXPECT_FALSE(decoder.assemble_file(2).has_value());
    ASSERT_TRUE(decoder.file_id().has_value());
    EXPECT_EQ(*decoder.file_id(), make_test_file_id());
}

TEST(Codec, Decoder_RejectsEncryptionModeChangeWithinFile) {
    const auto chunk0 = make_test_data(2048);
    const auto chunk1 = make_test_data(2048);
    const Encoder encoder(make_test_file_id());
    auto [plain_packets, m0] = encoder.encode_chunk(0, chunk0, false, false);
    auto [encrypted_packets, m1] = encoder.encode_chunk(1, chunk1, true, true);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, plain_packets);
    feed_all_packets_to_decoder(decoder, encrypted_packets);

    EXPECT_FALSE(decoder.is_encrypted());
    EXPECT_FALSE(decoder.is_chunk_complete(1));
    EXPECT_EQ(decoder.rejected_packets(), encrypted_packets.size());
}

TEST(Codec, Decoder_HugeChunkIndexDoesNotAllocateDenseStorage) {
    // Before sparse storage, a single CRC-valid packet with this index made
    // the decoder resize its chunk tables to ~4 billion entries.
    constexpr uint32_t max_index = std::numeric_limits<uint32_t>::max();
    const auto data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    auto [packets, manifest] = encoder.encode_chunk(max_index, data, true);

    Decoder decoder;
    EXPECT_TRUE(feed_all_packets_to_decoder(decoder, packets));
    EXPECT_TRUE(decoder.is_chunk_complete(max_index));
    EXPECT_EQ(decoder.completed_chunk_indices(), std::vector<uint32_t>{max_index});
    // index + 1 does not fit the packet format, so no end can be derived.
    EXPECT_FALSE(decoder.expected_chunk_count().has_value());
    EXPECT_FALSE(decoder.assemble_file(1).has_value());
}

TEST(Codec, Decoder_RejectsOutOfBoundsHeaderFields) {
    const auto data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, data);

    struct Mutation {
        const char *name;
        std::size_t offset;
        uint32_t value;
        bool is_u16;
    };
    const std::array<Mutation, 6> mutations{{
        {"chunk_size above CHUNK_SIZE_BYTES", CHUNK_SIZE_OFF, 0xFFFFFFFFu, false},
        {"chunk_size of a single symbol", CHUNK_SIZE_OFF, static_cast<uint32_t>(SYMBOL_SIZE_BYTES), false},
        {"k disagrees with chunk_size", K_OFF, 0x00FFFFFFu, false},
        {"original_size above chunk_size", ORIGINAL_SIZE_OFF, 4097u, false},
        {"symbol_size not the format constant", SYMBOL_SIZE_OFF, 128u, true},
        {"payload_len above symbol_size", PAYLOAD_LEN_OFF, 4096u, true},
    }};

    for (const Mutation &mutation: mutations) {
        SCOPED_TRACE(mutation.name);
        Packet packet = packets[0];
        if (mutation.is_u16) {
            const auto value = static_cast<uint16_t>(mutation.value);
            std::memcpy(packet.bytes.data() + mutation.offset, &value, sizeof(value));
        } else {
            write_u32_le(packet.bytes.data() + mutation.offset, mutation.value);
        }
        reseal_packet(packet);
        // symbol_size changes the checksummed payload span, so the CRC gate
        // is only asserted for mutations that keep it intact.
        const bool crc_span_intact = mutation.offset != SYMBOL_SIZE_OFF;
        if (crc_span_intact) {
            ASSERT_TRUE(Decoder::validate_raw_packet_crc(packet_span(packet)));
        }

        Decoder decoder;
        EXPECT_FALSE(decoder.process_packet(packet_span(packet)).has_value());
        if (crc_span_intact) {
            EXPECT_EQ(decoder.rejected_packets(), 1u);
        }
        // A refused packet must not lock the file identity.
        EXPECT_FALSE(decoder.file_id().has_value());
    }
}

TEST(Codec, Decoder_RejectsGeometryChangeWithinChunk) {
    const auto original = make_test_data(4096);
    const auto impostor = make_test_data(2048);
    const Encoder encoder(make_test_file_id());
    const auto [good_packets, m0] = encode_test_data(encoder, original);
    const auto [bad_packets, m1] = encode_test_data(encoder, impostor);

    Decoder decoder;
    (void) decoder.process_packet(packet_span(good_packets[0]));
    for (const Packet &packet: bad_packets) {
        EXPECT_FALSE(decoder.process_packet(packet_span(packet)).has_value());
    }
    EXPECT_EQ(decoder.rejected_packets(), bad_packets.size());

    for (std::size_t i = 1; i < good_packets.size(); ++i) {
        (void) decoder.process_packet(packet_span(good_packets[i]));
    }
    const auto recovered = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, original);
}

TEST(Codec, Decoder_ConflictingLastChunkMarkersBlockAssembly) {
    const auto chunk0 = make_test_data(2048);
    const auto chunk1 = make_test_data(2048);
    const Encoder encoder(make_test_file_id());
    auto [packets0, m0] = encoder.encode_chunk(0, chunk0, true);
    auto [packets1, m1] = encoder.encode_chunk(1, chunk1, true);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets0);
    feed_all_packets_to_decoder(decoder, packets1);

    EXPECT_TRUE(decoder.last_chunk_conflict());
    EXPECT_FALSE(decoder.expected_chunk_count().has_value());
    EXPECT_FALSE(decoder.assemble_file(1).has_value());
    EXPECT_FALSE(decoder.assemble_file(2).has_value());
}
