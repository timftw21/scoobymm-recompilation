#include "lower.h"
#include <algorithm>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>

namespace xbox_native {
namespace {
constexpr const char* registers[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
struct RegisterPart { unsigned index, bits, shift; };
RegisterPart reg(ZydisRegister value) {
    constexpr ZydisRegister full[] = {ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX, ZYDIS_REGISTER_EBX,
                                      ZYDIS_REGISTER_ESP, ZYDIS_REGISTER_EBP, ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI};
    constexpr ZydisRegister word[] = {ZYDIS_REGISTER_AX, ZYDIS_REGISTER_CX, ZYDIS_REGISTER_DX, ZYDIS_REGISTER_BX,
                                      ZYDIS_REGISTER_SP, ZYDIS_REGISTER_BP, ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI};
    constexpr ZydisRegister low[] = {ZYDIS_REGISTER_AL, ZYDIS_REGISTER_CL, ZYDIS_REGISTER_DL, ZYDIS_REGISTER_BL};
    constexpr ZydisRegister high[] = {ZYDIS_REGISTER_AH, ZYDIS_REGISTER_CH, ZYDIS_REGISTER_DH, ZYDIS_REGISTER_BH};
    for (unsigned i = 0; i < 8; ++i) {
        if (value == full[i]) return {i, 32, 0};
        if (value == word[i]) return {i, 16, 0};
        if (i < 4 && value == low[i]) return {i, 8, 0};
        if (i < 4 && value == high[i]) return {i, 8, 8};
    }
    throw std::runtime_error("Unsupported CPU register");
}
ZydisDecodedOperand register_operand(ZydisRegister value) {
    ZydisDecodedOperand operand{};
    operand.type = ZYDIS_OPERAND_TYPE_REGISTER;
    operand.reg.value = value;
    return operand;
}
std::string constant(std::uint32_t value) { return std::format("0x{:08X}U", value); }
std::string label(std::uint32_t address) { return std::format("i_{:08X}", address); }
// Reject a destination outside an evidence-bound set at the originating instruction.
std::string allowed_check(const Instruction& instruction, std::string_view at) {
    if (instruction.reject_transfer)
        return std::format("    throw xbox_native::NativeBoundary({}, \"indirect transfer has no original destination\");\n", at);
    if (instruction.address_taken_bound)
        return std::format("    if (!xbox_native::address_taken_target(target)) throw xbox_native::NativeBoundary({}, "
                           "std::format(\"indirect transfer to 0x{{:08X}}, which the image never names\", target));\n", at);
    if (instruction.allowed_targets.empty()) return {};
    std::string list;
    for (const auto target : instruction.allowed_targets) list += constant(target) + ",";
    return std::format("    {{ static constexpr std::uint32_t allowed[]{{{}}};\n"
                       "      if (!std::ranges::binary_search(allowed, target)) throw xbox_native::NativeBoundary({}, "
                       "std::format(\"indirect transfer to 0x{{:08X}} outside its evidence-bound targets\", target)); }}\n", list, at);
}
std::string jump_stack(std::uint32_t address) { return std::format("setjmp_esp_{:08X}", address); }
std::string type(unsigned bits) {
    if (bits != 8 && bits != 16 && bits != 32) throw std::runtime_error("Unsupported operand width");
    return std::format("std::uint{}_t", bits);
}

class Emitter {
public:
    std::set<unsigned> used, dirty;
    bool uses_flags = false, writes_flags = false;
    bool calls = false;
    bool restores = false;
    bool returns = false;
    bool floating = false;
    bool divide_faults = false;
    std::set<std::uint32_t> loop_headers;
    const Function* function = nullptr;
    std::set<std::uint32_t> jump_continuations;
    std::map<std::string,std::string> native_helpers;
    std::map<std::string,std::string> native_declarations;
    std::ostringstream body;
    bool relocatable = false;
    const Instruction* active = nullptr;

    std::string pc(std::uint32_t offset) const {
        return relocatable ? "(code_base + " + constant(offset) + ")" : constant(offset);
    }
    std::string relocated_word(unsigned byte_offset) const {
        return "memory.load<std::uint32_t>(" + pc(active->address + byte_offset) + ")";
    }

    std::string read_reg(ZydisRegister value) {
        const auto part = reg(value);
        used.insert(part.index);
        std::string result = registers[part.index];
        if (part.shift) result = "(" + result + " >> 8)";
        if (part.bits < 32) result = "(" + result + " & " + constant((1U << part.bits) - 1) + ')';
        return result;
    }
    std::string address(const ZydisDecodedOperand& operand, bool lea = false) {
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) throw std::runtime_error("Expected a memory operand");
        if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM && !(lea && operand.mem.type == ZYDIS_MEMOP_TYPE_AGEN))
            throw std::runtime_error("Special memory operand is unsupported");
        std::string value = constant(operand.mem.disp.has_displacement ? static_cast<std::uint32_t>(operand.mem.disp.value) : 0);
        if (active->relocated_displacement) value = relocated_word(active->decoded.raw.disp.offset);
        if (operand.mem.base != ZYDIS_REGISTER_NONE) value += " + " + read_reg(operand.mem.base);
        if (operand.mem.index != ZYDIS_REGISTER_NONE)
            value += " + " + read_reg(operand.mem.index) + " * " + std::to_string(operand.mem.scale) + 'U';
        if (!lea && operand.mem.segment == ZYDIS_REGISTER_FS) value += " + cpu.fs_base";
        if (!lea && operand.mem.segment == ZYDIS_REGISTER_GS) value += " + cpu.gs_base";
        return "std::uint32_t(" + value + ')';
    }
    std::string read(const ZydisDecodedOperand& operand) {
        switch (operand.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: return read_reg(operand.reg.value);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            if (active->relocated_immediate) return relocated_word(active->decoded.raw.imm[0].offset);
            return constant(operand.imm.is_signed ? static_cast<std::uint32_t>(operand.imm.value.s)
                                                  : static_cast<std::uint32_t>(operand.imm.value.u));
        case ZYDIS_OPERAND_TYPE_MEMORY: return "memory.load<" + type(operand.size) + ">(" + address(operand) + ')';
        default: throw std::runtime_error("Unsupported operand type");
        }
    }
    void write(const ZydisDecodedOperand& operand, const std::string& value) {
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            body << "    memory.store<" << type(operand.size) << ">(" << address(operand)
                 << ", static_cast<" << type(operand.size) << ">(" << value << "));\n";
        } else if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto part = reg(operand.reg.value);
            used.insert(part.index); dirty.insert(part.index);
            body << "    " << registers[part.index] << " = ";
            if (part.bits == 32) body << value;
            else {
                const auto field = ((1U << part.bits) - 1) << part.shift;
                body << '(' << registers[part.index] << " & " << constant(~field) << ") | ((("
                     << value << ") << " << part.shift << ") & " << constant(field) << ')';
            }
            body << ";\n";
        } else throw std::runtime_error("Unsupported destination operand");
    }
    std::string condition(ZydisMnemonic mnemonic) {
        std::string_view code = ZydisMnemonicGetString(mnemonic);
        if (code.starts_with("cmov")) code.remove_prefix(4);
        else if (code.starts_with("set")) code.remove_prefix(3);
        else if (code.starts_with('j')) code.remove_prefix(1);
        if (code == "ecxz") { used.insert(1); return "ecx == 0"; }
        uses_flags = true;
        if (code == "z") return "(flags & xbox_native::zero) != 0";
        if (code == "nz") return "(flags & xbox_native::zero) == 0";
        if (code == "b") return "(flags & xbox_native::carry) != 0";
        if (code == "nb") return "(flags & xbox_native::carry) == 0";
        if (code == "be") return "(flags & (xbox_native::carry | xbox_native::zero)) != 0";
        if (code == "nbe") return "(flags & (xbox_native::carry | xbox_native::zero)) == 0";
        if (code == "l") return "((flags >> 7) ^ (flags >> 11)) & 1U";
        if (code == "nl") return "(((flags >> 7) ^ (flags >> 11)) & 1U) == 0";
        if (code == "le") return "(flags & xbox_native::zero) || (((flags >> 7) ^ (flags >> 11)) & 1U)";
        if (code == "nle") return "!(flags & xbox_native::zero) && !(((flags >> 7) ^ (flags >> 11)) & 1U)";
        for (const auto& [name, flag] : {std::pair{"o", "overflow"}, {"s", "sign"}, {"p", "parity"}}) {
            if (code == name) return std::string("(flags & xbox_native::") + flag + ") != 0";
            if (code.starts_with('n') && code.substr(1) == name) return std::string("(flags & xbox_native::") + flag + ") == 0";
        }
        throw std::runtime_error("Unsupported condition code");
    }
    void flags(std::string_view operation, unsigned bits) {
        type(bits);
        uses_flags = writes_flags = true;
        body << "    flags = xbox_native::" << operation << '<' << bits << ">(flags, a";
        if (operation != "logic_flags") body << ", b";
        body << ");\n";
    }
    unsigned xmm_register(ZydisRegister value) {
        if (value < ZYDIS_REGISTER_XMM0 || value > ZYDIS_REGISTER_XMM7)
            throw std::runtime_error("Expected an Xbox XMM register");
        return value-ZYDIS_REGISTER_XMM0;
    }
    std::string vector(const ZydisDecodedOperand& operand, bool scalar = false) {
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER)
            return "xbox_native::xmm(cpu.floating,"+std::to_string(xmm_register(operand.reg.value))+')';
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) throw std::runtime_error("Expected SIMD register or memory");
        if (scalar) return "xbox_native::scalar_vector(memory.load<std::uint32_t>("+address(operand)+"))";
        return "xbox_native::load_vector(memory,"+address(operand)+')';
    }
    void vector_write(const ZydisDecodedOperand& operand, const std::string& value, bool scalar = false) {
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER)
            body << "    xbox_native::set_xmm(cpu.floating," << xmm_register(operand.reg.value) << ',' << value << ");\n";
        else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY)
            body << "    xbox_native::store_vector(memory," << address(operand) << ',' << value << ',' << (scalar?"true":"false") << ");\n";
        else throw std::runtime_error("Unsupported SIMD destination");
    }
    void mmx(const Instruction& instruction) {
        const auto& ins=instruction.decoded;
        if (ins.mnemonic == ZYDIS_MNEMONIC_PEXTRW || ins.mnemonic == ZYDIS_MNEMONIC_PMOVMSKB) {
            const auto& source = instruction.operands[1];
            if (source.type != ZYDIS_OPERAND_TYPE_REGISTER || source.reg.value < ZYDIS_REGISTER_MM0 ||
                source.reg.value > ZYDIS_REGISTER_MM7) throw std::runtime_error("Expected an MMX source register");
            const bool extract = ins.mnemonic == ZYDIS_MNEMONIC_PEXTRW;
            const auto immediate = extract ? instruction.operands[2].imm.value.u : 0;
            const auto modrm = 0xC0U | (source.reg.value - ZYDIS_REGISTER_MM0);
            const auto helper = std::format("xbox_mmx_result_{:02X}_{:02X}_{:02X}", ins.opcode, modrm, immediate);
            native_helpers[helper] = helper + " PROC\n    db 00Fh," +
                std::format("0{:02X}h,0{:02X}h", ins.opcode, modrm) +
                (extract ? std::format(",0{:02X}h", immediate) : "") + "\n    ret\n" + helper + " ENDP\n";
            native_declarations[helper] = "extern \"C\" std::uint32_t " + helper + "();\n";
            floating = true;
            write(instruction.operands[0], helper + "()");
            return;
        }
        const ZydisDecodedOperand* memory_operand=nullptr;
        const ZydisDecodedOperand* immediate=nullptr;
        const ZydisDecodedOperand* register_operand=nullptr;
        std::string pointer;
        for(unsigned i=0;i<ins.operand_count_visible;++i) {
            const auto& operand=instruction.operands[i];
            if(operand.type==ZYDIS_OPERAND_TYPE_MEMORY) memory_operand=&operand;
            else if(operand.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.size==8) immediate=&operand;
            else if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER && ins.mnemonic==ZYDIS_MNEMONIC_MOVD &&
                operand.reg.value>=ZYDIS_REGISTER_EAX && operand.reg.value<=ZYDIS_REGISTER_EDI) register_operand=&operand;
            else if(i==1 && operand.type==ZYDIS_OPERAND_TYPE_REGISTER && ins.mnemonic==ZYDIS_MNEMONIC_CVTPS2PI)
                pointer="cpu.floating.xmm["+std::to_string(xmm_register(operand.reg.value))+"].data()";
            else if(operand.type!=ZYDIS_OPERAND_TYPE_REGISTER || operand.reg.value<ZYDIS_REGISTER_MM0 || operand.reg.value>ZYDIS_REGISTER_MM7)
                throw std::runtime_error("Expected an MMX register or memory operand");
        }
        const auto width=(ins.mnemonic==ZYDIS_MNEMONIC_MOVD || ins.mnemonic==ZYDIS_MNEMONIC_PUNPCKLBW ||
            ins.mnemonic==ZYDIS_MNEMONIC_PUNPCKLWD || ins.mnemonic==ZYDIS_MNEMONIC_PUNPCKLDQ)?4U:8U;
        if(memory_operand) {
            if(memory_operand->size!=width*8) throw std::runtime_error("Unexpected MMX memory width");
            pointer="memory.access("+address(*memory_operand)+','+std::to_string(width)+')';
        }
        const bool register_write=register_operand==&instruction.operands[0];
        if(register_operand) {
            body << "    std::uint32_t mmx_word=" << (register_write?"0U":read(*register_operand)) << ";\n";
            pointer="&mmx_word";
        }
        floating=true;
        // Guest XMM/GPR operands live in C++ state; present their original
        // bits as memory while MMX continues to share the resident x87 state.
        const unsigned modrm=!pointer.empty()?(ins.raw.modrm.reg<<3)|1:(3U<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm;
        const bool empty=ins.mnemonic==ZYDIS_MNEMONIC_EMMS;
        const auto helper=(empty?std::string("xbox_mmx_77"):std::format("xbox_mmx_{:02X}{:02X}",ins.opcode,modrm))+
            (immediate?std::format("_{:02X}",immediate->imm.value.u):"");
        native_helpers[helper]=helper+" PROC\n    db 00Fh,"+std::format("0{:02X}h",ins.opcode)+
            (empty?"":std::format(",0{:02X}h",modrm))+(immediate?std::format(",0{:02X}h",immediate->imm.value.u):"")+"\n    ret\n"+helper+" ENDP\n";
        native_declarations[helper]="extern \"C\" void "+helper+'('+(!pointer.empty()?"void*":"")+");\n";
        body << "    " << helper << '(' << pointer << ");\n";
        if(register_write) write(*register_operand,"mmx_word");
    }
    void convert_packed_integer_to_float(const Instruction& instruction) {
        const auto& dst = instruction.operands[0];
        const auto& src = instruction.operands[1];
        const bool memory = src.type == ZYDIS_OPERAND_TYPE_MEMORY;
        if ((!memory && (src.type != ZYDIS_OPERAND_TYPE_REGISTER || src.reg.value < ZYDIS_REGISTER_MM0 ||
             src.reg.value > ZYDIS_REGISTER_MM7)) || src.size != 64) {
            throw std::runtime_error("Unsupported packed conversion operand");
        }
        const auto modrm = memory ? 0x02 : 0xc0 + src.reg.value - ZYDIS_REGISTER_MM0;
        const auto helper = std::format("xbox_cvtpi2ps_{:02X}", modrm);
        native_helpers[helper] = helper + " PROC\n    movups xmm0,xmmword ptr [rcx]\n" +
            std::format("    db 00Fh,02Ah,0{:02X}h\n", modrm) +
            "    movups xmmword ptr [rcx],xmm0\n    ret\n" + helper + " ENDP\n";
        native_declarations[helper] = "extern \"C\" void " + helper + (memory ? "(void*,void*);\n" : "(void*);\n");
        body << "    " << helper << "(cpu.floating.xmm[" << xmm_register(dst.reg.value) << "].data()";
        if (memory) body << ",memory.access(" << address(src) << ",8)";
        body << ");\n";
        floating = true;
    }
    void x87(const Instruction& instruction) {
        const auto& ins=instruction.decoded;
        const std::string_view mnemonic=ZydisMnemonicGetString(ins.mnemonic);
        if (mnemonic.starts_with("fcmov")) throw std::runtime_error("Floating conditional move needs guest flags");
        floating=true;
        const bool waiting=mnemonic=="fwait";
        const bool compare_flags=mnemonic.starts_with("fcomi") || mnemonic.starts_with("fucomi");
        const bool status=mnemonic=="fnstsw" && ins.raw.modrm.mod==3;
        const ZydisDecodedOperand* memory=nullptr;
        for (unsigned i=0;i<ins.operand_count_visible;++i)
            if (instruction.operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY) memory=&instruction.operands[i];
        const bool environment=mnemonic=="fnsave" || mnemonic=="frstor" || mnemonic=="fnstenv" || mnemonic=="fldenv";
        if (environment && ins.operand_width!=32) throw std::runtime_error("Only 32-bit x87 environments are supported");
        const unsigned modrm=memory ? (ins.raw.modrm.reg<<3)|1 : (3U<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm;
        const auto helper=waiting?std::string("xbox_x87_9B"):std::format("xbox_x87_{:02X}{:02X}",ins.opcode,modrm);
        std::string assembly=helper+" PROC\n    db "+std::format("0{:02X}h",ins.opcode);
        if (!waiting) assembly+=std::format(",0{:02X}h",modrm);
        assembly+='\n';
        if (compare_flags) assembly+="    pushfq\n    pop rax\n";
        if (status) assembly+="    movzx eax,ax\n";
        assembly+="    ret\n"+helper+" ENDP\n";
        native_helpers[helper]=assembly;
        native_declarations[helper]="extern \"C\" "+std::string(status||compare_flags?"std::uint32_t ":"void ")+helper+'('+(memory?"void*":"")+");\n";
        const bool control=waiting || mnemonic=="fnstsw" || mnemonic=="fnstcw" || mnemonic=="fldcw" ||
            mnemonic=="fnsave" || mnemonic=="frstor" || mnemonic=="fnstenv" || mnemonic=="fldenv" ||
            mnemonic=="fnclex" || mnemonic=="fninit" || mnemonic=="fnop";
        if (memory) {
            if (!memory->size || memory->size%8) throw std::runtime_error("Unsupported x87 memory width");
            body << "    const auto fp_address=" << address(*memory) << ";\n";
            // The RenderWare perspective frustum builder uses these three
            // horizontal extents when deciding which objects enter the view.
            if (!relocatable && (instruction.address == 0xA29F4 || instruction.address == 0xA29FA ||
                    instruction.address == 0xA29FD) && memory->size == 32 && memory->mem.base == ZYDIS_REGISTER_ESI &&
                    memory->mem.index == ZYDIS_REGISTER_NONE && memory->mem.disp.value == 0x68)
                body << "    auto fp_extent=xbox_native::camera_extent(cpu,memory,esi,0x68U); auto* fp_memory=&fp_extent;\n";
            else body << "    auto* fp_memory=memory.access(fp_address," << memory->size/8 << ");\n";
        }
        if (!control) {
            body << "    cpu.floating.instruction=" << pc(instruction.address) << ";\n"
                 << "    cpu.floating.opcode=" << (((ins.opcode&7U)<<8)|((3U<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm)) << "U;\n"
                 << "    cpu.floating.code_selector=cpu.cs_selector;\n";
            if (memory) {
                body << "    cpu.floating.data=fp_address;\n    cpu.floating.data_selector=cpu."
                    << (memory->mem.segment==ZYDIS_REGISTER_SS?"ss":memory->mem.segment==ZYDIS_REGISTER_FS?"fs":memory->mem.segment==ZYDIS_REGISTER_GS?"gs":"ds") << "_selector;\n";
                // Record the original ModRM, not the RCX addressing used by the host helper.
                body << "    cpu.floating.opcode=" << (((ins.opcode&7U)<<8)|((ins.raw.modrm.mod<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm)) << "U;\n";
            }
        }
        if (status || compare_flags) body << "    const auto fp_value=";
        else body << "    ";
        body << helper << '(' << (memory?"fp_memory":"") << ");\n";
        if (status) write(instruction.operands[0],"fp_value");
        if (compare_flags) {
            uses_flags=writes_flags=true;
            body << "    flags=(flags&~xbox_native::arithmetic_flags)|(fp_value&(xbox_native::carry|xbox_native::parity|xbox_native::zero));\n";
        }
        if (mnemonic=="fnsave" || mnemonic=="fnstenv") body << "    cpu.floating.environment(fp_memory);\n";
        if (mnemonic=="frstor" || mnemonic=="fldenv") body << "    cpu.floating.restore_environment(fp_memory);\n";
        if (mnemonic=="fnsave" || mnemonic=="fninit")
            body << "    cpu.floating.instruction=cpu.floating.data=0;\n    cpu.floating.opcode=cpu.floating.code_selector=cpu.floating.data_selector=0;\n";
    }
    void branch(const Instruction& instruction) {
        const auto target = instruction.target().value();
        if (!instruction.cross_function_branch && !instruction.missing_target) {
            body << "goto " << label(target) << ";";
            return;
        }
        calls = true;
        body << "commit(); cpu.eip = " << pc(target) << "; ";
        if (instruction.missing_target) {
            body << "throw xbox_native::NativeBoundary(cpu.eip, \"unmapped direct branch\");";
            return;
        }
        body << std::format("guest_{:08X}(cpu,memory); return;", target);
    }

    void conditional_branch(const Instruction& instruction) {
        body << "    if (" << condition(instruction.decoded.mnemonic) << ") { ";
        branch(instruction);
        body << " }\n";
    }
    void instruction(const Instruction& instruction) {
        active = &instruction;
        const auto& ins = instruction.decoded;
        const auto& operands = instruction.operands;
        const auto next = instruction.address + ins.length;
        const auto repeats = ins.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
        const bool string_move = ins.meta.category == ZYDIS_CATEGORY_STRINGOP &&
            (ins.mnemonic == ZYDIS_MNEMONIC_MOVSB || ins.mnemonic == ZYDIS_MNEMONIC_MOVSW || ins.mnemonic == ZYDIS_MNEMONIC_MOVSD);
        const bool string_fill = ins.mnemonic == ZYDIS_MNEMONIC_STOSB || ins.mnemonic == ZYDIS_MNEMONIC_STOSW || ins.mnemonic == ZYDIS_MNEMONIC_STOSD;
        const bool string_compare=ins.meta.category==ZYDIS_CATEGORY_STRINGOP &&
            (ins.mnemonic==ZYDIS_MNEMONIC_CMPSB || ins.mnemonic==ZYDIS_MNEMONIC_CMPSW || ins.mnemonic==ZYDIS_MNEMONIC_CMPSD);
        const bool string_scan=ins.mnemonic==ZYDIS_MNEMONIC_SCASB || ins.mnemonic==ZYDIS_MNEMONIC_SCASW || ins.mnemonic==ZYDIS_MNEMONIC_SCASD;
        const bool locked=ins.attributes&ZYDIS_ATTRIB_HAS_LOCK;
        if (ins.address_width != 32 || (locked && ins.mnemonic!=ZYDIS_MNEMONIC_XCHG && ins.mnemonic!=ZYDIS_MNEMONIC_XADD && ins.mnemonic!=ZYDIS_MNEMONIC_CMPXCHG) ||
            (repeats && !string_compare && !string_scan && !string_move && !string_fill) ||
            ((string_compare || string_scan || string_move || string_fill) &&
             (ins.attributes & (ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS))))
            throw std::runtime_error("Instruction prefixes need an implementation");
        body << label(instruction.address) << ": { // " << instruction.text << '\n';
        // The panel modules call the retail HUD naming stub through HUD API slot 0x28.
        // The exception cleanup thunk at 0xCC203 also jumps here without HUD arguments.
        if (!relocatable && instruction.address == 0x48300)
            body << "    if (cpu.kernel && cpu.kernel->name_widget) {\n"
                 << "        const auto sp = cpu.registers[xbox_native::esp], ret = memory.load<std::uint32_t>(sp);\n"
                 << "        if (ret >= 0x10005U && ret < 0x04000000U && memory.load<std::uint16_t>(ret-2) == 0xD0FF && memory.load<std::uint8_t>(ret-3) == 0x28)\n"
                 << "            cpu.kernel->name_widget(cpu.kernel->context,memory.load<std::uint32_t>(sp+4),memory.load<std::uint32_t>(sp+8));\n    }\n";
        // 0x770CA is past visibility and rectangle clipping, before this widget's draws.
        if (!relocatable && (instruction.address == 0x770CA || instruction.address == 0x4A733))
            body << "    if (cpu.kernel && cpu.kernel->draw_widget) cpu.kernel->draw_widget(cpu.kernel->context,"
                 << (instruction.address == 0x770CA ? "edi" : "0") << ");\n";

        // The supported title drains two read buffers through three DirectSound slots.
        // Its EOF path can see an idle voice while a completed read still awaits submission.
        if (!relocatable && instruction.address == 0x29445)
            body << "    if (eax == 1 && memory.load<std::uint8_t>(esi + 0x2F9)) goto i_00029456;\n";
        if (!relocatable && instruction.address == 0x29473) {
            // Re-read the current descriptor for every slot, including immediately after a rotation.
            // The original consumer initializes +AC to capacity, before the first file read completes.
            body << R"(    const auto bank = memory.load<std::uint32_t>(esi + 0x9C);
    if (!memory.load<std::uint8_t>(esi + 0x2F9) || !memory.load<std::uint8_t>(esi + 0x38 + bank))
        goto i_00029490;
    const auto descriptor = esi + 8 + 24 * bank;
    const auto length = memory.load<std::uint32_t>(descriptor + 4);
    const auto consumed = memory.load<std::uint32_t>(esi + 0xA8) - memory.load<std::uint32_t>(descriptor);
    if (consumed > length) throw NativeBoundary(0x29473, "Stream cursor exceeds the completed read");
    const bool final_bank = memory.load<std::uint32_t>(esi + 0xD8) == 6 &&
                            memory.load<std::uint32_t>(esi + 0xA0) == bank;
    memory.store<std::uint8_t>(esi + 0x2FA, final_bank);
    memory.store(esi + 0xAC, length - consumed);
    if (consumed == length && final_bank) {
        memory.store<std::uint8_t>(esi + 0x38 + bank, 0);
        memory.store<std::uint8_t>(esi + 0x2F9, 0);
        goto i_00029490;
    }
)";
        }
        const auto visible = std::span(operands).first(ins.operand_count_visible);
        if (ins.mnemonic != ZYDIS_MNEMONIC_LEA && std::ranges::any_of(visible, [](const auto& operand) {
                return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_NONE &&
                       operand.mem.index == ZYDIS_REGISTER_NONE && operand.mem.segment == ZYDIS_REGISTER_DS &&
                       operand.mem.disp.has_displacement && static_cast<std::uint32_t>(operand.mem.disp.value) >= 0xF0000000U;
            })) {
            calls = true;
            body << "    if (cpu.diagnostic) { commit(); cpu.eip = " << pc(instruction.address) << "; }\n";
        }
        if(loop_headers.contains(instruction.address)) {
            body << "    if(xbox_native::checkpoint_due(cpu)) { commit(); xbox_native::checkpoint(cpu," << pc(instruction.address) << "); }\n";
            body << lower_vertex_loop(*function,instruction.address);
        }
        bool terminator = false;
        const bool packed = std::ranges::any_of(visible, [](const auto& operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.reg.value >= ZYDIS_REGISTER_MM0 &&
                operand.reg.value <= ZYDIS_REGISTER_MM7;
        });
        if (ins.mnemonic == ZYDIS_MNEMONIC_CVTPI2PS) {
            convert_packed_integer_to_float(instruction);
        } else if (packed || ins.mnemonic == ZYDIS_MNEMONIC_EMMS) {
            if (ins.opcode_map != ZYDIS_OPCODE_MAP_0F) throw std::runtime_error("Unsupported packed opcode map");
            mmx(instruction);
        } else switch (ins.mnemonic) {
        case ZYDIS_MNEMONIC_SAHF:
            uses_flags=writes_flags=true;
            body << "    flags=(flags&~0xD5U)|((" << read_reg(ZYDIS_REGISTER_EAX) << ">>8)&0xD5U);\n";
            break;
        case ZYDIS_MNEMONIC_LAHF:
            uses_flags=true;
            write(register_operand(ZYDIS_REGISTER_AH),"(flags&0xD5U)|2U");
            break;
        case ZYDIS_MNEMONIC_XCHG: {
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY || operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY) {
                const auto& memory_operand=operands[operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY?0:1];
                const auto& register_operand=operands[operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY?1:0];
                body << "    const auto location=" << address(memory_operand) << ";\n"
                    << "    const auto value=memory.exchange<" << type(memory_operand.size) << ">(location,static_cast<"
                    << type(memory_operand.size) << ">(" << read(register_operand) << "));\n";
                write(register_operand,"value");
            } else {
                body << "    const auto a=" << read(operands[0]) << ";\n    const auto b=" << read(operands[1]) << ";\n";
                write(operands[0],"b"); write(operands[1],"a");
            }
            break;
        }
        case ZYDIS_MNEMONIC_XADD: {
            uses_flags=writes_flags=true;
            const auto bits=operands[0].size;
            body << "    const auto b=" << read(operands[1]) << ";\n";
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY) {
                body << "    const auto location=" << address(operands[0]) << ";\n    const auto a=memory."
                    << (locked?"fetch_add":"load") << '<' << type(bits) << ">(location";
                if(locked) body << ",static_cast<" << type(bits) << ">(b)";
                body << ");\n";
            } else body << "    const auto a=" << read(operands[0]) << ";\n";
            body << "    flags=xbox_native::add_flags<" << bits << ">(flags,a,b);\n";
            write(operands[1],"a");
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY) {
                if(!locked) body << "    memory.store<" << type(bits) << ">(location,static_cast<" << type(bits) << ">(a+b));\n";
            } else write(operands[0],"a+b");
            break;
        }
        case ZYDIS_MNEMONIC_CMPXCHG: {
            const auto bits=operands[0].size;
            type(bits); uses_flags=writes_flags=true;
            const auto accumulator=bits==8?ZYDIS_REGISTER_AL:bits==16?ZYDIS_REGISTER_AX:ZYDIS_REGISTER_EAX;
            body << "    const auto expected=" << read_reg(accumulator) << ";\n"
                << "    const auto desired=" << read(operands[1]) << ";\n";
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY && locked)
                body << "    const auto prior=memory.compare_exchange<" << type(bits) << ">(" << address(operands[0])
                    << ",static_cast<" << type(bits) << ">(expected),static_cast<" << type(bits) << ">(desired));\n";
            else body << "    const auto prior=" << read(operands[0]) << ";\n";
            body << "    flags=xbox_native::sub_flags<" << bits << ">(flags,expected,prior);\n    if(expected==prior) {\n";
            if(!locked) write(operands[0],"desired");
            body << "    } else {\n";
            write(register_operand(accumulator),"prior");
            body << "    }\n";
            break;
        }
        case ZYDIS_MNEMONIC_BSF: {
            uses_flags=writes_flags=true;
            body << "    const std::uint32_t value=" << read(operands[1]) << ";\n"
                << "    flags=(flags&~xbox_native::zero)|(value?0U:xbox_native::zero);\n    if(value) {\n";
            write(operands[0],"static_cast<std::uint32_t>(std::countr_zero(value))");
            body << "    }\n";
            break;
        }
        case ZYDIS_MNEMONIC_CLI: case ZYDIS_MNEMONIC_STI:
            uses_flags=writes_flags=true;
            body << "    flags " << (ins.mnemonic==ZYDIS_MNEMONIC_CLI?"&= ~":"|= ") << "0x200U;\n";
            break;
        case ZYDIS_MNEMONIC_SGDT:
            body << "    const auto location=" << address(operands[0]) << ";\n"
                << "    memory.access(location,6);\n    memory.store<std::uint16_t>(location,cpu.gdt_limit);\n"
                << "    memory.store<std::uint32_t>(location+2,cpu.gdt_base);\n";
            break;
        case ZYDIS_MNEMONIC_PREFETCHNTA:
            body << "    memory.prefetch(" << address(operands[0]) << ");\n";
            break;
        case ZYDIS_MNEMONIC_BSWAP:
            if (operands[0].size != 32) throw std::runtime_error("Unsupported BSWAP width");
            write(operands[0], "std::byteswap(" + read(operands[0]) + ")");
            break;
        case ZYDIS_MNEMONIC_XLAT: {
            const auto base = read_reg(ZYDIS_REGISTER_EBX);
            const auto index = read_reg(ZYDIS_REGISTER_AL);
            const auto segment = (ins.attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS) ? "cpu.fs_base+" :
                (ins.attributes & ZYDIS_ATTRIB_HAS_SEGMENT_GS) ? "cpu.gs_base+" : "";
            write(register_operand(ZYDIS_REGISTER_AL), "memory.load<std::uint8_t>(std::uint32_t(" +
                  std::string(segment) + base + "+" + index + "))");
            break;
        }
        case ZYDIS_MNEMONIC_INTO:
            uses_flags = calls = true;
            body << "    if (flags & xbox_native::overflow) { commit(); cpu.eip=" << pc(next)
                 << "; throw xbox_native::GuestTrap(4," << pc(instruction.address) << "," << pc(next) << "); }\n";
            break;
        case ZYDIS_MNEMONIC_SFENCE:
            body << "    _mm_sfence();\n";
            break;
        case ZYDIS_MNEMONIC_STMXCSR:
            floating=true;
            write(operands[0],"_mm_getcsr()");
            break;
        case ZYDIS_MNEMONIC_LDMXCSR:
            floating=true;
            body << "    const auto control=" << read(operands[0]) << ";\n"
                << "    if(control&~0xFFFFU) throw std::runtime_error(\"Guest MXCSR has reserved bits set\");\n"
                << "    _mm_setcsr(control);\n";
            break;
        case ZYDIS_MNEMONIC_SHUFPS: case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_UNPCKHPS:
        case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS: {
            floating=true;
            const auto name=ins.mnemonic==ZYDIS_MNEMONIC_SHUFPS?"shuffle_ps":
                ins.mnemonic==ZYDIS_MNEMONIC_UNPCKLPS?"unpacklo_ps":ins.mnemonic==ZYDIS_MNEMONIC_UNPCKHPS?"unpackhi_ps":
                ins.mnemonic==ZYDIS_MNEMONIC_MOVHLPS?"movehl_ps":"movelh_ps";
            auto value="_mm_"+std::string(name)+'('+vector(operands[0])+','+vector(operands[1]);
            if(ins.mnemonic==ZYDIS_MNEMONIC_SHUFPS) value+=','+read(operands[2]);
            vector_write(operands[0],value+')');
            break;
        }
        case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVHPS: {
            floating=true;
            const bool store=operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY;
            const auto& memory_operand=operands[store?0:1];
            const auto& register_operand=operands[store?1:0];
            const auto storage="cpu.floating.xmm["+std::to_string(xmm_register(register_operand.reg.value))+"].data()+"+
                (ins.mnemonic==ZYDIS_MNEMONIC_MOVHPS?"2":"0");
            const auto mapped="memory.access("+address(memory_operand)+",8)";
            body << "    std::memcpy(" << (store?mapped:storage) << ',' << (store?storage:mapped) << ",8);\n";
            break;
        }
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVSS: {
            floating=true;
            const bool scalar=ins.mnemonic==ZYDIS_MNEMONIC_MOVSS;
            if (ins.mnemonic==ZYDIS_MNEMONIC_MOVAPS) for(unsigned i=0;i<2;++i)
                if(operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY)
                    body << "    if(" << address(operands[i]) << "&15U) throw std::runtime_error(\"Unaligned guest MOVAPS\");\n";
            auto value=vector(operands[1],scalar);
            if (scalar && operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER)
                value="_mm_move_ss("+vector(operands[0])+','+value+')';
            vector_write(operands[0],value,scalar); break;
        }
        case ZYDIS_MNEMONIC_MOVNTPS:
            floating=true;
            body << "    xbox_native::stream_vector(memory," << address(operands[0]) << ',' << vector(operands[1]) << ");\n";
            break;
        case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI:
            floating=true;
            write(operands[0],std::string("static_cast<std::uint32_t>(")+
                (ins.mnemonic==ZYDIS_MNEMONIC_CVTTSS2SI?"_mm_cvtt_ss2si(":"_mm_cvt_ss2si(")+vector(operands[1],true)+"))");
            break;
        case ZYDIS_MNEMONIC_CVTSI2SS:
            floating=true;
            vector_write(operands[0],"_mm_cvtsi32_ss("+vector(operands[0])+",std::bit_cast<std::int32_t>("+read(operands[1])+"))");
            break;
        case ZYDIS_MNEMONIC_ADDSS: case ZYDIS_MNEMONIC_SUBSS: case ZYDIS_MNEMONIC_MULSS: case ZYDIS_MNEMONIC_DIVSS:
        case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_MAXSS: case ZYDIS_MNEMONIC_SQRTSS: case ZYDIS_MNEMONIC_RSQRTSS:
        case ZYDIS_MNEMONIC_RCPSS: case ZYDIS_MNEMONIC_ADDPS: case ZYDIS_MNEMONIC_SUBPS: case ZYDIS_MNEMONIC_MULPS:
        case ZYDIS_MNEMONIC_DIVPS: case ZYDIS_MNEMONIC_MINPS: case ZYDIS_MNEMONIC_MAXPS: case ZYDIS_MNEMONIC_SQRTPS:
        case ZYDIS_MNEMONIC_RSQRTPS: case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_ANDPS:
        case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ANDNPS: {
            floating=true;
            std::string name=ZydisMnemonicGetString(ins.mnemonic);
            const bool scalar=name.ends_with("ss");
            name.insert(name.size()-2,"_");
            const bool unary=name.starts_with("sqrt_") || name.starts_with("rsqrt_") || name.starts_with("rcp_");
            std::string value="_mm_"+name+'(';
            if (!unary) value+=vector(operands[0])+',';
            value+=vector(operands[1],scalar)+')';
            if (unary && scalar) value="_mm_move_ss("+vector(operands[0])+','+value+')';
            vector_write(operands[0],value); break;
        }
        case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX:
            // The object visibility and proxy frusta use the camera's view-window
            // getter. Other callers use the original width for layout and projection.
            if (!relocatable && instruction.address == 0x464D3)
                body << "    const auto caller = memory.load<std::uint32_t>(esp);\n"
                     << "    const auto value = caller == 0x30807U || caller == 0x4EDE8U ? "
                     << "xbox_native::camera_extent(cpu,memory,eax,0x68U) : " << read(operands[1]) << ";\n";
            else body << "    const auto value = " << read(operands[1]) << ";\n";
            write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_MOVSX:
            body << "    const auto value = xbox_native::sign_extend<" << operands[1].size << ">(" << read(operands[1]) << ");\n";
            type(operands[1].size); write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_LEA:
            body << "    const auto value = " << address(operands[1], true) << ";\n";
            write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP:
        case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB:
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: {
            body << "    const std::uint32_t a = " << read(operands[0]) << ";\n"
                 << "    const std::uint32_t b = " << read(operands[1]) << ";\n";
            const auto bits = operands[0].size;
            const auto m = ins.mnemonic;
            if (m == ZYDIS_MNEMONIC_ADD || m == ZYDIS_MNEMONIC_SUB || m == ZYDIS_MNEMONIC_CMP ||
                m == ZYDIS_MNEMONIC_ADC || m == ZYDIS_MNEMONIC_SBB) {
                const bool addition = m == ZYDIS_MNEMONIC_ADD || m == ZYDIS_MNEMONIC_ADC;
                const bool with_carry = m == ZYDIS_MNEMONIC_ADC || m == ZYDIS_MNEMONIC_SBB;
                if (with_carry) {
                    type(bits);
                    uses_flags = writes_flags = true;
                    body << "    const auto input_carry = flags & xbox_native::carry;\n"
                         << "    flags = xbox_native::" << (addition ? "add_flags" : "sub_flags") << '<' << bits
                         << ">(flags, a, b, input_carry);\n";
                } else flags(addition ? "add_flags" : "sub_flags", bits);
                if (m != ZYDIS_MNEMONIC_CMP)
                    write(operands[0], std::string(addition ? "a + b" : "a - b") +
                                      (with_carry ? (addition ? " + input_carry" : " - input_carry") : ""));
            } else {
                const char operation = m == ZYDIS_MNEMONIC_OR ? '|' : m == ZYDIS_MNEMONIC_XOR ? '^' : '&';
                body << "    const auto value = a " << operation << " b;\n";
                uses_flags = writes_flags = true;
                type(bits);
                body << "    flags = xbox_native::logic_flags<" << bits << ">(flags, value);\n";
                if (m != ZYDIS_MNEMONIC_TEST) write(operands[0], "value");
            }
            break;
        }
        case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETNB:
        case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL:
        case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_SETNLE:
            body << "    const std::uint32_t value = (" << condition(ins.mnemonic) << ") ? 1U : 0U;\n";
            write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JNB:
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JNL:
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNLE:
            conditional_branch(instruction);
            break;
        case ZYDIS_MNEMONIC_JMP:
            if (instruction.resumes_jump) {
                calls = true;
                body << "    const auto target = " << read(operands[0]) << ";\n"
                     << "    commit(); cpu.eip = target;\n"
                     << "    throw xbox_native::GuestLongJump(target);\n";
            } else if (operands[0].type == ZYDIS_OPERAND_TYPE_POINTER) {
                calls = true;
                body << "    commit(); cpu.eip = " << pc(instruction.address) << ";\n"
                     << "    xbox_native::load_code_selector(cpu,memory," << operands[0].ptr.segment << ","
                     << constant(operands[0].ptr.offset) << ");\n    ";
                branch(instruction);
                body << '\n';
            } else if (instruction.kernel_ordinal) {
                calls=restores=returns=true;
                body << "    commit();\n    cpu.eip=" << pc(instruction.address) << ";\n"
                    << "    xbox_native::call_kernel<" << *instruction.kernel_ordinal << ">(cpu,memory);\n    restore();\n    goto done;\n";
            } else if (!instruction.jump_targets.empty() || !instruction.external_jump_targets.empty()) {
                calls=true;
                body << "    switch(" << read(operands[0]) << (relocatable ? " - code_base" : "") << ") {\n";
                const std::set<std::uint32_t> targets(instruction.jump_targets.begin(),instruction.jump_targets.end());
                for(const auto target : targets) body << "    case " << constant(target) << ": goto " << label(target) << ";\n";
                const std::set<std::uint32_t> external(instruction.external_jump_targets.begin(), instruction.external_jump_targets.end());
                for (const auto target : external) {
                    body << "    case " << constant(target) << ": commit(); cpu.eip = " << pc(target) << "; "
                         << std::format("guest_{:08X}(cpu,memory); return;\n", target);
                }
                body << "    default: commit(); cpu.eip=" << pc(instruction.address)
                    << "; throw std::runtime_error(\"Guest jump table target is outside its recovered set\");\n    }\n";
            } else if (instruction.target()) {
                body << "    ";
                branch(instruction);
                body << '\n';
            }
            else {
                calls=restores=returns=true;
                body << "    const auto target=" << read(operands[0]) << ";\n" << allowed_check(instruction, pc(instruction.address))
                     << "    commit();\n"
                    << "    cpu.eip = " << pc(instruction.address) << ";\n"
                    << "    xbox_native::invoke_native(cpu,memory,target);\n    restore();\n    goto done;\n";
            }
            terminator = true; break;
        case ZYDIS_MNEMONIC_PUSH: {
            const auto width = ins.operand_width;
            if (width != 16 && width != 32) throw std::runtime_error("Unsupported push width");
            used.insert(4); dirty.insert(4);
            auto value = read(operands[0]);
            // The verified title passes 0.033 seconds per rendered frame to its
            // scene effects and interface timers. Offline execution keeps the literal.
            if (!relocatable && (instruction.address == 0x23CC7 || instruction.address == 0x23D8A ||
                    instruction.address == 0x24098) && operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                    operands[0].imm.value.u == 0x3D072B02 && width == 32) {
                value = "(cpu.clock && cpu.clock->frame_step ? cpu.clock->frame_step(cpu.clock->context, " + value + ") : " + value + ")";
            }
            body << "    const auto value = " << value << ";\n    esp -= " << width / 8
                 << "U;\n    memory.store<" << type(width) << ">(esp, static_cast<" << type(width) << ">(value));\n";
            break;
        }
        case ZYDIS_MNEMONIC_PUSHF: case ZYDIS_MNEMONIC_PUSHFD: {
            const unsigned width=ins.operand_width;
            if(width!=16 && width!=32) throw std::runtime_error("Unsupported flags stack width");
            used.insert(4); dirty.insert(4); uses_flags=true;
            body << "    esp-=" << width/8 << "U;\n    memory.store<" << type(width)
                << ">(esp,static_cast<" << type(width) << ">((flags&~0x30000U)|2U));\n";
            break;
        }
        case ZYDIS_MNEMONIC_POPF: case ZYDIS_MNEMONIC_POPFD: {
            const unsigned width=ins.operand_width;
            if(width!=16 && width!=32) throw std::runtime_error("Unsupported flags stack width");
            used.insert(4); dirty.insert(4); uses_flags=writes_flags=true;
            body << "    const auto value=memory.load<" << type(width) << ">(esp);\n    esp+=" << width/8
                << "U;\n    flags=xbox_native::pop_flags(flags,value," << width << ",cpu.cs_selector&3U);\n";
            break;
        }
        case ZYDIS_MNEMONIC_POP: {
            const auto width = ins.operand_width;
            if (width != 16 && width != 32) throw std::runtime_error("Unsupported pop width");
            used.insert(4); dirty.insert(4);
            body << "    const auto value = memory.load<" << type(width) << ">(esp);\n    esp += " << width / 8 << "U;\n";
            write(operands[0], "value"); break;
        }
        case ZYDIS_MNEMONIC_CALL: {
            if (ins.operand_width != 32 || operands[0].type==ZYDIS_OPERAND_TYPE_POINTER)
                throw std::runtime_error("Only 32-bit near native calls are supported");
            if (instruction.call_next_instruction) {
                used.insert(4); dirty.insert(4);
                body << "    esp -= 4U;\n    memory.store<std::uint32_t>(esp, " << pc(next) << ");\n";
                break;
            }
            calls = restores = true;
            const bool indirect=!instruction.target() && !instruction.kernel_ordinal;
            if(indirect) body << "    const auto target=" << read(operands[0]) << ";\n" << allowed_check(instruction, pc(instruction.address));
            if (!instruction.caller_targets.empty()) {
                body<<"    std::uint32_t callback = 0;\n    switch (guest_caller) {\n";
                for (const auto [caller,destination]:instruction.caller_targets)
                    body<<"    case "<<constant(caller)<<": callback = "<<constant(destination)<<"; break;\n";
                body<<"    }\n    if (!callback || target != callback) throw xbox_native::NativeBoundary("<<pc(instruction.address)
                    <<", \"callback differs from the original caller's argument\");\n";
            }
            if (instruction.checked_target)
                body << "    if (target != " << pc(*instruction.checked_target) << ") throw xbox_native::NativeBoundary("
                     << pc(instruction.address) << ", \"indirect call differs from its verified target\");\n";
            used.insert(4); dirty.insert(4);
            body << "    esp -= 4U;\n    memory.store<std::uint32_t>(esp, " << pc(next)
                 << ");\n    commit();\n    cpu.eip = " << pc(instruction.target().value_or(instruction.address)) << ";\n    ";
            if (instruction.kernel_ordinal) body << "xbox_native::call_kernel<" << *instruction.kernel_ordinal << ">(cpu,memory);\n";
            else if (instruction.missing_target)
                body << "throw xbox_native::NativeBoundary(cpu.eip, \"unmapped direct call\");\n";
            else if (instruction.target()) body << "guest_" << std::format("{:08X}", *instruction.target()) << "(cpu, memory);\n";
            else body << "xbox_native::invoke_native(cpu,memory,target);\n";
            body << "    if (cpu.eip != " << pc(next)
                 << ") throw std::runtime_error(\"Unexpected return from call at " << hex32(instruction.address) << "\");\n"
                 << "    restore();\n";
            if (instruction.saves_jump) {
                jump_continuations.insert(next);
                body << "    " << jump_stack(next) << " = esp;\n";
            }
            break;
        }
        case ZYDIS_MNEMONIC_WBINVD:
            calls = true;
            body << "    commit(); cpu.eip = " << pc(instruction.address)
                 << "; xbox_native::writeback_guest_cache(cpu);\n";
            break;
        case ZYDIS_MNEMONIC_RDTSC:
            calls=true;
            body << "    commit();\n    cpu.eip=" << pc(instruction.address) << ";\n"
                 << "    const auto timestamp=xbox_native::read_timestamp(cpu);\n";
            write(register_operand(ZYDIS_REGISTER_EAX),"static_cast<std::uint32_t>(timestamp)");
            write(register_operand(ZYDIS_REGISTER_EDX),"static_cast<std::uint32_t>(timestamp>>32)");
            break;
        case ZYDIS_MNEMONIC_IN: case ZYDIS_MNEMONIC_OUT: {
            calls=restores=true;
            const bool input=ins.mnemonic==ZYDIS_MNEMONIC_IN;
            const auto& port=operands[input?1:0];
            const auto& value=operands[input?0:1];
            body << "    const auto port=static_cast<std::uint16_t>(" << read(port) << ");\n";
            if(!input) body << "    const auto value=" << read(value) << ";\n";
            body << "    commit();\n    cpu.eip=" << pc(instruction.address) << ";\n";
            if(input) body << "    const auto value=xbox_native::read_port(cpu,port," << value.size << ");\n";
            else body << "    xbox_native::write_port(cpu,port," << value.size << ",value);\n";
            body << "    restore();\n";
            if(input) write(value,"value");
            break;
        }
        case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_INT:
            calls=true;
            body << "    commit();\n    cpu.eip=" << pc(next) << ";\n"
                 << "    throw xbox_native::GuestTrap(" << (ins.mnemonic==ZYDIS_MNEMONIC_INT3?3:operands[0].imm.value.u)
                 << ',' << pc(instruction.address) << ',' << pc(next) << ");\n";
            terminator=true; break;
        case ZYDIS_MNEMONIC_LEAVE:
            if (ins.operand_width != 32) throw std::runtime_error("Unsupported leave width");
            used.insert(4); used.insert(5); dirty.insert(4); dirty.insert(5);
            body << "    esp = ebp;\n    ebp = memory.load<std::uint32_t>(esp);\n    esp += 4U;\n";
            break;
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: {
            const bool increment = ins.mnemonic == ZYDIS_MNEMONIC_INC;
            type(operands[0].size);
            uses_flags = writes_flags = true;
            body << "    const auto a = " << read(operands[0]) << ";\n    flags = (xbox_native::"
                 << (increment ? "add_flags" : "sub_flags") << '<' << operands[0].size
                 << ">(flags, a, 1U) & ~xbox_native::carry) | (flags & xbox_native::carry);\n";
            write(operands[0], increment ? "a + 1U" : "a - 1U"); break;
        }
        case ZYDIS_MNEMONIC_NOT: case ZYDIS_MNEMONIC_NEG:
            body << "    const auto a = " << read(operands[0]) << ";\n";
            if (ins.mnemonic == ZYDIS_MNEMONIC_NEG) {
                type(operands[0].size);
                uses_flags = writes_flags = true;
                body << "    flags = xbox_native::sub_flags<" << operands[0].size << ">(flags, 0U, a);\n";
            }
            write(operands[0], ins.mnemonic == ZYDIS_MNEMONIC_NOT ? "~a" : "0U - a"); break;
        case ZYDIS_MNEMONIC_CDQ:
            used.insert(0); used.insert(2); dirty.insert(2);
            body << "    edx = 0U - (eax >> 31);\n"; break;
        case ZYDIS_MNEMONIC_CWDE:
            used.insert(0); dirty.insert(0);
            body << "    eax = xbox_native::sign_extend<16>(eax);\n"; break;
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            type(operands[0].size);
            uses_flags = writes_flags = true;
            body << "    const auto result = xbox_native::shift<" << operands[0].size << ">(flags, " << read(operands[0])
                 << ", " << read(operands[1]) << ", xbox_native::Shift::"
                 << (ins.mnemonic == ZYDIS_MNEMONIC_SHL ? "left" : ins.mnemonic == ZYDIS_MNEMONIC_SHR ? "right" : "arithmetic_right")
                 << ");\n    flags = result.flags;\n";
            write(operands[0], "result.value"); break;
        }
        case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCR: case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_RCL: {
            type(operands[0].size);
            uses_flags=writes_flags=true;
            body << "    const auto result=xbox_native::rotate_" << (ins.mnemonic==ZYDIS_MNEMONIC_ROL || ins.mnemonic==ZYDIS_MNEMONIC_RCL?"left":"right")
                << '<' << operands[0].size << ','
                << (ins.mnemonic==ZYDIS_MNEMONIC_RCR || ins.mnemonic==ZYDIS_MNEMONIC_RCL?"true":"false") << ">(" << read(operands[0]) << ','
                << read(operands[1]) << ",flags);\n    flags=result.flags;\n";
            write(operands[0],"result.value"); break;
        }
        case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD: {
            if(operands[0].size!=16 && operands[0].size!=32) throw std::runtime_error("Invalid double shift width");
            uses_flags=writes_flags=true;
            body << "    const auto result=xbox_native::double_shift<" << operands[0].size << ','
                << (ins.mnemonic==ZYDIS_MNEMONIC_SHRD?"true":"false") << ">(" << read(operands[0]) << ','
                << read(operands[1]) << ',' << read(operands[2]) << ",flags);\n    flags=result.flags;\n";
            write(operands[0],"result.value"); break;
        }
        case ZYDIS_MNEMONIC_MUL: case ZYDIS_MNEMONIC_IMUL: {
            uses_flags = writes_flags = true;
            const auto bits = operands[0].size;
            type(bits);
            const bool implicit = ins.operand_count_visible == 1;
            const auto accumulator = bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            const auto a = implicit ? read_reg(accumulator) : read(operands[ins.operand_count_visible == 3 ? 1 : 0]);
            const auto b = read(operands[implicit ? 0 : ins.operand_count_visible == 3 ? 2 : 1]);
            body << "    const auto product = xbox_native::multiply<" << bits << ", "
                 << (ins.mnemonic == ZYDIS_MNEMONIC_IMUL ? "true" : "false") << ">(flags, " << a << ", " << b
                 << ");\n    flags = product.flags;\n";
            if (implicit) {
                ZydisDecodedOperand dest{};
                dest.type = ZYDIS_OPERAND_TYPE_REGISTER;
                dest.reg.value = bits == 8 ? ZYDIS_REGISTER_AX : accumulator;
                write(dest, "static_cast<std::uint32_t>(product.value)");
                if (bits != 8) {
                    dest.reg.value = bits == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX;
                    write(dest, "static_cast<std::uint32_t>(product.value >> " + std::to_string(bits) + ')');
                }
            } else write(operands[0], "static_cast<std::uint32_t>(product.value)");
            break;
        }
        case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: {
            const auto bits = operands[0].size;
            type(bits);
            const auto accumulator = bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            const auto dividend = bits == 8 ? read_reg(ZYDIS_REGISTER_AX) :
                "(std::uint64_t(" + read_reg(bits == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX) + ") << " +
                std::to_string(bits) + ") | " + read_reg(accumulator);
            divide_faults = calls = restores = true;
            body << "    xbox_native::Quotient quotient;\n    try { quotient = xbox_native::divide<" << bits << ", "
                 << (ins.mnemonic == ZYDIS_MNEMONIC_IDIV ? "true" : "false") << ">(" << dividend << ", " << read(operands[0])
                 << ", " << pc(instruction.address) << "); } catch (const xbox_native::GuestTrap&) {\n"
                 << "        commit(); xbox_native::dispatch_divide_fault(cpu, memory, " << pc(instruction.address)
                 << "); restore(); goto resume_fault;\n    }\n";
            ZydisDecodedOperand dest{};
            dest.type = ZYDIS_OPERAND_TYPE_REGISTER;
            dest.reg.value = accumulator;
            write(dest, "quotient.value");
            dest.reg.value = bits == 8 ? ZYDIS_REGISTER_AH : bits == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX;
            write(dest, "quotient.remainder"); break;
        }
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: {
            if (!string_move && !string_fill) throw std::runtime_error("SIMD move needs its own lowering");
            const auto bits = (ins.mnemonic == ZYDIS_MNEMONIC_MOVSB || ins.mnemonic == ZYDIS_MNEMONIC_STOSB) ? 8 :
                              (ins.mnemonic == ZYDIS_MNEMONIC_MOVSW || ins.mnemonic == ZYDIS_MNEMONIC_STOSW) ? 16 : 32;
            uses_flags = true;
            used.insert(7); dirty.insert(7);
            if (repeats) { used.insert(1); dirty.insert(1); }
            body << "    const auto count = " << (repeats ? "ecx" : "1U") << ";\n"
                 << "    const bool descending = (flags & xbox_native::direction) != 0;\n";
            if (string_move) {
                used.insert(6); dirty.insert(6);
                body << "    memory.move_elements<" << type(bits) << ">(edi, esi, count, descending);\n";
            } else {
                used.insert(0);
                body << "    memory.fill_elements<" << type(bits) << ">(edi, static_cast<" << type(bits) << ">(eax), count, descending);\n";
            }
            body << "    const auto step = descending ? 0U - count * " << bits / 8 << "U : count * " << bits / 8 << "U;\n"
                 << "    edi += step;\n";
            if (string_move) body << "    esi += step;\n";
            if (repeats) body << "    ecx = 0;\n";
            break;
        }
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: {
            if(!string_compare && !string_scan) throw std::runtime_error("SIMD compare needs its own lowering");
            const auto bits=ins.mnemonic==ZYDIS_MNEMONIC_CMPSB || ins.mnemonic==ZYDIS_MNEMONIC_SCASB?8:
                ins.mnemonic==ZYDIS_MNEMONIC_CMPSW || ins.mnemonic==ZYDIS_MNEMONIC_SCASW?16:32;
            used.insert(7); dirty.insert(7);
            if(string_compare) { used.insert(6); dirty.insert(6); }
            else used.insert(0);
            uses_flags = writes_flags = true;
            if (repeats) { used.insert(1); dirty.insert(1); body << "    while (ecx != 0) {\n"; }
            body << "    const auto a = ";
            if(string_compare) body << "memory.load<" << type(bits) << ">(esi)";
            else body << "eax";
            body << ";\n    const auto b = memory.load<" << type(bits) << ">(edi);\n"
                 << "    flags = xbox_native::sub_flags<" << bits << ">(flags, a, b);\n"
                 << "    const auto step = (flags & xbox_native::direction) ? 0U-" << bits/8 << "U : " << bits/8 << "U;\n";
            if(string_compare) body << "    esi += step;\n";
            body << "    edi += step;\n";
            if (repeats) {
                body << "    --ecx;\n    if ((flags & xbox_native::zero) "
                     << ((ins.attributes & ZYDIS_ATTRIB_HAS_REPNE) ? "!= 0" : "== 0") << ") break;\n    }\n";
            }
            break;
        }
        case ZYDIS_MNEMONIC_CLD: case ZYDIS_MNEMONIC_STD:
            uses_flags = writes_flags = true;
            body << "    flags " << (ins.mnemonic == ZYDIS_MNEMONIC_CLD ? "&= ~" : "|= ") << "xbox_native::direction;\n";
            break;
        case ZYDIS_MNEMONIC_RET: {
            returns=true;
            if (ins.operand_width != 32) throw std::runtime_error("Unsupported return width");
            used.insert(4); dirty.insert(4);
            body << "    const auto return_address = memory.load<std::uint32_t>(esp);\n"
                 << "    esp += " << (4U + (ins.operand_count_visible ? static_cast<unsigned>(operands[0].imm.value.u) : 0U))
                 << "U;\n    cpu.eip = return_address;\n    goto done;\n";
            terminator = true; break;
        }
        case ZYDIS_MNEMONIC_NOP: break;
        default:
            if (ins.meta.category == ZYDIS_CATEGORY_COND_BR && instruction.target()) {
                conditional_branch(instruction);
            } else if (ins.meta.category == ZYDIS_CATEGORY_SETCC) {
                body << "    const std::uint32_t value = (" << condition(ins.mnemonic) << ") ? 1U : 0U;\n";
                write(operands[0], "value");
            } else if (ins.meta.category == ZYDIS_CATEGORY_CMOV) {
                body << "    const auto value = " << read(operands[1]) << ";\n    if (" << condition(ins.mnemonic) << ") {\n";
                write(operands[0], "value");
                body << "    }\n";
            } else if ((ins.opcode>=0xD8 && ins.opcode<=0xDF && ins.opcode_map==ZYDIS_OPCODE_MAP_DEFAULT) || ins.mnemonic==ZYDIS_MNEMONIC_FWAIT) {
                x87(instruction);
            } else throw std::runtime_error("Instruction has no native lowering yet");
        }
        if (!terminator && instruction.missing_fallthrough) {
            calls = true;
            body << "    commit(); cpu.eip = " << pc(next)
                 << "; throw xbox_native::NativeBoundary(cpu.eip, \"unmapped fall-through\");\n";
        } else if (!terminator && instruction.cross_function_fallthrough) {
            calls = true;
            body << "    commit(); cpu.eip = " << pc(next) << "; "
                 << std::format("guest_{:08X}(cpu,memory); return;\n", next);
        } else if (!terminator) {
            body << "    goto " << label(next) << ";\n";
        }
        body << "}\n";
    }
};
}

