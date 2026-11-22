#pragma once
#include "lower.h"

namespace xbox_native {
enum class PointerSource { unknown, stack, argument, constant, relocated, absolute_slot, module_slot,
                           argument_method, module_method, absolute_method, getter_result, returned_method, method_result, cached_lookup };
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
};
struct GetterStore {
    std::uint32_t site{}, slot{}, getter{}, name_offset{}, receiver=UINT32_MAX;
};
struct MethodStore { std::uint32_t allocation{}, site{}, field{}, target{}; };
struct AllocationStore { std::uint32_t site{}, slot{}, allocation{}; };
struct CallSources {
    std::vector<CallSource> calls;
    std::vector<GetterStore> getter_stores;
    std::vector<MethodStore> method_stores;
    std::vector<AllocationStore> allocation_stores;
    std::map<std::uint32_t,std::vector<PointerValue>> alternatives;
    std::map<std::uint32_t,std::uint32_t> nonzero_cells;
};
// Trace original pointer-producing instructions. A disagreement at a control-flow join loses the value.
CallSources trace_call_sources(const Function&, const std::map<std::uint32_t,std::uint32_t>& relocations = {});
std::string_view source_name(PointerSource source);
}
