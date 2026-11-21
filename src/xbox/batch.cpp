#include "batch.h"
#include "lower.h"
#include "boot_modules.h"
#include "boot_recovery.h"
#include "disc.h"
#include "platform_bindings.h"
#include "../analysis_map.h"

#include <algorithm>
#include <fstream>
#include <format>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>

namespace xbox_native {

std::optional<std::uint32_t> Instruction::target() const {
    const auto& operand = operands[0];
    if (operand.type == ZYDIS_OPERAND_TYPE_POINTER) return operand.ptr.offset;
    std::uint64_t result{};
    if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative ||
        !ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded, &operand, address, &result))) return {};
    return static_cast<std::uint32_t>(result);
}

namespace {
using analysis_map::Word;
using analysis_map::fields;
using analysis_map::line;
using analysis_map::number;
using analysis_map::table;
using Path = std::filesystem::path;

struct Row {
    Word address;
    unsigned length;
};

struct Site {
    Word address;
    std::string bytes, kind, reason;
    std::vector<Word> targets;
};

struct Unit {
    std::vector<Row> rows;
    std::vector<Word> dependencies;
    std::vector<Word> alternate_entries, additional_symbols;
    std::vector<Site> sites, bindings;
    bool lowered{}, closed{};
    bool recovered{}, recovered_from_static_flow{};
    std::string_view platform;
    std::map<Word, std::string_view> platform_alternates;
};

using Units = std::map<Word, Unit>;
struct JumpTable {
    std::vector<Word> targets;
    std::string_view evidence;
};
using JumpTargets = std::map<Word, JumpTable>;
bool falls_through(const Instruction& instruction);

std::optional<Word> absolute_word(const ZydisDecodedOperand& operand) {
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == 32 &&
        operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE &&
        operand.mem.segment == ZYDIS_REGISTER_DS && operand.mem.disp.has_displacement) {
        return static_cast<Word>(operand.mem.disp.value);
    }
    return {};
}

std::optional<Word> indexed_word(const ZydisDecodedOperand& operand) {
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == 32 &&
        operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index != ZYDIS_REGISTER_NONE &&
        operand.mem.scale == 4 && operand.mem.segment == ZYDIS_REGISTER_DS && operand.mem.disp.has_displacement) {
        return static_cast<Word>(operand.mem.disp.value);
    }
    return {};
}

bool file_backed_code(const Xbe& image, Word target) {
    return std::ranges::any_of(image.sections(), [&](const Section& section) {
        return section.executable() && target >= section.address &&
            target - section.address < std::min(section.size, section.raw_size);
    });
}

class CodeCache {
public:
    explicit CodeCache(Xbe& image) : image(image) {}

    Bytes read(Word address, unsigned length) {
        const auto bytes = window(address, length);
        if (bytes.size() != length) {
            throw std::runtime_error("Mapped instruction extends past XBE code at " + hex32(address));
        }
        return bytes;
    }

    Bytes window(Word address, unsigned length) {
        const std::uint64_t end = std::uint64_t{address} + length;
        if (address < first || end > std::uint64_t{first} + size) {
            size = image.code_bytes(address, data);
            first = address;
        }
        return Bytes(data).subspan(address - first, std::min<std::size_t>(length, size - (address - first)));
    }

private:
    Xbe& image;
    std::array<std::byte, 16 * 1024> data{};
    Word first{};
    std::size_t size{};
};

class Decoder {
public:
    Decoder() {
        if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32))) {
            throw std::runtime_error("Cannot initialize the native x86 decoder");
        }
    }

    Instruction decode(Word address, Bytes bytes) const {
        Instruction instruction;
        instruction.address = address;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data(), bytes.size(), &instruction.decoded,
                                                instruction.operands.data()))) {
            throw std::runtime_error("Unsupported mapped encoding at " + hex32(address) + " [" + hex_bytes(bytes) + "]");
        }
        std::ranges::copy(bytes.first(instruction.decoded.length), instruction.bytes.begin());
        instruction.text = ZydisMnemonicGetString(instruction.decoded.mnemonic);
        return instruction;
    }

private:
    ZydisDecoder decoder{};
};

struct ModuleObject {
    std::vector<std::byte> text;
    std::map<Word, Word> relocations;
    std::set<Word> unlinked_calls;
    std::set<Word> functions;
    std::vector<std::pair<Word, Word>> exports;
};

Bytes slice(Bytes bytes, std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("Truncated Xbox logic module object");
    return bytes.subspan(offset, size);
}

ModuleObject read_module(Bytes object, std::string_view name) {
    if (u16(object, 0) != 0x14C || u16(object, 2) != 1 || u16(object, 16) ||
        hex_bytes(slice(object, 20, 8)) != "2E74657874000000")
        throw std::runtime_error("Unsupported Xbox logic module COFF layout");
    const auto section = slice(object, u32(object, 40), u32(object, 36));
    const auto symbols = slice(object, u32(object, 8), std::size_t{18} * u32(object, 12));
    const auto strings = slice(object, u32(object, 8) + symbols.size(), object.size() - u32(object, 8) - symbols.size());
    const auto relocs = slice(object, u32(object, 44), std::size_t{10} * u16(object, 52));
    ModuleObject module;
    for (std::size_t offset = 0; offset < symbols.size();) {
        const auto symbol = slice(symbols, offset, 18);
        const auto value = u32(symbol, 8);
        const auto aux = std::to_integer<unsigned>(symbol[17]);
        if (u16(symbol, 12) == 1 && u16(symbol, 14) == 0x20) {
            module.functions.insert(value);
            const auto field = u32(symbol, 0) == 0 ? slice(strings, u32(symbol, 4), strings.size() - u32(symbol, 4)) : symbol.first(8);
            const std::string_view label(reinterpret_cast<const char*>(field.data()), field.size());
            const auto spelling = label.substr(0, label.find('\0'));
            if (spelling == "_" + std::string(name) + "Register") module.exports.emplace_back(value, 0x0003A6B5);
            if (spelling == "_" + std::string(name) + "Constructor") module.exports.emplace_back(value, 0x0003D42F);
        }
        if (aux && u16(symbol, 12) == 1 && hex_bytes(symbol.first(8)) == "2E74657874000000") {
            const auto text = slice(section, 0, u32(symbols, offset + 18));
            module.text.assign(text.begin(), text.end());
        }
        offset += 18 * (1 + aux);
        if (offset > symbols.size()) throw std::runtime_error("Invalid COFF symbol auxiliaries");
    }
    if (module.text.empty() || module.functions.empty() || *module.functions.rbegin() >= module.text.size())
        throw std::runtime_error("Logic module has no executable symbol index");
    std::set<Word> relocated;
    for (std::size_t offset = 0; offset < relocs.size(); offset += 10) {
        const auto at = u32(relocs, offset);
        const auto reference = slice(symbols, std::size_t{18} * u32(relocs, offset + 4), 18);
        const auto kind = u16(relocs, offset + 8);
        const auto section_number = u16(reference, 12);
        if ((kind != 6 && kind != 0x14) || section_number > 1)
            throw std::runtime_error(std::format("Unbound Xbox module relocation at {} (type {}, section {}, symbol {})",
                hex32(at), hex32(kind), hex32(section_number), hex_bytes(reference.first(8))));
        const auto value = u32(section, at) + u32(reference, 8);
        if (at < module.text.size()) {
            if (!relocated.insert(at).second || at + 4 > module.text.size())
                throw std::runtime_error("Invalid Xbox module relocation at " + hex32(at));
            if (kind == 6) module.relocations.emplace(at, value);
            else if (section_number == 0 && !value) {
                // At 0x0003A30B the loader assigns section zero the same base as section one.
                // A zero-valued external REL32 therefore leaves the encoded CALL to the next byte intact.
                module.unlinked_calls.insert(at);
            }
            else {
                // The original loader at 0x0003A38F resolves a same-section REL32 before execution.
                for (unsigned byte = 0; byte < 4; ++byte)
                    module.text[at + byte] = static_cast<std::byte>((value >> (byte * 8)) & 0xFF);
            }
        }
    }
    return module;
}

