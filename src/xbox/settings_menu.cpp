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
#include <cstdlib>
#include <format>
#include <utility>
#include <Windows.h>
#include <commdlg.h>

namespace xbox_native {
namespace {
// A binding waits this long for input. Axes bind past half travel.
constexpr std::uint64_t capture_time = 5'000'000'000;
constexpr int capture_travel = 16384;

std::string key_name(int code) {
    if (!code) return "Unbound";
    const auto scancode = static_cast<SDL_Scancode>(code);
    std::string name = SDL_GetKeyName(SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false));
    if (name.empty()) name = SDL_GetScancodeName(scancode);
    return name.empty() ? std::format("Key {}", code) : name;
}
constexpr ImVec4 success_color{.47f,.85f,.55f,1}, error_color{1,.48f,.42f,1};
constexpr ImVec4 title_gold{1,.82f,.3f,1};

// Shortens a path from its middle so its file name stays visible.
std::string elide_path(const std::string& text, std::size_t root, float width) {
    const auto fits = [&](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x <= width; };
    if (fits(text)) return text;
    for (auto at = text.find('\\', root); at != text.npos; at = text.find('\\', at + 1))
        if (auto shown = text.substr(0, root) + "..." + text.substr(at); fits(shown)) return shown;
    for (std::size_t at = 0; at < text.size(); ++at)
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && fits("..." + text.substr(at))) return "..." + text.substr(at);
    return "...";
}
// A read-only field showing a path, with the full path on hover.
void path_field(const std::filesystem::path& path, float width) {
    const auto& style = ImGui::GetStyle();
    const auto text = path_text(std::filesystem::path(path).make_preferred());
    const auto root = path_text(path.root_path()).size();
    const auto shown = text.empty() ? std::string("No file selected") : elide_path(text, root, width - 2 * style.FramePadding.x);
    const auto pos = ImGui::GetCursorScreenPos();
    const ImVec2 end{pos.x + width, pos.y + ImGui::GetFrameHeight()};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, end, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
    draw->AddRect(pos, end, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
    draw->AddText({pos.x + style.FramePadding.x, pos.y + style.FramePadding.y},
                  ImGui::GetColorU32(text.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text), shown.c_str());
    ImGui::Dummy({width, ImGui::GetFrameHeight()});
    if (shown != text && !text.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
}
// A colored dot and a wrapped message.
void status_line(const ImVec4& color, const std::string& text) {
    const float line = ImGui::GetTextLineHeight(), radius = line * .2f;
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled({pos.x + radius, pos.y + line * .5f}, radius, ImGui::GetColorU32(color));
    ImGui::Dummy({radius * 2, line});
    ImGui::SameLine(0, radius * 2);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// Face buttons are named by their Xbox positions.
std::string pad_name(PadInput input) {
    constexpr std::array<const char*, 21> buttons{"A", "B", "X", "Y", "Back", "Guide", "Start", "Left stick click",
        "Right stick click", "LB", "RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share",
        "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2", "Touchpad"};
    switch (input.kind) {
    case PadInput::none: return "Unbound";
    case PadInput::button:
        return input.index < buttons.size() ? buttons[input.index] : std::format("Button {}", input.index);
    case PadInput::positive:
    case PadInput::negative: break;
    }
    const bool positive = input.kind == PadInput::positive;
    switch (input.index) {
    case SDL_GAMEPAD_AXIS_LEFTX: return positive ? "Left stick right" : "Left stick left";
    case SDL_GAMEPAD_AXIS_LEFTY: return positive ? "Left stick down" : "Left stick up";
    case SDL_GAMEPAD_AXIS_RIGHTX: return positive ? "Right stick right" : "Right stick left";
    case SDL_GAMEPAD_AXIS_RIGHTY: return positive ? "Right stick down" : "Right stick up";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
    default: return std::format("Axis {}{}", input.index, positive ? '+' : '-');
    }
}
}

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

// A new binding swaps with the control that held that input, so every input
// stays bound once.
void SettingsMenu::bind(int control, bool pad, int key, PadInput input) {
    auto& b = settings_.bindings;
    if (pad) {
        const auto old = b.pad[control];
        if (input.kind != PadInput::none) std::ranges::replace(b.pad, input, old);
        b.pad[control] = input;
    } else {
        const auto old = b.keys[control];
        if (key) std::ranges::replace(b.keys, key, old);
        b.keys[control] = key;
    }
    message.clear();
    bindings_changed_ = true;
}

bool SettingsMenu::capture(const SDL_Event& event) {
    // Esc or B cancels a restart or exit confirmation instead of closing settings.
    if (confirm_ != Request::none) {
        const bool cancel = (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) ||
            (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST);
        if (cancel) confirm_ = Request::none;
        return cancel;
    }
    if (capture_.control < 0) return false;
    const int control = capture_.control;
    const bool pad = capture_.pad;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (event.key.repeat) return true;
        const auto code = event.key.scancode;
        if (code == SDL_SCANCODE_ESCAPE) capture_ = {};
        else if (code == SDL_SCANCODE_DELETE) { bind(control, pad, 0, {}); capture_ = {}; }
        else if (pad) return true;
        else if (code == SDL_SCANCODE_F8 || code == SDL_SCANCODE_F9)
            message = std::format("{} is reserved for diagnostics.", SDL_GetScancodeName(code));
        else { bind(control, false, code, {}); capture_ = {}; }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Guide opens and closes settings.
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) capture_ = {};
        else if (pad) { bind(control, true, 0, {PadInput::button, event.gbutton.button}); capture_ = {}; }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (pad && std::abs(int(event.gaxis.value)) >= capture_travel) {
            bind(control, true, 0, {event.gaxis.value > 0 ? PadInput::positive : PadInput::negative, event.gaxis.axis});
            capture_ = {};
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP: return true;
    default: return false;
    }
}

// The ImGui backend reads controllers directly. While a binding waits, and
// until the controller is released afterwards, menu navigation ignores it so
// the input that sets a binding does not also move or activate the menu.
void SettingsMenu::block_gamepad_navigation() {
    bool block = capture_.control >= 0;
    if (!block && pad_blocked_) {
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count && !block; ++i) {
            auto* pad = SDL_GetGamepadFromID(ids[i]);
            for (int b = 0; pad && b < SDL_GAMEPAD_BUTTON_COUNT && !block; ++b)
                block = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
            for (int a = 0; pad && a < SDL_GAMEPAD_AXIS_COUNT && !block; ++a)
                block = std::abs(int(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a)))) >
                    (a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 3277 : 8000);
        }
        SDL_free(ids);
    }
    if (block == pad_blocked_) return;
    pad_blocked_ = block;
    ImGui_ImplSDL3_SetGamepadMode(block ? ImGui_ImplSDL3_GamepadMode_Manual : ImGui_ImplSDL3_GamepadMode_AutoFirst, nullptr, 0);
}

