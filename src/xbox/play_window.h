#pragma once

#include <array>
#include <cstdint>
#include <string_view>

struct SDL_Window;
struct SDL_Gamepad;

namespace xbox_native {
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
    PlayWindow(bool hidden, bool fullscreen);
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
    bool hidden_;
    GamepadState state_;
    DiagnosticKeys diagnostic_keys_;
    std::uint64_t origin_{}, updated_{}, next_rumble_{};
    std::uint16_t low_{}, high_{};
    void refresh_gamepad();
};
}
