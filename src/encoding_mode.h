#pragma once

#include <string_view>

enum class EncodingMode {
    Resilient = 0,
    FastLocal = 1,
    HighCapacity = 2,
};

[[nodiscard]] constexpr std::string_view encoding_mode_name(
    const EncodingMode mode) {
    switch (mode) {
        case EncodingMode::FastLocal: return "fast-local";
        case EncodingMode::HighCapacity: return "high-capacity";
        default: return "resilient";
    }
}
