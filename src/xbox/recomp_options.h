#pragma once
#include "cpu.h"
#include "settings.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xbox_native {
// The "Recomp Options" entry of the title and pause menus and its pages.
// They are made from the game's own HUD widgets, text, sounds and prompts,
// and follow the layout of its Options page, so the player's settings live
// inside the original UI.
class RecompOptions {
public:
    struct Host {
        PlayerSettings& settings;
        std::function<void()> apply; // Saves and applies a changed setting.
        std::function<int()> max_samples;
    };
    explicit RecompOptions(Host);
    ~RecompOptions();
    RecompOptions(const RecompOptions&) = delete;
    RecompOptions& operator=(const RecompOptions&) = delete;
    // Each level's text tables, as the engine reads them from the disc, gain
    // this feature's strings, drawn with the glyphs of the font beside them
    // (font_path). Other files are left alone.
    bool extends(std::string_view disc_path) const;
    static std::string font_path(std::string_view text_path);
    std::optional<std::vector<std::byte>> text_table(std::span<const std::byte> original, std::span<const std::byte> font,
                                                     std::string_view disc_path);
    // A host routine to run in place of an engine call into a title or pause
    // menu input callback, or null to run the original.
    NativeFunction intercept(const CompiledModule&, std::uint32_t base, std::uint32_t offset, NativeFunction original);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
