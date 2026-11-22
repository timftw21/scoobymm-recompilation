#pragma once
#include <cstdint>

namespace xbox_native {
// Xbox kernel exports include data objects as well as callable entries.
constexpr bool kernel_data_export(std::uint32_t ordinal) {
    switch (ordinal) {
    case 16: case 40: case 71: case 156: case 164: case 322: case 323: case 324: case 326: case 356:
        return true;
    default: return false;
    }
}
constexpr std::uint32_t kernel_call_address(std::uint32_t ordinal) { return 0xfff00000u + ordinal * 16; }
}
