#include "xbe.h"
#include "kernel_names.h"
#include "target.h"

#include <algorithm>
#include <stdexcept>

namespace xbox_native {
Xbe::Xbe(const std::filesystem::path& path) : file_(path) {
    if (file_.size() != target::file_size) throw std::runtime_error("XBE size differs from the registered executable");
    sha256 = xbox_native::sha256(file_, 0, file_.size());
    if (sha256 != target::sha256) throw std::runtime_error("XBE identity differs from the registered executable");

    const auto header = file_.read(0, 0x178);
    if (u32(header, 0) != 0x48454258) throw std::runtime_error("Invalid Xbox executable header");
    base_ = u32(header, 0x104);
    headers_size_ = u32(header, 0x108);
    image_end = base_ + u32(header, 0x10C);
    stack_commit = u32(header, 0x130);
    const auto section_count = u32(header, 0x11C);
    if (!section_count || section_count > 256) throw std::runtime_error("Invalid XBE section count");
    const auto table_address = u32(header, 0x120);
    const auto table = file_.read(file_offset(table_address, section_count * 56), section_count * 56);
    for (std::uint32_t i = 0; i < section_count; ++i) {
        const auto offset = i * 56;
        const Section section{u32(table, offset), u32(table, offset + 4), u32(table, offset + 8),
                              u32(table, offset + 12), u32(table, offset + 16), table_address + offset,
                              u32(table, offset + 28), u32(table, offset + 32)};
        file_.check(section.raw_offset, section.raw_size);
        sections_.push_back(section);
    }

    constexpr std::uint32_t retail_entry_key = 0xA8FC57AB;
    constexpr std::uint32_t retail_thunk_key = 0x5B6D40B6;
    entry = u32(header, 0x128) ^ retail_entry_key;
    const auto kernel_thunks = u32(header, 0x158) ^ retail_thunk_key;
    if (entry != target::entry_point || kernel_thunks != target::kernel_thunks) {
        throw std::runtime_error("Decoded XBE addresses differ from the registered executable");
    }
    code_section(entry);

    constexpr std::uint32_t max_imports = 512;
    for (std::uint32_t i = 0; i <= max_imports; ++i) {
        const auto address = kernel_thunks + i * 4;
        const auto raw = u32(file_.read(file_offset(address, 4), 4), 0);
        if (!raw) return;
        if (i == max_imports) throw std::runtime_error("Unterminated XBE kernel import table");
        const auto ordinal = raw & 0x1FFU;
        if ((raw & ~0x1FFU) != 0x80000000U || !ordinal) {
            throw std::runtime_error("Invalid XBE kernel import at " + hex32(address));
        }
        imports_.push_back({address, ordinal, kernel_name(ordinal)});
    }
}

std::uint64_t Xbe::file_offset(std::uint32_t address, std::size_t count) const {
    if (address >= base_ && std::uint64_t{address} - base_ + count <= headers_size_) {
        return address - base_;
    }
    for (const auto& section : sections_) {
        if (address >= section.address && std::uint64_t{address} - section.address + count <=
                                         std::min(section.size, section.raw_size)) {
            return std::uint64_t{section.raw_offset} + address - section.address;
        }
    }
    throw std::runtime_error("Address is not backed by executable data: " + hex32(address));
}

const KernelImport* Xbe::kernel_import(std::uint32_t address) const {
    const auto found = std::ranges::lower_bound(imports_, address, {}, &KernelImport::address);
    return found != imports_.end() && found->address == address ? &*found : nullptr;
}

std::uint32_t Xbe::data_word(std::uint32_t address) {
    return u32(file_.read(file_offset(address, 4), 4), 0);
}

std::optional<std::uint32_t> Xbe::initial_word(std::uint32_t address) {
    for (const auto& section : sections_) {
        if (address >= section.address && std::uint64_t{address - section.address} + 4 <=
            std::min(section.size, section.raw_size)) {
            return data_word(address);
        }
    }
    return {};
}

const Section& Xbe::code_section(std::uint32_t address) const {
    for (const auto& section : sections_) {
        if (section.executable() && address >= section.address &&
            address - section.address < std::min(section.size, section.raw_size)) return section;
    }
    throw std::runtime_error("Address is not executable code: " + hex32(address));
}

std::size_t Xbe::code_bytes(std::uint32_t address, std::span<std::byte> output) {
    const auto& section = code_section(address);
    const auto available = std::min(section.size, section.raw_size) - (address - section.address);
    const auto count = std::min<std::size_t>(available, output.size());
    file_.read(file_offset(address, count), output.first(count));
    return count;
}

void Xbe::load_preloaded(std::uint32_t guest_base, std::span<std::byte> guest) {
    if (base_ < guest_base || image_end < base_ || image_end - guest_base > guest.size()) {
        throw std::runtime_error("XBE does not fit in guest memory");
    }
    auto image = guest.subspan(base_ - guest_base, image_end - base_);
    std::ranges::fill(image, std::byte{});
    file_.read(0, image.first(headers_size_));
    for (const auto& section : sections_) {
        if (section.flags & 2) {
            load_section(section, image.subspan(section.address - base_, section.size));
        }
    }
}

void Xbe::load_section(const Section& section, std::span<std::byte> destination) {
    std::ranges::fill(destination, std::byte{});
    file_.read(section.raw_offset, destination.first(std::min(section.size, section.raw_size)));
}
}
