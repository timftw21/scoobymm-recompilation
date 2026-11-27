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
    Bytes section;
    std::vector<std::byte> text;
    std::map<Word, Word> relocations;
    std::map<Word,Word> data_relocations;
    bool relative_data{};
    bool relative_relocations{};
    std::optional<Word> floor_events;
    std::set<Word> frame_step_operands;
    std::set<Word> data_references;
    std::set<Word> private_words;
    std::set<Word> zero_words;
    std::set<Word> data_symbols; // Starts of named and section data symbols.
    std::set<Word> unlinked_calls;
    std::set<Word> functions;
    std::vector<std::pair<Word, Word>> exports;
    std::optional<Word> destructor;
    // Named code and data of the objects the player extends (see named_symbol_modules).
    std::vector<std::pair<std::string, Word>> symbols;
};
// The player adds Recomp Options to the title and pause menus of these objects
// (recomp_options.cpp) and finds their callbacks and data by COFF symbol.
constexpr std::array<std::string_view, 1> named_symbol_modules{"panel"};

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
    module.section=section;
    const bool named=std::ranges::find(named_symbol_modules,name)!=named_symbol_modules.end();
    std::map<Word,bool> data_symbols;
    for (std::size_t offset = 0; offset < symbols.size();) {
        const auto symbol = slice(symbols, offset, 18);
        const auto value = u32(symbol, 8);
        const auto aux = std::to_integer<unsigned>(symbol[17]);
        if (named && u16(symbol,12)==1) {
            const auto field=u32(symbol,0)==0 ? slice(strings,u32(symbol,4),strings.size()-u32(symbol,4)) : symbol.first(8);
            const std::string_view label(reinterpret_cast<const char*>(field.data()),field.size());
            const auto spelling=label.substr(0,label.find('\0'));
            if (spelling.starts_with('_') && value<section.size()) module.symbols.emplace_back(spelling,value);
        }
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
            if (spelling=="_FloorEventManager") module.floor_events=value;
            // Collectibles load their per-update angle before converting it to radians.
            // Ghosts add a signed increment to their hover phase. Scale only the
            // increment, so the original height, direction and random reversals survive.
            if (spelling=="_Coll_BaseFUNC_Spin" && hex_bytes(slice(section,value+11,3))=="D94508")
                module.frame_step_operands.insert(value+11);
            if (spelling=="_Waving" && hex_bytes(slice(section,value+15,18))=="D9826C080000D88270080000D9926C080000")
                module.frame_step_operands.insert(value+21);
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
    for (const auto& [start,named]:data_symbols) module.data_symbols.insert(start);
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
        module.relative_relocations |= kind==0x14;
        const auto section_number = u16(reference, 12);
        if ((kind != 6 && kind != 0x14) || section_number > 1)
            throw std::runtime_error(std::format("Unbound Xbox module relocation at {} (type {}, section {}, symbol {})",
                hex32(at), hex32(kind), hex32(section_number), hex_bytes(reference.first(8))));
        const auto value = u32(section, at) + u32(reference, 8);
        if (at>=module.text.size()) {
            if (kind==6) {
                module.data_references.insert(value);
                module.data_relocations.emplace(at,value);
            } else module.relative_data=true;
        }
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
    struct Archive {
        std::string path;
        DiscEntry file;
        std::map<Word,std::string> objects;
    };
    std::vector<Archive> archives;
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
        auto& archive=index.archives.emplace_back(std::string(path),file,std::map<Word,std::string>{});
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
                archive.objects.emplace(object,hash);
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

// IDs are local to the archive. Locate every possible definition header,
// including unaligned records after variable-length components. Keep the
// original disc offsets so the reader can independently inspect the handoff.
using ReceiverKey=std::pair<std::string,Word>;
std::map<ReceiverKey,std::set<std::string>> descriptor_receivers(Disc& disc,const LogicIndex& inventory,
        const std::map<std::string,std::set<Word>>& requested,const Path& staging) {
    std::map<ReceiverKey,std::set<std::string>> result;
    if (requested.empty()) return result;
    std::ofstream out;
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out.open(staging/"module-descriptor-receivers.tsv");
    out<<"disc_path\tproducer_sha256\tspawn_id\tobject_definition_offset\tlogic_resource_id\treceiver_sha256\n";
    for (const auto& archive:inventory.archives) {
        std::map<Word,std::set<std::string>> ids;
        for (const auto& [at,hash]:archive.objects) if (const auto it=requested.find(hash);it!=requested.end())
            for (Word id:it->second) ids[id].insert(hash);
        if (ids.empty()) continue;
        std::array<std::byte,8> header;
        disc.read(archive.file,0,header);
        const auto count=u32(header,4);
        const auto payload=8+std::uint64_t(count)*16;
        if (payload>archive.file.size) throw std::runtime_error("Truncated XBR directory in "+archive.path);
        std::map<Word,std::string> logic;
        std::set<Word> found_ids;
        std::vector<std::pair<std::uint64_t,Word>> definitions;
        for (Word i=0;i<count;++i) {
            std::array<std::byte,16> record;
            disc.read(archive.file,8+std::uint64_t(i)*16,record);
            const auto id=u32(record,0),size=u32(record,4),type=u32(record,12);
            const auto at=payload+u32(record,8);
            if (at+size>archive.file.size) throw std::runtime_error("Truncated XBR resource in "+archive.path);
            if (type==0x1f) definitions.emplace_back(at,size);
            if ((type!=0x27 && type!=0x45) || size<12) continue;
            std::array<std::byte,12> envelope;
            disc.read(archive.file,at,envelope);
            const auto object=at+12+u32(envelope,8);
            if (object>=at+size) continue;
            const auto code=archive.objects.find(static_cast<Word>(object));
            if (code==archive.objects.end()) continue;
            const auto [it,added]=logic.emplace(id,code->second);
            if (!added && it->second!=code->second) it->second.clear();
        }
        constexpr std::size_t chunk=65536;
        std::vector<std::byte> data(chunk+11);
        for (const auto& [base,size]:definitions) for (std::uint64_t pos=0;pos+12<=size;pos+=chunk) {
            const auto block=std::span(data).first(std::min<std::uint64_t>(data.size(),size-pos));
            disc.read(archive.file,base+pos,block);
            for (std::size_t i=0;i+12<=block.size() && i<chunk;++i) {
                if (block[i]!=std::byte{0x20} || u32(block,i)!=0x20) continue;
                const Word id=u32(block,i+4),resource=u32(block,i+8);
                const auto consumers=ids.find(id);
                if (consumers==ids.end()) continue;
                found_ids.insert(id);
                const auto obj=logic.find(resource);
                for (const auto& producer:consumers->second) {
                    result[{producer,id}].insert(obj==logic.end() ? "" : obj->second);
                    out<<archive.path<<'\t'<<producer<<'\t'<<hex32(id)<<'\t'<<hex32(static_cast<Word>(base+pos+i+4))
                       <<'\t'<<hex32(resource)<<'\t'<<(obj==logic.end() || obj->second.empty() ? "-" : obj->second)<<'\n';
                }
            }
        }
        for (const auto& [id,producers]:ids) if (!found_ids.contains(id))
            for (const auto& producer:producers) {
                result[{producer,id}].insert("");
                out<<archive.path<<'\t'<<producer<<'\t'<<hex32(id)<<"\t-\t-\t-\n";
            }
    }
    out.close();
    return result;
}

struct GeneratedModules {
    std::string summary;
    std::size_t xbr_files{}, object_instances{}, unresolved_calls{}, fixed_calls{}, installed_calls{}, named_calls{}, named_methods{}, abi_calls{}, conditional_calls{}, passed_arguments{};
    std::size_t unreferenced_calls{},descriptor_methods{},object_interfaces{},enforced_calls{};
};

// The six floor-event transfers share the same original implementation on the disc.
// Its signed upper bound needs a separate proof for the input and saved lower bounds.
// The checked code, callers, and state invariant are described in the correctness audit.
constexpr Word floor_code_size=0x162;
std::map<Word,std::vector<LogicAbiCall>> floor_event_calls(const ModuleObject& module,const InterfaceCells& cells,
        const std::set<Word>& other_entries,const std::map<Word,CallSources>& sources) {
    const Word start=*module.floor_events, end=start+floor_code_size;
    if (module.relative_data || !std::ranges::equal(Bytes(module.text).subspan(start,floor_code_size),
                                                    module.section.subspan(start,floor_code_size))) return {};
    const auto in_code=[&](Word address){return address>=start && address<end;};
    if (std::ranges::any_of(other_entries,in_code) ||
        std::ranges::any_of(module.relocations,[&](const auto& r){return in_code(r.second);})) return {};
    const std::set<Word> functions{start,start+0x84,start+0xd8,start+0x11c};
    if (!std::ranges::equal(module.functions.lower_bound(start),module.functions.lower_bound(end),
                            functions.begin(),functions.end())) return {};
    constexpr std::array<Word,7> sizes{0xc,0x9a,0xbf,0xe7,0x10a,0x127,0x14b};
    constexpr std::array<Word,6> indices{0x13,0x3b,0x45,0x63,0xc7,0x153};
    constexpr std::array<Word,7> pointers{0x1c,0x4a,0x68,0x94,0xa5,0xf1,0x131};
    const auto cell_for=[&](const auto& positions) -> std::optional<Word> {
        auto first=module.relocations.find(start+positions.front());
        if (first==module.relocations.end()) return {};
        for (auto p:positions) {
            auto rel=module.relocations.find(start+p);
            if (rel==module.relocations.end() || rel->second!=first->second) return {};
        }
        return first->second;
    };
    const auto count=cell_for(sizes), index=cell_for(indices), pointer=cell_for(pointers);
    if (!count || !index || !pointer || std::set{*count,*index,*pointer}.size()!=3 ||
        std::distance(module.relocations.lower_bound(start),module.relocations.lower_bound(end))!=20 ||
        !module.zero_words.contains(*count) || !module.zero_words.contains(*index) || !module.zero_words.contains(*pointer) ||
        !cells.writers_within(*count,{start+0x98}) || !cells.writers_within(*pointer,{start+0x93})) return {};
    std::set<Word> index_writes{start+0x39,start+0x61,start+0xc5,start+0x151};
    for (auto [at,value]:module.relocations) if (!in_code(at)) {
        if (value==*pointer) return {}; // The array pointer may only be read by the reviewed routines.
        if (value==*index && at>=2 && at+8<=module.text.size() && u16(module.text,at-2)==0x05c7 &&
            u32(module.text,at+4)==UINT32_MAX && !module.relocations.contains(at+4)) index_writes.insert(at-2);
    }
    if (!cells.writers_within(*index,index_writes)) return {};
    std::optional<std::pair<Word,Word>> initial;
    std::set<Word> pointer_reads;
    for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) if (call.direct && in_code(call.target.base)) {
        const auto arg=call.first_argument;
        if (call.target.base==start && arg.kind!=PointerSource::unsigned_byte &&
            !(arg.kind==PointerSource::constant && arg.base<=INT32_MAX)) return {};
        if (call.target.base!=start+0x84) continue;
        const auto size=call.second_argument;
        // MOV EAX,[count]; PUSH EAX; MOV EAX,[table]; PUSH EAX; CALL constructor.
        const auto pc=call.site;
        if (arg.kind!=PointerSource::module_slot || size.kind!=PointerSource::module_slot || pc<12 ||
            module.text[pc-12]!=std::byte{0xa1} || u16(module.text,pc-7)!=0xa150 || module.text[pc-1]!=std::byte{0x50} ||
            !module.relocations.contains(pc-11) || module.relocations.at(pc-11)!=size.base ||
            !module.relocations.contains(pc-5) || module.relocations.at(pc-5)!=arg.base) return {};
        const std::pair input{arg.base,size.base};
        if (initial && *initial!=input) return {};
        initial=input;
        pointer_reads.insert(pc-5);
    }
    if (!initial) return {};
    const auto [array_cell,size_cell]=*initial;
    const auto array=module.data_relocations.find(array_cell);
    if (!cells.unchanged(array_cell) || !cells.unchanged(size_cell) || array==module.data_relocations.end() ||
        module.data_relocations.contains(size_cell)) return {};
    const auto length=u32(module.section,size_cell), table=array->second;
    if (!length || length>INT32_MAX || table<module.text.size() || std::uint64_t(table)+4ull*length>module.section.size()) return {};
    const auto in_table=[&](Word value){return value>=table && value-table<4ull*length;};
    for (auto [at,value]:module.data_relocations) if (in_table(value) && at!=array_cell) return {};
    for (auto [at,value]:module.relocations) if (in_table(value) || (value==array_cell && !pointer_reads.contains(at))) return {};
    std::map<Word,std::vector<LogicAbiCall>> result;
    for (Word i=0;i<length;++i) {
        const Word slot=table+4*i;
        if (auto ref=module.data_relocations.find(slot);ref!=module.data_relocations.end()) {
            if (!module.functions.contains(ref->second)) return {};
            for (Word offset:{0x2e,0x5c,0x7a,0xb7,0x102,0x143}) {
                // The second manager call indexes with EDX. The other five use EBX.
                const Word site=start+offset, index_register=offset==0x5c ? 2 : 3;
                result[site].push_back({site,i,ref->second,logic_indexed_local,table,index_register,*pointer});
            }
        } else if (u32(module.section,slot)!=0) return {};
    }
    return result;
}

