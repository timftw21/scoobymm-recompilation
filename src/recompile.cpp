#include "analysis_map.h"
#include "xbox/batch.h"
#ifdef XBOX_OFFLINE_BOOT
#include "xbox/boot.h"
#endif
#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using analysis_map::Word;
using analysis_map::Path;
using analysis_map::hex;
using analysis_map::number;
using analysis_map::fields;
using analysis_map::line;
using analysis_map::table;
using analysis_map::Functions;
using analysis_map::Maps;

std::string literal(Word value) { return hex(value) + "U"; }
std::string label(Word value) { return std::format("instruction_{:08X}", value); }
std::string function_name(Word value) { return std::format("function_{:08X}", value); }

class Image {
public:
    Image(const Path& path, Word base) : file(path, std::ios::binary | std::ios::ate), base(base) {
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) + base > (std::uint64_t{1} << 32)) {
            throw std::runtime_error("Raw image must fit in the 32-bit address space");
        }
        length = static_cast<std::uint64_t>(size);
    }

    const std::uint8_t* read(Word address, unsigned size) {
        const auto offset = std::uint64_t{address} - base;
        if (address < base || offset > length || size > length - offset) {
            throw std::runtime_error("Instruction outside raw image at " + hex(address));
        }
        if (offset < cache_start || offset + size > cache_start + cache_size) {
            cache_start = offset;
            cache_size = std::min<std::uint64_t>(cache.size(), length - offset);
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(reinterpret_cast<char*>(cache.data()), static_cast<std::streamsize>(cache_size));
            if (!file) throw std::runtime_error("Cannot read raw image");
        }
        return cache.data() + (offset - cache_start);
    }

private:
    std::ifstream file;
    Word base;
    std::uint64_t length{}, cache_start{}, cache_size{};
    std::array<std::uint8_t, 65536> cache{};
};

struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct RegisterPart {
    unsigned index, bits, shift;
};

RegisterPart register_part(ZydisRegister reg) {
    constexpr std::array<ZydisRegister, 8> full{
        ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX, ZYDIS_REGISTER_EBX,
        ZYDIS_REGISTER_ESP, ZYDIS_REGISTER_EBP, ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI};
    constexpr std::array<ZydisRegister, 8> word{
        ZYDIS_REGISTER_AX, ZYDIS_REGISTER_CX, ZYDIS_REGISTER_DX, ZYDIS_REGISTER_BX,
        ZYDIS_REGISTER_SP, ZYDIS_REGISTER_BP, ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI};
    constexpr std::array<ZydisRegister, 4> lo{ZYDIS_REGISTER_AL, ZYDIS_REGISTER_CL, ZYDIS_REGISTER_DL, ZYDIS_REGISTER_BL};
    constexpr std::array<ZydisRegister, 4> hi{ZYDIS_REGISTER_AH, ZYDIS_REGISTER_CH, ZYDIS_REGISTER_DH, ZYDIS_REGISTER_BH};
    for (unsigned i = 0; i < full.size(); ++i) {
        if (reg == full[i]) return {i, 32, 0};
        if (reg == word[i]) return {i, 16, 0};
        if (i < lo.size() && reg == lo[i]) return {i, 8, 0};
        if (i < hi.size() && reg == hi[i]) return {i, 8, 8};
    }
    throw Unsupported("Unsupported register");
}

unsigned scalar_bits(unsigned size) {
    if (size != 8 && size != 16 && size != 32) throw Unsupported("Unsupported scalar width");
    return size;
}

std::string read_register(ZydisRegister reg) {
    const auto part = register_part(reg);
    const auto value = std::format("cpu.gpr[{}]", part.index);
    if (part.bits == 32) return value;
    return std::format("(({} >> {}) & mask({}))", value, part.shift, part.bits);
}

std::string address(const ZydisDecodedOperand& operand) {
    const auto& mem = operand.mem;
    if (mem.segment == ZYDIS_REGISTER_FS || mem.segment == ZYDIS_REGISTER_GS) {
        throw Unsupported("Non-flat segment addressing");
    }
    std::string value = literal(mem.disp.has_displacement ? static_cast<Word>(mem.disp.value) : 0);
    if (mem.base != ZYDIS_REGISTER_NONE) value += " + " + read_register(mem.base);
    if (mem.index != ZYDIS_REGISTER_NONE) value += std::format(" + {} * {}U", read_register(mem.index), mem.scale);
    return "(" + value + ")";
}

std::string read_operand(const ZydisDecodedOperand& operand) {
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: return read_register(operand.reg.value);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        return literal(operand.imm.is_signed ? static_cast<Word>(operand.imm.value.s) : static_cast<Word>(operand.imm.value.u));
    case ZYDIS_OPERAND_TYPE_MEMORY:
        if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM) throw Unsupported("Unsupported memory operand");
        return std::format("cpu.load({}, {})", address(operand), scalar_bits(operand.size));
    default: throw Unsupported("Unsupported operand");
    }
}

