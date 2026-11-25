#include "settings.h"
#include "disc.h"
#include "xbe.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <Windows.h>

namespace xbox_native {
namespace {
// settings.cfg names controller inputs with SDL's mapping strings: "a",
// "leftshoulder", or an axis direction such as "-lefty".
std::string pad_text(PadInput input) {
    if (input.kind == PadInput::button) {
        const auto* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(input.index));
        return name ? name : "";
    }
    if (input.kind == PadInput::none) return {};
    const auto* name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(input.index));
    return name ? (input.kind == PadInput::positive ? "+" : "-") + std::string(name) : "";
}
std::optional<PadInput> parse_pad(const std::string& text) {
    if (text.empty()) return PadInput{};
    if (text[0] == '+' || text[0] == '-') {
        const auto axis = SDL_GetGamepadAxisFromString(text.c_str() + 1);
        if (axis == SDL_GAMEPAD_AXIS_INVALID) return {};
        return PadInput{text[0] == '+' ? PadInput::positive : PadInput::negative, static_cast<std::uint8_t>(axis)};
    }
    const auto button = SDL_GetGamepadButtonFromString(text.c_str());
    if (button == SDL_GAMEPAD_BUTTON_INVALID) return {};
    return PadInput{PadInput::button, static_cast<std::uint8_t>(button)};
}
}

InputBindings::InputBindings() {
    const auto button = [](SDL_GamepadButton b) { return PadInput{PadInput::button, static_cast<std::uint8_t>(b)}; };
    const auto axis = [](SDL_GamepadAxis a, bool positive) {
        return PadInput{positive ? PadInput::positive : PadInput::negative, static_cast<std::uint8_t>(a)};
    };
    keys = {SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_F,
            SDL_SCANCODE_Z, SDL_SCANCODE_C,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
            SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL,
            SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, 0, 0, 0, 0};
    // Black and White sit on the right and left bumpers.
    pad = {button(SDL_GAMEPAD_BUTTON_SOUTH), button(SDL_GAMEPAD_BUTTON_EAST), button(SDL_GAMEPAD_BUTTON_WEST),
           button(SDL_GAMEPAD_BUTTON_NORTH), button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
           button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, true),
           axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, true),
           button(SDL_GAMEPAD_BUTTON_DPAD_UP), button(SDL_GAMEPAD_BUTTON_DPAD_DOWN),
           button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
           button(SDL_GAMEPAD_BUTTON_START), button(SDL_GAMEPAD_BUTTON_BACK),
           button(SDL_GAMEPAD_BUTTON_LEFT_STICK), button(SDL_GAMEPAD_BUTTON_RIGHT_STICK),
           axis(SDL_GAMEPAD_AXIS_LEFTY, false), axis(SDL_GAMEPAD_AXIS_LEFTY, true),
           axis(SDL_GAMEPAD_AXIS_LEFTX, false), axis(SDL_GAMEPAD_AXIS_LEFTX, true),
           axis(SDL_GAMEPAD_AXIS_RIGHTY, false), axis(SDL_GAMEPAD_AXIS_RIGHTY, true),
           axis(SDL_GAMEPAD_AXIS_RIGHTX, false), axis(SDL_GAMEPAD_AXIS_RIGHTX, true)};
}

