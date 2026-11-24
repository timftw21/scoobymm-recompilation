#include "analysis_map.h"
#include "xbox/boot.h"
#include "xbox/file.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <Windows.h>

namespace {
constexpr std::array<std::wstring_view, 4> dpad_options{
    L"--up-at-scanout", L"--down-at-scanout", L"--left-at-scanout", L"--right-at-scanout"
};
constexpr std::array<std::wstring_view, 3> face_options{
    L"--confirm-at-scanout", L"--b-at-scanout", L"--x-at-scanout"
};

// The game is a windowed program, so starting it opens no console. Command-line
// runs keep their output: redirected handles are inherited, and a run with
// arguments writes to the console it was started from.
bool attach_console(bool arguments) {
    const auto usable = [](DWORD id) {
        const auto handle = GetStdHandle(id);
        return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    bool output = usable(STD_OUTPUT_HANDLE), errors = usable(STD_ERROR_HANDLE);
    if ((!output || !errors) && arguments && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream{};
        if (!output) output = !freopen_s(&stream, "CONOUT$", "w", stdout);
        if (!errors) errors = !freopen_s(&stream, "CONOUT$", "w", stderr);
    }
    return errors;
}

// Without a console, a player still learns why the game could not run.
void report_failure(std::string_view text, bool console, bool hidden) {
    std::cerr << text << '\n';
    if (console || hidden) return;
    const auto wide = std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
    MessageBoxW(nullptr, wide.c_str(), L"Scooby-Doo! Mystery Mayhem", MB_OK | MB_ICONERROR);
}

std::filesystem::path application_root() {
    std::array<wchar_t, 32768> path;
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot locate the game executable");
    const auto directory = std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
    for (auto candidate = directory;;) {
        if (std::filesystem::is_regular_file(candidate / "knowledge/supported_target.json")) return candidate;
        auto parent = candidate.parent_path();
        if (parent.empty() || candidate == parent) return directory;
        candidate = std::move(parent);
    }
}

void usage() {
    std::cout << "scoobymm [DEFAULT.xbe DISC.xiso] [--fullscreen] [--mute] [--no-vsync]\n"
                 "  Omit both media paths to use this project's verified Xbox files.\n"
                 "  --settings           Open settings before play (default with no arguments)\n"
                 "  --refresh-rate HZ    Rendering cap, or 0 for unlimited\n"
                 "  --refresh-at-scanout N HZ   Change the cap at a repeatable benchmark boundary\n"
                 "  --widescreen         Use the Xbox widescreen display setting\n"
                 "  --shader-cache DIR   Accepted for compatibility. Player shaders are embedded.\n"
                 "  --storage DIR        Playtest save directory (default data/user)\n"
                 "  --language en|fr|de   Xbox game language\n"
                 "  --report JSON        New session report\n"
                 "  --frame PNG [--frame-index N]   Save a scanout\n"
                 "  --capture-frame DIR [--capture-at N]   Request an F8 capture after scanout N\n"
                 "  --performance DIR [--performance-at N] [--performance-frames N]\n"
                 "                       Record F9 diagnostics after scanout N\n"
                 "  --hidden --frames N   Bounded run with a hidden window\n"
                 "  --start-at-scanout N, --confirm-at-scanout N, --b-at-scanout N, --x-at-scanout N   Script button input\n"
                 "  --up-at-scanout N, --down-at-scanout N, --left-at-scanout N, --right-at-scanout N   Script D-pad input\n"
                 "  --stick-at-scanout N FRAMES X Y   Hold the left stick at signed Xbox coordinates\n"
                 "Keyboard: Enter to start, Space to confirm, Left Shift to go back,\n"
                 "arrows for the D-pad, W/A/S/D for the left stick, Escape to quit.\n"
                 "F1 or the controller Guide button opens settings. F8 captures a frame. F9 starts or stops performance recording (60 seconds maximum).\n";
}
}

int wmain(int argc, wchar_t* argv[]) {
    const bool console = attach_console(argc > 1);
    const bool hidden = std::any_of(argv + 1, argv + argc, [](const wchar_t* option) { return std::wstring_view(option) == L"--hidden"; });
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") { usage(); return 0; }
    try {
        xbox_native::PlayOptions options;
        const auto root = application_root();
        options.launcher = argc == 1;
        int next = 1;
        if (argc > 1 && !std::wstring_view(argv[1]).starts_with(L"--")) {
            if (argc < 3 || std::wstring_view(argv[2]).starts_with(L"--")) {
                throw std::runtime_error("Provide both media paths or omit both");
            }
            options.xbe = argv[1];
            options.disc = argv[2];
            next = 3;
        } else {
            options.xbe = root / "data/local/default.xbe";
            options.disc = root / "Scooby-Doo! Mystery Mayhem/Scooby-Doo! Mystery Mayhem (USA, Europe) (En,Fr,De).xiso.iso";
        }
        options.storage = root / "data/user";
        bool set_fullscreen = false, set_mute = false, set_no_vsync = false, set_wide = false;
        std::optional<int> refresh;
        std::filesystem::path report_path;
        bool explicit_frame_index = false, capture_at = false, performance_at = false;
        for (int i=next;i<argc;++i) {
            const std::wstring_view option = argv[i];
            const auto argument = [&] {
                if (++i == argc || std::wstring_view(argv[i]).starts_with(L"--")) {
                    throw std::runtime_error("A play option is missing its value");
                }
                return std::filesystem::path(argv[i]);
            };
            const auto nonzero = [&] {
                const auto value = analysis_map::number(argument().string());
                if (!value) throw std::runtime_error("A scanout or frame count must be positive");
                return value;
            };
            const auto pulse = [&](auto& list) {
                if (list.size() == 32) throw std::runtime_error("Specify at most 32 pulses per button");
                list.push_back(nonzero());
            };
            const auto axis = [&] {
                const auto text = argument().string();
                std::int16_t value{};
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error != std::errc{} || end != text.data() + text.size())
                    throw std::runtime_error("Stick coordinates must be signed 16-bit integers");
                return value;
            };
            const auto rate = [&] {
                const auto n = analysis_map::number(argument().string());
                if (n > 1000) throw std::runtime_error("Choose a refresh rate between 0 and 1000 Hz");
                return static_cast<int>(n);
            };
            if (option == L"--fullscreen") set_fullscreen = true;
            else if (option == L"--mute") set_mute = true;
            else if (option == L"--no-vsync") set_no_vsync = true;
            else if (option == L"--widescreen") set_wide = true;
            else if (option == L"--settings") options.launcher = true;
            else if (option == L"--refresh-rate") refresh = rate();
            else if (option == L"--refresh-at-scanout") {
                options.refresh_at = nonzero();
                options.scripted_rate = rate();
            }
            else if (option == L"--hidden") options.hidden = true;
            else if (option == L"--frames") options.frames = nonzero();
            else if (option == L"--shader-cache") (void)argument();
            else if (option == L"--storage") options.storage = argument();
            else if (option == L"--language") options.language = xbox_native::xbox_language(std::filesystem::path(argument()).string());
            else if (option == L"--report") report_path = argument();
            else if (option == L"--frame") options.capture = argument();
            else if (option == L"--frame-index") { options.capture_index = nonzero(); explicit_frame_index = true; }
            else if (option == L"--capture-frame") options.diagnostics.frame = argument();
            else if (option == L"--capture-at") { options.diagnostics.capture_at = nonzero(); capture_at = true; }
            else if (option == L"--performance") options.diagnostics.performance = argument();
            else if (option == L"--performance-at") { options.diagnostics.performance_at = nonzero(); performance_at = true; }
            else if (option == L"--performance-frames") options.diagnostics.performance_frames = nonzero();
            else if (option == L"--start-at-scanout") pulse(options.input.start);
            else if (option == L"--stick-at-scanout") {
                if (options.input.stick.size() == 32) throw std::runtime_error("Specify at most 32 stick inputs");
                options.input.stick.push_back({nonzero(), nonzero(), axis(), axis()});
            } else if (const auto button = std::ranges::find(face_options, option); button != face_options.end()) {
                pulse(options.input.face[button - face_options.begin()]);
            } else if (const auto direction = std::ranges::find(dpad_options, option); direction != dpad_options.end()) {
                pulse(options.input.dpad[direction - dpad_options.begin()]);
            } else throw std::runtime_error("Unknown play option: " + std::filesystem::path(option).string());
        }
        const auto explicit_xbe = options.xbe, explicit_disc = options.disc;
        try { options.load(options.storage / "settings.cfg"); }
        catch (const std::exception& error) {
            if (options.hidden) throw;
            options.settings_error = error.what();
            options.launcher = true;
        }
        if (next > 1) { options.xbe = explicit_xbe; options.disc = explicit_disc; }
        if (set_fullscreen) options.fullscreen = true;
        if (set_mute) options.muted = true;
        if (set_no_vsync) options.vsync = false;
        if (set_wide) options.widescreen = true;
        if (refresh) options.refresh_rate = *refresh;
        if (options.hidden && options.launcher) throw std::runtime_error("The interactive launcher requires a visible window");
        if (explicit_frame_index && options.capture.empty()) throw std::runtime_error("Frame index requires --frame");
        if (capture_at && options.diagnostics.frame.empty()) throw std::runtime_error("--capture-at requires --capture-frame");
        if ((performance_at || options.diagnostics.performance_frames) && options.diagnostics.performance.empty())
            throw std::runtime_error("Performance timing options require --performance");
        for (const auto& directory : {options.diagnostics.frame, options.diagnostics.performance}) {
            if (!directory.empty() && std::filesystem::exists(directory))
                throw std::runtime_error("Diagnostic output must be a new directory");
        }
        if (!options.capture.empty() && (options.capture.extension() != ".png" || std::filesystem::exists(options.capture))) {
            throw std::runtime_error("Frame output must be a new PNG file");
        }
        if (report_path.empty()) {
            const auto started = std::chrono::system_clock::now().time_since_epoch().count();
            report_path = root / std::format("build/logs/playtest-{}.json", started);
            std::filesystem::create_directories(report_path.parent_path());
        }
        std::ofstream report(report_path, std::ios::binary | std::ios::noreplace);
        if (!report) throw std::runtime_error("Play session report must be a new file in an existing directory");
        options.diagnostics.root = (root.empty() ? application_root() : root) / "build/captures";
        options.diagnostics.session_report = std::filesystem::absolute(report_path);
        std::cout << "Session report " << xbox_native::path_text(std::filesystem::absolute(report_path)) << '\n';
        int code;
        try {
            code = xbox_native::play_game(options, report);
        } catch (const std::exception& error) {
            report << "{\"status\":\"startup_error\",\"reason\":" << xbox_native::json(error.what()) << "}\n";
            throw;
        }
        report.flush();
        if (!report) throw std::runtime_error("Cannot write the play session report");
        if (code) report_failure("The game stopped. See the session report for the guest address and reason:\n" +
                         xbox_native::path_text(std::filesystem::absolute(report_path)), console, hidden);
        return code;
    } catch (const std::exception& error) {
        report_failure(error.what(), console, hidden);
        return 1;
    }
}