std::string write_operand(const ZydisDecodedOperand& operand, std::string_view value) {
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const auto part = register_part(operand.reg.value);
        if (part.bits == 32) return std::format("cpu.gpr[{}] = {};", part.index, value);
        return std::format("cpu.write_register({}, {}, {}, {});", part.index, part.bits, part.shift, value);
    }
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.type == ZYDIS_MEMOP_TYPE_MEM) {
        return std::format("cpu.store({}, {}, {});", address(operand), scalar_bits(operand.size), value);
    }
    throw Unsupported("Unsupported destination operand");
}

std::string_view condition(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_JB: return "cpu.carry";
    case ZYDIS_MNEMONIC_JBE: return "cpu.carry || cpu.zero";
    case ZYDIS_MNEMONIC_JL: return "cpu.sign != cpu.overflow";
    case ZYDIS_MNEMONIC_JLE: return "cpu.zero || cpu.sign != cpu.overflow";
    case ZYDIS_MNEMONIC_JNB: return "!cpu.carry";
    case ZYDIS_MNEMONIC_JNBE: return "!cpu.carry && !cpu.zero";
    case ZYDIS_MNEMONIC_JNL: return "cpu.sign == cpu.overflow";
    case ZYDIS_MNEMONIC_JNLE: return "!cpu.zero && cpu.sign == cpu.overflow";
    case ZYDIS_MNEMONIC_JNO: return "!cpu.overflow";
    case ZYDIS_MNEMONIC_JNP: return "!cpu.parity";
    case ZYDIS_MNEMONIC_JNS: return "!cpu.sign";
    case ZYDIS_MNEMONIC_JNZ: return "!cpu.zero";
    case ZYDIS_MNEMONIC_JO: return "cpu.overflow";
    case ZYDIS_MNEMONIC_JP: return "cpu.parity";
    case ZYDIS_MNEMONIC_JS: return "cpu.sign";
    case ZYDIS_MNEMONIC_JZ: return "cpu.zero";
    case ZYDIS_MNEMONIC_JECXZ: return "cpu.gpr[1] == 0";
    default: return {};
    }
}

struct Edge {
    Word target;
    Word source;
    unsigned length;
};

class Lowering {
public:
    Lowering(const Maps& maps, std::vector<Edge>& edges, Word pc, Word entry, const ZydisDecodedInstruction& decoded)
        : maps(maps), edges(edges), pc(pc), entry(entry), decoded(decoded), next(pc + decoded.length) {}

