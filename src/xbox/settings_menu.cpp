#include "settings_menu.h"
#include "file.h"
#include "play_window.h"
#include "render.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl3.h>
#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <Windows.h>
#include <commdlg.h>

namespace xbox_native {
SettingsMenu::SettingsMenu(SDL_Window* window, Renderer& renderer, PlayerSettings& settings, std::filesystem::path file)
    : window_(window), renderer_(renderer), settings_(settings), file_(std::move(file)), custom_rate_(settings.refresh_rate) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (!ImGui_ImplSDL3_InitForD3D(window)) {
        ImGui::DestroyContext();
        throw std::runtime_error("Could not initialize the settings window");
    }
    if (!ImGui_ImplDX11_Init(renderer.native_device(), renderer.native_context())) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("Could not initialize settings graphics");
    }
    wchar_t windows[MAX_PATH];
    const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length && length < MAX_PATH) {
        const auto font = std::filesystem::path(windows) / "Fonts/segoeui.ttf";
        if (std::filesystem::is_regular_file(font)) io.Fonts->AddFontFromFileTTF(path_text(font).c_str(), 16);
    }
    auto& style = ImGui::GetStyle();
    style.FontSizeBase = 16;
    style.ItemSpacing = {9, 7};
    style.FrameBorderSize = 1;
    style.WindowRounding = 8;
    style.ChildRounding = style.FrameRounding = 4;
    style.WindowPadding = {14, 12};
    style.FramePadding = {8, 5};
    style.Colors[ImGuiCol_WindowBg] = {.055f,.10f,.13f,.98f};
    style.Colors[ImGuiCol_Header] = {.03f,.36f,.38f,1};
    style.Colors[ImGuiCol_Button] = {.05f,.32f,.35f,1};
    style.Colors[ImGuiCol_FrameBg] = {.10f,.19f,.22f,1};
    style.Colors[ImGuiCol_HeaderHovered] = style.Colors[ImGuiCol_ButtonHovered] = {.06f,.43f,.44f,1};
    style.Colors[ImGuiCol_HeaderActive] = style.Colors[ImGuiCol_ButtonActive] = {.03f,.32f,.34f,1};
    style.Colors[ImGuiCol_SliderGrab] = {.09f,.54f,.53f,1};
    style.Colors[ImGuiCol_SliderGrabActive] = {.14f,.68f,.63f,1};
    style.Colors[ImGuiCol_CheckMark] = {1,.81f,.26f,1};
}
SettingsMenu::~SettingsMenu() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}
void SettingsMenu::event(const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); }

bool SettingsMenu::browse(std::filesystem::path& selected, bool disc) {
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = disc ? L"Xbox disc images\0*.iso;*.xiso\0All files\0*.*\0" : L"Xbox executable\0*.xbe\0All files\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError()) throw std::runtime_error("Could not open the file picker.");
        return false;
    }
    selected = path.data();
    message.clear();
    return true;
}

void SettingsMenu::notify(Notice, std::string title, std::string detail) {
    notice_ = detail.empty() ? std::move(title) : title + ": " + detail;
    notice_until_ = SDL_GetTicksNS() + 4'000'000'000ULL;
}

