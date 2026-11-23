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
    std::optional<std::uint32_t> checked_target;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> caller_targets;
    std::vector<std::uint32_t> jump_targets;
    std::vector<std::uint32_t> external_jump_targets;
    // Evidence-bound destinations of an indirect CALL or JMP, sorted.
    std::vector<std::uint32_t> allowed_targets;
    bool reject_transfer{}; // No original value can reach this indirect transfer.
    bool address_taken_bound{}; // Any XBE routine whose address the image contains, or a module routine.
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
std::string lower_vertex_loop(const Function&, std::uint32_t header);

} // namespace xbox_native
