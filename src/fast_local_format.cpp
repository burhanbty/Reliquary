#include "fast_local_format.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
template <typename T>
void put_le(std::span<std::byte> out, const std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[offset + i] =
            static_cast<std::byte>((value >> (i * 8)) & 0xffu);
    }
}

template <typename T>
T get_le(const std::span<const std::byte> in, const std::size_t offset) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(
            std::to_integer<uint8_t>(in[offset + i])) << (i * 8);
    }
    return value;
}

void require_size(const std::span<const std::byte> bytes,
                  const std::size_t size, const char *what) {
    if (bytes.size() < size) {
        throw std::runtime_error(std::string("truncated ") + what);
    }
}
}

std::array<std::byte, FAST_LOCAL_FILE_HEADER_SIZE>
serialize_fast_local_file_header(const FastLocalFileHeader &header) {
    std::array<std::byte, FAST_LOCAL_FILE_HEADER_SIZE> out{};
    std::copy(FAST_LOCAL_MAGIC.begin(), FAST_LOCAL_MAGIC.end(), out.begin());
    put_le<uint16_t>(out, 8, header.version);
    put_le<uint16_t>(out, 10, FAST_LOCAL_FILE_HEADER_SIZE);
    out[12] = std::byte{1}; // Fast Local encoding mode
    out[13] = static_cast<std::byte>(header.flags);
    put_le<uint32_t>(out, 16, header.width);
    put_le<uint32_t>(out, 20, header.height);
    put_le<uint32_t>(out, 24, header.fps);
    put_le<uint64_t>(out, 32, header.original_size);
    put_le<uint64_t>(out, 40, header.total_frames);
    put_le<uint64_t>(out, 48, header.frame_payload_capacity);
    put_le<uint64_t>(out, 56, header.plain_frame_capacity);
    std::copy(header.file_id.begin(), header.file_id.end(), out.begin() + 64);
    std::copy(header.original_sha256.bytes.begin(),
              header.original_sha256.bytes.end(), out.begin() + 80);
    put_le<uint32_t>(out, 124, crc32c(
        std::span<const std::byte>(out.data(), 124)));
    return out;
}

FastLocalFileHeader deserialize_fast_local_file_header(
    const std::span<const std::byte> bytes) {
    require_size(bytes, FAST_LOCAL_FILE_HEADER_SIZE, "Fast Local file header");
    if (!std::equal(FAST_LOCAL_MAGIC.begin(), FAST_LOCAL_MAGIC.end(),
                    bytes.begin())) {
        throw std::runtime_error("invalid Fast Local magic");
    }
    const uint16_t version = get_le<uint16_t>(bytes, 8);
    if (version != FAST_LOCAL_FORMAT_VERSION) {
        throw std::runtime_error("unsupported Fast Local format version");
    }
    if (get_le<uint16_t>(bytes, 10) != FAST_LOCAL_FILE_HEADER_SIZE) {
        throw std::runtime_error("invalid Fast Local header size");
    }
    if (std::to_integer<uint8_t>(bytes[12]) != 1) {
        throw std::runtime_error("invalid Fast Local encoding mode");
    }
    if (get_le<uint32_t>(bytes, 124) !=
        crc32c(bytes.first(124))) {
        throw std::runtime_error("Fast Local header checksum mismatch");
    }

    FastLocalFileHeader header;
    header.version = version;
    header.flags = std::to_integer<uint8_t>(bytes[13]);
    header.width = get_le<uint32_t>(bytes, 16);
    header.height = get_le<uint32_t>(bytes, 20);
    header.fps = get_le<uint32_t>(bytes, 24);
    header.original_size = get_le<uint64_t>(bytes, 32);
    header.total_frames = get_le<uint64_t>(bytes, 40);
    header.frame_payload_capacity = get_le<uint64_t>(bytes, 48);
    header.plain_frame_capacity = get_le<uint64_t>(bytes, 56);
    std::copy_n(bytes.begin() + 64, header.file_id.size(),
                header.file_id.begin());
    std::copy_n(bytes.begin() + 80, header.original_sha256.bytes.size(),
                header.original_sha256.bytes.begin());
    if (header.width != FRAME_WIDTH || header.height != FRAME_HEIGHT ||
        header.fps != FRAME_FPS ||
        header.frame_payload_capacity != FAST_LOCAL_FRAME_PAYLOAD_CAPACITY ||
        header.plain_frame_capacity !=
            fast_local_plain_capacity(
                (header.flags & FastLocalEncrypted) != 0) ||
        header.total_frames == 0 ||
        header.total_frames > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("invalid Fast Local header fields");
    }
    return header;
}

