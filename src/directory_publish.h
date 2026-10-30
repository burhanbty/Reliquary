#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

// Windows may briefly deny a directory rename while a reader/scanner closes a
// child handle. Retry only those errors, keeping the same atomic rename and
// requiring the caller's safety/overwrite checks again before every attempt.
inline void publish_directory_with_retry(
    const std::filesystem::path &staged,
    const std::filesystem::path &target,
    const std::function<void()> &validate) {
    for (unsigned attempt = 0; ; ++attempt) {
        validate();
        std::error_code error;
        std::filesystem::rename(staged, target, error);
        if (!error) return;
#if defined(_WIN32)
        // ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION. There is no delete,
        // copy fallback or permission change; persistent denial still fails.
        if (attempt < 10 && (error.value() == 5 || error.value() == 32)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
#endif
        throw std::filesystem::filesystem_error(
            "could not publish directory", staged, target, error);
    }
}