std::vector<Word> module_jump_targets(const ModuleObject& module, const Function& fn, const Instruction& jump) {
    if (!jump.relocated_displacement || !indexed_word(jump.operands[0])) {
        throw std::runtime_error("Unsupported Xbox logic module jump operand at " + hex32(jump.address));
    }
    const auto table = module.relocations.at(jump.address + jump.decoded.raw.disp.offset);
    std::optional<Word> count;
    auto previous = fn.instructions.lower_bound(jump.address);
    if (std::distance(fn.instructions.begin(), previous) >= 2) {
        const auto& above = (--previous)->second;
        const auto& compare = (--previous)->second;
        if (above.address + above.decoded.length == jump.address && compare.address + compare.decoded.length == above.address &&
            above.decoded.mnemonic == ZYDIS_MNEMONIC_JNBE && compare.decoded.mnemonic == ZYDIS_MNEMONIC_CMP &&
            compare.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && compare.operands[0].reg.value == jump.operands[0].mem.index &&
            compare.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && compare.operands[1].imm.value.u < 4096) {
            count = static_cast<Word>(compare.operands[1].imm.value.u) + 1;
        }
    }
    std::vector<Word> targets;
    for (Word i = 0; i < count.value_or(256); ++i) {
        const auto slot = module.relocations.find(table + 4 * i);
        if (slot == module.relocations.end()) break;
        if (slot->second >= module.text.size()) throw std::runtime_error("Module jump table destination leaves code");
        targets.push_back(slot->second);
    }
    if (targets.empty() || (count && *count != targets.size()) ||
        (!count && targets.size() == 256 && module.relocations.contains(table + 4 * 256))) {
        throw std::runtime_error("Unbounded Xbox logic module jump table at " + hex32(jump.address));
    }
    return targets;
}

Function module_function(Word entry, const ModuleObject& module, const Decoder& decoder) {
    Function function;
    function.entry = entry;
    function.relocatable = true;
    std::vector<Word> pending{entry};
    while (!pending.empty()) {
        const auto address = pending.back();
        pending.pop_back();
        if (function.instructions.contains(address)) continue;
        if (address >= module.text.size()) throw std::runtime_error("Xbox module flow outside code");
        auto ins = decoder.decode(address, Bytes(module.text).subspan(address, std::min(module.text.size() - address, std::size_t{15})));
        const auto next = address + ins.decoded.length;
        const auto category = ins.decoded.meta.category;
        const bool calls = category == ZYDIS_CATEGORY_CALL;
        const bool branches = category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR;
        for (auto relocation = module.relocations.lower_bound(address);
             relocation != module.relocations.end() && relocation->first < next; ++relocation) {
            const auto offset = relocation->first - address;
            const auto& raw = ins.decoded.raw;
            if (raw.disp.size == 32 && offset == raw.disp.offset) ins.relocated_displacement = true;
            else if (raw.imm[0].size == 32 && !raw.imm[0].is_relative && offset == raw.imm[0].offset)
                ins.relocated_immediate = true;
            else throw std::runtime_error("Relocation is not an operand at " + hex32(relocation->first));
        }
        const auto unlinked = module.unlinked_calls.lower_bound(address);
        if (unlinked != module.unlinked_calls.end() && *unlinked < next) {
            if (!calls || *unlinked != address + ins.decoded.raw.imm[0].offset || ins.target() != next)
                throw std::runtime_error("Unbound relative module operand at " + hex32(*unlinked));
            ins.call_next_instruction = true;
        } else if (const auto target = (branches || calls) ? ins.target() : std::nullopt) {
            if (calls) {
                if (!module.functions.contains(*target))
                    throw std::runtime_error("Unindexed Xbox module call target " + hex32(*target));
            } else if (*target != entry && module.functions.contains(*target)) {
                ins.cross_function_branch = true;
            } else {
                pending.push_back(*target);
                if (*target <= address) function.loop_headers.push_back(*target);
            }
        } else if (category == ZYDIS_CATEGORY_UNCOND_BR) {
            for (const auto target : module_jump_targets(module, function, ins)) {
                if (target != entry && module.functions.contains(target)) ins.external_jump_targets.push_back(target);
                else {
                    ins.jump_targets.push_back(target);
                    pending.push_back(target);
                    if (target <= address) function.loop_headers.push_back(target);
                }
            }
        }
        if (falls_through(ins)) {
            if (next != entry && module.functions.contains(next)) ins.cross_function_fallthrough = true;
            else pending.push_back(next);
        }
        function.instructions.emplace(address, std::move(ins));
    }
    Word end = 0;
    for (const auto& [pc, ins] : function.instructions) {
        if (pc < end) throw std::runtime_error("Overlapping instructions in Xbox logic module");
        end = pc + ins.decoded.length;
    }
    return function;
}

struct LogicRecord {
    std::string name, path, hash;
    DiscEntry file;
    Word object, size;
};

struct LogicIndex {
    std::vector<LogicRecord> records;
    std::size_t files{}, instances{};
};

LogicIndex logic_records(Disc& disc) {
    constexpr std::size_t chunk = 256 * 1024, max_object = 1024 * 1024;
    constexpr std::string_view magic("\x4C\x01", 2), text_section(".text\0\0\0", 8), data_section(".data\0\0\0", 8), bss_section(".bss\0\0\0\0", 8);
    std::vector<std::byte> data(chunk + magic.size() - 1);
    std::map<std::string, LogicRecord> unique;
    LogicIndex index;
    disc.visit_files([&](std::string_view path, DiscEntry file) {
        if (path.size() < 4 || !std::ranges::equal(path.substr(path.size()-4),std::string_view(".XBR"),[](char a, char b) {
            return (a >= 'a' && a <= 'z' ? a-'a'+'A' : a) == b;
        })) return;
        ++index.files;
        for (std::uint64_t offset = 0; offset < file.size; offset += chunk) {
            const auto region = std::span(data).first(std::min<std::uint64_t>(data.size(), file.size - offset));
            disc.read(file, offset, region);
            const std::string_view block(reinterpret_cast<const char*>(region.data()), region.size());
            for (auto position = block.find(magic); position < chunk; position = block.find(magic, position + 1)) {
                const auto object = static_cast<Word>(offset + position);
                if (file.size - object < 60 || object < 14) continue;
                std::array<std::byte, 60> header;
                disc.read(file, object, header);
                const std::string_view bytes(reinterpret_cast<const char*>(header.data()), header.size());
                const auto section = bytes.substr(20, 8);
                if ((section != text_section && section != data_section && section != bss_section) || u16(header, 16)) continue;
                const auto behind = std::min(object, 12U + 256);
                std::array<std::byte, 12 + 256> envelope;
                const auto wrapped = std::span(envelope).first(behind);
                disc.read(file, object - behind, wrapped);
                std::string_view name;
                for (Word i = 0; i + 14 <= behind; ++i) {
                    const auto length = u32(wrapped, i + 8);
                    if (length != behind - i - 12) continue;
                    const auto text = reinterpret_cast<const char*>(wrapped.data() + i + 12);
                    const std::string_view padded(text, length);
                    const auto end = padded.find('\0');
                    if (!end || end == std::string_view::npos || padded.find_first_not_of('\0', end) != std::string_view::npos) continue;
                    const auto candidate = padded.substr(0, end);
                    if (candidate.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string_view::npos) continue;
                    if (!name.empty()) throw std::runtime_error("Ambiguous XBR logic record in " + std::string(path));
                    name = candidate;
                }
                if (name.empty()) continue;
                if (u16(header,2) != 1 || section != text_section || u32(header,40) != 60) {
                    throw std::runtime_error("Unrecognized COFF section layout for " + std::string(name) + " in " + std::string(path));
                }
                const auto strings = std::uint64_t{u32(header, 8)} + std::uint64_t{u32(header, 12)} * 18;
                if (strings + 4 > file.size - object || strings + 4 > max_object) throw std::runtime_error("XBR COFF symbols exceed their bounds in " + std::string(path));
                std::array<std::byte, 4> length;
                disc.read(file, object + strings, length);
                const auto size = strings + u32(length, 0);
                if (u32(length, 0) < 4 || size > file.size - object || size > max_object)
                    throw std::runtime_error("XBR COFF strings exceed their bounds in " + std::string(path));
                const auto hash = disc.file_sha256(file, object, size);
                ++index.instances;
                unique.try_emplace(hash, LogicRecord{std::string(name), std::string(path), hash, file, object, static_cast<Word>(size)});
            }
        }
    });
    if (unique.empty()) throw std::runtime_error("Verified disc has no XBR logic records");
    for (auto& [hash, record] : unique) index.records.push_back(std::move(record));
    return index;
}