std::array<std::byte, FAST_LOCAL_FRAME_HEADER_SIZE>
serialize_fast_local_frame_header(const FastLocalFrameHeader &header) {
    std::array<std::byte, FAST_LOCAL_FRAME_HEADER_SIZE> out{};
    put_le<uint32_t>(out, 0, FAST_LOCAL_FRAME_MAGIC);
    put_le<uint16_t>(out, 4, header.version);
    put_le<uint16_t>(out, 6, header.flags);
    put_le<uint32_t>(out, 8, header.frame_index);
    put_le<uint32_t>(out, 12, header.total_frames);
    put_le<uint32_t>(out, 16, header.payload_length);
    put_le<uint32_t>(out, 20, header.plain_length);
    put_le<uint32_t>(out, 24, header.payload_checksum);
    put_le<uint32_t>(out, 28, crc32c(
        std::span<const std::byte>(out.data(), 28)));
    return out;
}

FastLocalFrameHeader deserialize_fast_local_frame_header(
    const std::span<const std::byte> bytes) {
    require_size(bytes, FAST_LOCAL_FRAME_HEADER_SIZE, "Fast Local frame header");
    if (get_le<uint32_t>(bytes, 0) != FAST_LOCAL_FRAME_MAGIC) {
        throw std::runtime_error("invalid Fast Local frame magic");
    }
    const uint16_t version = get_le<uint16_t>(bytes, 4);
    if (version != FAST_LOCAL_FORMAT_VERSION) {
        throw std::runtime_error("unsupported Fast Local frame version");
    }
    if (get_le<uint32_t>(bytes, 28) != crc32c(bytes.first(28))) {
        throw std::runtime_error("Fast Local frame header checksum mismatch");
    }
    FastLocalFrameHeader header;
    header.version = version;
    header.flags = get_le<uint16_t>(bytes, 6);
    header.frame_index = get_le<uint32_t>(bytes, 8);
    header.total_frames = get_le<uint32_t>(bytes, 12);
    header.payload_length = get_le<uint32_t>(bytes, 16);
    header.plain_length = get_le<uint32_t>(bytes, 20);
    header.payload_checksum = get_le<uint32_t>(bytes, 24);
    if (header.payload_length > FAST_LOCAL_FRAME_PAYLOAD_CAPACITY ||
        header.plain_length > FAST_LOCAL_FRAME_PAYLOAD_CAPACITY) {
        throw std::runtime_error("invalid Fast Local frame lengths");
    }
    return header;
}

uint64_t fast_local_plain_capacity(const bool encrypted) {
    return FAST_LOCAL_FRAME_PAYLOAD_CAPACITY -
        (encrypted ? FAST_LOCAL_CRYPTO_OVERHEAD : 0);
}

uint64_t fast_local_frame_count(const uint64_t input_size,
                                const bool encrypted) {
    const uint64_t capacity = fast_local_plain_capacity(encrypted);
    if (input_size == 0) return 1;
    const uint64_t frames = input_size / capacity +
        (input_size % capacity != 0 ? 1 : 0);
    if (frames > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("Fast Local frame count exceeds format limit");
    }
    return frames;
}

uint64_t fast_local_padding_bytes(const uint64_t input_size,
                                  const bool encrypted) {
    const uint64_t frames = fast_local_frame_count(input_size, encrypted);
    const uint64_t capacity = fast_local_plain_capacity(encrypted);
    if (frames > std::numeric_limits<uint64_t>::max() / capacity) {
        throw std::overflow_error("Fast Local padding calculation overflow");
    }
    return frames * capacity - input_size;
}
