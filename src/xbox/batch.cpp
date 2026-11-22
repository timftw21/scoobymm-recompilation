#include "batch.h"
#include "lower.h"
#include "logic_exports.h"
#include "kernel_imports.h"
#include "call_evidence.h"
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
    std::set<Word> data_references;
    std::set<Word> private_words;
    std::set<Word> zero_words;
    std::set<Word> unlinked_calls;
    std::set<Word> functions;
    std::vector<std::pair<Word, Word>> exports;
    std::optional<Word> destructor;
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
    std::map<Word,bool> data_symbols;
    for (std::size_t offset = 0; offset < symbols.size();) {
        const auto symbol = slice(symbols, offset, 18);
        const auto value = u32(symbol, 8);
        const auto aux = std::to_integer<unsigned>(symbol[17]);
        if (u16(symbol,12)==1 && u16(symbol,14)==0) {
            const auto field=u32(symbol,0)==0 ? slice(strings,u32(symbol,4),strings.size()-u32(symbol,4)) : symbol.first(8);
            const char first=static_cast<char>(field.front());
            // At 0x3A410–0x3A52F the Xbox loader publishes only the three named
            // functions. Defined class-2 data has the same object-local relocation
            // base as class-3 data. Taking its address still disqualifies the cell.
            const auto storage=std::to_integer<unsigned>(symbol[16]);
            if (first=='_' || first=='.') data_symbols[value] |= first=='_' && (storage==2 || storage==3);
            if (first=='.' && aux) data_symbols.try_emplace(value+u32(symbols,offset+18),false);
        }
        if (u16(symbol, 12) == 1 && u16(symbol, 14) == 0x20) {
            module.functions.insert(value);
            const auto field = u32(symbol, 0) == 0 ? slice(strings, u32(symbol, 4), strings.size() - u32(symbol, 4)) : symbol.first(8);
            const std::string_view label(reinterpret_cast<const char*>(field.data()), field.size());
            const auto spelling = label.substr(0, label.find('\0'));
            if (spelling == "_" + std::string(name) + "Register") module.exports.emplace_back(value, logic_register_call);
            if (spelling == "_" + std::string(name) + "Constructor") module.exports.emplace_back(value, logic_constructor_call);
            if (spelling == "_" + std::string(name) + "Destructor") module.destructor = value;
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
    data_symbols.try_emplace(static_cast<Word>(section.size()),false);
    for (auto it=data_symbols.lower_bound(static_cast<Word>(module.text.size()));it!=data_symbols.end();++it) {
        const auto next=std::next(it);
        // BSS symbols may have alignment padding after the accessed word.
        if (it->second && next!=data_symbols.end() && next->first-it->first>=4) {
            module.private_words.insert(it->first);
            if (std::uint64_t(it->first)+4<=section.size() && !u32(section,it->first)) module.zero_words.insert(it->first);
        }
    }
    std::set<Word> relocated;
    for (std::size_t offset = 0; offset < relocs.size(); offset += 10) {
        const auto at = u32(relocs, offset);
        for (auto it=module.zero_words.lower_bound(at>3?at-3:0);it!=module.zero_words.end() && *it<std::uint64_t(at)+4;)
            it=module.zero_words.erase(it);
        const auto reference = slice(symbols, std::size_t{18} * u32(relocs, offset + 4), 18);
        const auto kind = u16(relocs, offset + 8);
        const auto section_number = u16(reference, 12);
        if ((kind != 6 && kind != 0x14) || section_number > 1)
            throw std::runtime_error(std::format("Unbound Xbox module relocation at {} (type {}, section {}, symbol {})",
                hex32(at), hex32(kind), hex32(section_number), hex_bytes(reference.first(8))));
        const auto value = u32(section, at) + u32(reference, 8);
        if (kind==6 && at>=module.text.size()) module.data_references.insert(value);
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
    bool has_registrar;
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
                if (!unique.contains(hash)) {
                    std::vector<std::byte> names(static_cast<std::size_t>(size-strings));
                    disc.read(file,object+strings,names);
                    auto label="_"+std::string(name)+"Register";
                    label.push_back('\0');
                    const std::string_view strings_view(reinterpret_cast<const char*>(names.data()),names.size());
                    unique.emplace(hash,LogicRecord{std::string(name),std::string(path),hash,file,object,static_cast<Word>(size),
                                                    strings_view.find(label)!=strings_view.npos});
                }
            }
        }
    });
    if (unique.empty()) throw std::runtime_error("Verified disc has no XBR logic records");
    for (auto& [hash, record] : unique) index.records.push_back(std::move(record));
    return index;
}