struct GeneratedModules { std::string summary; std::size_t xbr_files{}, object_instances{}, unresolved_calls{}; };

GeneratedModules generate_modules(const Path& disc, const Xbe& image, const Decoder& decoder, const Path& staging,
                                  std::map<std::string, std::string>& assembly) {
    std::optional<Disc> media;
    LogicIndex inventory;
    if (!disc.empty()) {
        media.emplace(disc);
        if (media->executable_sha256() != image.sha256)
            throw std::runtime_error("XISO executable differs from the verified XBE");
        if (media->disc_sha256() != verified_disc_sha256)
            throw std::runtime_error("XISO differs from the verified source media");
        inventory = logic_records(*media);
        for (const auto& observed : boot_modules) {
            if (std::ranges::find(inventory.records, observed.object_hash, &LogicRecord::hash) == inventory.records.end()) {
                throw std::runtime_error("Missing verified XBR logic object " + std::string(observed.name));
            }
        }
    }
    std::ostringstream declarations, index, summary;
    std::size_t count = 0, registry_size = 0, unresolved_calls = 0;
    std::ofstream unresolved;
    for (const auto& entry : inventory.records) {
        std::vector<std::byte> bytes(entry.size);
        media->read(entry.file, entry.object, bytes);
        const auto module = [&] {
            try {
                return read_module(bytes, entry.name);
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(entry.path + " (" + entry.name + "): " + error.what());
            }
        }();
        if (module.exports.empty()) throw std::runtime_error("No named registration export for " + entry.name + " in " + entry.path);
        const auto ns = "module_" + entry.hash;
        std::ostringstream output;
        output << "#include \"xbox/cpu.h\"\nnamespace xbox_native {\nnamespace " << ns << " {\n";
        for (auto address : module.functions) output << std::format("void guest_{:08X}(Cpu&, Memory&);\n", address);
        for (auto address : module.functions) {
            const auto lowered = [&] {
                try {
                    const auto fn = module_function(address, module, decoder);
                    for (const auto& [pc, ins] : fn.instructions) {
                        if (ins.decoded.meta.category != ZYDIS_CATEGORY_CALL || ins.target()) continue;
                        if (!unresolved_calls) {
                            unresolved.exceptions(std::ios::failbit | std::ios::badbit);
                            unresolved.open(staging / "module-indirect.tsv");
                            unresolved << "object_sha256\tfunction_offset\tsite_offset\tbytes\n";
                        }
                        ++unresolved_calls;
                        unresolved << entry.hash << '\t' << hex32(address) << '\t' << hex32(pc) << '\t'
                                   << hex_bytes(Bytes(ins.bytes).first(ins.decoded.length)) << '\n';
                    }
                    return lower(fn);
                } catch (const std::runtime_error& error) {
                    throw std::runtime_error(entry.path + " (" + entry.name + " at " + hex32(address) + "): " + error.what());
                }
            }();
            if (!lowered.issues.empty()) {
                const auto& issue = lowered.issues.front();
                throw std::runtime_error("Cannot lower Xbox logic module " + std::string(entry.name) + " at " +
                                         hex32(issue.address) + " [" + issue.bytes + "]: " + issue.reason);
            }
            output << lowered.source;
            for (const auto& [helper, definition] : lowered.native_helpers) assembly.emplace(helper, definition);
        }
        output << "constexpr CompiledFunction functions[] = {\n";
        for (auto address : module.functions) {
            output << std::format("    {{{}U, \"{}\", guest_{:08X}}},\n", hex32(address), image.sha256, address);
        }
        output << "};\nconstexpr std::array<ModuleRelocation," << module.relocations.size() << "> relocations{{\n";
        for (const auto [offset, value] : module.relocations)
            output << std::format("    {{{}U, {}U}},\n", hex32(offset), hex32(value));
        output << "}};\nconstexpr std::string_view code =\n";
        for (std::size_t offset = 0; offset < module.text.size(); offset += 64) {
            output << "    \"" << hex_bytes(Bytes(module.text).subspan(offset, std::min(module.text.size() - offset, std::size_t{64}))) << "\"\n";
        }
        output << ";\n}\n";
        for (const auto [offset, caller] : module.exports) {
            const auto symbol = std::format("{}_{}", ns, caller);
            declarations << "extern const CompiledModule " << symbol << ";\n";
            output << std::format("extern constinit const CompiledModule {}{{\"{}\", {}U, {}U, {}::code, {}::relocations, {}::functions}};\n",
                                  symbol, entry.name, hex32(offset), hex32(caller), ns, ns, ns);
            index << "    " << symbol << ",\n";
            ++registry_size;
        }
        output << "}\n";
        write_text(staging / ("game.logic." + entry.hash + ".cpp"), output.str());
        if (count) summary << ',';
        summary << "{\"name\":" << json(entry.name) << ",\"disc_path\":" << json(entry.path)
                << ",\"object_offset\":" << entry.object << ",\"object_sha256\":" << json(entry.hash)
                << ",\"functions_lowered\":" << module.functions.size()
                << ",\"entry_offset\":" << json(hex32(module.exports.front().first)) << '}';
        ++count;
    }
    const auto module_index = std::format("#include \"xbox/cpu.h\"\nnamespace xbox_native {{\n{}namespace {{\n"
        "const std::array<CompiledModule, {}> modules{{{{\n{}}}}};\n}}\n"
        "const std::span<const CompiledModule> compiled_modules{{modules}};\n}}\n", declarations.str(), registry_size, index.str());
    write_text(staging / "game.module.cpp", module_index);
    if (unresolved.is_open()) unresolved.close();
    return {summary.str(),inventory.files,inventory.instances,unresolved_calls};
}

struct Program;
std::vector<Word> guarded_jump_targets(const Instruction&, Word, const Program&, Xbe&, CodeCache&, const Decoder&);

struct Program {
    analysis_map::Maps map;
    Units units;
    std::map<Word, Word> alternate_owners;
    std::map<Word, std::set<Word>> slot_targets;
    std::map<Word, Word> indexed_jumps;
    std::vector<Word> starts;
    std::size_t mapped_rows{}, split_waits{}, recovered_instructions{}, static_instructions{};

