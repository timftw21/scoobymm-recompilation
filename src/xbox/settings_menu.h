#pragma once
#include "settings.h"
#include <cstdint>
#include <string>

struct SDL_Window;
union SDL_Event;
namespace xbox_native {
class Renderer;
struct DiagnosticKeys;
enum class Notice : int;
class SettingsMenu {
public:
    SettingsMenu(SDL_Window*, Renderer&, PlayerSettings&, std::filesystem::path file);
    ~SettingsMenu();
    void event(const SDL_Event&);
    // While a binding waits for input, takes the next key or controller
    // input, and while a confirmation is open, takes Esc and B. Returns true
    // when the event goes no further.
    bool capture(const SDL_Event&);
    bool draw(bool playing, DiagnosticKeys&, bool measuring);
    void notify(Notice, std::string title, std::string detail = {});
    bool visible = true;
    std::string message;
    // The player chose to exit or restart the game.
    enum class Request { none, exit, restart } request{};
private:
    SDL_Window* window_;
    Renderer& renderer_;
    PlayerSettings& settings_;
    std::filesystem::path file_;
    int page_{};
    int custom_rate_;
    bool shown_{};
    std::string notice_;
    std::uint64_t notice_until_{};
    struct Capture {
        int control = -1;
        bool pad{};
        std::uint64_t until{};
    } capture_;
    bool pad_blocked_{}, bindings_changed_{};
    enum class Media { unchecked, verified, failed } media_{};
    std::string media_error_;
    Request confirm_{};
    void confirm_dialog();
    bool browse(std::filesystem::path&, bool disc);
    bool media_page(bool playing);
    bool controls_page();
    void bind(int control, bool pad, int key, PadInput);
    void block_gamepad_navigation();
};
}
