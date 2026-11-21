#pragma once
#include <cstdint>

namespace xbox_native {
struct GuestAllocator {
    void* context;
    std::uint32_t (*allocate)(void*, std::uint32_t);
    void (*release)(void*, std::uint32_t);
};
}
