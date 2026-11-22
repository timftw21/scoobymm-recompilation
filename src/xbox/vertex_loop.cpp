#include "lower.h"
#include <algorithm>
#include <format>
#include <sstream>

namespace xbox_native {
namespace {
struct Window {
    std::int64_t first = INT64_MAX, end = INT64_MIN;
};
struct Access {
    ZydisRegister base;
    std::int64_t offset;
    ZydisRegister segment;
};
bool gpr(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.size == 32 &&
           operand.reg.value >= ZYDIS_REGISTER_EAX && operand.reg.value <= ZYDIS_REGISTER_EDI;
}
std::string window_name(ZydisRegister base) {
    return std::string("ram_") + ZydisRegisterGetString(base);
}
}

// A regular transform loop can validate all of one vertex's RAM accesses
// before executing its arithmetic. The ordinary lowering remains available
// for devices, inaccessible pages and unmasked floating-point exceptions.
std::string lower_vertex_loop(const Function& function, std::uint32_t header) {
    std::map<ZydisRegister,std::optional<std::int64_t>> offsets;
    std::map<ZydisRegister,Window> windows;
    std::map<std::uint32_t,Access> accesses;
    std::vector<const Instruction*> body;
    const Instruction* last_fp = nullptr;
    const Instruction* last_fp_memory = nullptr;
    unsigned fp_count = 0;
    bool closed = false;
    auto next = header;
    auto it = function.instructions.find(header);
    for (; it != function.instructions.end() && body.size() < 96; ++it) {
        const auto& [address, op] = *it;
        const auto& ins = op.decoded;
        const auto& a = op.operands[0];
        const auto& b = op.operands[1];
        if (address != next || op.missing_fallthrough || op.cross_function_fallthrough ||
            op.relocated_displacement || op.relocated_immediate ||
            (!function.relocatable && (address == 0x29445 || address == 0x29473)) ||
            (address != header && std::ranges::find(function.loop_headers,address) != function.loop_headers.end()))
            return {};
        next += ins.length;
        if (ins.mnemonic == ZYDIS_MNEMONIC_JNZ && op.target() == header &&
            !op.missing_target && !op.cross_function_branch) { closed = true; break; }
        if (ins.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE))
            return {};
        const bool move = ins.mnemonic == ZYDIS_MNEMONIC_MOV;
        const bool add = ins.mnemonic == ZYDIS_MNEMONIC_ADD, dec = ins.mnemonic == ZYDIS_MNEMONIC_DEC;
        const bool fp = ins.mnemonic == ZYDIS_MNEMONIC_FLD || ins.mnemonic == ZYDIS_MNEMONIC_FSTP ||
                        ins.mnemonic == ZYDIS_MNEMONIC_FADD || ins.mnemonic == ZYDIS_MNEMONIC_FMUL ||
                        ins.mnemonic == ZYDIS_MNEMONIC_FXCH;
        if (move) {
            if (!((gpr(a) && b.type == ZYDIS_OPERAND_TYPE_MEMORY && b.size == 32) ||
                  (gpr(b) && a.type == ZYDIS_OPERAND_TYPE_MEMORY && a.size == 32))) return {};
        } else if (add || dec) {
            if (!gpr(a) || (add && b.type != ZYDIS_OPERAND_TYPE_IMMEDIATE)) return {};
        } else if (!fp) return {};
        if (fp) { last_fp = &op; ++fp_count; }
        for (const auto& operand : std::span(op.operands).first(ins.operand_count_visible)) {
            if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
            const auto& mem = operand.mem;
            if (operand.size != 32 || mem.type != ZYDIS_MEMOP_TYPE_MEM || mem.index != ZYDIS_REGISTER_NONE ||
                (mem.segment != ZYDIS_REGISTER_DS && mem.segment != ZYDIS_REGISTER_SS) ||
                mem.base < ZYDIS_REGISTER_EAX || mem.base > ZYDIS_REGISTER_EDI || ins.address_width != 32)
                return {};
            // These loads invoke the camera binding in the ordinary lowering.
            if (!function.relocatable && (address == 0xA29F4 || address == 0xA29FA || address == 0xA29FD)) return {};
            auto& delta = offsets.try_emplace(mem.base,0).first->second;
            if (!delta) return {};
            const std::int64_t offset = *delta + (mem.disp.has_displacement ? mem.disp.value : 0);
            if (offset < INT32_MIN || offset > INT32_MAX) return {};
            auto& range = windows[mem.base];
            range.first = std::min(range.first,offset);
            range.end = std::max(range.end,offset+4);
            if (range.end-range.first > 4096) return {};
            accesses.emplace(address,Access{mem.base,offset,mem.segment});
            if (fp) last_fp_memory = &op;
        }
        if (move && gpr(a)) offsets[a.reg.value].reset();
        if (add || dec) {
            auto& delta = offsets.try_emplace(a.reg.value,0).first->second;
            if (delta) *delta += dec ? -1LL : static_cast<std::int32_t>(b.imm.value.u);
        }
        body.push_back(&op);
    }
    if (!closed || fp_count < 8 || !last_fp_memory || windows.size() > 4) return {};

