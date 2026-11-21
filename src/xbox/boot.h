#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <vector>

namespace xbox_native {
struct StickInput {
    std::uint32_t first, frames;
    std::int16_t x, y;
};

struct InputScript {
    std::vector<std::uint32_t> start;
    std::array<std::vector<std::uint32_t>, 3> face;
    std::array<std::vector<std::uint32_t>, 4> dpad;
    std::vector<StickInput> stick;
};

struct DiagnosticOptions {
    std::filesystem::path root, session_report, frame, performance;
    std::uint32_t capture_at = 1, performance_at = 1, performance_frames = 0;
};

struct PlayOptions {
    std::filesystem::path xbe, disc, shader_cache, capture, storage;
    std::uint32_t capture_index = 1, frames = 0;
    bool hidden = false, fullscreen = false, audio = true, vsync = true;
    InputScript input;
    DiagnosticOptions diagnostics;
};

int boot_command(int argc, char* argv[]);
int play_game(const PlayOptions&, std::ostream& report);
}
