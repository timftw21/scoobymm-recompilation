#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace xbox_native {
// Rendering beyond the original Xbox output. The defaults reproduce it.
struct RenderEnhancements {
    int scale = 1, samples = 1, anisotropy = 1;
    bool fxaa{};
    bool upscaled_hud{}; // Real-ESRGAN HUD and menu artwork at 2x and above.
    bool operator==(const RenderEnhancements&) const = default;
};

// The Xbox controller inputs a player can bind, in GamepadState order: the
// eight analog buttons, the eight digital buttons, then each stick direction.
enum class Control : int {
    a, b, x, y, black, white, left_trigger, right_trigger,
    dpad_up, dpad_down, dpad_left, dpad_right, start, back, left_thumb, right_thumb,
    left_up, left_down, left_left, left_right, right_up, right_down, right_left, right_right, count
};
inline constexpr std::size_t control_count = std::size_t(Control::count);
struct ControlName { std::string_view key, label; };
inline constexpr std::array<ControlName, control_count> control_names{{
    {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"black", "Black"}, {"white", "White"},
    {"left_trigger", "Left trigger"}, {"right_trigger", "Right trigger"},
    {"dpad_up", "D-pad up"}, {"dpad_down", "D-pad down"}, {"dpad_left", "D-pad left"}, {"dpad_right", "D-pad right"},
    {"start", "Start"}, {"back", "Back"}, {"left_thumb", "Left stick click"}, {"right_thumb", "Right stick click"},
    {"left_stick_up", "Left stick up"}, {"left_stick_down", "Left stick down"},
    {"left_stick_left", "Left stick left"}, {"left_stick_right", "Left stick right"},
    {"right_stick_up", "Right stick up"}, {"right_stick_down", "Right stick down"},
    {"right_stick_left", "Right stick left"}, {"right_stick_right", "Right stick right"},
}};
// A host controller button (SDL_GamepadButton), or one direction of an axis
// (SDL_GamepadAxis).
struct PadInput {
    enum Kind : std::uint8_t { none, button, positive, negative } kind = none;
    std::uint8_t index{};
    bool operator==(const PadInput&) const = default;
};
struct InputBindings {
    std::array<int, control_count> keys{}; // SDL_Scancode, 0 when unbound
    std::array<PadInput, control_count> pad{};
    InputBindings(); // The default layout.
    bool operator==(const InputBindings&) const = default;
};

// The display shape. Wider shapes extend the 4:3 scene at its sides and keep
// HUD widgets at their authored proportions and edge margins. 21:9 is the
// 64:27 shape of 2560 × 1080 and 5120 × 2160 displays.
enum class AspectRatio : int { standard, wide, ultrawide };
struct AspectShape { std::string_view name; int width, height; };
inline constexpr std::array<AspectShape, 3> aspect_shapes{{{"4:3", 4, 3}, {"16:9", 16, 9}, {"21:9", 64, 27}}};
inline constexpr const AspectShape& aspect_shape(AspectRatio aspect) { return aspect_shapes[std::size_t(aspect)]; }
// Width over height.
inline constexpr float aspect_value(AspectRatio aspect) {
    return float(aspect_shape(aspect).width) / float(aspect_shape(aspect).height);
}
// How much wider than 4:3 the shape is (1, 4/3 or 16/9), and its inverse.
inline constexpr float aspect_widening(AspectRatio aspect) {
    return float(3 * aspect_shape(aspect).width) / float(4 * aspect_shape(aspect).height);
}
inline constexpr float aspect_narrowing(AspectRatio aspect) {
    return float(4 * aspect_shape(aspect).height) / float(3 * aspect_shape(aspect).width);
}

struct PlayerSettings {
    std::filesystem::path xbe, disc;
    int refresh_rate = 30, volume = 100;
    AspectRatio aspect{};
    bool show_fps{}, muted{}, mute_unfocused{}, fullscreen{}, vsync = true;
    bool skip_intro{}; // Skips the boot movies before the title screen.
    RenderEnhancements enhancements;
    InputBindings bindings;

    bool widescreen() const { return aspect != AspectRatio::standard; }
    void load(const std::filesystem::path&);
    void save(const std::filesystem::path&) const;
    void validate_media() const;
};
}