    std::string emit(const ZydisDecodedOperand* operands) {
        const auto& dst = operands[0];
        const auto& src = operands[1];
        if (decoded.address_width != 32 || decoded.attributes &
            (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) {
            throw Unsupported("Unsupported instruction prefix");
        }

        std::string statement;
        switch (decoded.mnemonic) {
        case ZYDIS_MNEMONIC_NOP: break;
        case ZYDIS_MNEMONIC_MOV:
        case ZYDIS_MNEMONIC_MOVZX:
            statement = write_operand(dst, read_operand(src));
            break;
        case ZYDIS_MNEMONIC_MOVSX:
            statement = write_operand(dst, std::format("sign_extend({}, {})", read_operand(src), scalar_bits(src.size)));
            break;
        case ZYDIS_MNEMONIC_LEA:
            statement = write_operand(dst, address(src));
            break;
        case ZYDIS_MNEMONIC_ADD: statement = arithmetic("add", dst, read_operand(src)); break;
        case ZYDIS_MNEMONIC_SUB: statement = arithmetic("sub", dst, read_operand(src)); break;
        case ZYDIS_MNEMONIC_AND: statement = arithmetic("bit_and", dst, read_operand(src)); break;
        case ZYDIS_MNEMONIC_OR: statement = arithmetic("bit_or", dst, read_operand(src)); break;
        case ZYDIS_MNEMONIC_XOR: statement = arithmetic("bit_xor", dst, read_operand(src)); break;
        case ZYDIS_MNEMONIC_CMP: statement = "(void)" + calculate("sub", dst, read_operand(src)) + ";"; break;
        case ZYDIS_MNEMONIC_TEST: statement = "(void)" + calculate("bit_and", dst, read_operand(src)) + ";"; break;
        case ZYDIS_MNEMONIC_INC: statement = arithmetic("increment", dst, "1U"); break;
        case ZYDIS_MNEMONIC_DEC: statement = arithmetic("decrement", dst, "1U"); break;
        case ZYDIS_MNEMONIC_PUSH:
            require_32bit_stack();
            statement = std::format("cpu.push32({});", read_operand(dst));
            break;
        case ZYDIS_MNEMONIC_POP:
            require_32bit_stack();
            statement = "const auto popped = cpu.pop32(); " + write_operand(dst, "popped");
            break;
        case ZYDIS_MNEMONIC_RET: {
            require_32bit_stack();
            const auto adjustment = decoded.operand_count_visible ? read_operand(dst) : "0U";
            return std::format("const auto return_address = cpu.pop32(); cpu.gpr[4] += {}; return return_address;", adjustment);
        }
        case ZYDIS_MNEMONIC_CALL: {
            const auto target = direct_target(dst);
            if (!maps.functions.contains(target)) throw Unsupported("Call target has no function entry at " + hex(target));
            edges.push_back({target, pc, decoded.length});
            statement = std::format(
                "cpu.push32({0}); if ({1}(cpu) != {0}) throw std::runtime_error(\"Unexpected x86 return address\");",
                literal(next), function_name(target));
            break;
        }
        case ZYDIS_MNEMONIC_JMP: return jump(direct_target(dst));
        default: {
            const auto cond = condition(decoded.mnemonic);
            if (cond.empty()) throw Unsupported("Unsupported mnemonic");
            statement = std::format("if ({}) {{ {} }}", cond, jump(direct_target(dst)));
        }
        }

        const auto* range = maps.containing(next);
        if (!range || range->entry != entry) throw Unsupported("Fall-through leaves its function at " + hex(next));
        if (!statement.empty()) statement += ' ';
        return statement + jump(next);
    }

private:
    const Maps& maps;
    std::vector<Edge>& edges;
    Word pc, entry;
    const ZydisDecodedInstruction& decoded;
    Word next;

    void require_32bit_stack() const {
        if (decoded.operand_width != 32 || decoded.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
            throw Unsupported("Unsupported stack operand width or far control transfer");
        }
    }

    Word direct_target(const ZydisDecodedOperand& operand) const {
        require_32bit_stack();
        std::uint64_t target{};
        if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative ||
            !ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded, &operand, pc, &target))) {
            throw Unsupported("Control transfer has no direct target");
        }
        return static_cast<Word>(target);
    }

    std::string jump(Word target) {
        const auto* range = maps.containing(target);
        const bool local = range && range->entry == entry;
        if (!local && !maps.functions.contains(target)) {
            throw Unsupported("Branch target is outside its function and is not an entry at " + hex(target));
        }
        edges.push_back({target, pc, decoded.length});
        if (local) return "goto " + label(target) + ";";
        return "return " + function_name(target) + "(cpu);";
    }

    static std::string calculate(std::string_view kind, const ZydisDecodedOperand& dst, std::string_view src) {
        return std::format("cpu.alu(Alu::{}, {}, {}, {})", kind, scalar_bits(dst.size), read_operand(dst), src);
    }

    static std::string arithmetic(std::string_view kind, const ZydisDecodedOperand& dst, std::string_view src) {
        return "const auto result = " + calculate(kind, dst, src) + "; " + write_operand(dst, "result");
    }
};

void write_cpp(const Path& output, const Functions& functions, const std::set<Word>& targets) {
    std::ofstream file(output, std::ios::binary | std::ios::noreplace);
    if (!file) throw std::runtime_error("Cannot create a new output file: " + output.string());

    try {
        file << "#include \"native_cpu.h\"\n\nnamespace native_x86 {\nnamespace {\n";
        for (const auto& [entry, function] : functions) {
            file << "std::uint32_t " << function_name(entry) << "(Cpu& cpu);\n";
        }
        for (const auto& [entry, body] : functions) {
            file << "\nstd::uint32_t " << function_name(entry) << "(Cpu& cpu) {\n";
            file << "    goto " << label(entry) << ";\n" << body.body << "}\n";
        }
        file << "\n} // namespace\n\nstd::uint32_t execute(Cpu& cpu, std::uint32_t entry) {\n"
             << "    switch (entry) {\n";
        for (const auto entry : targets) {
            file << "    case " << literal(entry) << ": return " << function_name(entry) << "(cpu);\n";
        }
        file << "    default: throw std::out_of_range(\"Unknown x86 function entry\");\n"
             << "    }\n}\n\n} // namespace native_x86\n";
        file.close();
        if (!file) throw std::runtime_error("Cannot finish writing generated C++");
    } catch (...) {
        file.close();
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        throw;
    }
}