    Program(const Path& directory, Xbe& image, CodeCache& code, const Decoder& decoder) : map(directory) {
        for (const auto& [entry, metadata] : map.functions) units.try_emplace(entry);
        if (!units.contains(image.entry)) throw std::runtime_error("XBE entry is missing from the function map");
        auto input = table(directory / "instructions.tsv", "address\tbytes\tinstruction\tflow");
        std::string row;
        std::uint64_t previous_end{};
        while (line(input, row)) {
            const auto [address, bytes, text, flow] = fields<4>(row);
            const auto pc = number(address, 16);
            if (bytes.empty() || bytes.size() % 2 || bytes.size() > 2 * (ZYDIS_MAX_INSTRUCTION_LENGTH + 1)) {
                throw std::runtime_error("Invalid byte column at " + hex32(pc));
            }
            const auto length = static_cast<unsigned>(bytes.size() / 2);
            const auto* range = map.containing(pc);
            if (!range || std::uint64_t{pc} + length - 1 > range->last || pc < previous_end) {
                throw std::runtime_error("Unordered or out-of-range instruction at " + hex32(pc));
            }
            previous_end = std::uint64_t{pc} + length;
            const auto original = code.read(pc, length);
            if (hex_bytes(original) != bytes) {
                throw std::runtime_error("Instruction map bytes differ from the XBE at " + hex32(pc));
            }
            auto& instructions = units.at(range->entry).rows;
            const auto instruction = decoder.decode(pc, original);
            const auto& decoded = instruction.decoded;
            if (decoded.length != length) {
                if (decoded.mnemonic != ZYDIS_MNEMONIC_FWAIT || decoded.length != 1 ||
                    decoder.decode(pc + 1, original.subspan(1)).decoded.length != length - 1) {
                    throw std::runtime_error("Instruction map boundary differs from the XBE at " + hex32(pc));
                }
                add(instructions, pc, 1);
                add(instructions, pc + 1, length - 1);
                ++split_waits;
            } else add(instructions, pc, length);
            ++mapped_rows;
        }
        if (!input.eof()) throw std::runtime_error("Incomplete XBE instruction map");
        recover(image, code, decoder);
        recover_static_flow(image, code, decoder);
        for (const auto [address, owner] : alternate_owners) units.at(owner).alternate_entries.push_back(address);
        for (const auto& [entry, unit] : units) {
            if (!mapped(entry)) throw std::runtime_error("Map has no instruction for entry " + hex32(entry));
        }
        for (const auto& [api, name, prefix] : platform_bindings) {
            const auto entry = static_cast<Word>(api);
            const auto owner = callable_owner(entry);
            if (!owner || prefix.size() < 8 || prefix.size() % 2 ||
                hex_bytes(code.read(entry, static_cast<unsigned>(prefix.size() / 2))) != prefix) {
                throw std::runtime_error("Platform binding does not match verified XBE function " + hex32(entry));
            }
            auto& unit = units.at(*owner);
            if (entry == *owner) {
                if (!unit.platform.empty()) throw std::runtime_error("Duplicate platform binding " + hex32(entry));
                unit.platform = name;
            } else if (!unit.platform_alternates.emplace(entry, name).second) {
                throw std::runtime_error("Duplicate platform binding " + hex32(entry));
            }
        }
    }

    bool mapped(Word address) const { return std::ranges::binary_search(starts, address); }

    bool local(Word address, Word entry) const {
        const auto* range = map.containing(address);
        return range && range->entry == entry && mapped(address);
    }

