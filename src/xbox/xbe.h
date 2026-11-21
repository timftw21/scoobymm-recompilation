#pragma once
#include "file.h"
#include <optional>

namespace xbox_native {
struct Section {
    std::uint32_t flags, address, size, raw_offset, raw_size;
    std::uint32_t header, head_reference, tail_reference;
    bool executable() const { return (flags & 4) != 0; }
};
struct KernelImport {
    std::uint32_t address, ordinal;
    std::string_view name;
};
class Xbe {
public:
    explicit Xbe(const std::filesystem::path& path);
    std::uint32_t entry = 0;
    std::uint32_t stack_commit = 0;
    std::uint32_t image_end = 0;
    std::string sha256;
    std::uint32_t data_word(std::uint32_t address);
    std::optional<std::uint32_t> initial_word(std::uint32_t address);
    std::size_t code_bytes(std::uint32_t address, std::span<std::byte> output);
    const KernelImport* kernel_import(std::uint32_t address) const;
    std::span<const KernelImport> imports() const { return imports_; }
    std::span<const Section> sections() const { return sections_; }
    void load_preloaded(std::uint32_t guest_base, std::span<std::byte> guest);
    void load_section(const Section& section, std::span<std::byte> destination);
private:
    File file_;
    std::uint32_t base_ = 0, headers_size_ = 0;
    std::vector<Section> sections_;
    std::vector<KernelImport> imports_;
    const Section& code_section(std::uint32_t address) const;
    std::uint64_t file_offset(std::uint32_t address, std::size_t count) const;
};
}
