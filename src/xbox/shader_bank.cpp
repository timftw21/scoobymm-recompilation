#include "shaders.h"
#include "upscaler_model.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace xbox_native {
namespace {
constexpr auto kinds = static_cast<unsigned>(ShaderKind::count);
constexpr auto header_size = (kinds + 1) * sizeof(std::uint32_t);

Bytes bank() {
    static const auto bytes = []() -> Bytes {
        const auto resource = FindResourceW(nullptr, L"SCOOBYMM_SHADERS", MAKEINTRESOURCEW(10));
        if (!resource) throw std::runtime_error("Missing embedded shader bank");
        const auto size = SizeofResource(nullptr, resource);
        const auto* data = static_cast<const std::byte*>(LockResource(LoadResource(nullptr, resource)));
        if (!data || size < header_size) throw std::runtime_error("Invalid embedded shader bank");
        return {data, size};
    }();
    return bytes;
}
}

std::span<const StaticVertex> static_vertex_shaders() {
    static const auto materials = [] {
        std::vector<StaticVertex> result;
        const auto bytes = bank();
        std::size_t offset = u32(bytes, kinds * 4);
        if (offset < header_size || offset > bytes.size()) throw std::runtime_error("Invalid material bank offset");
        while (offset < bytes.size()) {
            StaticVertex entry;
            const auto count = u32(bytes, offset), size = u32(bytes, offset + 4);
            offset += 8;
            if (!count || count > entry.program.tokens.size() || count * 16 > bytes.size() - offset)
                throw std::runtime_error("Invalid embedded vertex program");
            std::memcpy(entry.program.tokens.data(), bytes.data() + offset, count * 16);
            offset += count * 16;
            if (size < 32 || size > bytes.size() - offset || u32(bytes, offset) != 0x43425844 || u32(bytes, offset + 24) != size)
                throw std::runtime_error("Invalid embedded material bytecode");
            entry.bytes = bytes.subspan(offset, size);
            offset += size;
            result.push_back(std::move(entry));
        }
        return result;
    }();
    return materials;
}

ShaderBinary shader_binary(ShaderKind kind, const VertexProgram* program) {
    if (kind == ShaderKind::vertex && program) {
        const auto materials = static_vertex_shaders();
        const auto match = std::ranges::lower_bound(materials, *program, {}, &StaticVertex::program);
        if (match != materials.end() && match->program == *program) return {match->bytes};
    }
    const auto index = static_cast<unsigned>(kind);
    const auto bytes = bank();
    if (index >= kinds) throw std::runtime_error("Invalid shader kind");
    const auto first = u32(bytes, index * 4), last = u32(bytes, (index + 1) * 4);
    if (first < header_size || first >= last || last > bytes.size()) throw std::runtime_error("Invalid shader range");
    return {bytes.subspan(first, last - first), false, program != nullptr};
}

// Only offline diagnostics and baking use a writable compiler cache.
void use_shader_cache(std::filesystem::path) {}

Bytes upscaler_checkpoint() {
    const auto resource = FindResourceW(nullptr, L"SCOOBYMM_UPSCALER", MAKEINTRESOURCEW(10));
    if (!resource) throw std::runtime_error("Missing embedded HUD upscaler model");
    const auto* data = static_cast<const std::byte*>(LockResource(LoadResource(nullptr, resource)));
    if (!data) throw std::runtime_error("Invalid embedded HUD upscaler model");
    return {data, SizeofResource(nullptr, resource)};
}
}