    std::optional<Word> callable_owner(Word address) const {
        if (units.contains(address)) return address;
        const auto alternate = alternate_owners.find(address);
        if (alternate != alternate_owners.end()) return alternate->second;
        return {};
    }

private:
    void recover_static_flow(Xbe& image, CodeCache& code, const Decoder& decoder) {
        struct Edge { Word destination, owner; bool call; };
        std::vector<Edge> pending;
        std::set<Word> slots;
        std::map<Word, std::set<Word>> assignments;
        const auto follow = [&](const Instruction& instruction, Word owner) {
            const auto category = instruction.decoded.meta.category;
            if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) {
                if (const auto target = instruction.target(); target &&
                    (category == ZYDIS_CATEGORY_CALL ? !callable_owner(*target) : !local(*target, owner))) {
                    pending.push_back({*target, owner, category == ZYDIS_CATEGORY_CALL});
                }
            }
            if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_UNCOND_BR) {
                if (const auto slot = absolute_word(instruction.operands[0]); slot && !image.kernel_import(*slot)) {
                    slots.insert(*slot);
                }
            }
            if (category == ZYDIS_CATEGORY_UNCOND_BR) {
                if (const auto table = indexed_word(instruction.operands[0])) {
                    indexed_jumps.emplace(instruction.address, *table);
                }
            }
            if (instruction.decoded.mnemonic == ZYDIS_MNEMONIC_MOV &&
                instruction.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && instruction.operands[1].size == 32) {
                if (const auto slot = absolute_word(instruction.operands[0])) {
                    assignments[*slot].insert(static_cast<Word>(instruction.operands[1].imm.value.u));
                }
            }
            const auto next = instruction.address + instruction.decoded.length;
            if (falls_through(instruction) && !local(next, owner)) pending.push_back({next, owner, false});
        };
        for (const auto& [owner, unit] : units) {
            for (const auto [address, length] : unit.rows) follow(decoder.decode(address, code.read(address, length)), owner);
        }
        // Each recovery starts at a verified direct edge and ends where decoded control flow ends or rejoins the map.
        constexpr std::size_t instruction_limit = 4096;
        std::size_t added = 0;
        std::set<Word> rejected;
        std::set<Word> guarded_tables;
        for (std::size_t i = 0;; ++i) {
            if (i == pending.size()) {
                for (const auto slot : slots) {
                    auto& targets = slot_targets[slot];
                    const auto add = [&](Word target) {
                        if (file_backed_code(image, target) && targets.insert(target).second) {
                            pending.push_back({target, target, true});
                        }
                    };
                    if (const auto initial = image.initial_word(slot)) add(*initial);
                    if (const auto found = assignments.find(slot); found != assignments.end()) {
                        for (const auto target : found->second) add(target);
                    }
                }
                for (const auto& [site, base] : indexed_jumps) {
                    if (guarded_tables.contains(site)) continue;
                    const auto owner = map.containing(site)->entry;
                    const auto jump = decoder.decode(site, code.window(site, ZYDIS_MAX_INSTRUCTION_LENGTH));
                    const auto targets = guarded_jump_targets(jump, owner, *this, image, code, decoder);
                    if (targets.empty()) continue;
                    guarded_tables.insert(site);
                    for (const auto target : targets) {
                        if (!local(target, owner)) pending.push_back({target, owner, false});
                    }
                }
                if (i == pending.size()) break;
            }
            const auto [destination, predecessor, call] = pending[i];
            if (callable_owner(destination)) continue;
            const auto location = std::ranges::lower_bound(starts, destination);
            const auto* containing = map.containing(destination);
            if (location != starts.end() && *location == destination) {
                if (containing->entry != predecessor || call) alternate_owners.emplace(destination, containing->entry);
                continue;
            }
            if (rejected.contains(destination)) continue;
            Instruction instruction;
            try {
                if (location != starts.begin()) {
                    const auto previous = *(location - 1);
                    if (destination - previous < ZYDIS_MAX_INSTRUCTION_LENGTH &&
                        previous + decoder.decode(previous, code.window(previous, ZYDIS_MAX_INSTRUCTION_LENGTH)).decoded.length > destination) {
                        rejected.insert(destination);
                        continue;
                    }
                }
                instruction = decoder.decode(destination, code.window(destination, ZYDIS_MAX_INSTRUCTION_LENGTH));
            } catch (const std::runtime_error&) {
                rejected.insert(destination);
                continue;
            }
            auto length = instruction.decoded.length;
            if ((location != starts.end() && destination + length > *location) ||
                (containing && destination + length - 1 > containing->last)) {
                rejected.insert(destination);
                continue;
            }
            if (++added > instruction_limit) throw std::runtime_error("Static flow recovery exceeded its instruction bound");
            const auto owner = containing ? containing->entry : call ? destination : predecessor;
            if (!containing) {
                auto after = std::ranges::upper_bound(map.ranges, destination, {}, &analysis_map::Range::first);
                if (after != map.ranges.end() && after->first < destination + length) {
                    rejected.insert(destination);
                    continue;
                }
                map.ranges.insert(after, {destination, destination + length - 1U, owner});
            }
            auto [unit, inserted] = units.try_emplace(owner);
            if (inserted) {
                unit->second.recovered = true;
                unit->second.recovered_from_static_flow = true;
            }
            if (owner != destination && (call || owner != predecessor)) alternate_owners.emplace(destination, owner);
            starts.insert(location, destination);
            unit->second.rows.push_back({destination, length});
            ++static_instructions;
            follow(instruction, owner);
        }
    }

    void recover(Xbe& image, CodeCache& code, const Decoder& decoder) {
        for (const auto [site, start, end, consumer] : boot_recovery) {
            const bool data_slot = !mapped(site);
            if ((data_slot && (!consumer || (site & 3))) ||
                (end && (start >= end || end - start > 4096)) || units.contains(start)) {
                throw std::runtime_error("Invalid boot recovery range at " + hex32(start));
            }
            const auto source = data_slot ? Instruction{} : decoder.decode(site, code.read(site, 15));
            const bool continuation = end && !consumer &&
                ((source.address + source.decoded.length == start && falls_through(source)) ||
                 (source.decoded.meta.category == ZYDIS_CATEGORY_COND_BR && source.target() == start));
            const bool registration = consumer && source.decoded.mnemonic == ZYDIS_MNEMONIC_PUSH &&
                source.operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                source.operands[0].imm.value.u == start;
            const bool store = source.decoded.mnemonic == ZYDIS_MNEMONIC_MOV &&
                source.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && source.operands[0].size == 32 &&
                source.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && source.operands[1].imm.value.u == start;
            if (consumer) {
                if (!mapped(consumer)) throw std::runtime_error("Missing callback consumer at " + hex32(consumer));
                const auto call = decoder.decode(consumer, code.read(consumer, 15));
                const auto absolute = [](const ZydisDecodedOperand& operand) {
                    return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == 32 &&
                        operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE &&
                        operand.mem.segment == ZYDIS_REGISTER_DS && operand.mem.disp.has_displacement;
                };
                if (data_slot) {
                    if (image.data_word(site) != start || call.decoded.meta.category != ZYDIS_CATEGORY_CALL || call.target()) {
                        throw std::runtime_error("Unverified static callback slot at " + hex32(site));
                    }
                } else if (registration) {
                    auto pc = site + source.decoded.length;
                    for (; pc < consumer && consumer - site <= 32 && mapped(pc);) {
                        const auto next = decoder.decode(pc, code.read(pc, 15));
                        if (!falls_through(next) || next.decoded.meta.category == ZYDIS_CATEGORY_CALL) break;
                        pc += next.decoded.length;
                    }
                    const auto& destination = call.operands[0];
                    const bool publish_frame = call.decoded.mnemonic == ZYDIS_MNEMONIC_MOV &&
                        destination.type == ZYDIS_OPERAND_TYPE_MEMORY && destination.size == 32 &&
                        destination.mem.segment == ZYDIS_REGISTER_FS && destination.mem.base == ZYDIS_REGISTER_NONE &&
                        destination.mem.index == ZYDIS_REGISTER_NONE && destination.mem.disp.value == 0 &&
                        call.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && call.operands[1].reg.value == ZYDIS_REGISTER_ESP;
                    const bool direct_call = call.decoded.meta.category == ZYDIS_CATEGORY_CALL &&
                        call.target() && units.contains(*call.target());
                    if (pc != consumer || (!publish_frame && !direct_call)) {
                        throw std::runtime_error("Unverified callback registration at " + hex32(site));
                    }
                } else if (!store || !absolute(source.operands[0]) ||
                    call.decoded.meta.category != ZYDIS_CATEGORY_CALL || !absolute(call.operands[0]) ||
                    source.operands[0].mem.disp.value != call.operands[0].mem.disp.value) {
                    throw std::runtime_error("Unverified boot callback at " + hex32(site));
                }
            } else if (!continuation && !store) {
                const auto category = source.decoded.meta.category;
                if ((category != ZYDIS_CATEGORY_CALL && category != ZYDIS_CATEGORY_UNCOND_BR) ||
                    source.target() != start) {
                    throw std::runtime_error("Unverified boot recovery branch at " + hex32(site));
                }
            }
            if (!end) {
                const auto* range = map.containing(start);
                if (!mapped(start) || !range || !alternate_owners.emplace(start, range->entry).second) {
                    throw std::runtime_error("Boot alias does not name a mapped instruction at " + hex32(start));
                }
                continue;
            }
            auto after = std::ranges::upper_bound(map.ranges, start, {}, &analysis_map::Range::first);
            if (registration && after != map.ranges.end() && after->first < end &&
                after->entry == after->first && after->last < end &&
                std::ranges::count(map.ranges, after->entry, &analysis_map::Range::entry) == 1) {
                for (auto pc = start; pc < after->entry;) {
                    const auto instruction = decoder.decode(pc, code.read(pc, std::min(end - pc, 15U)));
                    pc += instruction.decoded.length;
                    if (pc <= after->entry) continue;
                    // The apparent entry starts within an instruction of the registered callback.
                    for (const auto [address, length] : units.at(after->entry).rows) {
                        starts.erase(std::ranges::lower_bound(starts, address));
                    }
                    units.erase(after->entry);
                    after = map.ranges.erase(after);
                    break;
                }
            }
            if ((after != map.ranges.end() && after->first < end) ||
                (after != map.ranges.begin() && (after - 1)->last >= start)) {
                throw std::runtime_error("Boot recovery overlaps a mapped function at " + hex32(start));
            }
            const auto owner = continuation ? map.containing(site)->entry : start;
            map.ranges.insert(after, {start, end - 1, owner});
            auto& unit = units[owner];
            if (!continuation) unit.recovered = true;
            std::vector<Word> recovered_starts;
            for (auto pc = start; pc < end;) {
                const auto instruction = decoder.decode(pc, code.read(pc, std::min(end - pc, 15U)));
                unit.rows.push_back({pc, instruction.decoded.length});
                recovered_starts.push_back(pc);
                ++recovered_instructions;
                pc += instruction.decoded.length;
                if (pc == end && instruction.decoded.meta.category != ZYDIS_CATEGORY_RET &&
                    instruction.decoded.meta.category != ZYDIS_CATEGORY_UNCOND_BR &&
                    !(continuation && local(end, owner))) {
                    throw std::runtime_error("Recovered function does not end in a return or jump at " + hex32(start));
                }
            }
            starts.insert(std::ranges::lower_bound(starts, start), recovered_starts.begin(), recovered_starts.end());
        }
    }

    void add(std::vector<Row>& instructions, Word address, unsigned length) {
        instructions.push_back({address, length});
        starts.push_back(address);
    }
};

bool falls_through(const Instruction& instruction) {
    const auto category = instruction.decoded.meta.category;
    return category != ZYDIS_CATEGORY_UNCOND_BR && category != ZYDIS_CATEGORY_RET &&
        instruction.decoded.mnemonic != ZYDIS_MNEMONIC_INT3 && instruction.decoded.mnemonic != ZYDIS_MNEMONIC_INT;
}

