#include "play_window.h"
#include "render.h"
#include "settings_menu.h"

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

PlayWindow::PlayWindow(PlayOptions& options) : options_(options), hidden_(options.hidden) {
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_InitSubSystem(subsystems)) throw std::runtime_error(SDL_GetError());
    const auto flags = SDL_WINDOW_RESIZABLE | (hidden_ ? SDL_WINDOW_HIDDEN : 0) |
        (options.fullscreen && !hidden_ ? SDL_WINDOW_FULLSCREEN : 0);
    window_ = SDL_CreateWindow(game_title, options.widescreen ? 854 : 640, 480, flags);
    if (!window_) {
        const std::runtime_error failure(SDL_GetError());
        SDL_QuitSubSystem(subsystems);
        throw failure;
    }
    try {
        renderer_ = std::make_unique<Renderer>(640,480);
        renderer_->attach_window(native_handle());
        menu_ = std::make_unique<SettingsMenu>(window_,*renderer_,options_,options.storage / "settings.cfg");
        menu_->visible = options.launcher;
        menu_->message = options.settings_error;
        focused_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
        refresh_gamepad();
    } catch (...) {
        menu_.reset();
        renderer_.reset();
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(subsystems);
        throw;
    }
}

PlayWindow::~PlayWindow() {
    menu_.reset();
    renderer_.reset();
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(subsystems);
}

void* PlayWindow::native_handle() const {
    const auto handle = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!handle) throw std::runtime_error("SDL window has no Windows handle");
    return handle;
}

bool PlayWindow::launcher() {
    renderer_->begin(0xFF102529);
    renderer_->end();
    while (menu_->visible) {
        if (!poll()) return false;
        if (renderer_->display()) {
            menu_->draw(false, diagnostic_keys_, false);
            renderer_->present(options_.vsync);
        }
        pace(60);
    }
    return true;
}

void PlayWindow::overlay(bool measuring) {
    menu_->draw(true, diagnostic_keys_, measuring);
}

bool PlayWindow::frame_due(bool movie, std::uint32_t scanout) {
    if (options_.refresh_at == scanout) options_.refresh_rate = options_.scripted_rate;
    const auto hz = options_.refresh_rate;
    // The movie decoder already schedules each overlay from its own timestamps.
    if (movie) { skip_ = 0; return true; }
    // 0x184BB clamps a simulation step to 50 ms. For slow displays run full
    // updates at a multiple of the cap and select complete guest frames.
    if (hz <= 0 || hz >= 20) { skip_ = 0; return true; }
    const auto divisor = (30 + hz - 1) / hz;
    skip_ %= divisor;
    const bool show = skip_ == 0;
    ++skip_;
    return show;
}

void PlayWindow::pace_game(bool movie) {
    if (movie) { pacing_rate_ = -1; return; }
    const auto hz = options_.refresh_rate;
    pace(hz > 0 && hz < 20 ? hz * ((30 + hz - 1) / hz) : hz);
}

void PlayWindow::pace(int hz) {
    if (!hz) { pacing_rate_ = 0; return; }
    const auto now = SDL_GetTicksNS();
    if (hz != pacing_rate_) {
        pacing_rate_ = hz;
        deadline_ = now;
    }
    const auto period = 1'000'000'000ULL / hz;
    if (deadline_ > now) SDL_DelayPrecise(deadline_ - now);
    deadline_ = (now > deadline_ + period ? now : deadline_) + period;
}

float PlayWindow::gain() const {
    if (hidden_ || options_.muted || (options_.mute_unfocused && !focused_)) return 0;
    return options_.volume / 100.f;
}

void PlayWindow::begin() { playing_ = true; menu_->visible = false; pacing_rate_ = -1; origin_ = SDL_GetTicksNS(); }
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
        if (menu_->visible || options_.show_fps) menu_->event(event);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused_ = true;
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) focused_ = false;
        const bool toggle = (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            event.key.scancode == SDL_SCANCODE_F1) ||
            (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE);
        if (playing_ && toggle) menu_->visible = !menu_->visible;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
            if (playing_ && menu_->visible) menu_->visible = false;
            else return false;
        }
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
    if (!hidden_ && focused_) {
        const auto* keys = SDL_GetKeyboardState(nullptr);
        constexpr std::array digital{SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
            SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL};
        constexpr std::array analog{SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_Z, SDL_SCANCODE_C};
        for (unsigned i=0;i<digital.size();++i) if (keys[digital[i]]) state_.buttons |= 1U << i;
        for (unsigned i=0;i<analog.size();++i) if (keys[analog[i]]) state_.pressure[i] = 255;
        const int x = keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A], y = keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_A]) state_.axes[0] = static_cast<std::int16_t>(x * 32767);
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_S]) state_.axes[1] = static_cast<std::int16_t>(y * 32767);
    }
    // The button that activates Play or Return belongs to the menu until the
    // player releases it. Track each button so other inputs still respond.
    if (menu_->visible || menu_was_open_) menu_input_ = state_;
    menu_was_open_ = menu_->visible;
    if (menu_->visible) { state_ = {}; return true; }
    menu_input_.buttons &= state_.buttons;
    state_.buttons &= ~menu_input_.buttons;
    for (unsigned i=0; i<state_.pressure.size(); ++i) {
        if (!state_.pressure[i]) menu_input_.pressure[i] = 0;
        if (menu_input_.pressure[i]) state_.pressure[i] = 0;
    }
    return true;
}
}
