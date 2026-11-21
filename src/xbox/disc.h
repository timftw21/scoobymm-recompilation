#pragma once
#include "file.h"
#include <functional>
#include <optional>

namespace xbox_native {
struct DiscEntry {
    std::uint32_t sector, size;
    bool directory;
};

class Disc {
public:
    explicit Disc(const std::filesystem::path& path);
    std::optional<DiscEntry> lookup(std::string_view name);
    void visit_directory(DiscEntry, const std::function<bool(std::string_view, DiscEntry)>&);
    void visit_files(const std::function<void(std::string_view, DiscEntry)>& visit);
    void read(DiscEntry entry, std::uint64_t offset, std::span<std::byte> bytes);
    std::string file_sha256(DiscEntry entry, std::uint64_t offset, std::uint64_t length);
    std::string executable_sha256();
    std::string disc_sha256();
    std::uint64_t size() const { return file_.size(); }
private:
    File file_;
    DiscEntry root_;
};
}
