#pragma once
#include "lower.h"

namespace xbox_native {
enum class PointerSource { unknown, stack, argument, argument_address, constant, unsigned_byte, relocated, absolute_slot, module_slot, object_address,
                           argument_method, module_method, object_field_method, callback_result, absolute_method,
                           getter_result, returned_method, method_result, cached_lookup };
struct PointerValue {
    PointerSource kind{};
    std::uint32_t base{}, offset{}, producer{};
    bool operator==(const PointerValue&) const = default;
};
struct CallSource {
    std::uint32_t site{};
    PointerValue target, first_argument, second_argument, third_argument;
    bool direct{};
    std::uint32_t return_site{}, frame_arguments{};
    PointerValue fourth_argument{};
};
struct GetterStore {
    std::uint32_t site{}, slot{}, getter{}, name_offset{}, receiver=UINT32_MAX;
};
struct MethodStore { std::uint32_t allocation{}, site{}, field{}, target{}; };
struct AllocationStore { std::uint32_t site{}, slot{}, allocation{}; };
struct ObjectStore { std::uint32_t site{}; PointerValue address, value; };
// A store of argument number `argument` to the relocated word `slot`.
struct ArgumentStore { std::uint32_t site{}, slot{}, argument{}; };
struct CallSources {
    std::vector<CallSource> calls;
    std::vector<GetterStore> getter_stores;
    std::vector<MethodStore> method_stores;
    std::vector<AllocationStore> allocation_stores;
    std::vector<ObjectStore> object_stores;
    std::vector<ArgumentStore> argument_stores;
    std::map<std::uint32_t,std::vector<PointerValue>> alternatives;
    // The first three outgoing arguments on each replayed path of an alternative site.
    std::map<std::uint32_t,std::vector<std::array<PointerValue,3>>> alternative_arguments;
    std::map<std::uint32_t,std::uint32_t> nonzero_cells;
};
// Trace original pointer-producing instructions. A disagreement at a control-flow join loses the value.
CallSources trace_call_sources(const Function&, const std::map<std::uint32_t,std::uint32_t>& relocations = {});
std::string_view source_name(PointerSource source);
}