std::vector<Word> guarded_jump_targets(const Instruction& jump, Word entry, const Program& program,
                                       Xbe& image, CodeCache& code, const Decoder& decoder) {
    const auto& operand = jump.operands[0];
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.size != 32 ||
        operand.mem.base != ZYDIS_REGISTER_NONE || operand.mem.index == ZYDIS_REGISTER_NONE || operand.mem.scale != 4 ||
        operand.mem.segment != ZYDIS_REGISTER_DS || !operand.mem.disp.has_displacement) return {};
    auto cursor = std::ranges::lower_bound(program.starts, jump.address);
    auto next = jump.address;
    Instruction guard;
    for (unsigned i = 0; i <= 4 && cursor != program.starts.begin(); ++i) {
        const auto address = *--cursor;
        if (next - address > ZYDIS_MAX_INSTRUCTION_LENGTH || !program.local(address, entry)) return {};
        guard = decoder.decode(address, code.read(address, next - address));
        if (address + guard.decoded.length != next) return {};
        if (guard.decoded.mnemonic != ZYDIS_MNEMONIC_PUSH) break;
        next = address;
    }
    if (guard.decoded.mnemonic != ZYDIS_MNEMONIC_JNBE || cursor == program.starts.begin()) return {};
    const auto bound_address = *--cursor, guard_address = guard.address;
    if (guard_address - bound_address > ZYDIS_MAX_INSTRUCTION_LENGTH || !program.local(bound_address, entry)) return {};
    const auto bound = decoder.decode(bound_address, code.read(bound_address, guard_address - bound_address));
    if (bound_address + bound.decoded.length != guard_address || bound.decoded.mnemonic != ZYDIS_MNEMONIC_CMP ||
        bound.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || bound.operands[0].reg.value != operand.mem.index ||
        bound.operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || bound.operands[1].imm.value.u > 256)
        return {};
    const auto fallback = guard.target();
    const auto table_address = static_cast<Word>(operand.mem.disp.value);
    if (!fallback || *fallback <= jump.address || !program.local(*fallback, entry) || (table_address & 3)) return {};
    std::vector<Word> targets;
    try {
        for (Word index = 0; index <= bound.operands[1].imm.value.u; ++index) {
            const auto address = std::uint64_t{table_address} + index * 4;
            if (address >= UINT32_MAX - 3) return {};
            if (program.map.containing(static_cast<Word>(address))) return {};
            const auto target = image.data_word(static_cast<Word>(address));
            if (!file_backed_code(image, target)) return {};
            targets.push_back(target);
        }
    } catch (const std::runtime_error&) {
        return {};
    }
    return targets;
}

JumpTargets jump_targets(const Path& directory, const Program& program, Xbe& image,
                         CodeCache& code, const Decoder& decoder) {
    auto input = table(directory / "references.tsv", "from\tto\ttype\toperand");
    std::map<Word, std::set<Word>> references;
    std::string row;
    while (line(input, row)) {
        const auto [from, to, type, operand] = fields<4>(row);
        if (type == "COMPUTED_JUMP" && !to.empty() &&
            to.find_first_not_of("0123456789ABCDEFabcdef") == std::string_view::npos) {
            references[number(from, 16)].insert(number(to, 16));
        }
    }
    if (!input.eof()) throw std::runtime_error("Incomplete XBE reference map");

    struct TableExtent { Word jump, base; int first; unsigned count; };
    constexpr std::array corrections{
        TableExtent{0x000BB62D, 0x000BB640, 1, 3},   // Nonzero destination alignment selected at 0x000BB606.
        TableExtent{0x000BB634, 0x000BB73C, -4, 4},  // Carry from subtracting four, 0x000BB623.
        TableExtent{0x000BB7A6, 0x000BB878, -7, 8},  // Count under eight negated at 0x000BB7A4.
        TableExtent{0x000BB7C1, 0x000BB7CC, 1, 3},   // Nonzero destination alignment selected at 0x000BB78A.
        TableExtent{0x0011E34B, 0x0011E388, 0, 8},   // Depth format 0x2A through 0x31. The analyzed references name a different table.
    };
    std::set<Word> bases;
    for (const auto& [site, base] : program.indexed_jumps) bases.insert(base);
    JumpTargets result;
    for (const auto [site, base] : program.indexed_jumps) {
        const auto entry = program.map.containing(site)->entry;
        const auto jump = decoder.decode(site, code.window(site, ZYDIS_MAX_INSTRUCTION_LENGTH));
        if (const auto known = std::ranges::find(corrections, site, &TableExtent::jump); known != corrections.end()) {
            if (base != known->base) throw std::runtime_error("Corrected jump table operand differs at " + hex32(site));
            auto& resolved = result[site];
            resolved.evidence = "Verified signed index range and original table words";
            for (unsigned i = 0; i < known->count; ++i) {
                const auto cell = base + static_cast<Word>((known->first + static_cast<int>(i)) * 4);
                const auto target = image.data_word(cell);
                if (program.mapped(cell) || (!program.local(target, entry) && !program.callable_owner(target))) {
                    throw std::runtime_error("Corrected jump table target lacks compiled code at " + hex32(cell));
                }
                resolved.targets.push_back(target);
            }
            continue;
        }
        if (auto guarded = guarded_jump_targets(jump, entry, program, image, code, decoder); !guarded.empty() &&
            std::ranges::all_of(guarded, [&](Word target) {
                return program.local(target, entry) || program.callable_owner(target).has_value();
            })) {
            result.emplace(site, JumpTable{std::move(guarded), "Unsigned compare and original table words"});
            continue;
        }
        const auto known = references.find(site);
        if (known == references.end()) continue;
        const auto code_after = std::ranges::lower_bound(program.starts, base);
        const auto table_after = bases.upper_bound(base);
        auto end = std::uint64_t{base} + 4 * 256;
        if (code_after != program.starts.end()) end = std::min(end, std::uint64_t{*code_after});
        if (table_after != bases.end()) end = std::min(end, std::uint64_t{*table_after});
        std::vector<Word> words;
        for (auto cell = std::uint64_t{base}; cell + 4 <= end; cell += 4) {
            const auto target = image.initial_word(static_cast<Word>(cell));
            if (!target || (!program.local(*target, entry) && !program.callable_owner(*target))) break;
            words.push_back(*target);
        }
        if (std::set<Word>(words.begin(), words.end()) == known->second) {
            result.emplace(site, JumpTable{std::move(words), "Original table words match all indexed control-flow references"});
        }
    }
    for (const auto& [site, expected] : references) {
        if (program.indexed_jumps.contains(site) || !program.mapped(site)) continue;
        const auto jump = decoder.decode(site, code.window(site, ZYDIS_MAX_INSTRUCTION_LENGTH));
        if (jump.decoded.mnemonic != ZYDIS_MNEMONIC_JMP || jump.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) continue;
        const auto entry = program.map.containing(site)->entry;
        const auto reg = jump.operands[0].reg.value;
        std::set<Word> assigned;
        for (const auto& [address, length] : program.units.at(entry).rows) {
            const auto ins = decoder.decode(address, code.read(address, length));
            if (ins.decoded.mnemonic == ZYDIS_MNEMONIC_MOV && ins.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ins.operands[0].reg.value == reg && ins.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                const auto value = static_cast<Word>(ins.operands[1].imm.value.u);
                if (program.local(value, entry)) assigned.insert(value);
            }
        }
        if (assigned == expected) {
            result.emplace(site, JumpTable{{assigned.begin(), assigned.end()}, "Indexed references match original register assignments"});
        } else {
            result.emplace(site, JumpTable{{}, "Indexed destinations differ from original register assignments"});
        }
    }
    return result;
}

