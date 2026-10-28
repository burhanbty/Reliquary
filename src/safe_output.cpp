#include "safe_output.h"

#include <atomic>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
bool fail_recovery_io(const std::string_view stage) {
#if defined(VIDSTOREX_ENABLE_TEST_HOOKS)
    const char *value = std::getenv("VIDSTOREX_TEST_RECOVERY_IO_FAILURE");
    return value && stage == value;
#else
    return false;
#endif
}
}

bool output_conflicts_with_input(const std::filesystem::path &input,
                                 const std::filesystem::path &output) {
    if (!std::filesystem::exists(output)) return false;
    return std::filesystem::equivalent(input, output);
}

SafeOutputFile::SafeOutputFile(const std::filesystem::path &target,
                               const Mode mode)
    : target_(target), exclusive_(mode == Mode::ExclusiveWrite) {
    static std::atomic<uint64_t> sequence{0};
    std::filesystem::path parent = target_.parent_path();
    if (parent.empty()) parent = std::filesystem::current_path();
    const std::string extension = target_.extension().string();
    // Keep recovery temporary names short even for a long target filename.
    const std::string prefix = exclusive_ ? ".vidstorex-part-" :
        "." + target_.stem().string() + ".vidstorex-part-";
    std::random_device random;
    if (exclusive_ && fail_recovery_io("create"))
        throw std::runtime_error("injected recovery temporary creation failure");
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        partial_ = parent /
            (prefix +
             std::to_string(random()) + "-" +
             std::to_string(sequence.fetch_add(1)) +
             extension);
        std::error_code error;
        if (exclusive_) {
            // C++23 noreplace performs exclusive creation, without a separate
            // exists/open race. A collision never truncates another file.
            stream_.open(partial_, std::ios::binary | std::ios::out |
                                   std::ios::noreplace);
            if (stream_) return;
            stream_.clear();
            if (std::filesystem::exists(partial_, error) && !error) continue;
            throw std::runtime_error("could not create recovery temporary file");
        }
        if (!std::filesystem::exists(partial_, error) && !error) {
            return;
        }
    }
    throw std::runtime_error(
        "could not allocate a unique partial output path");
}

SafeOutputFile::~SafeOutputFile() {
    // Windows cannot remove an open stream owned by this object.
    if (stream_.is_open()) stream_.close();
    if (!committed_) {
        std::error_code ignored;
        std::filesystem::remove(partial_, ignored);
    }
}

void SafeOutputFile::write(const std::span<const std::byte> bytes) {
    if (!exclusive_ || !stream_.is_open() || ready_)
        throw std::logic_error("recovery output is not open for writing");
    stream_.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (fail_recovery_io("write")) stream_.setstate(std::ios::badbit);
    if (!stream_) throw std::runtime_error("recovery output write failed");
}

void SafeOutputFile::finish_writing() {
    if (!exclusive_ || !stream_.is_open())
        throw std::logic_error("recovery output is not open");
    stream_.flush();
    if (fail_recovery_io("flush")) stream_.setstate(std::ios::badbit);
    if (!stream_) throw std::runtime_error("recovery output flush failed");
    stream_.close();
    if (fail_recovery_io("close")) stream_.setstate(std::ios::badbit);
    if (!stream_) throw std::runtime_error("recovery output close failed");
    ready_ = true;
}

void SafeOutputFile::commit() {
    if (exclusive_) {
        if (!ready_) throw std::logic_error("recovery output was not finalized");
        if (fail_recovery_io("publish"))
            throw std::runtime_error("injected recovery publication failure");
    }
#if defined(_WIN32)
    // Same-directory rename only: never delete the target first, and never
    // fall back to a cross-volume copy/delete operation.
    if (!MoveFileExW(
            partial_.c_str(), target_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::filesystem::filesystem_error(
            "could not replace output file",
            partial_, target_,
            std::error_code(
                static_cast<int>(GetLastError()),
                std::system_category()));
    }
#else
    std::filesystem::rename(partial_, target_);
#endif
    committed_ = true;
}