void PlayerSettings::load(const std::filesystem::path& file) {
    if (!std::filesystem::exists(file)) return;
    std::ifstream in(file, std::ios::binary);
    if (!in || std::filesystem::file_size(file) > 256 * 1024)
        throw std::runtime_error("Cannot read settings.cfg");
    auto next = *this;
    std::string line;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) line.pop_back();
        const auto split = line.find('=');
        if (split == line.npos) continue;
        const auto key = line.substr(0, split), value = line.substr(split + 1);
        const auto invalid = [&] { return std::runtime_error("Invalid settings.cfg value for " + key); };
        const auto number = [&](int low, int high) {
            int n{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (error != std::errc{} || end != value.data() + value.size() || n < low || n > high) throw invalid();
            return n;
        };
        const auto control = [&](std::string_view prefix) -> std::ptrdiff_t {
            if (!key.starts_with(prefix)) return -1;
            const auto found = std::ranges::find(control_names, std::string_view(key).substr(prefix.size()), &ControlName::key);
            return found == control_names.end() ? -1 : found - control_names.begin();
        };
        if (key == "xbe" || key == "disc") {
            auto path = std::filesystem::path(std::u8string(value.begin(), value.end()));
            (key == "xbe" ? next.xbe : next.disc) = std::move(path);
        } else if (key == "refresh_rate") next.refresh_rate = number(0, 1000);
        else if (key == "volume") next.volume = number(0, 100);
        else if (key == "show_fps") next.show_fps = number(0, 1) != 0;
        else if (key == "muted") next.muted = number(0, 1) != 0;
        else if (key == "mute_unfocused") next.mute_unfocused = number(0, 1) != 0;
        else if (key == "widescreen") next.widescreen = number(0, 1) != 0;
        else if (key == "fullscreen") next.fullscreen = number(0, 1) != 0;
        else if (key == "vsync") next.vsync = number(0, 1) != 0;
        else if (key == "skip_intro") next.skip_intro = number(0, 1) != 0;
        else if (key == "render_scale") next.enhancements.scale = number(1, 6);
        else if (key == "fxaa") next.enhancements.fxaa = number(0, 1) != 0;
        else if (key == "upscaled_hud") next.enhancements.upscaled_hud = number(0, 1) != 0;
        else if (key == "msaa" || key == "anisotropy") {
            const auto n = number(1, key == "msaa" ? 8 : 16);
            if (n & (n - 1)) throw invalid();
            (key == "msaa" ? next.enhancements.samples : next.enhancements.anisotropy) = n;
        } else if (const auto i = control("keyboard_"); i >= 0) next.bindings.keys[i] = number(0, SDL_SCANCODE_COUNT - 1);
        else if (const auto j = control("controller_"); j >= 0) {
            const auto input = parse_pad(value);
            if (!input) throw invalid();
            next.bindings.pad[j] = *input;
        }
    }
    if (in.bad()) throw std::runtime_error("Error reading settings.cfg");
    *this = std::move(next);
}

void PlayerSettings::save(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    auto pending = file;
    pending += std::format(".{}.tmp", GetCurrentProcessId());
    try {
        std::ofstream out(pending, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "xbe=" << path_text(xbe) << "\ndisc=" << path_text(disc)
            << "\nrefresh_rate=" << refresh_rate << "\nvolume=" << volume
            << "\nshow_fps=" << show_fps << "\nmuted=" << muted << "\nmute_unfocused=" << mute_unfocused
            << "\nwidescreen=" << widescreen << "\nfullscreen=" << fullscreen << "\nvsync=" << vsync
            << "\nrender_scale=" << enhancements.scale << "\nmsaa=" << enhancements.samples
            << "\nfxaa=" << enhancements.fxaa << "\nanisotropy=" << enhancements.anisotropy
            << "\nupscaled_hud=" << enhancements.upscaled_hud
            << "\nskip_intro=" << skip_intro << '\n';
        for (std::size_t i = 0; i < control_count; ++i) out << "keyboard_" << control_names[i].key << '=' << bindings.keys[i] << '\n';
        for (std::size_t i = 0; i < control_count; ++i)
            out << "controller_" << control_names[i].key << '=' << pad_text(bindings.pad[i]) << '\n';
        out.close();
        if (!MoveFileExW(pending.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Saving settings.cfg");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        throw;
    }
}

void PlayerSettings::validate_media() const {
    Xbe executable(xbe);
    if (Disc(disc).executable_sha256() != executable.sha256)
        throw std::runtime_error("The disc and the verified default.xbe do not match.");
}
}