bool SettingsMenu::enhancements_page() {
    auto& e = settings_.enhancements;
    bool changed = false;
    const auto choice = [&](const char* id, int& value, std::span<const int> values, auto&& name, int limit) {
        if (!ImGui::BeginCombo(id, name(value).c_str())) return;
        for (const int option : values) {
            ImGui::BeginDisabled(option > limit);
            if (ImGui::Selectable(name(option).c_str(), value == option) && value != option) { value = option; changed = true; }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    };
    const auto [width, height] = renderer_.scanout_size();
    ImGui::TextUnformatted("Internal resolution");
    constexpr std::array scales{1,2,3,4,5,6};
    choice("##Internal resolution", e.scale, scales, [&](int n) {
        return std::format("{}x{} ({}x{})", n, n == 1 ? " native" : "", width * n, height * n);
    }, 6);
    ImGui::TextUnformatted("Anti-aliasing");
    constexpr std::array samples{1,2,4,8};
    choice("##MSAA", e.samples, samples, [](int n) {
        return n == 1 ? std::string("MSAA off") : std::format("{}x MSAA", n);
    }, renderer_.max_samples());
    changed |= ImGui::Checkbox("FXAA", &e.fxaa);
    ImGui::TextUnformatted("Anisotropic filtering");
    constexpr std::array levels{1,2,4,8,16};
    choice("##Anisotropic filtering", e.anisotropy, levels, [](int n) {
        return n == 1 ? std::string("Game default") : std::format("{}x", n);
    }, 16);
    ImGui::Spacing();
    ImGui::TextDisabled("The defaults reproduce the original Xbox image.");
    if (const auto error = renderer_.enhancement_error(); !error.empty()) ImGui::TextWrapped("%s", error.c_str());
    return changed;
}

bool SettingsMenu::draw(bool playing, DiagnosticKeys& keys, bool measuring) {
    auto& s = settings_;
    const bool noticing = !notice_.empty() && SDL_GetTicksNS() < notice_until_;
    if (!visible && !s.show_fps && !noticing) return false;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const auto area = ImGui::GetMainViewport()->WorkSize;
    bool play = false, changed = false;
    if (visible) {
        const auto width = std::min(790.f, std::max(1.f,area.x - 16));
        const auto height = std::min(480.f, std::max(1.f,area.y - 16));
        ImGui::SetNextWindowPos({area.x / 2, area.y / 2}, ImGuiCond_Always, {.5f,.5f});
        ImGui::SetNextWindowSize({width, height});
        ImGui::Begin("Mystery Mayhem settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::PushFont(nullptr,22);
        ImGui::TextColored({1,.82f,.3f,1}, "Scooby-Doo! Mystery Mayhem");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("Settings");
        ImGui::Separator();
        constexpr std::array pages{"Game Files","General","Audio","Graphics","Enhancements","Diagnostics"};
        const float body = std::max(40.f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 4);
        constexpr auto child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;
        ImGui::BeginChild("Navigation",{std::min(140.f,width * .28f),body}, child_flags);
        for (int i = 0; i < static_cast<int>(pages.size()); ++i)
            if (ImGui::Selectable(pages[i],page_ == i,0,{0,30})) page_ = i;
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Page",{0, body}, child_flags);
        ImGui::SeparatorText(pages[page_]);
        try {
            switch (page_) {
            case 0:
                for (int i = 0; i < 2; ++i) {
                    auto& path = i ? s.disc : s.xbe;
                    ImGui::PushID(i);
                    ImGui::TextUnformatted(i ? "Xbox disc image" : "Game executable");
                    ImGui::TextWrapped("%s", path_text(path).c_str());
                    ImGui::BeginDisabled(playing);
                    if (ImGui::Button("Browse...")) changed |= browse(path, i != 0);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                if (playing) ImGui::TextWrapped("Choose game files before starting the game.");
                if (ImGui::Button("Validate game files")) { s.validate_media(); message = "Game files verified."; }
                break;
            case 1: {
                const auto label = s.refresh_rate ? std::format("{} Hz",s.refresh_rate) : "Unlimited";
                ImGui::TextUnformatted("Refresh rate");
                if (ImGui::BeginCombo("##Refresh rate",label.c_str())) {
                    for (int rate : {30,60,75,90,120,144,165,180,240,360,480,0}) {
                        const auto name = rate ? std::format("{} Hz",rate) : "Unlimited";
                        if (ImGui::Selectable(name.c_str(),s.refresh_rate == rate)) {
                            custom_rate_ = s.refresh_rate = rate;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Custom Hz",&custom_rate_,0,0);
                ImGui::SameLine();
                ImGui::BeginDisabled(custom_rate_ < 0 || custom_rate_ > 1000);
                if (ImGui::Button("Apply")) { s.refresh_rate = custom_rate_; changed = true; }
                ImGui::EndDisabled();
                changed |= ImGui::Checkbox("Show frame rate",&s.show_fps);
                break;
            }
            case 2:
                ImGui::TextUnformatted("Master volume");
                ImGui::SliderInt("##Master volume",&s.volume,0,100,"%d%%",ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::IsItemDeactivatedAfterEdit();
                changed |= ImGui::Checkbox("Mute",&s.muted);
                changed |= ImGui::Checkbox("Mute on focus loss",&s.mute_unfocused);
                break;
            case 3: {
                int aspect = s.widescreen ? 1 : 0;
                ImGui::TextUnformatted("Output aspect ratio");
                if (ImGui::Combo("##Aspect ratio",&aspect,"Standard (4:3)\0Widescreen (16:9)\0")) {
                    s.widescreen = aspect == 1;
                    changed = true;
                }
                changed |= ImGui::Checkbox("V-sync",&s.vsync);
                if (ImGui::Checkbox("Fullscreen",&s.fullscreen)) {
                    if (!SDL_SetWindowFullscreen(window_,s.fullscreen)) {
                        s.fullscreen = !s.fullscreen;
                        throw std::runtime_error(SDL_GetError());
                    }
                    changed = true;
                }
                break;
            }
            case 4:
                changed |= enhancements_page();
                break;
            case 5:
                ImGui::BeginDisabled(!playing);
                if (ImGui::Button("Capture frame (F8)")) keys.capture = true;
                if (ImGui::Button(measuring ? "Stop recording (F9)" : "Record performance (F9)")) keys.toggle_performance = true;
                ImGui::EndDisabled();
                ImGui::TextWrapped("Captures and performance recordings are saved in build/captures.");
                break;
            }
            if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
        } catch (const std::exception& error) { message = error.what(); }
        ImGui::EndChild();
        if (ImGui::Button(playing ? "Return to game" : "Play")) {
            try {
                if (!playing) s.validate_media();
                visible = false;
                play = true;
            } catch (const std::exception& error) { page_ = 0; message = error.what(); }
        }
        if (!playing && width >= 490) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("F1 or Guide opens settings in game");
        }
        ImGui::SameLine();
        const auto button_width = ImGui::CalcTextSize("Restore defaults").x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x-button_width));
        if (ImGui::Button("Restore defaults")) {
            PlayerSettings defaults;
            defaults.xbe = s.xbe;
            defaults.disc = s.disc;
            if (s.fullscreen && !SDL_SetWindowFullscreen(window_,false)) message = SDL_GetError();
            else { s = std::move(defaults); custom_rate_ = s.refresh_rate; changed = true; }
        }
        ImGui::End();
        if (changed) {
            renderer_.enhance(s.enhancements);
            try { s.save(file_); }
            catch (const std::exception& error) { message = error.what(); }
        }
    }
    if (playing && s.show_fps) {
        ImGui::SetNextWindowPos({area.x-8,8},ImGuiCond_Always,{1,0});
        ImGui::SetNextWindowBgAlpha(.75f);
        ImGui::Begin("Frame rate",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f FPS",ImGui::GetIO().Framerate);
        ImGui::End();
    }
    if (noticing) {
        ImGui::SetNextWindowPos({area.x-8,area.y-8},ImGuiCond_Always,{1,1});
        ImGui::SetNextWindowBgAlpha(.75f);
        ImGui::Begin("Notice",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(notice_.c_str());
        ImGui::End();
    }
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return play;
}
}