    std::ostringstream out;
    out << "#if defined(__clang__) && defined(_M_X64)\n    {\n"
           "    std::uint16_t control; asm volatile(\"fnstcw %0\" : \"=m\"(control));\n"
           "    if ((control & 63) == 63) {\n";
    for (const auto& [base,range] : windows)
        out << std::format("    const auto {0}_address = {1} + 0x{2:08X}U;\n    std::byte* {0};\n",
                           window_name(base),ZydisRegisterGetString(base),static_cast<std::uint32_t>(range.first));
    out << "    const bool mapped = [&] { try {\n";
    for (const auto& [base,range] : windows)
        out << std::format("        {0} = static_cast<std::byte*>(memory.access({0}_address,{1}));\n",
                           window_name(base),range.end-range.first);
    out << "        return true;\n    } catch (const std::runtime_error&) { return false; } }();\n    if (mapped) {\n";
    for (const auto* op : body) {
        const auto& ins = op->decoded;
        const auto& a = op->operands[0];
        const auto& b = op->operands[1];
        const auto access = accesses.find(op->address);
        std::string pointer;
        if (access != accesses.end()) {
            const auto& [base,offset,segment] = access->second;
            pointer = std::format("{} + {}",window_name(base),offset-windows.at(base).first);
        }
        if (ins.mnemonic == ZYDIS_MNEMONIC_MOV) {
            if (gpr(a)) out << std::format("        std::memcpy(&{}, {}, 4);\n",ZydisRegisterGetString(a.reg.value),pointer);
            else out << std::format("        std::memcpy({}, &{}, 4);\n",pointer,ZydisRegisterGetString(b.reg.value));
        } else if (ins.mnemonic == ZYDIS_MNEMONIC_ADD) {
            out << std::format("        flags=add_flags<32>(flags,{0},{1}U); {0}+={1}U;\n",
                               ZydisRegisterGetString(a.reg.value),static_cast<std::uint32_t>(b.imm.value.u));
        } else if (ins.mnemonic == ZYDIS_MNEMONIC_DEC) {
            out << std::format("        flags=(sub_flags<32>(flags,{0},1U)&~carry)|(flags&carry); --{0};\n",
                               ZydisRegisterGetString(a.reg.value));
        } else if (!pointer.empty()) {
            const auto mnemonic = ZydisMnemonicGetString(ins.mnemonic);
            const auto constraint = ins.mnemonic == ZYDIS_MNEMONIC_FSTP ? "\"+m\"" : ": \"m\"";
            out << std::format("        asm volatile(\"{}s %0\" : {}(*reinterpret_cast<std::byte(*)[4]>({})));\n",
                               mnemonic,constraint,pointer);
        } else {
            const auto modrm = 0xC0 | (ins.raw.modrm.reg<<3) | ins.raw.modrm.rm;
            out << std::format("        asm volatile(\".byte 0x{:02X},0x{:02X}\" : : );\n",ins.opcode,modrm);
        }
    }
    const auto& fp = last_fp->decoded;
    const auto opcode = ((fp.opcode & 7U)<<8) | (fp.raw.modrm.mod<<6) | (fp.raw.modrm.reg<<3) | fp.raw.modrm.rm;
    const auto& data = accesses.at(last_fp_memory->address);
    out << std::format("        cpu.floating.instruction={}0x{:08X}U; cpu.floating.opcode={};\n",
                       function.relocatable ? "code_base + " : "",last_fp->address,opcode)
        << std::format("        cpu.floating.data={}_address+{}U; cpu.floating.data_selector=cpu.{}_selector;\n",
                       window_name(data.base),data.offset-windows.at(data.base).first,data.segment == ZYDIS_REGISTER_SS ? "ss" : "ds")
        << "        cpu.floating.code_selector=cpu.cs_selector;\n"
        << std::format("        if (!(flags & zero)) goto i_{:08X};\n        goto i_{:08X};\n",header,next)
        << "    }\n    }\n    }\n#endif\n";
    return out.str();
}
}
