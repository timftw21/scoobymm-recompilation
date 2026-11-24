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
    bool draw(bool playing, DiagnosticKeys&, bool measuring);
    void notify(Notice, std::string title, std::string detail = {});
    bool visible = true;
    std::string message;
private:
    SDL_Window* window_;
    Renderer& renderer_;
    PlayerSettings& settings_;
    std::filesystem::path file_;
    int page_{};
    int custom_rate_;
    std::string notice_;
    std::uint64_t notice_until_{};
    bool browse(std::filesystem::path&, bool disc);
    bool enhancements_page();
};
}
