#pragma once
#include "cpu.h"

namespace xbox_native {
struct GuestLongJump : NativeBoundary {
    explicit GuestLongJump(std::uint32_t address)
        : NativeBoundary(address, "longjmp has no matching active setjmp frame") {}
};

void unwind_guest(Cpu&, Memory&, std::uint32_t frame, std::uint32_t resume,
                  std::uint32_t record, std::uint32_t value);
}
