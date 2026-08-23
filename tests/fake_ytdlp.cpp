#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    const char *sourceValue = std::getenv("VIDSTOREX_FAKE_YTDLP_SOURCE");
    if (!sourceValue || !*sourceValue) return 12;
    std::filesystem::path outputPattern;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--output")
            outputPattern = std::filesystem::u8path(argv[++i]);
    if (outputPattern.empty()) return 13;
    // A controlled pause makes active-download layout assertions observable.
    int delayMilliseconds = 0;
    if (const char *delay = std::getenv("VIDSTOREX_FAKE_YTDLP_DELAY_MS"))
        delayMilliseconds = (std::max)(0, std::atoi(delay));
    const auto output = outputPattern.parent_path();
    std::filesystem::create_directories(output);
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(
             std::filesystem::u8path(sourceValue)))
        if (entry.is_regular_file() && entry.path().extension() == ".mkv")
            ++count;
    std::size_t index = 0;
    for (const auto &entry : std::filesystem::directory_iterator(
             std::filesystem::u8path(sourceValue))) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mkv")
            continue;
        ++index;
        const auto destination = output /
            ("Returned shuffled clip " + std::to_string(count - index + 1) + ".mkv");
        std::cout << "Downloading item " << index << " of " << count << "\n"
                  << "[download] 12.5% of 1.00MiB at 1.00MiB/s ETA 00:01\n"
                  << std::flush;
        if (delayMilliseconds > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(delayMilliseconds));
        std::filesystem::copy_file(entry.path(), destination,
            std::filesystem::copy_options::overwrite_existing);
        std::cout << "[download] 100.0% of 1.00MiB at 10.00MiB/s ETA 00:00\n"
                  << "[download] Destination: " << destination.string() << "\n"
                  << std::flush;
    }
    return count == 0 ? 14 : 0;
}