Function build_function(Word entry, Unit& unit, const Program& program, const JumpTargets& jumps,
                        Xbe& image, CodeCache& code, const Decoder& decoder) {
    Function function;
    function.entry = entry;
    function.alternate_entries = unit.alternate_entries;
    function.platform_binding = !unit.platform.empty();
    for (const auto& [address, name] : unit.platform_alternates) function.platform_alternates.push_back(address);
    const auto reference = [&](Word address, Word owner) {
        unit.dependencies.push_back(owner);
        if (program.units.at(owner).recovered || address != owner) unit.additional_symbols.push_back(address);
    };
    for (const auto [address, length] : unit.rows) {
        auto instruction = decoder.decode(address, code.read(address, length));
        const auto category = instruction.decoded.meta.category;
        const bool branch = category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_COND_BR ||
                            category == ZYDIS_CATEGORY_UNCOND_BR;
        const auto add_site = [&](std::string kind, std::string reason) {
            unit.sites.push_back({address, hex_bytes(Bytes(instruction.bytes).first(length)),
                                  std::move(kind), std::move(reason), {}});
        };
        const auto add_binding = [&](std::string kind, std::string reason, std::vector<Word> targets = {}) {
            unit.bindings.push_back({address, hex_bytes(Bytes(instruction.bytes).first(length)),
                                     std::move(kind), std::move(reason), std::move(targets)});
        };
        if (const auto destination = branch ? instruction.target() : std::nullopt) {
            if (category == ZYDIS_CATEGORY_CALL && *destination == 0x000BC778) {
                if (hex_bytes(code.read(*destination, 8)) != "8B542404892A895A" || !program.local(address + length, entry))
                    throw std::runtime_error("Unverified setjmp continuation at " + hex32(address));
                instruction.saves_jump = true;
                add_binding("setjmp", "Original CRT saves the stack pointer and the next instruction", {address + length});
            }
            const bool inside = category != ZYDIS_CATEGORY_CALL && program.local(*destination, entry);
            if (inside) {
                if (*destination <= address) function.loop_headers.push_back(*destination);
            } else if (const auto owner = program.callable_owner(*destination)) {
                instruction.cross_function_branch = category != ZYDIS_CATEGORY_CALL;
                reference(*destination, *owner);
            } else {
                instruction.missing_target = true;
                add_site("direct_target", "No mapped entry at " + hex32(*destination));
            }
        } else if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_UNCOND_BR) {
            const auto& operand = instruction.operands[0];
            const auto slot = absolute_word(operand);
            const auto* import = slot ? image.kernel_import(*slot) : nullptr;
            if (entry == 0x000BC6FC && address == 0x000BC771) {
                if (hex_bytes(Bytes(instruction.bytes).first(length)) != "FF6214" ||
                    hex_bytes(code.read(entry, 8)) != "8B5C24048B2B8B73")
                    throw std::runtime_error("Unverified longjmp transfer");
                instruction.resumes_jump = true;
                add_binding("longjmp", "Original CRT restores the saved registers and jumps to the saved continuation");
            } else if (import) {
                instruction.kernel_ordinal = import->ordinal;
                add_binding("kernel_import", std::string(import->name) + " (ordinal " + std::to_string(import->ordinal) + ")");
            } else if (const auto found = jumps.find(address); found != jumps.end()) {
                if (found->second.targets.empty()) add_site("indexed_jump", std::string(found->second.evidence));
                for (const auto target : found->second.targets) {
                    if (program.local(target, entry)) {
                        instruction.jump_targets.push_back(target);
                        if (target <= address) function.loop_headers.push_back(target);
                    } else {
                        instruction.external_jump_targets.push_back(target);
                        reference(target, program.callable_owner(target).value());
                    }
                }
                auto targets = found->second.targets;
                std::ranges::sort(targets);
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                if (!targets.empty()) add_binding("indexed_jump", std::string(found->second.evidence), std::move(targets));
            } else if (indexed_word(operand) && category == ZYDIS_CATEGORY_UNCOND_BR) {
                add_site("indexed_jump", "No verified table extent for operand at " + hex32(*indexed_word(operand)));
            } else if (slot && program.slot_targets.contains(*slot) && !program.slot_targets.at(*slot).empty()) {
                const auto& targets = program.slot_targets.at(*slot);
                for (const auto target : targets) {
                    if (const auto owner = program.callable_owner(target)) reference(target, *owner);
                    else add_site("callback_target", "No compiled entry for " + hex32(target) + " stored in " + hex32(*slot));
                }
                add_binding("callback_slot", "Original initialization or immediate assignment to " + hex32(*slot),
                            {targets.begin(), targets.end()});
            } else {
                add_binding("runtime_dispatch", "Target is read from guest state and checked against the compiled index");
            }
        }
        if (falls_through(instruction) && !program.local(address + length, entry)) {
            if (const auto owner = program.callable_owner(address + length)) {
                instruction.cross_function_fallthrough = true;
                reference(address + length, *owner);
            } else {
                instruction.missing_fallthrough = true;
                add_site("fallthrough", "No mapped continuation at " + hex32(address + length));
            }
        }
        function.instructions.emplace(address, std::move(instruction));
    }
    return function;
}

std::string symbol(Word address) { return std::format("guest_{:08X}", address); }

std::string report(const Xbe& image, const Program& program, std::size_t helpers, bool complete,
                   bool targets_verified, const GeneratedModules& modules) {
    std::map<std::string, std::size_t> totals, bindings;
    std::size_t lowered{}, closed{};
    std::ostringstream functions;
    for (const auto& [entry, unit] : program.units) {
        if (entry != program.units.begin()->first) functions << ',';
        lowered += unit.lowered;
        closed += unit.closed;
        functions << "\n    {\"entry\":" << json(hex32(entry)) << ",\"instructions\":" << unit.rows.size()
                  << ",\"lowered\":" << (unit.lowered ? "true" : "false")
                  << (unit.recovered_from_static_flow ? ",\"static_flow_recovered\":true" : unit.recovered ? ",\"boot_recovered\":true" : "")
                  << ",\"closed\":" << (unit.closed ? "true" : "false");
        const auto write_sites = [&](std::string_view key, const auto& sites, auto& counts) {
            functions << ',' << json(key) << ":[";
            bool first = true;
            for (const auto& site : sites) {
                if (!first) functions << ',';
                first = false;
                ++counts[site.kind];
                functions << "{\"address\":" << json(hex32(site.address)) << ",\"bytes\":" << json(site.bytes)
                          << ",\"kind\":" << json(site.kind) << ",\"reason\":" << json(site.reason);
                if (!site.targets.empty()) {
                    functions << ",\"targets\":[";
                    for (std::size_t i = 0; i < site.targets.size(); ++i) {
                        if (i) functions << ',';
                        functions << json(hex32(site.targets[i]));
                    }
                    functions << ']';
                }
                functions << '}';
            }
            functions << ']';
        };
        write_sites("issues", unit.sites, totals);
        if (!unit.bindings.empty()) write_sites("bindings", unit.bindings, bindings);
        if (!unit.platform.empty()) functions << ",\"platform_binding\":" << json(unit.platform);
        if (!unit.alternate_entries.empty()) {
            functions << ",\"alternate_entries\":[";
            for (std::size_t i = 0; i < unit.alternate_entries.size(); ++i) {
                if (i) functions << ',';
                functions << json(hex32(unit.alternate_entries[i]));
            }
            functions << ']';
        }
        functions << '}';
    }
    std::ostringstream out;
    out << "{\n  \"format\":\"xbox-native-batch-v2\",\n  \"sha256\":" << json(image.sha256)
        << ",\n  \"entry\":" << json(hex32(image.entry)) << ",\n  \"complete\":" << (complete ? "true" : "false")
        << ",\n  \"xbe_closed\":" << (closed == program.units.size() ? "true" : "false")
        << ",\n  \"per_site_targets_verified\":" << (targets_verified ? "true" : "false")
        << ",\n  \"disc_modules_verified\":" << (modules.object_instances ? "true" : "false")
        << ",\n  \"functions_mapped\":" << program.units.size() << ",\n  \"functions_lowered\":" << lowered
        << ",\n  \"functions_closed\":" << closed << ",\n  \"mapped_instruction_rows\":" << program.mapped_rows
        << ",\n  \"boot_recovered_instructions\":" << program.recovered_instructions
        << ",\n  \"static_recovered_instructions\":" << program.static_instructions
        << ",\n  \"alternate_entries\":" << program.alternate_owners.size()
        << ",\n  \"native_instructions\":" << program.starts.size() << ",\n  \"split_wait_pairs\":" << program.split_waits
        << ",\n  \"assembly_helpers\":" << helpers;
    const auto write_counts = [&](std::string_view key, const auto& counts) {
        out << ",\n  " << json(key) << ":{";
        bool first = true;
        for (const auto& [kind, count] : counts) {
            if (!first) out << ',';
            first = false;
            out << json(kind) << ':' << count;
        }
        out << '}';
    };
    write_counts("issue_counts", totals);
    write_counts("binding_counts", bindings);
    out << ",\n  \"xbr_files_indexed\":" << modules.xbr_files << ",\n  \"logic_object_instances\":" << modules.object_instances
        << ",\n  \"unresolved_module_calls\":" << modules.unresolved_calls;
    if (modules.unresolved_calls) out << ",\n  \"module_indirect_sites\":\"module-indirect.tsv\"";
    out << ",\n  \"logic_modules\":[" << modules.summary << "],\n  \"functions\":[" << functions.str() << "\n  ]\n}\n";
    return out.str();
}

