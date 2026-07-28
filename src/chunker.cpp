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

#include "chunker.h"
#include "configuration.h"
#include "encoding_reliability.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

ChunkedStorageData chunkByteData(std::span<const std::byte> data) {
    ChunkedStorageData result;
    result.storage.assign(data.begin(), data.end());

    if (const std::size_t size = result.storage.size(); size == 0) {
        result.chunks.push_back(ChunkSlice{0, 0});
    } else {
        result.chunks.reserve((size + CHUNK_SIZE_BYTES - 1) / CHUNK_SIZE_BYTES);
        for (std::size_t off = 0; off < size; off += CHUNK_SIZE_BYTES) {
            const std::size_t len = (std::min<std::size_t>)(CHUNK_SIZE_BYTES, size - off);
            result.chunks.push_back(ChunkSlice{off, len});
        }
    }

    return result;
}

ChunkedStorageData chunkFile(const char *path, const std::size_t chunk_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("open failed");
    }

    const std::size_t size = file.tellg();
    file.seekg(0);

    ChunkedStorageData result;
    result.storage.resize(size);
    if (!file.read(reinterpret_cast<char *>(result.storage.data()), static_cast<long long>(size))) {
        throw std::runtime_error("read failed");
    }

    const std::size_t cs = (chunk_size > 0) ? chunk_size : CHUNK_SIZE_BYTES;

    if (size == 0) {
        result.chunks.push_back(ChunkSlice{0, 0});
    } else {
        result.chunks.reserve((size + cs - 1) / cs);
        for (std::size_t off = 0; off < size; off += cs) {
            const std::size_t len = (std::min<std::size_t>)(cs, size - off);
            result.chunks.push_back(ChunkSlice{off, len});
        }
    }

    return result;
}

FileChunkReader::FileChunkReader(const char *path, const std::size_t chunk_size)
    : path_(path)
      , chunk_size_(chunk_size > 0 ? chunk_size : CHUNK_SIZE_BYTES)
      , file_(path, std::ios::binary) {
    if (!file_) {
        throw std::runtime_error("open failed");
    }
    file_.seekg(0, std::ios::end);
    file_size_ = file_.tellg();
    file_.seekg(0);
    if (chunk_size_ == CHUNK_SIZE_BYTES) {
        num_chunks_ = static_cast<std::size_t>(
            calculate_chunk_count(file_size_, false));
    } else if (chunk_size_ == CHUNK_SIZE_PLAIN_MAX_ENCRYPTED) {
        num_chunks_ = static_cast<std::size_t>(
            calculate_chunk_count(file_size_, true));
    } else {
        num_chunks_ = file_size_ == 0
                          ? 1
                          : file_size_ / chunk_size_ +
                                (file_size_ % chunk_size_ != 0 ? 1 : 0);
    }

    if (file_size_ == 0) {
        return;
    }

#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    HANDLE mapping = CreateFileMappingA(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(h);
        return;
    }
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(h);
        return;
    }
    file_handle_ = h;
    mapping_handle_ = mapping;
    mapped_ = static_cast<const std::byte *>(view);
#else
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return;
    const void *view = ::mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        return;
    }
    file_handle_ = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
    mapped_ = static_cast<const std::byte *>(view);
#endif
}

FileChunkReader::~FileChunkReader() {
#if defined(_WIN32)
    if (mapped_) UnmapViewOfFile(const_cast<std::byte *>(mapped_));
    if (mapping_handle_) CloseHandle(mapping_handle_);
    if (file_handle_) CloseHandle(file_handle_);
#else
    if (mapped_ && file_size_ > 0) ::munmap(const_cast<std::byte *>(mapped_), file_size_);
    if (file_handle_) ::close(static_cast<int>(reinterpret_cast<intptr_t>(file_handle_)));
#endif
}

std::span<const std::byte> FileChunkReader::chunk_view(const std::size_t index) const {
    if (index >= num_chunks_) {
        throw std::runtime_error("chunk index out of range");
    }
    const std::size_t offset = index * chunk_size_;
    if (offset >= file_size_) {
        return {};
    }
    if (!mapped_) {
        throw std::runtime_error("chunk_view called without active mapping");
    }
    const std::size_t len = (std::min)(chunk_size_, file_size_ - offset);
    return {mapped_ + offset, len};
}

std::vector<std::byte> FileChunkReader::read_chunk(const std::size_t index) const {
    if (index >= num_chunks_) {
        throw std::runtime_error("chunk index out of range");
    }

    const std::size_t offset = index * chunk_size_;
    if (offset >= file_size_) {
        return {};
    }
    const std::size_t len = (std::min)(chunk_size_, file_size_ - offset);

    if (mapped_) {
        return {mapped_ + offset, mapped_ + offset + len};
    }

    if (offset != file_pos_) {
        file_.seekg(static_cast<std::streamoff>(offset));
    }

    std::vector<std::byte> data(len);
    if (!file_.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(len))) {
        throw std::runtime_error("read failed");
    }

    file_pos_ = offset + len;
    return data;
}