std::string_view binding_name(const LogicAbiCall& rule) {
    switch (rule.source) {
    case logic_unreachable: return "guarded-null";
    case logic_unreferenced: return "unreferenced-function";
    case logic_local_function: return "local-callback";
    case logic_indexed_local: return "indexed-local-callback";
    case logic_entry_contract: return "entry-contract";
    case logic_field_function: return "field-callback";
    case logic_unset_field: return "unset-field";
    case logic_saved_interface: return "saved-interface-method";
    case logic_installed_callback: return "installed-callback";
    case logic_descriptor_callback: return "descriptor-callback";
    case logic_registered_callback: return "registered-callback";
    default: return rule.named!=UINT32_MAX ? "named-method" : "engine-method";
    }
}

using Field=std::pair<Word,Word>; // Registered pointer cell and byte offset.

// The original global-data service can reallocate a shared buffer and write
// its address to several output cells. Examine references to *all* of them,
// including code outside the constructor and relocations in static data.
std::optional<ObjectUses> allocation_uses(const ModuleObject& module,const Decoder& decoder,
        const std::map<Word,CallSources>& sources,
        const std::map<Word,Word>& allocated,const std::map<Word,Word>& allocations,const std::set<Word>& external) {
    std::set<Word> roots;
    for (auto [root,size]:allocated) roots.insert(root);
    if (roots.empty()) return {};
    InterfaceCells cells(roots,module.relocations,module.data_references);
    ObjectUses uses;
    using Return=std::tuple<Word,ZydisRegister,Word>;
    std::set<Return> returned;
    std::vector<Return> pending;
    const auto save_returns=[&](Word fn) {
        for (auto [reg,offset]:uses.returns) {
            const Return value{fn,reg,offset};
            if (returned.insert(value).second) pending.push_back(value);
        }
        uses.returns.clear();
    };
    for (Word address:module.functions) {
        const auto fn=module_function(address,module,decoder);
        for (auto it=fn.instructions.begin();it!=fn.instructions.end();++it) {
            const auto& [pc,ins]=*it;
            if (ins.relocated_immediate && ins.decoded.mnemonic==ZYDIS_MNEMONIC_PUSH) {
                const auto cell=module.relocations.at(pc+ins.decoded.raw.imm[0].offset);
                const auto mov=fn.instructions.find(pc+ins.decoded.length);
                if (roots.contains(cell) && mov!=fn.instructions.end()) {
                    const auto& load=mov->second;
                    const Word call=mov->first+load.decoded.length;
                    if (load.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && load.operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                        load.operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY && load.operands[1].mem.base!=ZYDIS_REGISTER_ESP &&
                        load.operands[1].mem.index==ZYDIS_REGISTER_NONE && load.operands[1].mem.disp.value==4 &&
                        allocations.contains(call) && allocations.at(call)==cell) {
                        cells.output_argument(ins);
                        continue;
                    }
                }
            }
            cells.instruction(address,ins);
            if (!ins.relocated_displacement || !roots.contains(module.relocations.at(pc+ins.decoded.raw.disp.offset))) continue;
            if (!absolute_word(ins.operands[1]) || !object_accesses(fn,pc,uses)) return {};
        }
        save_returns(address);
    }
    std::map<Word,std::vector<std::pair<Word,Word>>> callers;
    for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls)
        if (call.direct) callers[call.target.base].emplace_back(fn,call.return_site);
    while (!pending.empty()) {
        const auto [fn,reg,offset]=pending.back();pending.pop_back();
        if (external.contains(fn) || returned.size()>4096) return {};
        for (auto [parent,pc]:callers[fn]) {
            if (!object_accesses_after(module_function(parent,module,decoder),pc,reg,offset,uses)) return {};
            save_returns(parent);
        }
    }
    if (!std::ranges::all_of(roots,[&](Word cell){return cells.unchanged(cell);})) return {};
    return uses;
}