struct GeneratedModules {
    std::string summary;
    std::size_t xbr_files{}, object_instances{}, unresolved_calls{}, fixed_calls{}, installed_calls{}, named_calls{}, named_methods{}, abi_calls{}, conditional_calls{}, passed_arguments{};
};

GeneratedModules generate_modules(const Path& disc, const Xbe& image, const Decoder& decoder, const Path& staging,
                                  std::map<std::string, std::string>& assembly, CallEvidence& evidence, bool emit) {
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
    std::ostringstream declarations, index, destructors, summary, abi_tables, abi_index;
    const auto interface_slots=evidence.interface_tables(abi_tables);
    std::size_t conditional_calls=0, passed_arguments=0;
    std::size_t count = 0, registry_size = 0, unresolved_calls = 0, fixed_calls = 0, abi_modules = 0, abi_calls = 0;
    std::ofstream unresolved, argument_evidence;
    std::ofstream exports, fixed, abi_sites;
    if (!inventory.records.empty()) {
        exports.exceptions(std::ios::failbit | std::ios::badbit);
        exports.open(staging / "module-exports.tsv");
        exports << "xbe_site\tobject_sha256\tsymbol\tfunction_offset\n";
        abi_sites.exceptions(std::ios::failbit | std::ios::badbit);
        abi_sites.open(staging / "module-abi-calls.tsv");
        abi_sites << "object_sha256\tfunction_offset\tsite_offset\tengine_offset\txbe_target\tmodule_slot\ttable\tmethod_offset\tgetter\tname\tbinding\n";
    }
    // Only 33 supported objects export Register. Index their named methods before
    // resolving consumers so disc ordering and level-specific providers cannot affect the result.
    for (const auto& entry:inventory.records) if (entry.has_registrar) {
        std::vector<std::byte> bytes(entry.size);
        media->read(entry.file,entry.object,bytes);
        const auto module=read_module(bytes,entry.name);
        std::map<Word,CallSources> flows;
        std::set<Word> exports,taken=module.data_references;
        for (auto [offset,caller]:module.exports) exports.insert(offset);
        for (auto [at,value]:module.relocations) if (module.functions.contains(value)) taken.insert(value);
        InterfaceCells cells(module.private_words,module.relocations,module.data_references);
        for (auto fn:module.functions) {
            const auto original=module_function(fn,module,decoder);
            flows.emplace(fn,trace_call_sources(original,module.relocations));
            for (const auto& [pc,ins]:original.instructions) {
                cells.instruction(fn,ins);
                if (ins.cross_function_branch && ins.target()) taken.insert(*ins.target());
                if (ins.cross_function_fallthrough) taken.insert(pc+ins.decoded.length);
                taken.insert(ins.external_jump_targets.begin(),ins.external_jump_targets.end());
            }
        }
        const auto trusted=CallEvidence::interface_functions(exports,taken,flows);
        std::map<Word,std::string_view> missing;
        const auto getters=cells.verified_getters(trusted,flows,missing);
        for (const auto& [offset,caller]:module.exports) if (caller==logic_register_call && trusted.contains(offset)) {
            evidence.register_provider(entry.hash,std::format("module_{}_{}",entry.hash,module.exports.front().second),module.text,
                                       module.functions,offset,flows.at(offset),getters);
        }
    }
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
        std::map<Word,CallSources> sources;
        InterfaceCells cells(module.private_words,module.relocations,module.data_references);
        std::set<Word> api_entries;
        for (const auto& [offset,caller]:module.exports) api_entries.insert(offset);
        std::map<Word,std::vector<LogicAbiCall>> rules;
        std::set<Word> other_entries=module.data_references, shared_sites;
        struct Unresolved { Word fn, pc; std::string bytes; PointerValue value; const CallSource* flow; };
        std::vector<Unresolved> remaining;
        std::ostringstream output;
        output << "#include \"xbox/cpu.h\"\nnamespace xbox_native {\nnamespace " << ns << " {\n";
        for (auto address : module.functions) output << std::format("void guest_{:08X}(Cpu&, Memory&);\n", address);
        for (auto address : module.functions) {
            const auto lowered = [&] {
                try {
                    auto fn = module_function(address, module, decoder);
                    const auto& flow = sources.emplace(address,trace_call_sources(fn,module.relocations)).first->second;
                    std::map<Word,const CallSource*> origins;
                    for (const auto& call : flow.calls) {
                        if (!call.direct) origins.emplace(call.site,&call);
                        if (call.direct || call.target.kind != PointerSource::relocated || !module.functions.contains(call.target.base)) continue;
                        fn.instructions.at(call.site).checked_target = call.target.base;
                        if (!fixed.is_open()) {
                            fixed.exceptions(std::ios::failbit | std::ios::badbit);
                            fixed.open(staging / "module-fixed-calls.tsv");
                            fixed << "object_sha256\tfunction_offset\tsite_offset\ttarget_offset\n";
                        }
                        fixed << entry.hash << '\t' << hex32(address) << '\t' << hex32(call.site) << '\t' << hex32(call.target.base) << '\n';
                        ++fixed_calls;
                    }
                    for (const auto& [pc, ins] : fn.instructions) {
                        cells.instruction(address,ins);
                        if (ins.cross_function_branch && ins.target()) other_entries.insert(*ins.target());
                        if (ins.cross_function_fallthrough) other_entries.insert(pc+ins.decoded.length);
                        other_entries.insert(ins.external_jump_targets.begin(),ins.external_jump_targets.end());
                        if (ins.decoded.meta.category != ZYDIS_CATEGORY_CALL || ins.target() || ins.checked_target) continue;
                        const auto origin=origins.find(pc);
                        const auto* source=origin==origins.end() ? nullptr : origin->second;
                        remaining.push_back({address,pc,hex_bytes(Bytes(ins.bytes).first(ins.decoded.length)),source ? source->target : PointerValue{},source});
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
            if (emit) output << lowered.source;
            for (const auto& [helper, definition] : lowered.native_helpers) assembly.emplace(helper, definition);
        }
        // Only the loader supplies the canonical interface. A function also called
        // internally or through a relocated pointer can have a different first argument.
        for (auto [at,value]:module.relocations) if (module.functions.contains(value)) other_entries.insert(value);
        const auto interface_functions=CallEvidence::interface_functions(api_entries,other_entries,sources);
        evidence.module(entry.hash,module.text,module.functions,api_entries,sources);
        evidence.add_registered_cells(entry.hash,sources);
        std::map<Word,std::string_view> missing_cells;
        const auto getters=cells.verified_getters(interface_functions,sources,missing_cells);
        const auto names=CallEvidence::cache_names(module.text,sources,getters);
        other_entries.insert(api_entries.begin(),api_entries.end());
        if (module.destructor) other_entries.insert(*module.destructor);
        const auto table_args=CallEvidence::table_arguments(other_entries,getters,sources);
        std::map<std::pair<Word,Word>,std::set<Word>> callbacks;
        std::set<std::pair<Word,Word>> unknown_arguments;
        for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) {
            if (!call.direct || other_entries.contains(call.target.base)) continue;
            const std::array args{call.first_argument,call.second_argument,call.third_argument};
            for (Word i=0;i<args.size();++i) {
                const auto key=std::pair{call.target.base,i};
                if (args[i].kind==PointerSource::relocated && module.functions.contains(args[i].base)) callbacks[key].insert(args[i].base);
                else unknown_arguments.insert(key);
            }
        }
        const auto abi_rule=[&](Word fn,Word pc,PointerValue value) {
            CallSource call{};
            call.site=pc;call.target=value;
            std::optional<LogicAbiCall> rule;
            if (value.kind==PointerSource::module_method) {
                const auto it=getters.find(value.base);
                if (it!=getters.end()) rule=evidence.table_call(call,it->second);
                if (!rule && names.contains(value.base)) rule=evidence.named_call(call,names.at(value.base));
            } else if (value.kind==PointerSource::returned_method && interface_functions.contains(fn)) {
                call.target.base=logic_getter_result;
                rule=evidence.table_call(call,value.base);
            } else if (interface_functions.contains(fn)) rule=evidence.export_call(call);
            else if (value.kind==PointerSource::argument_method && !value.base && table_args.contains(fn)) {
                call.target.base=table_args.at(fn);
                rule=evidence.table_call(call,getters.at(call.target.base));
                if (!rule && names.contains(call.target.base)) rule=evidence.named_call(call,names.at(call.target.base));
            }
            return rule;
        };
        for (const auto& site:remaining) {
            std::span<const PointerValue> origins{&site.value,1};
            const auto& alternatives=sources.at(site.fn).alternatives;
            if (site.value.kind==PointerSource::unknown && alternatives.contains(site.pc)) origins=alternatives.at(site.pc);
            std::vector<LogicAbiCall> accepted;
            const auto& guards=sources.at(site.fn).nonzero_cells;
            if (auto guard=guards.find(site.pc);guard!=guards.end() && module.zero_words.contains(guard->second) && cells.unchanged(guard->second)) {
                // Every path to this CALL requires a private, permanently null pointer to be nonzero.
                accepted.push_back({site.pc,guard->second,0,logic_unreachable});
                origins={};
            }
            for (auto value:origins) {
                const auto key=std::pair{site.fn,value.base};
                if (value.kind==PointerSource::argument && callbacks.contains(key) && !unknown_arguments.contains(key)) {
                    for (auto offset:callbacks.at(key)) accepted.push_back({site.pc,value.base,offset,logic_local_function});
                } else if (auto rule=abi_rule(site.fn,site.pc,value)) accepted.push_back(*rule);
                else {accepted.clear();break;}
            }
            if (accepted.empty()) {shared_sites.insert(site.pc);continue;}
            const auto [pos,added]=rules.try_emplace(site.pc,std::move(accepted));
            if (!added && pos->second!=accepted) shared_sites.insert(site.pc);
        }
        for (auto site:shared_sites) rules.erase(site);
        // Conditional checks are kept in the unresolved inventory until every
        // incoming edge is known. At runtime a verified caller can establish
        // the table type for this particular invocation of a callback.
        std::map<Word,std::optional<LogicAbiCall>> conditional;
        std::map<std::pair<Word,Word>,Word> passed;
        for (const auto& site:remaining) {
            if (const auto it=rules.find(site.pc);it!=rules.end()) {
                for (const auto& rule:it->second) abi_sites<<entry.hash<<'\t'<<hex32(site.fn)<<'\t'<<hex32(site.pc)<<'\t'<<hex32(rule.slot)<<'\t'<<hex32(rule.target)
                         <<'\t'<<hex32(rule.source)<<'\t'<<hex32(rule.table)<<'\t'<<hex32(rule.method)<<'\t'<<hex32(rule.getter)
                         <<'\t'<<(rule.named==UINT32_MAX ? "-" : names.at(rule.source))<<'\t'
                         <<(rule.source==logic_unreachable ? "guarded-null" : rule.source==logic_local_function ? "local-callback" :
                            rule.named!=UINT32_MAX ? "named-method" : "engine-method")<<'\n';
                ++abi_calls;
            } else {
                const auto arg=site.value.base;
                std::optional<LogicAbiCall> parameter;
                if (site.flow && site.value.kind==PointerSource::argument_method && arg<3 &&
                    (site.flow->frame_arguments & (1u<<arg)) && !(site.value.offset&3))
                    parameter=LogicAbiCall{site.pc,arg,0,logic_passed_argument,0,site.value.offset};
                auto [item,added]=conditional.try_emplace(site.pc,parameter);
                if (!added && item->second!=parameter) item->second.reset();
                const auto inputs=site.flow ? std::array{site.flow->first_argument,site.flow->second_argument,site.flow->third_argument}
                                            : std::array<PointerValue,3>{};
                const auto ret=site.pc+static_cast<Word>(site.bytes.size()/2);
                for (Word i=0;i<inputs.size();++i) {
                    const auto& input=inputs[i];
                    const bool known=input.kind==PointerSource::module_slot && getters.contains(input.base) &&
                                     interface_slots.contains(getters.at(input.base));
                    const Word cell=known ? input.base : UINT32_MAX;
                    auto [p,first]=passed.try_emplace(std::pair{ret,i},cell);
                    if (!first && p->second!=cell) p->second=UINT32_MAX;
                }
                if (!unresolved.is_open()) {
                    unresolved.exceptions(std::ios::failbit|std::ios::badbit);
                    unresolved.open(staging/"module-indirect.tsv");
                    unresolved<<"object_sha256\tfunction_offset\tsite_offset\tbytes\tsource\tbase\tfield\tmissing_evidence\n";
                }
                std::string_view missing="pointer-origin";
                if (site.value.kind==PointerSource::module_method) {
                    const auto it=missing_cells.find(site.value.base);
                    missing=it!=missing_cells.end()?it->second:!module.private_words.contains(site.value.base)?"non-private-cell":
                        !getters.contains(site.value.base)?"unexplained-cell-writer":"interface-method";
                } else if (site.value.kind==PointerSource::argument_method) missing="caller-argument";
                unresolved<<entry.hash<<'\t'<<hex32(site.fn)<<'\t'<<hex32(site.pc)<<'\t'<<site.bytes<<'\t'<<source_name(site.value.kind)
                          <<'\t'<<hex32(site.value.base)<<'\t'<<hex32(site.value.offset)<<'\t'<<missing<<'\n';
                ++unresolved_calls;
            }
        }
        std::erase_if(conditional,[](const auto& item){return !item.second;});
        std::erase_if(passed,[](const auto& item){return item.second==UINT32_MAX;});
        if ((!conditional.empty() || !passed.empty()) && !argument_evidence.is_open()) {
            argument_evidence.exceptions(std::ios::failbit|std::ios::badbit);
            argument_evidence.open(staging/"module-argument-calls.tsv");
            argument_evidence<<"object_sha256\trole\toffset\targument\tmethod_offset\tcached_table_cell\tengine_getter_offset\n";
        }
        for (const auto& [pc,rule]:conditional) {
            rules[pc].push_back(*rule);
            argument_evidence<<entry.hash<<"\tmethod-call\t"<<hex32(pc)<<'\t'<<rule->slot<<'\t'<<hex32(rule->method)<<"\t-\t-\n";
        }
        for (const auto& [key,cell]:passed)
            argument_evidence<<entry.hash<<"\tcaller-return\t"<<hex32(key.first)<<'\t'<<key.second<<"\t-\t"
                             <<hex32(cell)<<'\t'<<hex32(getters.at(cell))<<'\n';
        conditional_calls+=conditional.size();
        passed_arguments+=passed.size();
        if (emit && (!rules.empty() || !passed.empty())) {
            std::string checks="{}", arguments="{}";
            if (!rules.empty()) {
                checks="abi_"+std::to_string(abi_modules);
                abi_tables<<"constexpr LogicAbiCall "<<checks<<"[]{\n";
                for (const auto& [site,choices]:rules) for (const auto& rule:choices)
                    abi_tables<<std::format("    {{{},{},{},{}U,{},{},{},{}U}},\n",site,rule.slot,rule.target,rule.source,rule.table,rule.method,rule.getter,rule.named);
                abi_tables<<"};\n";
            }
            if (!passed.empty()) {
                arguments="args_"+std::to_string(abi_modules);
                abi_tables<<"constexpr LogicPassedInterface "<<arguments<<"[]{\n";
                for (const auto& [key,cell]:passed)
                    abi_tables<<std::format("    {{{},{},{},&iface_{}}},\n",key.first,key.second,cell,getters.at(cell));
                abi_tables<<"};\n";
            }
            abi_index<<std::format("    {{&{}_{}, {}, {}}},\n",ns,module.exports.front().second,checks,arguments);
            ++abi_modules;
        }
        output << "constexpr CompiledFunction functions[] = {\n";
        for (auto address : module.functions) {
            output << std::format("    {{{}U, \"{}\", guest_{:08X}}},\n", hex32(address), image.sha256, address);
        }
        output << "};\nconstexpr std::array<ModuleRelocation," << module.relocations.size() << "> relocations{{\n";
        for (const auto [offset, value] : module.relocations)
            output << std::format("    {{{}U, {}U}},\n", hex32(offset), hex32(value));
        output << "}};\nconstexpr std::string_view code =\n";
        for (std::size_t offset = 0; emit && offset < module.text.size(); offset += 64) {
            output << "    \"" << hex_bytes(Bytes(module.text).subspan(offset, std::min(module.text.size() - offset, std::size_t{64}))) << "\"\n";
        }
        output << ";\n}\n";
        for (const auto [offset, caller] : module.exports) {
            const auto symbol = std::format("{}_{}", ns, caller);
            declarations << "extern const CompiledModule " << symbol << ";\n";
            output << std::format("extern constinit const CompiledModule {}{{\"{}\", {}U, {}U, {}::code, {}::relocations, {}::functions}};\n",
                                  symbol, entry.name, hex32(offset), hex32(caller), ns, ns, ns);
            index << "    " << symbol << ",\n";
            exports << hex32(caller) << '\t' << entry.hash << "\t_" << entry.name
                    << (caller == logic_register_call ? "Register" : "Constructor") << '\t' << hex32(offset) << '\n';
            ++registry_size;
        }
        if (module.destructor) {
            const auto symbol = std::format("{}_{}",ns,module.exports.front().second);
            destructors << std::format("    destructor({}, {}U),\n",symbol,hex32(*module.destructor));
            exports << hex32(logic_destructor_call) << '\t' << entry.hash << "\t_" << entry.name
                    << "Destructor\t" << hex32(*module.destructor) << '\n';
            ++registry_size;
        }
        output << "}\n";
        if (emit) write_text(staging / ("game.logic." + entry.hash + ".cpp"), output.str());
        if (count) summary << ',';
        summary << "{\"name\":" << json(entry.name) << ",\"disc_path\":" << json(entry.path)
                << ",\"object_offset\":" << entry.object << ",\"object_sha256\":" << json(entry.hash)
                << ",\"functions_lowered\":" << module.functions.size()
                << ",\"entry_offset\":" << json(hex32(module.exports.front().first)) << '}';
        ++count;
    }
    const auto module_index = std::format("#include \"xbox/cpu.h\"\n#include \"xbox/logic_exports.h\"\nnamespace xbox_native {{\n{}namespace {{\n"
        "CompiledModule destructor(CompiledModule module, std::uint32_t offset) {{\n"
        "    module.registration = offset; module.registration_call = logic_destructor_call; return module;\n}}\n"
        "const std::array<CompiledModule, {}> modules{{{{\n{}{}}}}};\n{}const std::array<LogicAbiModule,{}> abi_modules{{{{\n{}}}}};\n{} }}\n"
        "const std::span<const CompiledModule> compiled_modules{{modules}};\nconst std::span<const LogicAbiModule> compiled_logic_abi{{abi_modules}};\n"
        "const std::span<const LogicNamedMethod> compiled_named_methods{{named_methods}};\n}}\n",
        declarations.str(), registry_size, index.str(),destructors.str(),abi_tables.str(),abi_modules,abi_index.str(),evidence.provider_tables());
    if (emit) write_text(staging / "game.module.cpp", module_index);
    if (unresolved.is_open()) unresolved.close();
    if (argument_evidence.is_open()) argument_evidence.close();
    return {summary.str(),inventory.files,inventory.instances,unresolved_calls,fixed_calls,evidence.installed_sites,evidence.named_sites,evidence.named_methods,abi_calls,conditional_calls,passed_arguments};
}

struct Program;
std::vector<Word> guarded_jump_targets(const Instruction&, Word, const Program&, Xbe&, CodeCache&, const Decoder&);

struct Program {
    analysis_map::Maps map;
    Units units;
    std::map<Word, Word> alternate_owners;
    std::map<Word, std::set<Word>> slot_targets;
    std::map<Word, Word> indexed_jumps;
    std::map<Word,std::set<Word>> callers;
    std::set<Word> branch_targets, file_pointers;
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

    void index_file_pointers(File& file) {
        // An unaligned pointer in code, data or a loader record can publish an
        // entry without a direct CALL. Treat every such word conservatively.
        constexpr std::size_t chunk=64*1024;
        std::array<std::byte,chunk+3> buffer;
        for (std::uint64_t at=0;at<file.size();at+=chunk) {
            const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),file.size()-at));
            file.read(at,std::span(buffer).first(n));
            for (std::size_t i=0;i+4<=n;++i) {
                const auto value=u32(buffer,i);
                if (units.contains(value)) file_pointers.insert(value);
            }
        }
    }

    std::map<Word,Word> callback_callers(Word entry,Word argument,CodeCache& code,const Decoder& decoder) const {
        const auto sites=callers.find(entry);
        if (sites==callers.end() || file_pointers.contains(entry) || branch_targets.contains(entry) ||
            !units.at(entry).alternate_entries.empty()) return {};
        std::map<Word,Word> result;
        for (const auto site:sites->second) {
            const auto call=decoder.decode(site,code.window(site,15));
            const auto owner=map.containing(site)->entry;
            auto it=std::ranges::lower_bound(starts,site);
            auto cursor=site, left=argument;
            bool bound=false;
            // Only an uninterrupted sequence which constructs the actual stack
            // argument can establish the callback for this return address.
            for (unsigned i=0;i<32 && it!=starts.begin() && !branch_targets.contains(cursor) &&
                                !callers.contains(cursor) && !callable_owner(cursor);++i) {
                const auto pc=*--it;
                const auto ins=decoder.decode(pc,code.window(pc,15));
                if (!local(pc,owner) || pc+ins.decoded.length!=cursor) break;
                if (ins.decoded.mnemonic==ZYDIS_MNEMONIC_PUSH && ins.decoded.operand_width==32) {
                    if (!left) {
                        const auto& op=ins.operands[0];
                        if (op.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && callable_owner(static_cast<Word>(op.imm.value.u))) {
                            result.emplace(site+call.decoded.length,static_cast<Word>(op.imm.value.u));
                            bound=true;
                        }
                        break;
                    }
                    --left;
                } else if (ins.decoded.meta.category==ZYDIS_CATEGORY_CALL || !falls_through(ins) ||
                           std::ranges::any_of(std::span(ins.operands).first(ins.decoded.operand_count),[](const auto& op) {
                               return (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) && (op.type==ZYDIS_OPERAND_TYPE_MEMORY ||
                                   (op.type==ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetLargestEnclosing(
                                       ZYDIS_MACHINE_MODE_LEGACY_32,op.reg.value)==ZYDIS_REGISTER_ESP));
                           })) break;
                cursor=pc;
            }
            if (!bound) return {};
        }
        return result;
    }

