#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace xbox_native {
struct CompiledModule;
inline constexpr std::uint32_t logic_root_argument=UINT32_MAX, logic_getter_result=UINT32_MAX-1, logic_local_function=UINT32_MAX-2;
inline constexpr std::uint32_t logic_unreachable=UINT32_MAX-3;
inline constexpr std::uint32_t logic_passed_argument=UINT32_MAX-4;
struct LogicAbiCall {
    std::uint32_t site, slot, target;
    std::uint32_t source=UINT32_MAX, table{}, method{}, getter{}, named=UINT32_MAX;
    bool operator==(const LogicAbiCall&) const = default;
};
struct LogicInterface {
    std::uint32_t slot, getter, table;
    std::span<const std::uint32_t> methods;
};
struct LogicPassedInterface {
    std::uint32_t return_site, argument, cell;
    const LogicInterface* definition;
};
struct LogicAbiModule {
    const CompiledModule* object;
    std::span<const LogicAbiCall> calls;
    std::span<const LogicPassedInterface> arguments;
};
extern const std::span<const LogicAbiModule> compiled_logic_abi;
struct LogicProvider { const CompiledModule* object; std::uint32_t offset; };
struct LogicNamedMethod { std::string_view name; std::uint32_t field; std::span<const LogicProvider> providers; };
extern const std::span<const LogicNamedMethod> compiled_named_methods;
inline constexpr std::uint32_t logic_engine_api = 0x01ef5718;
// EF_LOGIC_CODE stores the COFF exports at these fields after matching their names.
inline constexpr std::uint32_t logic_register_call = 0x3A6B5, logic_constructor_call = 0x3D42F,
                               logic_destructor_call = 0x3D4EE;
constexpr unsigned logic_export_field(std::uint32_t caller) {
    switch (caller) {
    case logic_register_call: return 0x18;
    case logic_constructor_call: return 0x10;
    case logic_destructor_call: return 0x14;
    default: return 0;
    }
}
}