bool SettingsMenu::controls_page() {
    auto& b = settings_.bindings;
    bool changed = false;
    const auto now = SDL_GetTicksNS();
    ImGui::TextWrapped("Select a binding, then press a key or controller input. Esc cancels; Delete or right-click clears.");
    ImGui::BeginDisabled(b == InputBindings{});
    if (ImGui::Button("Reset controls")) { b = {}; capture_ = {}; changed = true; }
    ImGui::EndDisabled();
    ImGui::Spacing();
    constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("Bindings", 3, flags)) {
        ImGui::TableSetupColumn("Xbox control", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(control_count); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const auto label = control_names[i].label;
            ImGui::TextUnformatted(label.data(), label.data() + label.size());
            for (const bool pad : {false, true}) {
                ImGui::TableNextColumn();
                const bool waiting = capture_.control == i && capture_.pad == pad;
                const auto text = !waiting ? (pad ? pad_name(b.pad[i]) : key_name(b.keys[i]))
                    : std::format("{} ({})", pad ? "Press a controller input" : "Press a key",
                                  (capture_.until - std::min(now, capture_.until) + 999'999'999) / 1'000'000'000);
                ImGui::PushID(i * 2 + pad);
                if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{.55f,.40f,.08f,1});
                // A fixed id keeps keyboard and controller focus on the cell as its text changes.
                if (ImGui::Button(std::format("{}###binding", text).c_str(), {-FLT_MIN, 0}))
                    capture_ = waiting ? Capture{} : Capture{i, pad, now + capture_time};
                if (waiting) ImGui::PopStyleColor();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { bind(i, pad, 0, {}); capture_ = {}; }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    return changed;
}

bool SettingsMenu::media_page(bool playing) {
    auto& s = settings_;
    bool changed = false;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", playing ? "These files are in use. Choose different files before starting the game."
                                     : "Choose your Xbox disc image and its default.xbe, then validate them.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    const auto& style = ImGui::GetStyle();
    const float browse_width = ImGui::CalcTextSize("Browse...").x + 2 * style.FramePadding.x;
    for (int i = 0; i < 2; ++i) {
        auto& path = i ? s.disc : s.xbe;
        ImGui::PushID(i);
        ImGui::TextUnformatted(i ? "Xbox disc image" : "Game executable");
        std::error_code error;
        if (path.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(not selected)"); }
        else if (!std::filesystem::is_regular_file(path, error)) {
            ImGui::SameLine();
            ImGui::TextColored(error_color, "(file not found)");
        }
        const float field = ImGui::GetContentRegionAvail().x - (playing ? 0 : browse_width + style.ItemSpacing.x);
        path_field(path, field);
        if (!playing) {
            ImGui::SameLine();
            if (ImGui::Button("Browse...") && browse(path, i != 0)) { media_ = Media::unchecked; changed = true; }
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Validate game files")) {
        try { s.validate_media(); media_ = Media::verified; }
        catch (const std::exception& failure) { media_ = Media::failed; media_error_ = failure.what(); }
    }
    if (media_ == Media::verified) status_line(success_color, "The disc and executable match the supported release.");
    else if (media_ == Media::failed) status_line(error_color, media_error_);
    else if (!playing) status_line(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Not validated yet.");
    return changed;
}

// Restarting or exiting during play asks first. Cancel has the initial focus.
void SettingsMenu::confirm_dialog() {
    constexpr auto id = "Confirm";
    if (confirm_ != Request::none && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {.5f,.5f});
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (confirm_ == Request::none) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const bool restart = confirm_ == Request::restart;
    ImGui::PushFont(nullptr, 20);
    ImGui::TextUnformatted(restart ? "Restart the game?" : "Exit the game?");
    ImGui::PopFont();
    ImGui::TextDisabled("Progress since your last save will be lost.");
    ImGui::Spacing();
    const ImVec2 size{std::max(110.f, ImGui::CalcTextSize("Restart").x + 2 * ImGui::GetStyle().FramePadding.x), 0};
    if (ImGui::Button(restart ? "Restart" : "Exit", size)) {
        request = confirm_;
        confirm_ = Request::none;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", size)) {
        confirm_ = Request::none;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}

bool SettingsMenu::draw(bool playing, DiagnosticKeys& keys, bool measuring) {
    auto& s = settings_;
    if (visible != shown_) { shown_ = visible; custom_rate_ = s.refresh_rate; capture_ = {}; }
    if (capture_.control >= 0 && SDL_GetTicksNS() >= capture_.until) capture_ = {};
    if (std::exchange(bindings_changed_, false)) {
        try { s.save(file_); }
        catch (const std::exception& error) { message = error.what(); }
    }
    const bool noticing = !notice_.empty() && SDL_GetTicksNS() < notice_until_;
    if (!visible && !s.show_fps && !noticing) return false;
    // The game hides an idle pointer while settings are closed.
    auto& io = ImGui::GetIO();
    if (visible) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    block_gamepad_navigation();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const auto area = ImGui::GetMainViewport()->WorkSize;
    bool play = false, changed = false;
    if (visible) {
        const auto width = std::min(890.f, std::max(1.f,area.x - 16));
        const auto height = std::min(540.f, std::max(1.f,area.y - 16));
        ImGui::SetNextWindowPos({area.x / 2, area.y / 2}, ImGuiCond_Always, {.5f,.5f});
        ImGui::SetNextWindowSize({width, height});
        ImGui::PushFont(nullptr,18);
        ImGui::Begin("Mystery Mayhem settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::PushFont(nullptr,24);
        const float icon_size = ImGui::GetFrameHeight();
        ImGui::Image(renderer_.window_icon(), {icon_size, icon_size});
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(title_gold, "Scooby-Doo! Mystery Mayhem");
        ImGui::PopFont();
        ImGui::Separator();
        // Display, enhancement and audio settings are in the game's Recomp Options menu.
        constexpr std::array pages{"Game Files","General","Controls","Diagnostics"};
        const float body = std::max(40.f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 4);
        constexpr auto child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;
        ImGui::BeginChild("Navigation",{std::min(158.f,width * .28f),body}, child_flags);
        for (int i = 0; i < static_cast<int>(pages.size()); ++i)
            if (ImGui::Selectable(pages[i],page_ == i,0,{0,34})) { page_ = i; capture_ = {}; }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Page",{0, body}, child_flags);
        ImGui::SeparatorText(pages[page_]);
        try {
            switch (page_) {
            case 0:
                changed |= media_page(playing);
                break;
            case 1: {
                ImGui::TextWrapped("Display, enhancement and audio settings are in Recomp Options on the game's title and pause menus.");
                ImGui::Spacing();
                ImGui::TextUnformatted("Custom frame rate limit");
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Hz",&custom_rate_,0,0);
                ImGui::SameLine();
                ImGui::BeginDisabled(custom_rate_ < 0 || custom_rate_ > 1000 || custom_rate_ == s.refresh_rate);
                if (ImGui::Button("Apply")) { s.refresh_rate = custom_rate_; changed = true; }
                ImGui::EndDisabled();
                const auto current = s.refresh_rate ? std::format("{} Hz", s.refresh_rate) : std::string("unlimited");
                ImGui::TextDisabled("Any rate up to 1000 Hz, or 0 for unlimited. Current limit: %s.", current.c_str());
                ImGui::Spacing();
                changed |= ImGui::Checkbox("Show frame rate",&s.show_fps);
                break;
            }
            case 2:
                changed |= controls_page();
                break;
            case 3:
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
        ImGui::PushStyleColor(ImGuiCol_Button, title_gold);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1,.88f,.47f,1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{.90f,.70f,.21f,1});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{.09f,.07f,.03f,1});
        const bool pressed = ImGui::Button(playing ? "Return to game" : "Play");
        ImGui::PopStyleColor(4);
        if (pressed) {
            try {
                if (!playing) s.validate_media();
                visible = false;
                play = true;
            } catch (const std::exception& error) { page_ = 0; media_ = Media::failed; media_error_ = error.what(); }
        }
        // The right side holds Restart game (during play), Restore defaults and Exit game.
        const auto& style = ImGui::GetStyle();
        const auto button_width = [&](const char* label) { return ImGui::CalcTextSize(label).x + 2 * style.FramePadding.x; };
        const float right = (playing ? button_width("Restart game") + style.ItemSpacing.x : 0) +
            button_width("Restore defaults") + style.ItemSpacing.x + button_width("Exit game");
        constexpr auto hint = "Esc or Guide opens settings in game";
        ImGui::SameLine();
        if (!playing && ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize(hint).x + right + style.ItemSpacing.x) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", hint);
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - right));
        if (playing) {
            if (ImGui::Button("Restart game")) confirm_ = Request::restart;
            ImGui::SameLine();
        }
        if (ImGui::Button("Restore defaults")) {
            PlayerSettings defaults;
            defaults.xbe = s.xbe;
            defaults.disc = s.disc;
            if (s.fullscreen && !SDL_SetWindowFullscreen(window_,false)) message = SDL_GetError();
            else { s = std::move(defaults); custom_rate_ = s.refresh_rate; capture_ = {}; changed = true; }
        }
        ImGui::SameLine();
        // Before play nothing is lost, so Exit needs no confirmation.
        if (ImGui::Button("Exit game")) {
            if (playing) confirm_ = Request::exit;
            else request = Request::exit;
        }
        confirm_dialog();
        ImGui::End();
        ImGui::PopFont();
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
