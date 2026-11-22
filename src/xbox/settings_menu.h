#pragma once
#include "settings.h"
#include <cstdint>

struct SDL_Window;
union SDL_Event;
namespace xbox_native {
class Renderer;
struct DiagnosticKeys;
class SettingsMenu {
public:
    SettingsMenu(SDL_Window*, Renderer&, PlayerSettings&, std::filesystem::path file);
    ~SettingsMenu();
    void event(const SDL_Event&);
    bool draw(bool playing, DiagnosticKeys&, bool measuring);
    bool visible = true;
    std::string message;
private:
    SDL_Window* window_;
    PlayerSettings& settings_;
    std::filesystem::path file_;
    int page_{};
    int custom_rate_;
    bool browse(std::filesystem::path&, bool disc);
};
}
