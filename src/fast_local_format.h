#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "configuration.h"
#include "crypto.h"
#include "integrity.h"

constexpr std::size_t FAST_LOCAL_FILE_HEADER_SIZE = 128;
constexpr std::size_t FAST_LOCAL_FRAME_HEADER_SIZE = 32;
constexpr std::size_t FAST_LOCAL_RESERVED_PREFIX =
    FAST_LOCAL_FILE_HEADER_SIZE + FAST_LOCAL_FRAME_HEADER_SIZE;
constexpr std::size_t FAST_LOCAL_FRAME_BYTES =
    static_cast<std::size_t>(FRAME_WIDTH) * FRAME_HEIGHT;
constexpr std::size_t FAST_LOCAL_FRAME_PAYLOAD_CAPACITY =
    FAST_LOCAL_FRAME_BYTES - FAST_LOCAL_RESERVED_PREFIX;
constexpr std::size_t FAST_LOCAL_CRYPTO_OVERHEAD =
    CRYPTO_PLAIN_SIZE_HEADER + CRYPTO_AEAD_TAG_BYTES;
constexpr uint16_t FAST_LOCAL_FORMAT_VERSION = 1;
constexpr uint32_t FAST_LOCAL_FRAME_MAGIC = 0x31464c56u; // "VLF1" LE
constexpr std::array<std::byte, 8> FAST_LOCAL_MAGIC{
    std::byte{'V'}, std::byte{'S'}, std::byte{'X'}, std::byte{'F'},
    std::byte{'A'}, std::byte{'S'}, std::byte{'T'}, std::byte{'1'}};

enum FastLocalFlags : uint8_t {
    FastLocalEncrypted = 1u << 0,
};

struct FastLocalFileHeader {
    uint16_t version = FAST_LOCAL_FORMAT_VERSION;
    uint8_t flags = 0;
    uint32_t width = FRAME_WIDTH;
    uint32_t height = FRAME_HEIGHT;
    uint32_t fps = FRAME_FPS;
    uint64_t original_size = 0;
    uint64_t total_frames = 0;
    uint64_t frame_payload_capacity = FAST_LOCAL_FRAME_PAYLOAD_CAPACITY;
    uint64_t plain_frame_capacity = FAST_LOCAL_FRAME_PAYLOAD_CAPACITY;
    std::array<std::byte, 16> file_id{};
    Sha256Digest original_sha256{};
};

struct FastLocalFrameHeader {
    uint16_t version = FAST_LOCAL_FORMAT_VERSION;
    uint16_t flags = 0;
    uint32_t frame_index = 0;
    uint32_t total_frames = 0;
    uint32_t payload_length = 0;
    uint32_t plain_length = 0;
    uint32_t payload_checksum = 0;
};

[[nodiscard]] std::array<std::byte, FAST_LOCAL_FILE_HEADER_SIZE>
serialize_fast_local_file_header(const FastLocalFileHeader &header);

[[nodiscard]] FastLocalFileHeader deserialize_fast_local_file_header(
    std::span<const std::byte> bytes);

[[nodiscard]] std::array<std::byte, FAST_LOCAL_FRAME_HEADER_SIZE>
serialize_fast_local_frame_header(const FastLocalFrameHeader &header);

[[nodiscard]] FastLocalFrameHeader deserialize_fast_local_frame_header(
    std::span<const std::byte> bytes);

[[nodiscard]] uint64_t fast_local_plain_capacity(bool encrypted);
[[nodiscard]] uint64_t fast_local_frame_count(
    uint64_t input_size, bool encrypted);
[[nodiscard]] uint64_t fast_local_padding_bytes(
    uint64_t input_size, bool encrypted);
