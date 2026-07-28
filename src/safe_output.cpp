#include "safe_output.h"

#include <atomic>
#include <random>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

SafeOutputFile::SafeOutputFile(const std::filesystem::path &target)
    : target_(target) {
    static std::atomic<uint64_t> sequence{0};
    std::filesystem::path parent = target_.parent_path();
    if (parent.empty()) parent = std::filesystem::current_path();
    const std::string extension = target_.extension().string();
    const std::string stem = target_.stem().string();
    std::random_device random;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        partial_ = parent /
            ("." + stem + ".vidstorex-part-" +
             std::to_string(random()) + "-" +
             std::to_string(sequence.fetch_add(1)) +
             extension);
        std::error_code error;
        if (!std::filesystem::exists(partial_, error) && !error) {
            return;
        }
    }
    throw std::runtime_error(
        "could not allocate a unique partial output path");
}

SafeOutputFile::~SafeOutputFile() {
    if (!committed_) {
        std::error_code ignored;
        std::filesystem::remove(partial_, ignored);
    }
}

void SafeOutputFile::commit() {
#if defined(_WIN32)
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