std::map<Field,std::set<Word>> saved_interfaces(const std::map<Word,CallSources>& sources,const std::set<Word>& trusted,
        const std::map<Word,Word>& allocated,const ObjectUses& uses) {
    std::map<Field,std::set<Word>> result;
    if (!uses.pushes.empty()) return result;
    for (const auto& [fn,flow]:sources) if (trusted.contains(fn)) for (const auto& store:flow.object_stores) {
        const auto addr=store.address;
        if ((addr.kind==PointerSource::object_address || addr.kind==PointerSource::module_slot) &&
            store.value==PointerValue{PointerSource::argument,0} && allocated.contains(addr.base) &&
            std::uint64_t(addr.offset)+4<=allocated.at(addr.base))
            result[{addr.base,addr.offset}].insert(store.site);
    }
    std::erase_if(result,[&](const auto& field){
        const Word offset=field.first.second;
        return std::ranges::any_of(uses.writes,[&](const ObjectAccess& w){
            if (w.bytes && (w.offset-offset<4 || offset-w.offset<w.bytes))
                return w.offset!=offset || w.bytes!=4 || !field.second.contains(w.pc);
            return false;
        });
    });
    return result;
}

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
    std::ostringstream declarations, index, destructors, summary, abi_tables, abi_index, symbol_tables, symbol_index;
    std::size_t symbol_modules = 0;
    const auto interface_slots=evidence.interface_tables(abi_tables);
    std::size_t conditional_calls=0, passed_arguments=0;
    std::size_t unreferenced_calls=0, descriptor_methods=0, object_interfaces=0;
    std::size_t count = 0, registry_size = 0, unresolved_calls = 0, fixed_calls = 0, abi_modules = 0, abi_calls = 0;
    std::ofstream unresolved, argument_evidence, descriptor_evidence, field_evidence;
    std::map<std::string,std::set<Word>> descriptor_ids;
    constexpr std::string_view generic_fx="6FB36D1D87BA407457FADE9EF620E48CE7EEA9E74BB03C08835F0F69F0807DAE";
    std::set<Word> effect_callers;
    struct EffectMethod { std::string object; Word fn,site,id,method,target; };
    std::vector<EffectMethod> effect_methods;
    // Provider routines bound by entry contracts, checked against every named caller after all objects.
    struct Contract { std::string object; Word named, argument, fn, site; std::string bytes; PointerValue value; std::set<Word> uncalled{}; };
    std::vector<Contract> contracts, particle_contracts, descriptor_sites;
    std::map<Word,std::vector<std::pair<std::string,Word>>> named_callers; // named method -> (object, return PC)
    std::set<std::tuple<std::string,Word,Word>> passed_interfaces; // (object, return PC, argument)
    std::vector<std::pair<std::string,Word>> descriptor_callbacks; // (module symbol, routine)
    // Arguments at each named-method call: a routine of the calling object, zero, or unknown.
    struct ArgumentFact { std::string symbol; std::array<PointerValue,4> values; std::array<bool,4> routine; };
    std::map<Word,std::vector<ArgumentFact>> named_arguments;
    std::vector<std::pair<Word,Word>> callback_registries; // (named method, argument)
    std::vector<Contract> registry_sites; // named holds the registry index
    std::size_t enforced_calls=0;
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
        struct CallbackCell { std::set<Word> writers, targets; };
        // Routines stored at each displacement from a non-stack base register.
        std::map<Word,std::set<Word>> field_functions;
        // Relocated data addresses taken other than by absolute operands, and absolute writes.
        std::set<Word> address_taken=module.data_references;
        std::map<Word,std::set<Word>> taken_by; // Address-taken value -> routines taking it.
        struct AbsoluteWrite { Word pc, address, bytes; };
        std::vector<AbsoluteWrite> absolute_writes;
        std::map<Word,CallbackCell> callback_cells;
        std::map<Word,CallbackCell> immediate_cells; // Unfiltered immediate routine or zero stores.
        // Include references to interior instructions and shared tails. The
        // original loader exposes only the named exports outside the object.
        std::set<Word> code_references=module.data_references;
        for (const auto& [field,target]:module.relocations) code_references.insert(target);
        // Roots are the loader exports and any function containing a relocated
        // address. Every other entry is reached only through direct flow edges.
        std::set<Word> roots=api_entries;
        if (module.destructor) roots.insert(*module.destructor);
        std::map<Word,std::set<Word>> flow_edges;
        bool linked_entries=!module.relative_relocations;
        const auto floor=module.floor_events;
        bool reviewed_floor=floor && std::uint64_t(*floor)+floor_code_size<=module.text.size() &&
            media->file_sha256(entry.file,std::uint64_t(entry.object)+u32(bytes,40)+*floor,floor_code_size)==
                "E838677D9941160EEA5C84BE3F3CB40D8F7060B934B5FCF688F9234CD6B5C4ED";
        struct Unresolved { Word fn, pc; std::string bytes; PointerValue value; const CallSource* flow; };
        std::vector<Unresolved> remaining;
        std::ostringstream output;
        output << "#include \"xbox/cpu.h\"\nnamespace xbox_native {\nnamespace " << ns << " {\n";
        for (auto address : module.functions) output << std::format("void guest_{:08X}(Cpu&, Memory&);\n", address);
        for (auto address : module.functions) {
            const auto lowered = [&] {
                try {
                    auto fn = module_function(address, module, decoder);
                    for (const auto site:module.frame_step_operands)
                        if (auto it=fn.instructions.find(site);it!=fn.instructions.end()) it->second.frame_step_operand=true;
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
                        const auto after=pc+ins.decoded.length;
                        const auto ref=code_references.lower_bound(pc);
                        if (ref!=code_references.end() && *ref<after) roots.insert(address);
                        auto& edges=flow_edges[address];
                        if (ins.decoded.meta.category==ZYDIS_CATEGORY_CALL && ins.target()) edges.insert(*ins.target());
                        if (ins.cross_function_branch && ins.target()) edges.insert(*ins.target());
                        if (ins.cross_function_fallthrough) edges.insert(after);
                        edges.insert(ins.external_jump_targets.begin(),ins.external_jump_targets.end());
                        if (ins.call_next_instruction || (ins.decoded.meta.category==ZYDIS_CATEGORY_CALL &&
                            ins.target()==after)) linked_entries=false;
                        if (reviewed_floor && pc-*floor<floor_code_size) {
                            const Word offset=pc-*floor;
                            const auto owner=*floor+(offset<0x84 ? 0 : offset<0xd8 ? 0x84 : offset<0x11c ? 0xd8 : 0x11c);
                            if (address!=owner) reviewed_floor=false;
                        }
                        cells.instruction(address,ins);
                        if (ins.relocated_displacement && ins.decoded.mnemonic==ZYDIS_MNEMONIC_MOV &&
                            absolute_word(ins.operands[0]) && ins.operands[1].type==ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                            const auto slot=module.relocations.at(pc+ins.decoded.raw.disp.offset);
                            if (ins.relocated_immediate) {
                                const Word target=module.relocations.at(pc+ins.decoded.raw.imm[0].offset);
                                if (module.functions.contains(target)) {immediate_cells[slot].writers.insert(pc);immediate_cells[slot].targets.insert(target);}
                            } else if (!ins.operands[1].imm.value.u) immediate_cells[slot].writers.insert(pc);
                            if (module.zero_words.contains(slot)) {
                                if (ins.relocated_immediate) {
                                    const Word target=module.relocations.at(pc+ins.decoded.raw.imm[0].offset);
                                    if (module.functions.contains(target)) {
                                        callback_cells[slot].writers.insert(pc);
                                        callback_cells[slot].targets.insert(target);
                                    }
                                } else if (!ins.operands[1].imm.value.u) callback_cells[slot].writers.insert(pc);
                            }
                        }
                        if (ins.relocated_immediate) {
                            const Word value=module.relocations.at(pc+ins.decoded.raw.imm[0].offset);
                            address_taken.insert(value);
                            taken_by[value].insert(address);
                        }
                        if (ins.relocated_displacement) {
                            const Word value=module.relocations.at(pc+ins.decoded.raw.disp.offset);
                            for (const auto& op:std::span(ins.operands).first(ins.decoded.operand_count_visible)) {
                                if (op.type!=ZYDIS_OPERAND_TYPE_MEMORY) continue;
                                if (op.mem.base!=ZYDIS_REGISTER_NONE || op.mem.index!=ZYDIS_REGISTER_NONE ||
                                    ins.decoded.mnemonic==ZYDIS_MNEMONIC_LEA || !op.size) {address_taken.insert(value);taken_by[value].insert(address);}
                                else if (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) absolute_writes.push_back({pc,value,Word(op.size/8)});
                            }
                        }
                        if (const auto& dst=ins.operands[0]; ins.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && ins.relocated_immediate &&
                            dst.type==ZYDIS_OPERAND_TYPE_MEMORY && dst.size==32 && dst.mem.index==ZYDIS_REGISTER_NONE && !ins.relocated_displacement &&
                            dst.mem.base!=ZYDIS_REGISTER_NONE && dst.mem.base!=ZYDIS_REGISTER_ESP && dst.mem.base!=ZYDIS_REGISTER_EBP) {
                            const Word stored=module.relocations.at(pc+ins.decoded.raw.imm[0].offset);
                            if (module.functions.contains(stored)) field_functions[static_cast<Word>(dst.mem.disp.value)].insert(stored);
                        }
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
        std::erase_if(callback_cells,[&](const auto& cell){
            return cell.second.targets.empty() || !cells.writers_within(cell.first,cell.second.writers);
        });
        const auto interface_functions=CallEvidence::interface_functions(api_entries,other_entries,sources);
        evidence.module(entry.hash,module.text,module.functions,api_entries,sources);
        evidence.add_registered_cells(entry.hash,sources);
        std::map<Word,std::string_view> missing_cells;
        const auto getters=cells.verified_getters(interface_functions,sources,missing_cells);
        const auto names=CallEvidence::cache_names(module.text,sources,getters);
        // Constructor and destructor results die in the original scheduler.
        // 0x3D435 and 0x3D4F4 both call 0x3D2D0, whose helper at
        // 0x27030 overwrites EDX and EAX before using either. A second entry
        // via a COFF pointer or branch is still external to the return trace.
        auto external_returns=other_entries;
        for (const auto& [fn,loader]:module.exports) if (loader!=logic_constructor_call) external_returns.insert(fn);
        other_entries.insert(api_entries.begin(),api_entries.end());
        if (module.destructor) other_entries.insert(*module.destructor);
        const auto floor_calls=reviewed_floor ? floor_event_calls(module,cells,other_entries,sources) : std::map<Word,std::vector<LogicAbiCall>>{};
        const auto table_args=CallEvidence::table_arguments(other_entries,getters,sources);
        // MiscFunc+0x1c forwards the array and count in message 0x16f.
        // Record both original instructions even when another use of the
        // callback or a recipient of the message still needs a proof.
        struct Submission { Word pc, object, cell, start, count; };
        std::vector<Submission> submissions;
        std::map<std::pair<Word,Word>,LogicAbiCall> effect_targets;
        std::map<Word,Word> allocated, allocations;
        std::set<Word> local_cells;
        bool known_allocations=true;
        for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) {
            const auto ptr=call.first_argument, size=call.second_argument;
            // Engine +8 reaches 0x48010, which allocates or grows the current
            // instance's zero-filled buffer at 0x3D750 and binds the cell to it.
            if (interface_functions.contains(fn) && call.target==PointerValue{PointerSource::argument_method,0,8}) {
                if (ptr.kind==PointerSource::relocated && module.private_words.contains(ptr.base))
                    if (auto rule=evidence.export_call(call);rule && rule->target==0x48010) local_cells.insert(ptr.base);
                continue;
            }
            if (interface_functions.contains(fn) && call.target==PointerValue{PointerSource::argument_method,0,4}) {
                if (ptr.kind!=PointerSource::relocated || size.kind!=PointerSource::constant || !module.private_words.contains(ptr.base)) {
                    known_allocations=false;
                    continue;
                }
                if (auto rule=evidence.export_call(call);rule && rule->target==0x47fa0) {
                    allocations[call.site]=ptr.base;
                    auto [p,first]=allocated.emplace(ptr.base,size.base);
                    if (!first) p->second=std::min(p->second,size.base);
                }
            }
            if (call.target.kind!=PointerSource::module_method || call.target.offset!=0x1c ||
                !names.contains(call.target.base) || names.at(call.target.base)!="MiscFunc" ||
                !evidence.named_call(call,"MiscFunc") || ptr.kind!=PointerSource::constant ||
                (size.kind!=PointerSource::object_address && size.kind!=PointerSource::module_slot) ||
                call.third_argument.kind!=PointerSource::constant || call.third_argument.base>5) continue;
            submissions.push_back({call.site,ptr.base,size.base,size.offset,call.third_argument.base});
        }
        const bool field_calls=std::ranges::any_of(remaining,[](const Unresolved& s){
            return s.value.kind==PointerSource::object_field_method;
        });
        const auto uses=known_allocations && (field_calls || !submissions.empty()) ?
            allocation_uses(module,decoder,sources,allocated,allocations,external_returns) : std::optional<ObjectUses>{};
        if (uses && field_calls) {
            const auto fields=saved_interfaces(sources,interface_functions,allocated,*uses);
            std::map<Word,std::set<std::pair<Word,Word>>> installed;
            for (const auto& [fn,flow]:sources) if (interface_functions.contains(fn)) for (const auto& call:flow.calls) {
                if (call.target!=PointerValue{PointerSource::argument_method,0,0} ||
                    call.first_argument.kind!=PointerSource::constant || call.second_argument.kind!=PointerSource::relocated ||
                    !module.functions.contains(call.second_argument.base)) continue;
                const auto setter=evidence.export_call(call);
                if (setter && setter->target==0x47d60)
                    installed[call.first_argument.base].emplace(call.site,call.second_argument.base);
            }
            std::map<std::pair<Word,Word>,Field> lookups;
            for (const auto& site:remaining) if (site.value.kind==PointerSource::object_field_method) {
                const auto field=fields.find({site.value.base,site.value.producer});
                if (field==fields.end()) continue;
                CallSource original{};
                original.target={PointerSource::argument_method,0,site.value.offset};
                const auto callee=evidence.export_call(original);
                if (!callee) continue;
                if (!field_evidence.is_open()) {
                    field_evidence.exceptions(std::ios::failbit|std::ios::badbit);
                    field_evidence.open(staging/"module-object-interfaces.tsv");
                    field_evidence<<"object_sha256\tfunction_offset\tsite_offset\tcell\tfield\tmodule_store"
                        "\tmethod\txbe_target\tlookup_site\tcallback_index\tinstaller_site\tmodule_target\n";
                }
                for (Word writer:field->second)
                    field_evidence<<entry.hash<<'\t'<<hex32(site.fn)<<'\t'<<hex32(site.pc)<<'\t'<<hex32(site.value.base)
                        <<'\t'<<hex32(site.value.producer)<<'\t'<<hex32(writer)<<'\t'<<hex32(site.value.offset)
                        <<'\t'<<hex32(callee->target)<<"\t-\t-\t-\t-\n";
                ++object_interfaces;
                if (callee->target==0x47dc0) lookups[{site.fn,site.pc}]=field->first;
            }
            for (const auto& site:remaining) if (site.value.kind==PointerSource::callback_result) {
                const auto input=lookups.find({site.fn,site.value.producer});
                if (input==lookups.end() || !installed.contains(site.value.offset)) continue;
                for (Word writer:fields.at(input->second)) for (auto [install,target]:installed.at(site.value.offset))
                    field_evidence<<entry.hash<<'\t'<<hex32(site.fn)<<'\t'<<hex32(site.pc)<<'\t'<<hex32(input->second.first)
                        <<'\t'<<hex32(input->second.second)<<'\t'<<hex32(writer)<<"\t0x00000074\t0x00047DC0\t"
                        <<hex32(site.value.producer)<<'\t'<<hex32(site.value.offset)<<'\t'<<hex32(install)<<'\t'<<hex32(target)<<'\n';
            }
        }
        if (!submissions.empty()) {
            const auto object_args=CallEvidence::table_arguments(other_entries,allocated,sources);
            std::map<Word,std::set<Word>> callback_ids;
            for (const auto& [fn,flow]:sources) for (const auto& store:flow.object_stores) {
                if (store.value.kind!=PointerSource::relocated || !module.functions.contains(store.value.base)) continue;
                const auto addr=store.address;
                Word cell=addr.base;
                if (addr.kind==PointerSource::argument || addr.kind==PointerSource::argument_address) {
                    const auto resolved=object_args.find({fn,addr.base});
                    if (resolved==object_args.end()) continue;
                    cell=resolved->second;
                }
                for (const auto& sub:submissions) {
                    // generic_fx reads +0x84 and advances 0xac bytes per descriptor.
                    if (cell!=sub.cell || !allocated.contains(cell) ||
                        std::uint64_t(sub.start)+sub.count*0xacu>allocated.at(cell) ||
                        addr.offset<std::uint64_t(sub.start)+0x84) continue;
                    const Word offset=addr.offset-sub.start-0x84;
                    if (offset%0xac || offset/0xac>=sub.count) continue;
                    descriptor_ids[entry.hash].insert(sub.object);
                    callback_ids[store.value.base].insert(sub.object);
                    if (!descriptor_evidence.is_open()) {
                        descriptor_evidence.exceptions(std::ios::failbit|std::ios::badbit);
                        descriptor_evidence.open(staging/"module-descriptors.tsv");
                        descriptor_evidence<<"object_sha256\tstore_function\tstore_site\tallocated_cell\tallocation_bytes\tpublish_site"
                            "\tspawn_id\tarray_offset\tcount\tindex\tcallback_field\tcallback_function\tproducer_field_reads\tproducer_pointer_pushes\n";
                    }
                    descriptor_evidence<<entry.hash<<'\t'<<hex32(fn)<<'\t'<<hex32(store.site)<<'\t'<<hex32(cell)
                        <<'\t'<<allocated.at(cell)<<'\t'<<hex32(sub.pc)<<'\t'<<hex32(sub.object)<<'\t'<<hex32(sub.start)
                        <<'\t'<<sub.count<<'\t'<<offset/0xac<<'\t'<<hex32(addr.offset)<<'\t'<<hex32(store.value.base);
                    if (uses) descriptor_evidence<<'\t'<<std::ranges::count_if(uses->reads,[&](const ObjectAccess& r){
                        return r.offset-addr.offset<4 || addr.offset-r.offset<r.bytes;
                    })<<'\t'<<uses->pushes.size()<<'\n';
                    else descriptor_evidence<<"\t-\t-\n";
                }
            }
            for (const auto& [fn,ids]:callback_ids)
                descriptor_callbacks.emplace_back(ns+"_"+std::to_string(module.exports.front().second),fn);
            for (const auto& site:remaining) {
                if (site.value.kind!=PointerSource::argument_method || site.value.base!=0 ||
                    !callback_ids.contains(site.fn) || !site.flow) continue;
                const auto binding=evidence.table_call(*site.flow,logic_particle_interface_slot);
                if (binding && binding->table==0x1f03930 && binding->getter==0x47ec0) {
                    effect_targets[{site.fn,site.pc}]=*binding;
                    for (Word id:callback_ids.at(site.fn))
                        effect_methods.push_back({entry.hash,site.fn,site.pc,id,site.value.offset,binding->target});
                }
            }
        }
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
            } else if (value.kind==PointerSource::argument_method && !value.base && interface_functions.contains(fn))
                rule=evidence.export_call(call);
            else if (value.kind==PointerSource::argument_method && table_args.contains({fn,value.base})) {
                call.target.base=table_args.at({fn,value.base});
                rule=evidence.table_call(call,getters.at(call.target.base));
                if (!rule && names.contains(call.target.base)) rule=evidence.named_call(call,names.at(call.target.base));
            }
            return rule;
        };
        for (const auto& site:remaining) {
            std::span<const PointerValue> origins{&site.value,1};
            const auto& alternatives=sources.at(site.fn).alternatives;
            if (alternatives.contains(site.pc)) origins=alternatives.at(site.pc);
            std::vector<LogicAbiCall> accepted;
            // This COFF spills the particle interface from +58FC at +3093.
            // Its only address-taken local is the adjacent vector [EBP-10h].
            // The +3113 service reads its three components via 6BC70,
            // 9C410 and A1CA3. No path writes the spill [EBP-14h] again.
            const PointerValue spilled{PointerSource::module_method,0x58fc,0x3c};
            if (entry.hash=="D991A36480482AD6F203131650A3EEB11397D638083A57D2BF14A6D188210E69" &&
                site.fn==0x306c && (site.pc==0x312e || site.pc==0x3163 || site.pc==0x3191) &&
                getters.contains(0x58f0) && getters.at(0x58f0)==0x24 && getters.contains(spilled.base) &&
                getters.at(spilled.base)==logic_particle_interface_slot) origins={&spilled,1};
            if (auto table=floor_calls.find(site.pc);table!=floor_calls.end()) {
                accepted=table->second;
                origins={};
            }
            const auto& guards=sources.at(site.fn).nonzero_cells;
            if (auto guard=guards.find(site.pc);guard!=guards.end() && module.zero_words.contains(guard->second) && cells.unchanged(guard->second)) {
                // Every path to this CALL requires a private, permanently null pointer to be nonzero.
                accepted.push_back({site.pc,guard->second,0,logic_unreachable});
                origins={};
            }
            for (auto value:origins) {
                const auto key=std::pair{site.fn,value.base};
                if (value.kind==PointerSource::relocated && module.functions.contains(value.base)) {
                    accepted.push_back({site.pc,0,value.base,logic_local_function});
                } else if (value.kind==PointerSource::module_slot && callback_cells.contains(value.base) &&
                    guards.contains(site.pc) && guards.at(site.pc)==value.base) {
                    for (auto target:callback_cells.at(value.base).targets)
                        accepted.push_back({site.pc,value.base,target,logic_local_function});
                } else if (value.kind==PointerSource::argument && callbacks.contains(key) && !unknown_arguments.contains(key)) {
                    for (auto offset:callbacks.at(key)) accepted.push_back({site.pc,value.base,offset,logic_local_function});
                } else if (auto rule=abi_rule(site.fn,site.pc,value)) accepted.push_back(*rule);
                else {accepted.clear();break;}
            }
            if (accepted.empty()) {shared_sites.insert(site.pc);continue;}
            const auto [pos,added]=rules.try_emplace(site.pc,std::move(accepted));
            if (!added && pos->second!=accepted) shared_sites.insert(site.pc);
        }
        for (auto site:shared_sites) rules.erase(site);
        // A module address exists at run time only after the loader adds the
        // base to a relocated field or publishes one of the three exports
        // (0x3A410-0x3A52F). Without REL32 fields or CALL $+5, a routine outside
        // the closure of those roots and their direct calls, branches, switch
        // edges and fallthroughs can never execute. A shared instruction stays
        // open while any owner is reachable.
        if (linked_entries && !shared_sites.empty()) {
            std::set<Word> reachable;
            std::vector<Word> pending(roots.begin(),roots.end());
            while (!pending.empty()) {
                const Word fn=pending.back();pending.pop_back();
                if (!reachable.insert(fn).second) continue;
                if (const auto out=flow_edges.find(fn);out!=flow_edges.end()) pending.insert(pending.end(),out->second.begin(),out->second.end());
            }
            std::set<Word> live;
            for (const auto& site:remaining) if (reachable.contains(site.fn)) live.insert(site.pc);
            for (const auto& site:remaining) if (shared_sites.contains(site.pc) && !live.contains(site.pc))
                rules[site.pc]={{site.pc,site.fn,0,logic_unreferenced}};
        }
        // Evidence-bound transfers. Each target set is taken from every original
        // instruction in this object that installs a routine where the CALL loads
        // it. The player rejects any other destination at the originating CALL.
        for (const auto& [fn,flow]:sources) for (const auto& store:flow.object_stores) {
            const auto kind=store.address.kind;
            if (store.value.kind==PointerSource::relocated && module.functions.contains(store.value.base) &&
                (kind==PointerSource::object_address || kind==PointerSource::module_slot ||
                 kind==PointerSource::argument || kind==PointerSource::argument_address))
                field_functions[store.address.offset].insert(store.value.base);
        }
        std::map<Word,std::set<Word>> installed_callbacks;
        for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) {
            const auto bound=rules.find(call.site);
            if (call.direct || bound==rules.end() || bound->second.empty() || call.first_argument.kind!=PointerSource::constant ||
                std::ranges::any_of(bound->second,[](const LogicAbiCall& r){return r.target!=0x47d60;})) continue;
            // Engine +0x00 (0x47D60) stores argument one at index argument zero of the current module record.
            if (call.second_argument.kind==PointerSource::relocated && module.functions.contains(call.second_argument.base))
                installed_callbacks[call.first_argument.base].insert(call.second_argument.base);
            else installed_callbacks[call.first_argument.base].insert(UINT32_MAX);
        }
        std::set<std::pair<Word,Word>> saved_api_fields;
        for (const auto& [fn,flow]:sources) if (interface_functions.contains(fn)) for (const auto& store:flow.object_stores)
            if (store.value==PointerValue{PointerSource::argument,0} &&
                (store.address.kind==PointerSource::object_address || store.address.kind==PointerSource::module_slot))
                saved_api_fields.emplace(store.address.base,store.address.offset);
        std::set<Word> state_functions;
        for (const auto& [field,targets]:field_functions) state_functions.insert(targets.begin(),targets.end());
        const auto provided=evidence.provided_methods(ns+"_"+std::to_string(module.exports.front().second));
        const auto engine_method=[&](Word slot) {
            CallSource call{};
            call.target={PointerSource::argument_method,0,slot};
            const auto rule=evidence.export_call(call);
            return rule ? rule->target : 0;
        };
        for (const auto& site:remaining) {
            if (rules.contains(site.pc)) continue;
            std::span<const PointerValue> origins{&site.value,1};
            if (const auto& alternatives=sources.at(site.fn).alternatives;alternatives.contains(site.pc)) origins=alternatives.at(site.pc);
            std::vector<LogicAbiCall> bound;
            for (const auto value:origins) {
                const auto before=bound.size();
                const Word arg=value.base, field=value.offset;
                const bool frame=site.flow && arg<3 && (site.flow->frame_arguments & (1u<<arg)) && !(field&3);
                if (value.kind==PointerSource::module_method && (local_cells.contains(arg) || allocated.contains(arg))) {
                    // A cell given to engine +4 or +8 holds the start of the module or instance buffer.
                    if (const auto it=field_functions.find(field);it!=field_functions.end())
                        for (Word target:it->second) bound.push_back({site.pc,arg,target,logic_field_function});
                    else bound.push_back({site.pc,arg,0,logic_unset_field});
                } else if (value.kind==PointerSource::object_field_method && field==0x74 &&
                           saved_api_fields.contains({arg,value.producer}) && engine_method(0x74)==0x47dc0) {
                    bound.push_back({site.pc,0x74,0x47dc0,logic_saved_interface});
                } else if (value.kind==PointerSource::callback_result && rules.contains(value.producer) &&
                           std::ranges::all_of(rules.at(value.producer),[](const LogicAbiCall& r){return r.source==logic_saved_interface;})) {
                    // 0x47DC0 returns entry argument zero of the current record's table, or zero past its end (0x3AA80).
                    const auto it=installed_callbacks.find(field);
                    if (it==installed_callbacks.end()) bound.push_back({site.pc,field,0,logic_unset_field});
                    else if (!it->second.contains(UINT32_MAX))
                        for (Word target:it->second) bound.push_back({site.pc,field,target,logic_installed_callback});
                } else if (value.kind==PointerSource::argument_method && frame && effect_targets.contains({site.fn,site.pc})) {
                    const auto& effect=effect_targets.at({site.fn,site.pc});
                    bound.push_back({site.pc,arg,effect.target,logic_entry_contract,effect.table,field,effect.getter});
                    particle_contracts.push_back({entry.hash,0,arg,site.fn,site.pc,site.bytes,value});
                } else if (value.kind==PointerSource::argument_method && frame && provided.contains(site.fn)) {
                    bound.push_back({site.pc,arg,0,logic_entry_contract,0,field,0});
                    for (Word named:provided.at(site.fn)) contracts.push_back({entry.hash,named,arg,site.fn,site.pc,site.bytes,value});
                } else if (value.kind==PointerSource::argument_method && state_functions.contains(site.fn) &&
                           field_functions.contains(field) && field_functions.at(field).contains(site.fn)) {
                    // A state routine stored at this field loads its successor from the same field of its argument.
                    for (Word target:field_functions.at(field)) bound.push_back({site.pc,arg,target,logic_field_function});
                } else if (value.kind==PointerSource::module_slot && arg>=module.text.size() &&
                           std::uint64_t(arg)+4<=module.section.size() && !u32(module.section,arg) &&
                           !module.data_relocations.contains(arg)) {
                    // A provider method stores one of its arguments in this private cell.
                    // Every writer must be such a store, a relocated routine or zero.
                    std::set<Word> writers;
                    std::vector<std::size_t> registries;
                    for (const auto& [fn,flow]:sources) for (const auto& store:flow.argument_stores) {
                        if (store.slot!=arg || store.argument>3 || !provided.contains(fn)) continue;
                        writers.insert(store.site);
                        for (Word named:provided.at(fn)) {
                            const std::pair key{named,store.argument};
                            auto it=std::ranges::find(callback_registries,key);
                            if (it==callback_registries.end()) it=callback_registries.insert(it,key);
                            registries.push_back(static_cast<std::size_t>(it-callback_registries.begin()));
                        }
                    }
                    const auto local=immediate_cells.find(arg);
                    if (local!=immediate_cells.end()) writers.insert(local->second.writers.begin(),local->second.writers.end());
                    // Outside a private word, no address may be taken within 0x100 bytes
                    // below the cell in its COFF data symbol (static BSS shares one symbol),
                    // so every write that can reach it is an absolute operand.
                    // A provider routine taking such an address is accepted only if no
                    // object calls it; that is checked after every object is indexed.
                    std::set<Word> uncalled;
                    const auto direct_only=[&] {
                        constexpr Word window=0x100;
                        const auto end=module.data_symbols.upper_bound(arg);
                        if (end==module.data_symbols.begin() || end==module.data_symbols.end()) return false;
                        const Word start=*std::prev(end), stop=*end;
                        if (std::uint64_t(arg)+4>stop) return false;
                        const Word low=std::max(start,arg>window ? arg-window : Word{0});
                        for (auto taken=address_taken.lower_bound(low);taken!=address_taken.end() && *taken<std::min<std::uint64_t>(stop,std::uint64_t(arg)+4);++taken) {
                            const auto by=taken_by.find(*taken);
                            if (by==taken_by.end()) return false;
                            for (Word fn:by->second) {
                                if (!provided.contains(fn)) return false;
                                uncalled.insert(provided.at(fn).begin(),provided.at(fn).end());
                            }
                        }
                        return std::ranges::all_of(absolute_writes,[&](const AbsoluteWrite& w) {
                            if (!(w.address<std::uint64_t(arg)+4 && arg<std::uint64_t(w.address)+w.bytes)) return true;
                            return w.address==arg && w.bytes==4 && writers.contains(w.pc);
                        });
                    };
                    if (!registries.empty() && (module.private_words.contains(arg) ? cells.writers_within(arg,writers) : direct_only())) {
                        if (local!=immediate_cells.end())
                            for (Word target:local->second.targets) bound.push_back({site.pc,arg,target,logic_local_function});
                        for (auto index:registries) {
                            bound.push_back({site.pc,arg,0,logic_registered_callback,static_cast<Word>(index)});
                            registry_sites.push_back({entry.hash,static_cast<Word>(index),0,site.fn,site.pc,site.bytes,value,uncalled});
                        }
                    }
                } else if (value.kind==PointerSource::unknown && entry.hash==generic_fx && (site.pc==0xec7 || site.pc==0xf2f)) {
                    bound.push_back({site.pc,0,0,logic_descriptor_callback});
                    descriptor_sites.push_back({entry.hash,0,0,site.fn,site.pc,site.bytes,value});
                }
                if (bound.size()==before) {bound.clear();break;}
            }
            if (!bound.empty()) rules[site.pc]=std::move(bound);
        }
        // Conditional checks are kept in the unresolved inventory until every
        // incoming edge is known. At runtime a verified caller can establish
        // the table type for this particular invocation of a callback.
        std::map<Word,std::optional<LogicAbiCall>> conditional;
        std::map<std::pair<Word,Word>,Word> passed;
        // A resolved call can supply an interface to a callee whose other callers
        // are unknown. Join all owners of the return instruction before publishing it.
        for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) {
            const std::array inputs{call.first_argument,call.second_argument,call.third_argument};
            const auto verified=[&](PointerValue input) {
                return input.kind==PointerSource::module_slot && getters.contains(input.base) &&
                       interface_slots.contains(getters.at(input.base));
            };
            const auto paths=flow.alternative_arguments.find(call.site);
            for (Word i=0;i<inputs.size();++i) {
                const auto input=inputs[i];
                Word cell=verified(input) ? input.base : UINT32_MAX;
                // At a join, one replayed path may pass a verified interface while
                // another selects a different routine. The player compares the
                // frame argument with that interface's table before accepting.
                if (cell==UINT32_MAX && paths!=flow.alternative_arguments.end()) {
                    std::set<Word> cells;
                    for (const auto& path:paths->second) if (verified(path[i])) cells.insert(path[i].base);
                    if (cells.size()==1) cell=*cells.begin();
                }
                auto [p,first]=passed.try_emplace(std::pair{call.return_site,i},cell);
                if (!first && p->second!=cell) p->second=UINT32_MAX;
            }
        }
        for (const auto& site:remaining) {
            if (const auto it=rules.find(site.pc);it!=rules.end()) {
                unreferenced_calls+=it->second.front().source==logic_unreferenced;
                for (const auto& rule:it->second) abi_sites<<entry.hash<<'\t'<<hex32(site.fn)<<'\t'<<hex32(site.pc)<<'\t'<<hex32(rule.slot)<<'\t'<<hex32(rule.target)
                         <<'\t'<<hex32(rule.source)<<'\t'<<hex32(rule.table)<<'\t'<<hex32(rule.method)<<'\t'<<hex32(rule.getter)
                         <<'\t'<<(rule.named==UINT32_MAX ? "-" : names.at(rule.source))<<'\t'
                         <<binding_name(rule)<<'\n';
                ++abi_calls;
                enforced_calls+=std::ranges::any_of(it->second,[](const LogicAbiCall& r){
                    return r.source>=logic_registered_callback && r.source<=logic_entry_contract;
                });
                const Word returned=site.flow ? site.flow->return_site : site.pc+Word(site.bytes.size()/2);
                for (const auto& rule:it->second) if (rule.named!=UINT32_MAX) named_callers[rule.named].emplace_back(entry.hash,returned);
                if (site.flow) for (const auto& rule:it->second) if (rule.named!=UINT32_MAX) {
                    ArgumentFact fact{ns+"_"+std::to_string(module.exports.front().second),
                        {site.flow->first_argument,site.flow->second_argument,site.flow->third_argument,site.flow->fourth_argument},{}};
                    for (unsigned i=0;i<4;++i) fact.routine[i]=fact.values[i].kind==PointerSource::relocated && module.functions.contains(fact.values[i].base);
                    named_arguments[rule.named].push_back(std::move(fact));
                }
            } else {
                const auto arg=site.value.base;
                std::optional<LogicAbiCall> parameter;
                if (site.flow && site.value.kind==PointerSource::argument_method && arg<3 &&
                    (site.flow->frame_arguments & (1u<<arg)) && !(site.value.offset&3)) {
                    parameter=LogicAbiCall{site.pc,arg,0,logic_passed_argument,0,site.value.offset};
                    if (const auto effect=effect_targets.find({site.fn,site.pc});effect!=effect_targets.end()) {
                        parameter->table=effect->second.table;
                        parameter->target=effect->second.target;
                        parameter->getter=effect->second.getter;
                    }
                }
                auto [item,added]=conditional.try_emplace(site.pc,parameter);
                if (!added && item->second!=parameter) item->second.reset();
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
        for (const auto& [key,cell]:passed) passed_interfaces.emplace(entry.hash,key.first,key.second);
        if (entry.hash==generic_fx) for (Word pc:{0xec9u,0xf31u}) {
            const auto arg=passed.find({pc,0});
            if (arg!=passed.end() && getters.at(arg->second)==0x40) effect_callers.insert(pc);
        }
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
        if (!module.symbols.empty()) {
            symbol_tables << "const std::array<ModuleSymbol," << module.symbols.size() << "> symbols_" << symbol_modules << "{{\n";
            for (const auto& [name, offset] : module.symbols) symbol_tables << "    {\"" << name << "\", " << hex32(offset) << "U},\n";
            symbol_tables << "}};\n";
            symbol_index << std::format("    {{&{}_{}, symbols_{}}},\n", ns, module.exports.front().second, symbol_modules++);
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
    std::map<ReceiverKey,std::set<std::string>> receivers;
    if (media) receivers=descriptor_receivers(*media,inventory,descriptor_ids,staging);
    // Check evidence that spans objects. A failed check returns the site to the open inventory.
    std::set<std::tuple<std::string,Word,Word>> reopened;
    const auto reopen=[&](const std::string& object,Word fn,Word pc,const std::string& bytes,PointerValue value,std::string_view missing) {
        if (!reopened.emplace(object,fn,pc).second) return;
        if (!unresolved.is_open()) {
            unresolved.exceptions(std::ios::failbit|std::ios::badbit);
            unresolved.open(staging/"module-indirect.tsv");
            unresolved<<"object_sha256\tfunction_offset\tsite_offset\tbytes\tsource\tbase\tfield\tmissing_evidence\n";
        }
        unresolved<<object<<'\t'<<hex32(fn)<<'\t'<<hex32(pc)<<'\t'<<bytes<<'\t'<<source_name(value.kind)
                  <<'\t'<<hex32(value.base)<<'\t'<<hex32(value.offset)<<'\t'<<missing<<'\n';
        ++unresolved_calls;
        --abi_calls;
        --enforced_calls;
    };
    for (const auto& c:contracts) {
        const auto callers=named_callers.find(c.named);
        if (callers==named_callers.end() || std::ranges::any_of(callers->second,[&](const auto& caller){
                return !passed_interfaces.contains({caller.first,caller.second,c.argument});
            })) reopen(c.object,c.fn,c.site,c.bytes,c.value,"caller-without-verified-interface");
    }
    std::set<std::pair<std::string,Word>> published;
    for (const auto& m:effect_methods) {
        const auto it=receivers.find({m.object,m.id});
        if (effect_callers.size()==2 && it!=receivers.end() && it->second.size()==1 && *it->second.begin()==generic_fx)
            published.emplace(m.object,m.site);
    }
    for (const auto& m:effect_methods) {
        const auto it=receivers.find({m.object,m.id});
        if (!(effect_callers.size()==2 && it!=receivers.end() && it->second.size()==1 && *it->second.begin()==generic_fx))
            published.erase({m.object,m.site});
    }
    for (const auto& c:particle_contracts)
        if (!published.contains({c.object,c.site})) reopen(c.object,c.fn,c.site,c.bytes,c.value,"descriptor-receiver");
    for (const auto& c:descriptor_sites)
        if (descriptor_callbacks.empty() || effect_callers.size()!=2) reopen(c.object,c.fn,c.site,c.bytes,c.value,"descriptor-callbacks");
    std::string callback_list, registry_lists, registry_spans;
    for (const auto& [symbol,offset]:descriptor_callbacks) callback_list+=std::format("    {{&{},{}U}},\n",symbol,offset);
    for (std::size_t i=0;i<callback_registries.size();++i) {
        const auto [named,argument]=callback_registries[i];
        std::set<std::pair<std::string,Word>> routines;
        const auto facts=named_arguments.find(named);
        // With no named caller the stored routine stays at its zero initial value.
        bool known=facts==named_arguments.end() ? named_callers[named].empty() : named_callers[named].size()==facts->second.size();
        if (known) for (const auto& fact:facts->second) {
            const auto value=fact.values[argument];
            if (fact.routine[argument]) routines.emplace(fact.symbol,value.base);
            else if (value!=PointerValue{PointerSource::constant,0}) known=false;
        }
        if (!known) for (const auto& c:registry_sites)
            if (c.named==i) reopen(c.object,c.fn,c.site,c.bytes,c.value,"registered-callback-argument");
        for (const auto& c:registry_sites) if (c.named==i && std::ranges::any_of(c.uncalled,[&](Word method){
                return named_callers.contains(method) && !named_callers.at(method).empty();
            })) reopen(c.object,c.fn,c.site,c.bytes,c.value,"called-provider-address");
        std::string entries;
        for (const auto& [symbol,offset]:routines) entries+=std::format("    {{&{},{}U}},\n",symbol,offset);
        registry_lists+=std::format("const std::array<LogicProvider,{}> registry_{}{{{{\n{}}}}};\n",routines.size(),i,entries);
        registry_spans+=std::format("    registry_{},\n",i);
    }
    const auto module_index = std::format("#include \"xbox/cpu.h\"\n#include \"xbox/logic_exports.h\"\nnamespace xbox_native {{\n{}namespace {{\n"
        "CompiledModule destructor(CompiledModule module, std::uint32_t offset) {{\n"
        "    module.registration = offset; module.registration_call = logic_destructor_call; return module;\n}}\n"
        "const std::array<CompiledModule, {}> modules{{{{\n{}{}}}}};\n{}const std::array<LogicAbiModule,{}> abi_modules{{{{\n{}}}}};\n{}"
        "const std::array<LogicProvider,{}> descriptor_callback_list{{{{\n{}}}}};\n{}"
        "const std::array<std::span<const LogicProvider>,{}> registered_callback_list{{{{\n{}}}}};\n }}\n"
        "const std::span<const CompiledModule> compiled_modules{{modules}};\nconst std::span<const LogicAbiModule> compiled_logic_abi{{abi_modules}};\n"
        "const std::span<const LogicNamedMethod> compiled_named_methods{{named_methods}};\n"
        "const std::span<const LogicProvider> compiled_descriptor_callbacks{{descriptor_callback_list}};\n"
        "const std::span<const std::span<const LogicProvider>> compiled_registered_callbacks{{registered_callback_list}};\n}}\n",
        declarations.str(), registry_size, index.str(),destructors.str(),abi_tables.str(),abi_modules,abi_index.str(),evidence.provider_tables(),
        descriptor_callbacks.size(),callback_list,registry_lists,callback_registries.size(),registry_spans);
    const auto symbol_index_text = std::format("#include \"xbox/module_symbols.h\"\nnamespace xbox_native {{\nnamespace {{\n{}"
        "const std::array<ModuleSymbols,{}> symbol_index{{{{\n{}}}}};\n}}\n"
        "const std::span<const ModuleSymbols> compiled_module_symbols{{symbol_index}};\n}}\n",
        symbol_tables.str(), symbol_modules, symbol_index.str());
    if (emit) write_text(staging / "game.module.cpp", module_index + symbol_index_text);
    if (unresolved.is_open()) unresolved.close();
    if (argument_evidence.is_open()) argument_evidence.close();
    if (descriptor_evidence.is_open()) descriptor_evidence.close();
    if (media) {
        if (!effect_methods.empty() && effect_callers.size()==2) {
            std::ofstream methods;
            methods.exceptions(std::ios::failbit|std::ios::badbit);
            methods.open(staging/"module-descriptor-methods.tsv");
            methods<<"producer_sha256\tcallback_function\tsite_offset\tspawn_id\tmethod_offset\txbe_target\n";
            for (const auto& m:effect_methods) {
                const auto it=receivers.find({m.object,m.id});
                if (it!=receivers.end() && it->second.size()==1 && *it->second.begin()==generic_fx) {
                    methods<<m.object<<'\t'<<hex32(m.fn)<<'\t'<<hex32(m.site)<<'\t'<<hex32(m.id)
                           <<'\t'<<hex32(m.method)<<'\t'<<hex32(m.target)<<'\n';
                    ++descriptor_methods;
                }
            }
            methods.close();
        }
    }
    if (field_evidence.is_open()) field_evidence.close();
    return {summary.str(),inventory.files,inventory.instances,unresolved_calls,fixed_calls,evidence.installed_sites,evidence.named_sites,
            evidence.named_methods,abi_calls,conditional_calls,passed_arguments,unreferenced_calls,descriptor_methods,object_interfaces,enforced_calls};
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
                if (units.contains(value) || alternate_owners.contains(value)) file_pointers.insert(value);
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

// Runs of routine addresses in read-only XBE data, and the positions in them
// that an instruction names. An object's first word holds one of these.
struct VirtualTables {
    std::map<Word,Word> words;  // aligned address -> code address
    std::set<Word> callable;    // the compiled routines among those code addresses
    std::set<Word> starts;      // run positions named by an instruction operand
    std::vector<Word> slot(Word offset) const {
        std::set<Word> targets;
        if (offset%4 || offset>0x1000) return {};
        // A named position can lie inside another table (0x152E34 inside the
        // table at 0x152E30), so a table extends to the end of its run.
        for (auto start=starts.begin();start!=starts.end();++start) {
            bool contiguous=true;
            for (Word at=*start;at<=*start+offset && contiguous;at+=4) contiguous=words.contains(at);
            if (contiguous && callable.contains(words.at(*start+offset))) targets.insert(words.at(*start+offset));
        }
        return {targets.begin(),targets.end()};
    }
};

VirtualTables virtual_tables(const Program& program, Xbe& image, CodeCache& code, const Decoder& decoder) {
    VirtualTables result;
    for (const auto& section:image.sections()) {
        // Library objects also take tables from initialized writable data.
        const auto end=std::uint64_t(section.address)+std::min(section.size,section.raw_size);
        for (std::uint64_t at=(section.address+3u)&~3u;at+4<=end;at+=4)
            if (const auto word=image.initial_word(static_cast<Word>(at)); word && file_backed_code(image,*word))
            {
                result.words.emplace(static_cast<Word>(at),*word);
                if (program.callable_owner(*word)) result.callable.insert(*word);
            }
    }
    // A statically initialized object holds its table address as a data word.
    for (const auto& section:image.sections()) {
        const auto end=std::uint64_t(section.address)+std::min(section.size,section.raw_size);
        for (std::uint64_t at=(section.address+3u)&~3u;at+4<=end;at+=4)
            if (const auto word=image.initial_word(static_cast<Word>(at)); word && result.words.contains(*word) && *word!=at)
                result.starts.insert(*word);
    }
    for (const auto& [entry,unit]:program.units) for (const auto [address,length]:unit.rows) {
        const auto ins=decoder.decode(address,code.read(address,length));
        for (const auto& op:std::span(ins.operands).first(ins.decoded.operand_count_visible)) {
            Word value=0;
            if (op.type==ZYDIS_OPERAND_TYPE_IMMEDIATE) value=static_cast<Word>(op.imm.value.u);
            else if (op.type==ZYDIS_OPERAND_TYPE_MEMORY && ins.decoded.mnemonic==ZYDIS_MNEMONIC_LEA) value=static_cast<Word>(op.mem.disp.value);
            if (result.words.contains(value)) result.starts.insert(value);
        }
    }
    return result;
}

// Registers holding an object's first word (its routine table) or a word read
// from such a table, joined over every path through the function.
struct TableValue {
    // prcb is FS:[0x20]; platform_null is its +0x250 callback, which only a
    // platform provider sets. The runtime zero-fills each thread's block.
    // field and global name a load from a structure field or a global word or
    // array (displacement at least 0x10000); result is EAX after a direct CALL.
    enum Kind : std::uint8_t { unknown, first_word, slot, routine, prcb, platform_null, field, global, result, data } kind{};
    Word offset{};
    bool operator==(const TableValue&) const = default;
};
// A fixed routine comes from an immediate or a read-only XBE word. It survives
// a CALL only in EBX, EBP, ESI or EDI, which the title's callees preserve.
std::map<Word,std::array<TableValue,8>> table_registers(const Function& function,
        const std::function<std::optional<Word>(Word)>& routine, const std::function<std::optional<Word>(Word)>& read_only) {
    using State=std::array<TableValue,8>;
    const auto index=[](ZydisRegister reg) {
        const auto full=ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,reg);
        return full>=ZYDIS_REGISTER_EAX && full<=ZYDIS_REGISTER_EDI ? int(full-ZYDIS_REGISTER_EAX) : -1;
    };
    std::map<Word,State> before;
    std::deque<Word> work;
    const auto enter=[&](Word pc,const State& state) {
        if (!function.instructions.contains(pc)) return;
        auto [it,added]=before.try_emplace(pc,state);
        bool changed=added;
        if (!added) for (unsigned r=0;r<8;++r) if (it->second[r]!=state[r] && it->second[r].kind!=TableValue::unknown) {
            it->second[r]={};
            changed=true;
        }
        if (changed) work.push_back(pc);
    };
    enter(function.entry,{});
    for (const auto alternate:function.alternate_entries) enter(alternate,{});
    while (!work.empty()) {
        const Word pc=work.front();work.pop_front();
        const auto& ins=function.instructions.at(pc);
        auto state=before.at(pc);
        const auto& dst=ins.operands[0];
        const auto& src=ins.operands[1];
        TableValue result;
        if (ins.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && dst.type==ZYDIS_OPERAND_TYPE_REGISTER && dst.size==32) {
            if (src.type==ZYDIS_OPERAND_TYPE_REGISTER && index(src.reg.value)>=0) result=state[index(src.reg.value)];
            else if (src.type==ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                if (const auto target=routine(static_cast<Word>(src.imm.value.u))) result={TableValue::routine,*target};
                else result={TableValue::data};
            } else if (src.type==ZYDIS_OPERAND_TYPE_MEMORY && src.size==32 && src.mem.base==ZYDIS_REGISTER_NONE &&
                       src.mem.index==ZYDIS_REGISTER_NONE && src.mem.segment==ZYDIS_REGISTER_FS) {
                if (src.mem.disp.value==0x20) result={TableValue::prcb};
            } else if (src.type==ZYDIS_OPERAND_TYPE_MEMORY && src.size==32 && src.mem.base==ZYDIS_REGISTER_NONE &&
                       src.mem.index==ZYDIS_REGISTER_NONE && src.mem.segment==ZYDIS_REGISTER_DS) {
                if (const auto target=read_only(static_cast<Word>(src.mem.disp.value))) result={TableValue::routine,*target};
                else result={TableValue::global,static_cast<Word>(src.mem.disp.value)};
            } else if (src.type==ZYDIS_OPERAND_TYPE_MEMORY && src.size==32 && src.mem.index==ZYDIS_REGISTER_NONE &&
                     src.mem.segment==ZYDIS_REGISTER_DS && index(src.mem.base)>=0 && src.mem.base!=ZYDIS_REGISTER_ESP &&
                     src.mem.base!=ZYDIS_REGISTER_EBP && !ins.relocated_displacement) {
                const auto base=state[index(src.mem.base)];
                const auto disp=static_cast<Word>(src.mem.disp.value);
                if (base.kind==TableValue::prcb && disp==0x250) result={TableValue::platform_null};
                else if (!disp) result={TableValue::first_word};
                else if (base.kind==TableValue::first_word && src.mem.disp.value>0) result={TableValue::slot,disp};
                else if (disp>=0x10000 && disp<0x80000000) result={TableValue::global,disp};
                else if (src.mem.disp.value>0) result={TableValue::field,disp};
            } else if (src.type==ZYDIS_OPERAND_TYPE_MEMORY && src.size==32 && src.mem.index!=ZYDIS_REGISTER_NONE &&
                       src.mem.segment==ZYDIS_REGISTER_DS && src.mem.disp.value>=0x10000 && src.mem.disp.value<0x80000000) {
                result={TableValue::global,static_cast<Word>(src.mem.disp.value)};
            }
        }
        // Arithmetic never yields a routine entry: such a value is data.
        switch (ins.decoded.mnemonic) {
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC:
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_SHL:
        case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_NEG:
        case ZYDIS_MNEMONIC_NOT: case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_SBB:
        case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_LEA:
            if (dst.type==ZYDIS_OPERAND_TYPE_REGISTER && index(dst.reg.value)>=0 && dst.size==32 &&
                (ins.decoded.mnemonic!=ZYDIS_MNEMONIC_LEA || src.mem.disp.value || src.mem.index!=ZYDIS_REGISTER_NONE))
                result={TableValue::data};
            break;
        default: break;
        }
        if (ins.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && dst.type==ZYDIS_OPERAND_TYPE_REGISTER && dst.size==32 &&
            src.type==ZYDIS_OPERAND_TYPE_MEMORY && src.size!=32) result={TableValue::data};
        for (const auto& op:std::span(ins.operands).first(ins.decoded.operand_count))
            if (op.type==ZYDIS_OPERAND_TYPE_REGISTER && (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) && index(op.reg.value)>=0)
                state[index(op.reg.value)]={};
        if (ins.decoded.meta.category==ZYDIS_CATEGORY_CALL) {
            state[0]=state[1]=state[2]={};
            if (const auto to=ins.target()) state[0]={TableValue::result,*to};
        }
        if (result.kind!=TableValue::unknown) state[index(dst.reg.value)]=result;
        const Word next=pc+ins.decoded.length;
        if (falls_through(ins) && !ins.cross_function_fallthrough) enter(next,state);
        const auto category=ins.decoded.meta.category;
        if ((category==ZYDIS_CATEGORY_COND_BR || category==ZYDIS_CATEGORY_UNCOND_BR) && !ins.cross_function_branch)
            if (const auto to=ins.target()) enter(*to,state);
        for (const auto to:ins.jump_targets) enter(to,state);
    }
    return before;
}

// Table values for an XBE function. Immediates name compiled routines; a
// kernel import or a read-only word holds the same routine for the whole run.
std::map<Word,std::array<TableValue,8>> xbe_registers(const Function& function, const Program& program, Xbe& image) {
    return table_registers(function,[&](Word value) -> std::optional<Word> {
        return program.callable_owner(value) ? std::optional<Word>(value) : std::nullopt;
    },[&](Word slot) -> std::optional<Word> {
        if (const auto* import=image.kernel_import(slot))
            return kernel_data_export(import->ordinal) ? std::nullopt : std::optional<Word>(kernel_call_address(import->ordinal));
        const bool constant=std::ranges::any_of(image.sections(),[&](const Section& s) {
            return !(s.flags & 1) && slot>=s.address && std::uint64_t(slot-s.address)+4<=std::min(s.size,s.raw_size);
        });
        const auto word=constant ? image.initial_word(slot) : std::nullopt;
        return word && program.callable_owner(*word) ? word : std::nullopt;
    });
}

Function build_function(Word entry, Unit& unit, const Program& program, const JumpTargets& jumps,
                        Xbe& image, CodeCache& code, const Decoder& decoder, const VirtualTables& tables) {
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
    // Evidence-bound transfers. The generated code rejects any other destination.
    std::optional<std::map<Word,std::array<TableValue,8>>> registers;
    for (auto& binding:unit.bindings) {
        if (binding.kind!="runtime_dispatch" && binding.kind!="callback_slot") continue;
        auto& ins=function.instructions.at(binding.address);
        if (binding.kind=="callback_slot") {
            // The initial word plus every original immediate assignment to the slot.
            ins.allowed_targets=binding.targets;
            binding.kind="slot_assignment";
            continue;
        }
        // MOV R,[object]; CALL [R+n], or MOV R,[R+n]; CALL R, reads slot n of the
        // object's routine table on every path to the transfer.
        if (!registers) registers=xbe_registers(function,program,image);
        const auto state=registers->find(binding.address);
        if (state==registers->end()) continue;
        const auto register_index=[](ZydisRegister reg) {
            const auto full=ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,reg);
            return full>=ZYDIS_REGISTER_EAX && full<=ZYDIS_REGISTER_EDI ? int(full-ZYDIS_REGISTER_EAX) : -1;
        };
        const auto& op=ins.operands[0];
        if (op.type==ZYDIS_OPERAND_TYPE_MEMORY && op.mem.index==ZYDIS_REGISTER_NONE && register_index(op.mem.base)>=0 &&
            state->second[register_index(op.mem.base)].kind==TableValue::platform_null) {
            ins.reject_transfer=true;
            binding.kind="platform_null";
            binding.reason="Callback table from processor block +0x250, which no title instruction or runtime service sets";
            continue;
        }
        std::optional<Word> slot;
        if (op.type==ZYDIS_OPERAND_TYPE_MEMORY && op.mem.index==ZYDIS_REGISTER_NONE && op.mem.segment==ZYDIS_REGISTER_DS &&
            register_index(op.mem.base)>=0 && op.mem.disp.value>=0 &&
            state->second[register_index(op.mem.base)].kind==TableValue::first_word) slot=static_cast<Word>(op.mem.disp.value);
        else if (op.type==ZYDIS_OPERAND_TYPE_REGISTER && register_index(op.reg.value)>=0 &&
                 state->second[register_index(op.reg.value)].kind==TableValue::slot) slot=state->second[register_index(op.reg.value)].offset;
        if (op.type==ZYDIS_OPERAND_TYPE_REGISTER && register_index(op.reg.value)>=0 &&
            state->second[register_index(op.reg.value)].kind==TableValue::routine) {
            const Word target=state->second[register_index(op.reg.value)].offset;
            ins.checked_target=target;
            binding.kind="fixed_call";
            binding.reason="Register holds the same original routine on every path";
            binding.targets={target};
            continue;
        }
        if (!slot) continue;
        auto targets=tables.slot(*slot);
        if (targets.empty()) continue;
        binding.kind="virtual_slot";
        binding.reason=std::format("Slot +{} of every routine table named by an instruction ({} targets)",hex32(*slot),targets.size());
        ins.allowed_targets=targets;
        binding.targets=std::move(targets);
    }
    return function;
}

// Routine values that reach each global word or array (keyed by displacement)
// and each structure field displacement. A store of any other value, other
// than zero, makes the location unknown. Values move through registers that
// hold loads from other locations.
struct XbeFlow {
    using Key=std::pair<TableValue::Kind,Word>;
    struct Location { std::set<Word> routines; std::set<Key> sources; bool unknown{}, module_callbacks{}; };
    std::map<Key,Location> locations;
    static std::optional<Key> key(const ZydisDecodedOperand& op, const std::array<TableValue,8>& state) {
        if (op.type!=ZYDIS_OPERAND_TYPE_MEMORY || op.size!=32 || op.mem.segment!=ZYDIS_REGISTER_DS) return {};
        const auto disp=static_cast<Word>(op.mem.disp.value);
        if (op.mem.base==ZYDIS_REGISTER_NONE && op.mem.index==ZYDIS_REGISTER_NONE) return Key{TableValue::global,disp};
        if (op.mem.disp.value>=0x10000 && op.mem.disp.value<0x80000000) return Key{TableValue::global,disp};
        if (op.mem.index!=ZYDIS_REGISTER_NONE || op.mem.base==ZYDIS_REGISTER_ESP || op.mem.base==ZYDIS_REGISTER_EBP ||
            op.mem.disp.value<0) return {};
        const auto full=ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,op.mem.base);
        if (full<ZYDIS_REGISTER_EAX || full>ZYDIS_REGISTER_EDI) return {};
        const auto base=state[full-ZYDIS_REGISTER_EAX].kind;
        if (base==TableValue::first_word || base==TableValue::prcb) return {};
        return Key{TableValue::field,disp};
    }
};

