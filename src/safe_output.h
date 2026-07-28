#pragma once

#include <filesystem>

class SafeOutputFile {
public:
    explicit SafeOutputFile(const std::filesystem::path &target);
    ~SafeOutputFile();

    SafeOutputFile(const SafeOutputFile &) = delete;
    SafeOutputFile &operator=(const SafeOutputFile &) = delete;

    [[nodiscard]] const std::filesystem::path &partial_path() const {
        return partial_;
    }

    void commit();

private:
    std::filesystem::path target_;
    std::filesystem::path partial_;
    bool committed_ = false;
};
