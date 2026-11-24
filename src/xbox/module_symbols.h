#pragma once
#include "xbox/cpu.h"
#include <optional>
#include <span>
#include <string_view>

namespace xbox_native {
// COFF symbols of the logic objects the player extends, by object-relative offset.
struct ModuleSymbol {
    std::string_view name;
    std::uint32_t offset;
};
struct ModuleSymbols {
    const CompiledModule* module;
    std::span<const ModuleSymbol> symbols;
};
extern const std::span<const ModuleSymbols> compiled_module_symbols;

inline std::span<const ModuleSymbol> module_symbols(const CompiledModule& module) {
    for (const auto& entry : compiled_module_symbols)
        if (entry.module->functions.data() == module.functions.data()) return entry.symbols;
    return {};
}
inline std::optional<std::uint32_t> module_symbol(std::span<const ModuleSymbol> symbols, std::string_view name) {
    for (const auto& symbol : symbols)
        if (symbol.name == name) return symbol.offset;
    return std::nullopt;
}
}