XbeFlow xbe_flow(const std::map<Word,Function>& functions, const Program& program, Xbe& image) {
    XbeFlow flow;
    for (const auto& [entry,function]:functions) {
        const auto states=xbe_registers(function,program,image);
        for (const auto& [pc,ins]:function.instructions) {
            const auto state=states.find(pc);
            if (state==states.end()) continue;
            const auto mnemonic=ins.decoded.mnemonic;
            const auto& dst=ins.operands[0];
            const auto& src=ins.operands[1];
            if ((mnemonic!=ZYDIS_MNEMONIC_MOV && mnemonic!=ZYDIS_MNEMONIC_XCHG) || dst.type!=ZYDIS_OPERAND_TYPE_MEMORY) continue;
            const auto key=XbeFlow::key(dst,state->second);
            if (!key) continue;
            auto& location=flow.locations[*key];
            if (src.type==ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                const auto value=static_cast<Word>(src.imm.value.u);
                if (program.callable_owner(value)) location.routines.insert(value);
                continue;
            }
            const auto full=src.type==ZYDIS_OPERAND_TYPE_REGISTER ? ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,src.reg.value) : ZYDIS_REGISTER_NONE;
            if (full<ZYDIS_REGISTER_EAX || full>ZYDIS_REGISTER_EDI || src.size!=32) {location.unknown=true;continue;}
            const auto value=state->second[full-ZYDIS_REGISTER_EAX];
            if (value.kind==TableValue::routine) location.routines.insert(value.offset);
            else if (value.kind==TableValue::field || value.kind==TableValue::global) location.sources.insert({value.kind,value.offset});
            // 0x3AA80 returns an entry of the current module record's callback table.
            else if (value.kind==TableValue::result && value.offset==0x3aa80) location.module_callbacks=true;
            else if (value.kind==TableValue::data) continue;
            else location.unknown=true;
        }
    }
    // Initialized writable words keep their image value until a store replaces it.
    for (auto& [key,location]:flow.locations) if (key.first==TableValue::global)
        if (const auto word=image.initial_word(key.second); word && program.callable_owner(*word)) location.routines.insert(*word);
    for (bool changed=true;changed;) {
        changed=false;
        for (auto& [key,location]:flow.locations) for (const auto& source:location.sources) {
            const auto found=flow.locations.find(source);
            if (found==flow.locations.end() || &found->second==&location) continue;
            const auto& from=found->second;
            const auto before=location.routines.size();
            location.routines.insert(from.routines.begin(),from.routines.end());
            changed|=location.routines.size()!=before || (from.unknown && !location.unknown) || (from.module_callbacks && !location.module_callbacks);
            location.unknown|=from.unknown;
            location.module_callbacks|=from.module_callbacks;
        }
    }
    return flow;
}

