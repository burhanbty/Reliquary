#pragma once

#include <filesystem>
#include <cstddef>
#include <fstream>
#include <span>

// Follows filesystem identity (including hardlinks), and throws on metadata
// errors rather than treating an inaccessible target as a different file.
[[nodiscard]] bool output_conflicts_with_input(
    const std::filesystem::path &input, const std::filesystem::path &output);

class SafeOutputFile {
public:
    enum class Mode { PathOnly, ExclusiveWrite };
    explicit SafeOutputFile(const std::filesystem::path &target,
                            Mode mode = Mode::PathOnly);
    ~SafeOutputFile();

    SafeOutputFile(const SafeOutputFile &) = delete;
    SafeOutputFile &operator=(const SafeOutputFile &) = delete;

    [[nodiscard]] const std::filesystem::path &partial_path() const {
        return partial_;
    }

    void commit();

    // ExclusiveWrite owns the stream from exclusive creation through close;
    // callers must not reopen partial_path() in this mode.
    void write(std::span<const std::byte> bytes);
    void finish_writing();

private:
    std::filesystem::path target_;
    std::filesystem::path partial_;
    bool committed_ = false;
    bool exclusive_ = false;
    bool ready_ = false;
    std::ofstream stream_;
};
