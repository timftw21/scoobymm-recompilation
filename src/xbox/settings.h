#pragma once
#include <filesystem>
#include <string>

namespace xbox_native {
struct PlayerSettings {
    std::filesystem::path xbe, disc;
    int refresh_rate = 30, volume = 100;
    bool show_fps{}, muted{}, mute_unfocused{}, widescreen{}, fullscreen{}, vsync = true;

    void load(const std::filesystem::path&);
    void save(const std::filesystem::path&) const;
    void validate_media() const;
};
}