// Bind indirect transfers that read a location whose every original value is known.
void bind_flow_sites(std::map<Word,Function>& functions, Units& units, const Program& program, Xbe& image, const XbeFlow& flow,
                     std::size_t& module_dispatch) {
    for (auto& [entry,function]:functions) {
        auto& unit=units.at(entry);
        if (std::ranges::none_of(unit.bindings,[](const Site& s){return s.kind=="runtime_dispatch";})) continue;
        const auto states=xbe_registers(function,program,image);
        for (auto& binding:unit.bindings) {
            if (binding.kind!="runtime_dispatch") continue;
            auto& ins=function.instructions.at(binding.address);
            const auto state=states.find(binding.address);
            if (state==states.end()) continue;
            const auto& op=ins.operands[0];
            std::optional<XbeFlow::Key> key;
            if (op.type==ZYDIS_OPERAND_TYPE_MEMORY) key=XbeFlow::key(op,state->second);
            else if (op.type==ZYDIS_OPERAND_TYPE_REGISTER) {
                const auto full=ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,op.reg.value);
                if (full>=ZYDIS_REGISTER_EAX && full<=ZYDIS_REGISTER_EDI) {
                    const auto value=state->second[full-ZYDIS_REGISTER_EAX];
                    if (value.kind==TableValue::field || value.kind==TableValue::global) key=XbeFlow::Key{value.kind,value.offset};
                    if (value.kind==TableValue::first_word) key=XbeFlow::Key{TableValue::field,0};
                }
            }
            if (!key) continue;
            const auto location=flow.locations.find(*key);
            if (location==flow.locations.end() || location->second.unknown) continue;
            const auto& found=location->second;
            if (found.module_callbacks) {
                if (!found.routines.empty()) continue;
                // Modules install their own routines and engine routines read from its tables.
                ins.address_taken_bound=true;
                binding.kind="module_callback";
                binding.reason="Entry of the current module record's callback table (0x3AA80), installed through engine +0x00";
                ++module_dispatch;
                continue;
            }
            if (found.routines.empty()) continue;
            ins.allowed_targets.assign(found.routines.begin(),found.routines.end());
            binding.kind="stored_routine";
            binding.reason=std::format("Every original store to {} {} ({} routines)",
                key->first==TableValue::global ? "global" : "field", hex32(key->second), found.routines.size());
            binding.targets=ins.allowed_targets;
        }
    }
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
    if (modules.unreferenced_calls) out<<",\n  \"unreferenced_module_calls\":"<<modules.unreferenced_calls;
    if (modules.enforced_calls) out<<",\n  \"evidence_bound_module_calls\":"<<modules.enforced_calls;
    if (modules.descriptor_methods) out<<",\n  \"descriptor_method_paths\":"<<modules.descriptor_methods
        <<",\n  \"module_descriptors\":\"module-descriptors.tsv\",\n  \"module_descriptor_receivers\":\"module-descriptor-receivers.tsv\""
        <<",\n  \"module_descriptor_methods\":\"module-descriptor-methods.tsv\"";
    if (modules.object_interfaces) out<<",\n  \"module_object_interface_sites\":"<<modules.object_interfaces
        <<",\n  \"module_object_interfaces\":\"module-object-interfaces.tsv\"";
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
    const auto tables = virtual_tables(program, image, code, decoder);
    for (const auto& [site,jump]:jumps) program.branch_targets.insert(jump.targets.begin(),jump.targets.end());
    constexpr unsigned shard_count = 64;
    constexpr std::size_t max_source_bytes = 256 * 1024 * 1024;
    std::array<std::string, shard_count + 1> shards;
    std::array<bool, shard_count + 1> uses_unwind{};
    std::array<std::set<Word>, shard_count + 1> extra_declarations;
    std::map<std::string, std::string> assembly;
    std::ostringstream header, index, index_declarations;
    std::set<Word> symbols;

    header << banner(image) << "#pragma once\n#include \"xbox/cpu.h\"\n#include <algorithm>\nnamespace xbox_native {\n";
    index << "namespace {\n"
          << "const std::array<CompiledFunction," << program.units.size() + program.alternate_owners.size() << "> functions{{\n";
    std::size_t total_source{}, assigned_rows{}, lowered_count{};
    std::size_t total_rows = 0;
    for (const auto& [entry, unit] : program.units)
        if (!unit.recovered) total_rows += unit.rows.size();
    const auto rows_per_shard = std::max<std::size_t>(1,(total_rows+shard_count-1)/shard_count);
    CallEvidence evidence;
    // Build every function first: a stored routine can be read in another function.
    std::map<Word, Function> built;
    for (auto& [entry, unit] : program.units)
        built.emplace(entry, build_function(entry, unit, program, jumps, image, code, decoder, tables));
    std::size_t module_dispatch = 0;
    bind_flow_sites(built, program.units, program, image, xbe_flow(built, program, image), module_dispatch);
    // The rest are bounded only by address: an XBE routine whose address appears in
    // the image, a kernel thunk, or an entry of a loaded and verified module.
    for (auto& [entry, unit] : program.units) for (auto& binding : unit.bindings) if (binding.kind == "runtime_dispatch") {
        built.at(entry).instructions.at(binding.address).address_taken_bound = true;
        binding.kind = "address_taken";
        binding.reason = "Address-taken XBE routine, kernel thunk or verified module entry";
    }
    for (auto& [entry, unit] : program.units) {
        const auto name = symbol(entry);
        auto& declarations = unit.recovered ? index_declarations : header;
        declarations << "void " << name << "(Cpu&, Memory&);\n";
        auto function = std::move(built.at(entry));
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
    // Disc modules load into the engine's heap inside the executable's BSS, so
    // the check names the compiled XBE entries whose address the image never holds.
    std::vector<Word> unnamed;
    for (const auto address : symbols) if (!program.file_pointers.contains(address)) unnamed.push_back(address);
    header << "extern const std::uint32_t unnamed_routines[];\nextern const std::size_t unnamed_count;\n"
           << "inline bool address_taken_target(std::uint32_t target) {\n"
           << "    return !std::binary_search(unnamed_routines, unnamed_routines + unnamed_count, target);\n}\n";
    header << "}\n";
    index << "}};\n}\nconst std::span<const CompiledFunction> compiled_batch{functions};\n";
    index << "const std::uint32_t unnamed_routines[]{";
    for (const auto address : unnamed) index << hex32(address) << "U,";
    index << "};\nconst std::size_t unnamed_count = " << unnamed.size() << ";\n}\n";
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