LoweredFunction lower(const Function& function) {
    LoweredFunction result;
    Emitter emitter;
    emitter.function = &function;
    emitter.relocatable = function.relocatable;
    emitter.loop_headers.insert(function.loop_headers.begin(), function.loop_headers.end());
    emitter.calls=!emitter.loop_headers.empty();
    for (const auto& [pc, instruction] : function.instructions) {
        try { emitter.instruction(instruction); }
        catch (const std::exception& error) {
            result.issues.push_back({pc, hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length)),
                                     instruction.text, error.what()});
        }
    }
    if (!result.issues.empty()) return result;
    std::ostringstream out;
    for(const auto& [name,declaration] : emitter.native_declarations) out << declaration;
    const auto symbol = std::format("guest_{:08X}", function.entry);
    const bool multiple_entries = !function.alternate_entries.empty();
    if (multiple_entries) {
        out << "static void " << symbol << "_body(Cpu& cpu, [[maybe_unused]] Memory& memory, std::uint32_t entry) {\n";
    } else {
        out << "void " << symbol << "(Cpu& cpu, [[maybe_unused]] Memory& memory) {\n";
    }
    if (function.relocatable) {
        out << "    const auto code_base = cpu.eip - " << (multiple_entries ? "entry" : constant(function.entry)) << ";\n";
    }
    if (std::ranges::any_of(function.instructions,[](const auto& pair){return !pair.second.caller_targets.empty();}))
        out<<"    const auto guest_caller=memory.load<std::uint32_t>(cpu.registers[esp]);\n";
    if (emitter.floating) out << "    FloatingScope floating_scope(cpu.floating);\n";
    out << "    if(xbox_native::checkpoint_due(cpu)) xbox_native::checkpoint(cpu,"
        << (multiple_entries ? (function.relocatable ? "code_base + entry" : "entry") : emitter.pc(function.entry)) << ");\n";
    if (function.platform_binding) {
        if (multiple_entries) out << "    if (entry == " << constant(function.entry) << ") ";
        else out << "    ";
        out << "if (xbox_native::invoke_platform(cpu, memory, " << constant(function.entry) << ")) return;\n";
    }
    for (const auto address : function.platform_alternates) {
        out << "    if (entry == " << constant(address) << " && xbox_native::invoke_platform(cpu, memory, "
            << constant(address) << ")) return;\n";
    }
    for (const auto index : emitter.used)
        out << "    auto " << registers[index] << " = cpu.registers[" << index << "];\n";
    if (emitter.uses_flags) out << "    auto flags = cpu.flags;\n";
    if (emitter.calls) {
        out << "    const auto commit = [&] {\n";
        for (const auto index : emitter.dirty) out << "        cpu.registers[" << index << "] = " << registers[index] << ";\n";
        if (emitter.writes_flags) out << "        cpu.flags = flags;\n";
        out << "    };\n";
        if (emitter.restores) {
            out << "    const auto restore = [&] {\n";
            for (const auto index : emitter.used) out << "        " << registers[index] << " = cpu.registers[" << index << "];\n";
            if (emitter.uses_flags) out << "        flags = cpu.flags;\n";
            out << "    };\n";
        }
    }
    const auto& jump_continuations = emitter.jump_continuations;
    if (!jump_continuations.empty()) {
        for (const auto address : jump_continuations)
            out << "    std::uint32_t " << jump_stack(address) << " = 0;\n";
        out << "    std::uint32_t resume = 0;\n    for (;;) {\n    try {\n    switch (resume) {\n";
        for (const auto address : jump_continuations)
            out << "    case " << constant(address) << ": goto " << label(address) << ";\n";
        out << "    }\n";
    }
    if (multiple_entries) {
        out << "    switch (entry) {\n";
        for (const auto entry : function.alternate_entries) {
            out << "    case " << constant(entry) << ": goto " << label(entry) << ";\n";
        }
        out << "    default: goto " << label(function.entry) << ";\n    }\n";
    } else {
        out << "    goto " << label(function.entry) << ";\n";
    }
    if (emitter.divide_faults) {
        out << "resume_fault:\n    switch (" << (function.relocatable ? "cpu.eip - code_base" : "cpu.eip") << ") {\n";
        for (const auto& [address, instruction] : function.instructions)
            out << "    case " << constant(address) << ": goto " << label(address) << ";\n";
        out << "    default: xbox_native::invoke_native(cpu,memory,cpu.eip); return;\n    }\n";
    }
    out << emitter.body.str();
    if(emitter.returns) {
        out << "done:\n";
        for (const auto index : emitter.dirty)
            out << "    cpu.registers[" << index << "] = " << registers[index] << ";\n";
        if (emitter.writes_flags) out << "    cpu.flags = flags;\n";
    }
    if (!jump_continuations.empty()) {
        out << "    return;\n    } catch (const xbox_native::GuestLongJump& jump) {\n"
            << "        switch (jump.address) {\n";
        for (const auto address : jump_continuations) {
            out << "        case " << constant(address) << ":\n"
                << "            if (!" << jump_stack(address) << " || " << jump_stack(address)
                << " != cpu.registers[xbox_native::esp]) throw;\n"
                << "            break;\n";
        }
        out << "        default: throw;\n        }\n"
            << "        restore();\n        resume = jump.address;\n    }\n    }\n";
    }
    out << "}\n";
    if (multiple_entries) {
        const auto wrapper = [&](std::uint32_t entry) {
            out << std::format("void guest_{:08X}(Cpu& cpu, Memory& memory) {{ {}_body(cpu, memory, {}); }}\n",
                               entry, symbol, constant(entry));
        };
        wrapper(function.entry);
        for (const auto entry : function.alternate_entries) wrapper(entry);
    }
    result.source = out.str();
    result.floating=emitter.floating;
    result.native_helpers=std::move(emitter.native_helpers);
    return result;
}

}
