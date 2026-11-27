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
// The pointer hides after this long without mouse activity during play.
constexpr std::uint64_t cursor_timeout = 3'000'000'000;
// Binding values span a full axis direction. Digital controls press at half travel.
constexpr int full = 32767, pressed = 16384;

int pad_value(SDL_Gamepad* pad, PadInput input) {
    switch (input.kind) {
    case PadInput::button: return SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(input.index)) ? full : 0;
    case PadInput::positive: return std::max(0, int(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(input.index))));
    case PadInput::negative: return std::max(0, -int(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(input.index))));
    case PadInput::none: break;
    }
    return 0;
}
}

PlayWindow::PlayWindow(PlayOptions& options) : options_(options), enhanced_(options.enhancements), hidden_(options.hidden) {
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_InitSubSystem(subsystems)) throw std::runtime_error(SDL_GetError());
    const auto flags = SDL_WINDOW_RESIZABLE | (hidden_ ? SDL_WINDOW_HIDDEN : 0) |
        (options.fullscreen && !hidden_ ? SDL_WINDOW_FULLSCREEN : 0);
    // 640, 854 or 1138 pixels wide.
    const auto& shape = aspect_shape(options.aspect);
    window_ = SDL_CreateWindow(game_title, (480 * shape.width + shape.height - 1) / shape.height, 480, flags);
    if (!window_) {
        const std::runtime_error failure(SDL_GetError());
        SDL_QuitSubSystem(subsystems);
        throw failure;
    }
    try {
        renderer_ = std::make_unique<Renderer>(640,480);
        renderer_->attach_window(native_handle());
        renderer_->enhance(options.enhancements);
        renderer_->hud_cache(options.storage / "hud-cache");
        menu_ = std::make_unique<SettingsMenu>(window_,*renderer_,options_,options.storage / "settings.cfg");
        menu_->visible = options.launcher;
        menu_->message = options.settings_error;
        // Take keyboard focus, including after Restart game relaunches the game.
        if (!hidden_) SDL_RaiseWindow(window_);
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
    renderer_->begin();
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

void PlayWindow::begin() {
    playing_ = true;
    menu_->visible = false;
    pacing_rate_ = -1;
    origin_ = mouse_moved_ = SDL_GetTicksNS();
}
std::uint64_t PlayWindow::time() const { return (SDL_GetTicksNS() - origin_) / 100; }

DiagnosticKeys PlayWindow::take_diagnostic_keys() { return std::exchange(diagnostic_keys_, {}); }

void PlayWindow::settings_changed() {
    renderer_->enhance(options_.enhancements);
    if (options_.enhancements != enhanced_) {
        enhanced_ = options_.enhancements;
        if (const auto error = renderer_->enhancement_error(); !error.empty()) notify(Notice::error, "Enhancements", error);
    }
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!hidden_ && fullscreen != options_.fullscreen && !SDL_SetWindowFullscreen(window_, options_.fullscreen))
        options_.fullscreen = fullscreen;
    try { options_.save(options_.storage / "settings.cfg"); }
    catch (const std::exception& error) { notify(Notice::error, "Settings were not saved", error.what()); }
}

void PlayWindow::notify(Notice tone, std::string_view title, std::string_view detail) {
    if (!hidden_) menu_->notify(tone, std::string(title), std::string(detail));
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
    // Exit and Restart close the session like the window's close button.
    if (menu_->request != SettingsMenu::Request::none) {
        options_.restart = menu_->request == SettingsMenu::Request::restart;
        return false;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // A binding waiting for input takes the next key or controller input.
        if (menu_->visible && menu_->capture(event)) continue;
        if (menu_->visible || options_.show_fps) menu_->event(event);
        if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL) mouse_moved_ = now;
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused_ = true;
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) focused_ = false;
        // Esc and Guide open and close settings during play. Esc leaves the launcher;
        // during play, Exit game in settings closes the game.
        const bool escape = event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE;
        const bool guide = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE;
        if (playing_ && (escape || guide)) menu_->visible = !menu_->visible;
        else if (escape) return false;
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
    const bool hide_cursor = !hidden_ && playing_ && !menu_->visible && now - mouse_moved_ >= cursor_timeout;
    if (hide_cursor != cursor_hidden_ && (hide_cursor ? SDL_HideCursor() : SDL_ShowCursor())) cursor_hidden_ = hide_cursor;
    // Each Xbox control takes the stronger of its controller and keyboard
    // bindings. A stick axis is its positive direction less its negative one.
    state_ = {};
    const auto& bindings = options_.bindings;
    const bool* keys = !hidden_ && focused_ ? SDL_GetKeyboardState(nullptr) : nullptr;
    std::array<int, control_count> value{};
    for (std::size_t i = 0; i < control_count; ++i) {
        if (gamepad_) value[i] = pad_value(gamepad_, bindings.pad[i]);
        if (keys && bindings.keys[i] > 0 && keys[bindings.keys[i]]) value[i] = std::max(value[i], full);
    }
    for (unsigned i = 0; i < state_.pressure.size(); ++i)
        state_.pressure[i] = static_cast<std::uint8_t>(std::min(255, value[i] * 255 / full));
    for (unsigned i = 0; i < 8; ++i) if (value[8 + i] >= pressed) state_.buttons |= 1U << i;
    for (unsigned stick = 0; stick < 2; ++stick) {
        const auto* d = &value[std::size_t(Control::left_up) + stick * 4]; // up, down, left, right
        state_.axes[stick * 2] = static_cast<std::int16_t>(std::clamp(d[3] - d[2], -32768, 32767));
        state_.axes[stick * 2 + 1] = static_cast<std::int16_t>(std::clamp(d[0] - d[1], -32768, 32767));
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
