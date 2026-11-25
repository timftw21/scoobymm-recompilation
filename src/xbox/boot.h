#pragma once
#include "settings.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string_view>
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

struct PlayOptions : PlayerSettings {
    std::filesystem::path capture, storage;
    std::string settings_error;
    std::uint32_t capture_index = 1, frames = 0;
    std::uint32_t language = 1;
    std::uint32_t refresh_at = 0;
    int scripted_rate = 0;
    bool hidden = false, launcher = false;
    bool restart = false; // Set when the player chose Restart game.
    InputScript input;
    DiagnosticOptions diagnostics;
};

int boot_command(int argc, char* argv[]);
int check_dialogue_stream(const std::filesystem::path& xbe, const std::filesystem::path& disc);
std::uint32_t xbox_language(std::string_view code);
int play_game(PlayOptions&, std::ostream& report);
}
