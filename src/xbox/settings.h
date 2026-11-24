#pragma once
#include <filesystem>
#include <string>

namespace xbox_native {
// Rendering beyond the original Xbox output. The defaults reproduce it.
struct RenderEnhancements {
    int scale = 1, samples = 1, anisotropy = 1;
    bool fxaa{};
    bool operator==(const RenderEnhancements&) const = default;
};
struct PlayerSettings {
    std::filesystem::path xbe, disc;
    int refresh_rate = 30, volume = 100;
    bool show_fps{}, muted{}, mute_unfocused{}, widescreen{}, fullscreen{}, vsync = true;
    RenderEnhancements enhancements;

    void load(const std::filesystem::path&);
    void save(const std::filesystem::path&) const;
    void validate_media() const;
};
}
