#pragma once

#include "boot.h"
#include <array>
#include <memory>
#include <cstdint>
#include <string_view>

struct SDL_Window;
struct SDL_Gamepad;

namespace xbox_native {
class Renderer;
class SettingsMenu;
struct DiagnosticKeys {
    bool capture{}, toggle_performance{};
};

struct GamepadState {
    std::uint16_t buttons{};
    std::array<std::uint8_t, 8> pressure{};
    std::array<std::int16_t, 4> axes{};
    bool operator==(const GamepadState&) const = default;
};

class PlayWindow {
public:
    explicit PlayWindow(PlayOptions& options);
    bool launcher();
    void overlay(bool measuring);
    bool frame_due(bool movie, std::uint32_t scanout);
    void pace_game(bool movie);
    float gain() const;
    Renderer& renderer() const { return *renderer_; }
    const PlayerSettings& settings() const { return options_; }
    ~PlayWindow();
    PlayWindow(const PlayWindow&) = delete;
    PlayWindow& operator=(const PlayWindow&) = delete;
    void* native_handle() const;
    std::uint64_t time() const;
    void begin();
    bool poll();
    DiagnosticKeys take_diagnostic_keys();
    void diagnostic_title(std::string_view status);
    const GamepadState& input() const { return state_; }
    std::uint32_t rumble(std::uint16_t low, std::uint16_t high);
private:
    SDL_Window* window_{};
    SDL_Gamepad* gamepad_{};
    PlayOptions& options_;
    bool hidden_, playing_{}, focused_{};
    int pacing_rate_ = -1;
    std::uint64_t deadline_{};
    unsigned skip_{};
    void pace(int hz);
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<SettingsMenu> menu_;
    GamepadState state_, menu_input_;
    bool menu_was_open_{};
    DiagnosticKeys diagnostic_keys_;
    std::uint64_t origin_{}, updated_{}, next_rumble_{};
    std::uint16_t low_{}, high_{};
    void refresh_gamepad();
};
}
