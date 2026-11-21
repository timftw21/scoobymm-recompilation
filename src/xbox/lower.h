#pragma once

#include "xbe.h"
#include <Zydis/Zydis.h>
#include <array>
#include <map>

namespace xbox_native {

struct Instruction {
    std::uint32_t address{};
    ZydisDecodedInstruction decoded{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    std::array<std::byte, ZYDIS_MAX_INSTRUCTION_LENGTH> bytes{};
    std::string text;
    std::optional<std::uint32_t> kernel_ordinal;
    std::vector<std::uint32_t> jump_targets;
    std::vector<std::uint32_t> external_jump_targets;
    bool cross_function_branch{};
    bool missing_target{};
    bool missing_fallthrough{};
    bool cross_function_fallthrough{};
    bool relocated_displacement{}, relocated_immediate{}, call_next_instruction{};
    bool saves_jump{}, resumes_jump{};
    std::optional<std::uint32_t> target() const;
};

struct Function {
    std::uint32_t entry{};
    std::map<std::uint32_t, Instruction> instructions;
    std::vector<std::uint32_t> loop_headers;
    std::vector<std::uint32_t> alternate_entries;
    bool platform_binding{};
    std::vector<std::uint32_t> platform_alternates;
    bool relocatable{};
};

struct LoweringIssue {
    std::uint32_t address;
    std::string bytes, instruction, reason;
};

struct LoweredFunction {
    std::string source;
    std::vector<LoweringIssue> issues;
    std::map<std::string, std::string> native_helpers;
    bool floating{};
};

LoweredFunction lower(const Function& function);

} // namespace xbox_native
