#pragma once

namespace video_set_cli {

[[nodiscard]] bool is_command(const char *command);
[[nodiscard]] int run(int argc, char *argv[]);

} // namespace video_set_cli
