#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace xbox_native {
struct CompiledModule;
inline constexpr std::uint32_t logic_root_argument=UINT32_MAX, logic_getter_result=UINT32_MAX-1, logic_local_function=UINT32_MAX-2;
inline constexpr std::uint32_t logic_unreachable=UINT32_MAX-3;
inline constexpr std::uint32_t logic_passed_argument=UINT32_MAX-4;
// An indexed local binding uses slot for the array index, method for the x86
// index register, and getter for the module cell holding the relocated array.
// Both table and target are offsets in the originating module.
inline constexpr std::uint32_t logic_indexed_local=UINT32_MAX-5;
inline constexpr std::uint32_t logic_unreferenced=UINT32_MAX-6;
// Evidence-bound transfers. Their target sets come from every original
// instruction that installs a value at the loaded location. The player
// rejects any other transfer at the originating CALL.
// An entry contract uses slot for the argument and method for the field. The
// caller's saved return PC must name an original call that passes a verified
// interface in that argument. A nonzero target must also match.
inline constexpr std::uint32_t logic_entry_contract=UINT32_MAX-7;
// Target is a routine of the same object stored at the loaded field.
inline constexpr std::uint32_t logic_field_function=UINT32_MAX-8;
// No original instruction stores a nonzero value at the loaded field.
inline constexpr std::uint32_t logic_unset_field=UINT32_MAX-9;
// The engine interface saved in an object field: target is engine method slot.
inline constexpr std::uint32_t logic_saved_interface=UINT32_MAX-10;
// A routine this object installs at callback index slot with engine +0x00.
inline constexpr std::uint32_t logic_installed_callback=UINT32_MAX-11;
// A particle descriptor callback stored by a producer object.
inline constexpr std::uint32_t logic_descriptor_callback=UINT32_MAX-12;
// A routine passed to a provider method that stores it in the loaded cell.
// Table indexes compiled_registered_callbacks.
inline constexpr std::uint32_t logic_registered_callback=UINT32_MAX-13;
inline constexpr std::uint32_t logic_particle_interface_slot=0x40;
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
extern const std::span<const LogicProvider> compiled_descriptor_callbacks;
extern const std::span<const std::span<const LogicProvider>> compiled_registered_callbacks;
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