void close_dependencies(Units& units) {
    std::map<Word, std::vector<Word>> callers;
    std::queue<Word> pending;
    for (auto& [entry, unit] : units) {
        unit.closed = unit.lowered && unit.sites.empty();
        if (!unit.closed) pending.push(entry);
        for (const auto target : unit.dependencies) callers[target].push_back(entry);
    }
    while (!pending.empty()) {
        const auto target = pending.front();
        pending.pop();
        for (const auto entry : callers[target]) {
            auto& unit = units.at(entry);
            if (!unit.closed) continue;
            unit.closed = false;
            pending.push(entry);
        }
    }
}

std::string banner(const Xbe& image) { return "// Generated from verified XBE SHA-256 " + image.sha256 + "\n"; }

} // namespace

int recompile_xbe(const Path& input, const Path& maps, const Path& output, bool allow_partial, const Path& disc) {
    Xbe image(input);
    if (std::filesystem::exists(output)) throw std::runtime_error("Output directory already exists");
    Decoder decoder;
    CodeCache code(image);
    Program program(maps, image, code, decoder);
    const auto jumps = jump_targets(maps, program, image, code, decoder);
    constexpr unsigned shard_count = 64;
    constexpr std::size_t max_source_bytes = 256 * 1024 * 1024;
    std::array<std::string, shard_count + 1> shards;
    std::array<bool, shard_count + 1> uses_unwind{};
    std::array<std::set<Word>, shard_count + 1> extra_declarations;
    std::map<std::string, std::string> assembly;
    std::ostringstream header, index, index_declarations;
    std::set<Word> symbols;

    header << banner(image) << "#pragma once\n#include \"xbox/cpu.h\"\nnamespace xbox_native {\n";
    index << "namespace {\n"
          << "const std::array<CompiledFunction," << program.units.size() + program.alternate_owners.size() << "> functions{{\n";
    std::size_t total_source{}, function_index{}, lowered_count{};
    for (auto& [entry, unit] : program.units) {
        const auto name = symbol(entry);
        auto& declarations = unit.recovered ? index_declarations : header;
        declarations << "void " << name << "(Cpu&, Memory&);\n";
        auto function = build_function(entry, unit, program, jumps, image, code, decoder);
        auto lowered = lower(function);
        unit.lowered = lowered.issues.empty();
        for (auto& issue : lowered.issues) {
            unit.sites.push_back({issue.address, std::move(issue.bytes), "lowering",
                                  issue.instruction + ": " + issue.reason, {}});
        }
        if (unit.lowered) {
            ++lowered_count;
            for (const auto& [helper, definition] : lowered.native_helpers) {
                if (const auto [existing, added] = assembly.emplace(helper, definition); !added && existing->second != definition) {
                    throw std::runtime_error("Conflicting native helper at " + helper);
                }
            }
        } else {
            const auto stub = [&](Word address) {
                return std::format(
                    "void {}(Cpu& cpu, Memory&) {{\n    cpu.eip = {}U;\n"
                    "    throw NativeBoundary(cpu.eip, \"function has unsupported instructions\");\n}}\n",
                    symbol(address), hex32(address));
            };
            lowered.source = stub(entry);
            for (const auto address : unit.alternate_entries) lowered.source += stub(address);
        }
        total_source += lowered.source.size();
        if (total_source > max_source_bytes) throw std::runtime_error("Generated source exceeds 256 MiB");
        const auto shard = unit.recovered ? shard_count : function_index++ % shard_count;
        uses_unwind[shard] |= std::ranges::any_of(function.instructions, [](const auto& pair) {
            return pair.second.saves_jump || pair.second.resumes_jump;
        });
        shards[shard] += lowered.source;
        extra_declarations[shard].insert(unit.additional_symbols.begin(), unit.additional_symbols.end());
        symbols.insert(entry);
        for (const auto address : unit.alternate_entries) {
            index_declarations << "void " << symbol(address) << "(Cpu&, Memory&);\n";
            symbols.insert(address);
        }
    }
    for (const auto address : symbols) {
        index << "    {" << hex32(address) << "U, \"" << image.sha256 << "\", " << symbol(address) << "},\n";
    }
    header << "}\n";
    index << "}};\n}\nconst std::span<const CompiledFunction> compiled_batch{functions};\n}\n";
    close_dependencies(program.units);
    const bool closed = std::ranges::all_of(program.units, [](const auto& pair) { return pair.second.closed; });
    const bool xbe_targets_verified = std::ranges::none_of(program.units, [](const auto& pair) {
        if (!pair.second.sites.empty()) return true;
        return std::ranges::any_of(pair.second.bindings, [](const Site& site) {
            // Initial values and observed assignments do not bound future writes to callback slots.
            return site.kind == "runtime_dispatch" || site.kind == "callback_slot";
        });
    });
    bool complete = false;

    auto staging = output;
    staging += ".tmp";
    if (!std::filesystem::create_directory(staging)) throw std::runtime_error("Cannot create batch staging directory");
    try {
        const auto modules = (closed || allow_partial) ? generate_modules(disc, image, decoder, staging, assembly) : GeneratedModules{};
        const bool targets_verified = xbe_targets_verified && modules.object_instances && !modules.unresolved_calls;
        complete = closed && targets_verified;
        const auto summary = report(image, program, assembly.size(), complete, targets_verified, modules);
        write_text(staging / "batch.json", summary);
        if (complete || allow_partial) {
            std::ostringstream helpers;
            helpers << "option casemap:none\n.code\n";
            for (const auto& [name, code] : assembly) helpers << code << '\n';
            helpers << "END\n";
            write_text(staging / "game.h", header.str());
            write_text(staging / "game.asm", helpers.str());
            for (unsigned i = 0; i < shards.size(); ++i) if (!shards[i].empty()) {
                std::string declarations;
                for (const auto entry : extra_declarations[i]) {
                    declarations += "void " + symbol(entry) + "(Cpu&, Memory&);\n";
                }
                const auto includes = uses_unwind[i] ? "#include \"game.h\"\n#include \"xbox/unwind.h\"\n" : "#include \"game.h\"\n";
                write_text(staging / std::format("game.{:02}.cpp", i),
                           banner(image) + includes + "namespace xbox_native {\n" + declarations + shards[i] + "}\n");
            }
            write_text(staging / "game.cpp", banner(image) + "#include \"game.h\"\n#include <array>\nnamespace xbox_native {\n"
                       + index_declarations.str() + index.str());
        } else {
            // The staging directory is new and contains only this generation's output.
            for (const auto& file : std::filesystem::directory_iterator(staging))
                if (file.path().extension() == ".cpp") std::filesystem::remove(file.path());
        }
        if (std::filesystem::exists(output)) throw std::runtime_error("Output directory already exists");
        std::filesystem::rename(staging, output);
    } catch (...) {
        std::error_code error;
        for (const auto& file : std::filesystem::directory_iterator(staging, error)) {
            std::filesystem::remove(file.path(), error);
        }
        std::filesystem::remove(staging, error);
        throw;
    }
    std::cout << lowered_count << " of " << program.units.size() << " functions lowered from "
              << program.mapped_rows << " mapped instruction rows. Report: " << path_text(output / "batch.json") << '\n';
    return complete || allow_partial ? 0 : 3;
}

} // namespace xbox_native
