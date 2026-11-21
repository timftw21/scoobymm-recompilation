#include "disc.h"
#include <algorithm>
#include <stdexcept>

namespace xbox_native {
namespace {
constexpr std::uint64_t sector_size = 2048;

bool equal_name(std::string_view first, std::string_view second) {
    const auto upper = [](char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; };
    return std::ranges::equal(first, second, [&](char a, char b) { return upper(a) == upper(b); });
}
}

Disc::Disc(const std::filesystem::path& path) : file_(path) {
    constexpr std::string_view magic = "MICROSOFT*XBOX*MEDIA";
    const auto data = file_.read(32 * sector_size, sector_size);
    const std::string_view volume(reinterpret_cast<const char*>(data.data()), data.size());
    if (!volume.starts_with(magic) || !volume.ends_with(magic)) throw std::runtime_error("Not an Xbox XISO");
    root_ = {u32(data, 0x14), u32(data, 0x18), true};
    file_.check(root_.sector * sector_size, root_.size);
}

std::optional<DiscEntry> Disc::lookup(std::string_view path) {
    auto current = root_;
    while (!path.empty()) {
        const auto separator = path.find('\\');
        const auto name = path.substr(0, separator);
        if (!current.directory || name.empty() || name == "." || name == "..") return {};
        bool found = false;
        visit_directory(current, [&](std::string_view candidate, DiscEntry entry) {
            if (equal_name(name, candidate)) {
                current = entry;
                found = true;
            }
            return !found;
        });
        if (!found) return {};
        if (separator == std::string_view::npos) return current;
        path.remove_prefix(separator + 1);
    }
    return current;
}

void Disc::visit_directory(DiscEntry entry, const std::function<bool(std::string_view, DiscEntry)>& visit) {
    if (!entry.directory || entry.size > 1024 * 1024) throw std::runtime_error("Invalid or oversized XISO directory");
    if (!entry.size) return;
    const auto directory = file_.read(entry.sector * sector_size, entry.size);
    std::vector<std::size_t> pending{0};
    std::vector<bool> seen((directory.size() + 3) / 4);
    while (!pending.empty()) {
        const auto offset = pending.back();
        pending.pop_back();
        if (offset > directory.size() || directory.size() - offset < 14 || seen[offset / 4]) {
            throw std::runtime_error("Invalid XISO directory entry");
        }
        seen[offset / 4] = true;
        const auto length = std::to_integer<unsigned>(directory[offset + 13]);
        if (!length || length > directory.size() - offset - 14) throw std::runtime_error("Invalid XISO filename");
        const std::string_view name(reinterpret_cast<const char*>(directory.data() + offset + 14), length);
        const DiscEntry child{u32(directory, offset + 4), u32(directory, offset + 8),
                              (directory[offset + 12] & std::byte{0x10}) != std::byte{0}};
        file_.check(child.sector * sector_size, child.size);
        if (!visit(name, child)) return;
        for (const auto next : {u16(directory, offset), u16(directory, offset + 2)}) if (next) pending.push_back(next * 4U);
    }
}

void Disc::visit_files(const std::function<void(std::string_view, DiscEntry)>& visit) {
    std::size_t count = 0;
    const auto walk = [&](const auto& self, DiscEntry directory, const std::string& prefix, unsigned depth) -> void {
        if (depth > 32) throw std::runtime_error("XISO directory exceeds depth limit");
        visit_directory(directory, [&](std::string_view name, DiscEntry child) {
            if (++count > 32768 || name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos) {
                throw std::runtime_error("Invalid XISO path or entry count");
            }
            const auto path = prefix + std::string(name);
            if (child.directory) self(self, child, path + '\\', depth + 1);
            else visit(path, child);
            return true;
        });
    };
    walk(walk, root_, {}, 0);
}

void Disc::read(DiscEntry entry, std::uint64_t offset, std::span<std::byte> bytes) {
    if (entry.directory || offset > entry.size || bytes.size() > entry.size - offset) {
        throw std::runtime_error("Read outside XISO file");
    }
    file_.read(entry.sector * sector_size + offset, bytes);
}

std::string Disc::executable_sha256() {
    const auto entry = lookup("default.xbe");
    if (!entry || entry->directory || entry->size > 64 * 1024 * 1024) {
        throw std::runtime_error("XISO has no supported default.xbe file");
    }
    return file_sha256(*entry, 0, entry->size);
}

std::string Disc::file_sha256(DiscEntry entry, std::uint64_t offset, std::uint64_t length) {
    if (entry.directory || offset > entry.size || length > entry.size - offset)
        throw std::runtime_error("Hash outside XISO file");
    return sha256(file_, entry.sector * sector_size + offset, length);
}

std::string Disc::disc_sha256() { return sha256(file_, 0, file_.size()); }
}
