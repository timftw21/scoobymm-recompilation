#include "play_window.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace xbox_native {
namespace {
constexpr auto subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
constexpr auto game_title = "Scooby-Doo! Mystery Mayhem";
constexpr std::uint64_t poll_interval = 2'000'000, rumble_interval = 500'000'000;
}

PlayWindow::PlayWindow(bool hidden, bool fullscreen) : hidden_(hidden) {
    if (!SDL_InitSubSystem(subsystems)) throw std::runtime_error(SDL_GetError());
    const auto flags = SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0) |
        (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    window_ = SDL_CreateWindow(game_title, 640, 480, flags);
    if (!window_) {
        const std::runtime_error failure(SDL_GetError());
        SDL_QuitSubSystem(subsystems);
        throw failure;
    }
    refresh_gamepad();
}

PlayWindow::~PlayWindow() {
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(subsystems);
}

void* PlayWindow::native_handle() const {
    const auto handle = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!handle) throw std::runtime_error("SDL window has no Windows handle");
    return handle;
}

void PlayWindow::begin() { origin_ = SDL_GetTicksNS(); }
std::uint64_t PlayWindow::time() const { return (SDL_GetTicksNS() - origin_) / 100; }

DiagnosticKeys PlayWindow::take_diagnostic_keys() { return std::exchange(diagnostic_keys_, {}); }

void PlayWindow::diagnostic_title(std::string_view status) {
    if (status.empty()) SDL_SetWindowTitle(window_, game_title);
    else SDL_SetWindowTitle(window_, (std::string(game_title) + " | " + std::string(status)).c_str());
}

void PlayWindow::refresh_gamepad() {
    if (hidden_) return;
    if (gamepad_ && !SDL_GamepadConnected(gamepad_)) {
        SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
        next_rumble_ = 0;
    }
    if (gamepad_) return;
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count && !gamepad_; ++i) gamepad_ = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

std::uint32_t PlayWindow::rumble(std::uint16_t low, std::uint16_t high) {
    if (!gamepad_ || !SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad_), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
        return 50;
    if (!SDL_RumbleGamepad(gamepad_, low, high, 1000)) return 31;
    low_ = low;
    high_ = high;
    next_rumble_ = (low || high) ? SDL_GetTicksNS() + rumble_interval : 0;
    return 0;
}

bool PlayWindow::poll() {
    const auto now = SDL_GetTicksNS();
    if (updated_ && now - updated_ < poll_interval) return true;
    updated_ = now;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) refresh_gamepad();
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.windowID == SDL_GetWindowID(window_)) {
            if (event.key.scancode == SDL_SCANCODE_F8) diagnostic_keys_.capture = true;
            if (event.key.scancode == SDL_SCANCODE_F9) diagnostic_keys_.toggle_performance = !diagnostic_keys_.toggle_performance;
        }
    }
    if (next_rumble_ && now >= next_rumble_) {
        next_rumble_ = SDL_RumbleGamepad(gamepad_, low_, high_, 1000) ? now + rumble_interval : 0;
    }
    state_ = {};
    if (gamepad_) {
        constexpr std::array digital{SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,
            SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
        constexpr std::array analog{SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
            SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
        for (unsigned i=0;i<digital.size();++i) if (SDL_GetGamepadButton(gamepad_, digital[i])) state_.buttons |= 1U << i;
        for (unsigned i=0;i<analog.size();++i) if (SDL_GetGamepadButton(gamepad_, analog[i])) state_.pressure[i] = 255;
        state_.pressure[6] = static_cast<std::uint8_t>(std::max(0,int(SDL_GetGamepadAxis(gamepad_,SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) * 255 / 32767);
        state_.pressure[7] = static_cast<std::uint8_t>(std::max(0,int(SDL_GetGamepadAxis(gamepad_,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) * 255 / 32767);
        for (unsigned i=0;i<state_.axes.size();++i) {
            const auto axis = SDL_GetGamepadAxis(gamepad_, static_cast<SDL_GamepadAxis>(i));
            state_.axes[i] = (i & 1) ? static_cast<std::int16_t>(std::min(-int(axis),32767)) : axis;
        }
    }
    if (hidden_ || !(SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS)) return true;
    const auto* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_ESCAPE]) return false;
    constexpr std::array digital{SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL};
    constexpr std::array analog{SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_E, SDL_SCANCODE_Q,
        SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_Z, SDL_SCANCODE_C};
    for (unsigned i=0;i<digital.size();++i) if (keys[digital[i]]) state_.buttons |= 1U << i;
    for (unsigned i=0;i<analog.size();++i) if (keys[analog[i]]) state_.pressure[i] = 255;
    const int x = keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A], y = keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_A]) state_.axes[0] = static_cast<std::int16_t>(x * 32767);
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_S]) state_.axes[1] = static_cast<std::int16_t>(y * 32767);
    return true;
}
}
