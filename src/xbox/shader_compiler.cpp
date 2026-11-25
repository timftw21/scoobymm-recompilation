#include "shaders.h"
#include "upscaler_model.h"
#include <Windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <map>

namespace xbox_native {
namespace {
std::filesystem::path cache_directory;
constexpr auto max_blob = 2 * 1024 * 1024;
using ShaderOffsets = std::array<std::uint32_t, static_cast<unsigned>(ShaderKind::count) + 1>;

std::filesystem::path cache_file(std::string_view source, const char* profile, bool gui) {
    if (cache_directory.empty()) return {};
    std::string key = gui ? "dxbc-default-v1 " : "dxbc-strict-o3-v1 ";
    key += profile;
    key += '\n';
    key += source;
    std::array<std::byte, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(key.data()),
                   static_cast<ULONG>(key.size()), reinterpret_cast<PUCHAR>(digest.data()), digest.size()) < 0) {
        throw std::runtime_error("Could not identify shader source for the bytecode cache");
    }
    return cache_directory / (hex_bytes(digest) + ".dxbc");
}
}

void use_shader_cache(std::filesystem::path directory) {
    if (!directory.empty()) std::filesystem::create_directories(directory);
    cache_directory = std::move(directory);
}

ShaderBinary shader_binary(ShaderKind kind, const VertexProgram* program) {
    using Microsoft::WRL::ComPtr;
    const bool translated = kind == ShaderKind::vertex || kind == ShaderKind::launch;
    if (program && !translated) throw std::runtime_error("Invalid shader program");
    const auto normalized = program ? normalize_vertex_program(*program) : VertexProgram{};
    const auto key = std::pair{kind, normalized};
    // One thread owns rendering. Keep bytecode backing stable for ID3DBlob views.
    static std::map<std::pair<ShaderKind, VertexProgram>, std::vector<std::byte>> cache;
    if (const auto found = cache.find(key); found != cache.end()) return {found->second, false};
    if (cache.size() >= 1024) throw std::runtime_error("Shader bytecode cache is full");
    const auto source = program ? translate_vertex_program(normalized, kind == ShaderKind::launch).source : shader_source(kind);
    const bool gui = kind == ShaderKind::gui_vertex || kind == ShaderKind::gui_pixel;
    const auto path = cache_file(source, shader_profile(kind), gui);
    if (!path.empty() && std::filesystem::exists(path)) {
        File file(path);
        if (file.size() < 32 || file.size() > max_blob) throw std::runtime_error("Invalid cached shader bytecode size");
        auto data = file.read(0, static_cast<std::size_t>(file.size()));
        if (u32(data, 0) != 0x43425844U || u32(data, 24) != data.size()) throw std::runtime_error("Invalid cached DXBC header");
        return {cache.emplace(key, std::move(data)).first->second, false};
    }
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source.data(), source.size(), "Xbox native shader", nullptr, nullptr, "main",
        shader_profile(kind), gui ? 0 : D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result)) {
        throw std::runtime_error(std::format("Shader {} compilation failed: {}", unsigned(kind),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed"));
    }
    const auto size = code->GetBufferSize();
    if (size > max_blob) throw std::runtime_error("Compiled shader exceeds bytecode limit");
    const auto* begin = static_cast<const std::byte*>(code->GetBufferPointer());
    if (!path.empty()) {
        auto temporary = path;
        temporary += std::format(".{}.tmp", GetCurrentProcessId());
        write_text(temporary, {reinterpret_cast<const char*>(begin), size});
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary);
            if (!std::filesystem::exists(path)) throw std::system_error(error);
        }
    }
    auto [stored, inserted] = cache.emplace(key, std::vector(begin, begin + size));
    return {stored->second, inserted};
}
void bake_shaders(const std::filesystem::path& output, std::span<const VertexProgram> programs) {
    ShaderOffsets offsets{};
    std::vector<std::byte> bank(sizeof(offsets));
    const auto append = [&](Bytes bytes) { bank.insert(bank.end(), bytes.begin(), bytes.end()); };
    for (unsigned i = 0; i < static_cast<unsigned>(ShaderKind::count); ++i) {
        offsets[i] = static_cast<std::uint32_t>(bank.size());
        const auto kind = static_cast<ShaderKind>(i);
        append(shader_binary(kind).bytes);
    }
    offsets.back() = static_cast<std::uint32_t>(bank.size());
    std::memcpy(bank.data(),offsets.data(),sizeof(offsets));
    for (const auto& program : programs) {
        const auto code = shader_binary(ShaderKind::vertex, &program).bytes;
        const auto count = validate_vertex_program(program, false).instructions;
        const std::array header{count, static_cast<std::uint32_t>(code.size())};
        append(std::as_bytes(std::span(header)));
        append(std::as_bytes(std::span(program.tokens).subspan(program.start, count)));
        append(code);
    }
    auto temporary = output;
    temporary += std::format(".{}.tmp",GetCurrentProcessId());
    try {
        write_text(temporary,{reinterpret_cast<const char*>(bank.data()),bank.size()});
        std::filesystem::rename(temporary,output);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary,ignored);
        throw;
    }
}
std::span<const StaticVertex> static_vertex_shaders() { return {}; }
}
namespace xbox_native {
// The tool reads the checkpoint the game embeds from the source tree.
Bytes upscaler_checkpoint() {
    static const auto bytes = [] {
        File file(SCOOBYMM_UPSCALER_MODEL);
        return file.read(0, static_cast<std::size_t>(file.size()));
    }();
    return bytes;
}
}
