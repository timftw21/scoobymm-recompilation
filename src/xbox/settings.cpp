#include "settings.h"
#include "disc.h"
#include "xbe.h"

#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>
#include <Windows.h>

namespace xbox_native {
void PlayerSettings::load(const std::filesystem::path& file) {
    if (!std::filesystem::exists(file)) return;
    std::ifstream in(file, std::ios::binary);
    if (!in || std::filesystem::file_size(file) > 256 * 1024)
        throw std::runtime_error("Cannot read settings.cfg");
    auto next = *this;
    std::string line;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) line.pop_back();
        const auto split = line.find('=');
        if (split == line.npos) continue;
        const auto key = line.substr(0, split), value = line.substr(split + 1);
        const auto number = [&](int low, int high) {
            int n{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (error != std::errc{} || end != value.data() + value.size() || n < low || n > high)
                throw std::runtime_error("Invalid settings.cfg value for " + key);
            return n;
        };
        if (key == "xbe" || key == "disc") {
            auto path = std::filesystem::path(std::u8string(value.begin(), value.end()));
            (key == "xbe" ? next.xbe : next.disc) = std::move(path);
        } else if (key == "refresh_rate") next.refresh_rate = number(0, 1000);
        else if (key == "volume") next.volume = number(0, 100);
        else if (key == "show_fps") next.show_fps = number(0, 1) != 0;
        else if (key == "muted") next.muted = number(0, 1) != 0;
        else if (key == "mute_unfocused") next.mute_unfocused = number(0, 1) != 0;
        else if (key == "widescreen") next.widescreen = number(0, 1) != 0;
        else if (key == "fullscreen") next.fullscreen = number(0, 1) != 0;
        else if (key == "vsync") next.vsync = number(0, 1) != 0;
        else if (key == "render_scale") next.enhancements.scale = number(1, 6);
        else if (key == "fxaa") next.enhancements.fxaa = number(0, 1) != 0;
        else if (key == "msaa" || key == "anisotropy") {
            const auto n = number(1, key == "msaa" ? 8 : 16);
            if (n & (n - 1)) throw std::runtime_error("Invalid settings.cfg value for " + key);
            (key == "msaa" ? next.enhancements.samples : next.enhancements.anisotropy) = n;
        }
    }
    if (in.bad()) throw std::runtime_error("Error reading settings.cfg");
    *this = std::move(next);
}

void PlayerSettings::save(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    auto pending = file;
    pending += std::format(".{}.tmp", GetCurrentProcessId());
    try {
        std::ofstream out(pending, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "xbe=" << path_text(xbe) << "\ndisc=" << path_text(disc)
            << "\nrefresh_rate=" << refresh_rate << "\nvolume=" << volume
            << "\nshow_fps=" << show_fps << "\nmuted=" << muted << "\nmute_unfocused=" << mute_unfocused
            << "\nwidescreen=" << widescreen << "\nfullscreen=" << fullscreen << "\nvsync=" << vsync
            << "\nrender_scale=" << enhancements.scale << "\nmsaa=" << enhancements.samples
            << "\nfxaa=" << enhancements.fxaa << "\nanisotropy=" << enhancements.anisotropy << '\n';
        out.close();
        if (!MoveFileExW(pending.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Saving settings.cfg");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        throw;
    }
}

void PlayerSettings::validate_media() const {
    Xbe executable(xbe);
    if (Disc(disc).executable_sha256() != executable.sha256)
        throw std::runtime_error("The disc and the verified default.xbe do not match.");
}
}