private:
    void recover_static_flow(Xbe& image, CodeCache& code, const Decoder& decoder) {
        struct Edge { Word destination, owner; bool call; };
        std::vector<Edge> pending;
        std::set<Word> slots;
        std::map<Word, std::set<Word>> assignments;
        // The title builds its four allocator callbacks on the stack. The SDK's
        // non-null initialization path copies that table over its CRT defaults.
        if (hex_bytes(code.read(0x4cebe,16)) != "68000020008D4424205350E8B2910500" ||
            hex_bytes(code.read(0xa60dd,10)) != "8B4C240851E879250000" ||
            hex_bytes(code.read(0xa866a,8)) != "8B44240485C07429")
            throw std::runtime_error("Unverified XBE allocator callback installation");
        constexpr std::array<Word,4> stack_stores{0x4cea9,0x4cea1,0x4ceb1,0x4ce99};
        constexpr std::array<Word,4> copies{0xa8672,0xa867a,0xa8683,0xa868c};
        for (unsigned i=0; i<stack_stores.size(); ++i) {
            const auto from=decoder.decode(stack_stores[i],code.read(stack_stores[i],8));
            const auto read=decoder.decode(copies[i],code.window(copies[i],15));
            const auto pc=copies[i]+read.decoded.length;
            const auto store=decoder.decode(pc,code.window(pc,15));
            const auto slot=absolute_word(store.operands[0]);
            if (from.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV || from.operands[0].type!=ZYDIS_OPERAND_TYPE_MEMORY ||
                from.operands[0].mem.base!=ZYDIS_REGISTER_ESP || from.operands[0].mem.disp.value!=0x1c+4*i ||
                from.operands[1].type!=ZYDIS_OPERAND_TYPE_IMMEDIATE || read.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV ||
                read.operands[1].type!=ZYDIS_OPERAND_TYPE_MEMORY || read.operands[1].mem.base!=ZYDIS_REGISTER_EAX ||
                read.operands[1].mem.disp.value!=4*i || store.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV ||
                read.operands[0].reg.value!=store.operands[1].reg.value || slot!=0x1f4e790+4*i)
                throw std::runtime_error("Invalid allocator callback field at "+hex32(pc));
            assignments[*slot].insert(static_cast<Word>(from.operands[1].imm.value.u));
        }
        const auto follow = [&](const Instruction& instruction, Word owner) {
            const auto category = instruction.decoded.meta.category;
            if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) {
                if (const auto target=instruction.target()) {
                    if (category==ZYDIS_CATEGORY_CALL) callers[*target].insert(instruction.address);
                    else branch_targets.insert(*target);
                }
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
                } else if (instruction.operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                           instruction.operands[0].size==32 && file_backed_code(image,static_cast<Word>(instruction.operands[1].imm.value.u))) {
                    const auto next=instruction.address+instruction.decoded.length;
                    if (local(next,owner)) {
                        const auto store=decoder.decode(next,code.window(next,ZYDIS_MAX_INSTRUCTION_LENGTH));
                        const auto destination=absolute_word(store.operands[0]);
                        if (destination && store.decoded.mnemonic==ZYDIS_MNEMONIC_MOV &&
                            store.operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                            store.operands[1].reg.value==instruction.operands[0].reg.value)
                            assignments[*destination].insert(static_cast<Word>(instruction.operands[1].imm.value.u));
                    }
                }
            }
            const auto next = instruction.address + instruction.decoded.length;
            if (falls_through(instruction) && !local(next, owner)) pending.push_back({next, owner, false});
            if (falls_through(instruction) && units.contains(next) && next!=owner) branch_targets.insert(next);
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
            } else if (logic_export_field(address)) {
                const auto opcode = address == logic_register_call ? "FF5118" :
                                    address == logic_constructor_call ? "FF15EC55EF01" : "FF15E855EF01";
                if (hex_bytes(Bytes(instruction.bytes).first(length)) != opcode ||
                    (address!=logic_destructor_call && hex_bytes(code.read(address-5,5))!="681857EF01") ||
                    hex_bytes(code.read(0x3A47A,3)) != "894810" || hex_bytes(code.read(0x3A4CA,3)) != "894814" ||
                    hex_bytes(code.read(0x3A512,3)) != "894218")
                    throw std::runtime_error("Unverified logic export installation at " + hex32(address));
                add_binding("module_export","Named COFF export in module-exports.tsv; loaded owner, text, relocations and field checked at dispatch");
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
                add_binding("callback_slot", "Original initialization or callback assignment to " + hex32(*slot),
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
    if (std::ranges::any_of(unit.bindings, [](const Site& s) {return s.kind=="runtime_dispatch" || s.kind=="callback_slot";})) {
        for (const auto& call: trace_call_sources(function).calls) {
            if (call.direct) continue;
            const auto binding=std::ranges::find(unit.bindings,call.site,&Site::address);
            if (binding==unit.bindings.end() || (binding->kind!="runtime_dispatch" && binding->kind!="callback_slot")) continue;
            const auto source=call.target;
            if (source.kind==PointerSource::argument) {
                const auto callers=program.callback_callers(entry,source.base,code,decoder);
                if (!callers.empty()) {
                    auto& pairs=function.instructions.at(call.site).caller_targets;
                    pairs.assign(callers.begin(),callers.end());
                    binding->kind="caller_callback";
                    binding->reason="Original stack argument "+std::to_string(source.base+1)+" at verified return addresses";
                    std::set<Word> targets;
                    for (auto [return_pc,target]:callers) {
                        binding->reason+=" "+hex32(return_pc)+"->"+hex32(target);
                        reference(target,*program.callable_owner(target));
                        targets.insert(target);
                    }
                    binding->targets.assign(targets.begin(),targets.end());
                }
                continue;
            }
            std::optional<Word> target;
            std::string reason;
            if (source.kind==PointerSource::constant) {
                target=source.base;
                reason="Register target fixed by the original instructions";
            } else if (source.kind==PointerSource::absolute_slot) {
                const auto immutable=std::ranges::any_of(image.sections(),[&](const Section& s) {
                    return !(s.flags & 1) && source.base>=s.address && std::uint64_t(source.base-s.address)+4<=std::min(s.size,s.raw_size);
                });
                if (!immutable) continue;
                const auto* import=image.kernel_import(source.base);
                if (import && kernel_data_export(import->ordinal)) continue;
                if (import) {
                    target=kernel_call_address(import->ordinal);
                    reason=std::string(import->name)+" from the verified import at "+hex32(source.base);
                } else {
                    target=image.initial_word(source.base);
                    reason="Pointer from the read-only XBE word at "+hex32(source.base);
                }
            }
            if (!target) continue;
            const auto owner=program.callable_owner(*target);
            if (owner) reference(*target,*owner);
            else if (source.kind!=PointerSource::absolute_slot || !image.kernel_import(source.base)) continue;
            function.instructions.at(call.site).checked_target=*target;
            binding->kind="fixed_call";
            binding->reason=std::move(reason);
            binding->targets={*target};
        }
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
    if (modules.fixed_calls) out << ",\n  \"fixed_module_calls\":" << modules.fixed_calls << ",\n  \"module_fixed_sites\":\"module-fixed-calls.tsv\"";
    if (modules.object_instances) out << ",\n  \"module_exports\":\"module-exports.tsv\""
        << ",\n  \"verified_module_abi_calls\":" << modules.abi_calls << ",\n  \"module_abi_sites\":\"module-abi-calls.tsv\""
        << ",\n  \"conditional_argument_methods\":" << modules.conditional_calls << ",\n  \"verified_outgoing_table_arguments\":" << modules.passed_arguments
        << ",\n  \"module_call_evidence\":\"module-call-evidence.tsv\",\n  \"module_api_providers\":\"module-api-providers.tsv\""
        << ",\n  \"installed_module_calls\":" << modules.installed_calls << ",\n  \"named_module_calls\":" << modules.named_calls
        << ",\n  \"named_provider_methods\":" << modules.named_methods;
    if (modules.conditional_calls || modules.passed_arguments) out << ",\n  \"module_argument_evidence\":\"module-argument-calls.tsv\"";
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
    if (hex_bytes(code.read(0x68480,24))!="8B4C2408C1E90285C9760C8B44240C578B7C2408F3AB5FC3")
        throw std::runtime_error("Unverified SDK interface table fill");
    Program program(maps, image, code, decoder);
    File executable(input);
    program.index_file_pointers(executable);
    const auto jumps = jump_targets(maps, program, image, code, decoder);
    for (const auto& [site,jump]:jumps) program.branch_targets.insert(jump.targets.begin(),jump.targets.end());
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
    std::size_t total_source{}, assigned_rows{}, lowered_count{};
    std::size_t total_rows = 0;
    for (const auto& [entry, unit] : program.units)
        if (!unit.recovered) total_rows += unit.rows.size();
    const auto rows_per_shard = std::max<std::size_t>(1,(total_rows+shard_count-1)/shard_count);
    CallEvidence evidence;
    for (auto& [entry, unit] : program.units) {
        const auto name = symbol(entry);
        auto& declarations = unit.recovered ? index_declarations : header;
        declarations << "void " << name << "(Cpu&, Memory&);\n";
        auto function = build_function(entry, unit, program, jumps, image, code, decoder);
        evidence.executable_function(function);
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
        // Keep neighboring functions together so the host compiler can inline their direct calls.
        const auto shard = unit.recovered ? shard_count : std::min<std::size_t>(shard_count-1,assigned_rows/rows_per_shard);
        if (!unit.recovered) assigned_rows += unit.rows.size();
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
        GeneratedModules modules;
        if (closed || allow_partial) {
            // Release every stream before removing a failed staging directory on Windows.
            auto writer=std::move(evidence);
            if (!disc.empty()) writer.open(staging);
            modules=generate_modules(disc,image,decoder,staging,assembly,writer,allow_partial || xbe_targets_verified);
            if (!disc.empty()) writer.close();
        }
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