int recompile(Image& image, const Path& directory, const Path& output) {
    Maps maps(directory);
    auto instructions = table(directory / "instructions.tsv", "address\tbytes\tinstruction\tflow");
    ZydisDecoder decoder{};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32))) {
        throw std::runtime_error("Cannot initialize the x86 decoder");
    }
    std::vector<Edge> edges;
    std::set<Word> starts, entries;
    std::uint64_t previous_end{};
    unsigned issues{};
    std::string row;
    while (line(instructions, row)) {
        const auto values = fields<4>(row);
        const auto pc = number(values[0], 16);
        const auto bytes = values[1];
        if (bytes.empty() || bytes.size() % 2 || bytes.size() > 2 * ZYDIS_MAX_INSTRUCTION_LENGTH) {
            throw std::runtime_error("Invalid byte column at " + hex(pc));
        }
        const auto length = static_cast<unsigned>(bytes.size() / 2);
        const auto* range = maps.containing(pc);
        if (!range || std::uint64_t{pc} + length - 1 > range->last || pc < previous_end) {
            throw std::runtime_error("Unordered instruction or instruction outside function at " + hex(pc));
        }
        previous_end = std::uint64_t{pc} + length;
        const auto original = image.read(pc, length);
        for (unsigned i = 0; i < length; ++i) {
            if (number(bytes.substr(i * 2, 2), 16) != original[i]) {
                throw std::runtime_error("Map bytes differ from raw image at " + hex(pc));
            }
        }
        starts.insert(pc);
        if (maps.functions.contains(pc)) entries.insert(pc);

        ZydisDecodedInstruction decoded{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        const auto edge_count = edges.size();
        try {
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, original, length, &decoded, operands.data())) ||
                decoded.length != length) {
                throw Unsupported("Decoder disagrees with the mapped instruction boundary");
            }
            auto body = Lowering(maps, edges, pc, range->entry, decoded).emit(operands.data());
            maps.functions.at(range->entry).body += std::format("{}: {{ {} }}\n", label(pc), body);
        } catch (const Unsupported& error) {
            edges.resize(edge_count);
            ++issues;
            std::cerr << hex(pc) << '\t' << bytes << '\t' << values[2] << '\t' << error.what() << '\n';
        }
    }
    if (!instructions.eof() || entries.size() != maps.functions.size()) {
        throw std::runtime_error("Instruction map is missing one or more function entries");
    }
    for (const auto& [target, source, length] : edges) {
        if (starts.contains(target)) continue;
        ++issues;
        std::string encoding;
        const auto bytes = image.read(source, length);
        for (unsigned i = 0; i < length; ++i) encoding += std::format("{:02X}", bytes[i]);
        std::cerr << hex(source) << '\t' << encoding << "\tMissing instruction at " << hex(target) << '\n';
    }
    if (issues) {
        std::cerr << issues << " lowering or control-flow issue(s)\n";
        return 3;
    }
    write_cpp(output, maps.functions, entries);
    std::cout << "Generated " << entries.size() << " functions from " << starts.size() << " instructions\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef XBOX_OFFLINE_BOOT
    if (argc > 1 && std::string_view(argv[1]) == "boot") {
        return xbox_native::boot_command(argc, argv);
    }
    if (argc == 4 && std::string_view(argv[1]) == "check-dialogue")
        return xbox_native::check_dialogue_stream(argv[2], argv[3]);
#endif
    if (argc >= 5 && std::string_view(argv[1]) == "xbe") {
        try {
            bool partial = false;
            std::filesystem::path disc;
            for (int i = 5; i < argc; ++i) {
                const std::string_view option = argv[i];
                if (option == "--allow-partial" && !partial) partial = true;
                else if (option == "--disc" && disc.empty() && i + 1 < argc) disc = argv[++i];
                else throw std::runtime_error("Invalid or duplicate XBE generation option");
            }
            return xbox_native::recompile_xbe(argv[2], argv[3], argv[4], partial, disc);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: native-recompiler IMAGE.bin BASE_ADDRESS MAP_DIRECTORY NEW_OUTPUT.cpp\n"
                     "       native-recompiler xbe DEFAULT.xbe MAP_DIRECTORY NEW_OUTPUT_DIRECTORY [--allow-partial] [--disc XISO]\n"
#ifdef XBOX_OFFLINE_BOOT
                     "       native-recompiler check-dialogue DEFAULT.xbe DISC.xiso\n"
                     "       native-recompiler boot DEFAULT.xbe CHECKPOINT_BUDGET [STOP_ADDRESS] [--disc XISO] [--language en|fr|de] [--audio] [--frame PNG] [--frame-index N] [--start-at-scanout N] [--shader-cache DIR]\n"
#endif
                     "Reads functions.tsv, function-ranges.tsv and instructions.tsv for a flat 32-bit x86 image.\n"
                     "Generated C++ uses native_cpu.h. Exit code 3 reports unsupported instructions or flow.\n";
        return 0;
    }
    if (argc != 5) {
        std::cerr << "Usage: native-recompiler IMAGE.bin BASE_ADDRESS MAP_DIRECTORY NEW_OUTPUT.cpp\n";
        return 1;
    }
    try {
        Image image(argv[1], number(argv[2]));
        return recompile(image, argv[3], argv[4]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
