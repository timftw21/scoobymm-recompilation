#pragma once
#include "call_sources.h"
#include "logic_exports.h"
#include <filesystem>
#include <fstream>
#include <set>

namespace xbox_native {
// Index accesses to private four-byte COFF objects. An address which escapes,
// an overlap, or a writer with unknown provenance leaves the cell uncertified.
class InterfaceCells {
    using Word=std::uint32_t;
public:
    InterfaceCells(const std::set<Word>& words,const std::map<Word,Word>& relocations,const std::set<Word>& data_references);
    void instruction(Word function,const Instruction&);
    bool unchanged(Word slot) const;
    std::map<Word,Word> verified_getters(const std::set<Word>& interface_functions,const std::map<Word,CallSources>&,
                                        std::map<Word,std::string_view>& missing) const;
private:
    const std::set<Word>& words_;
    const std::map<Word,Word>& relocations_;
    std::set<Word> escaped_;
    std::map<Word,Word> unchecked_;
    std::map<Word,std::set<std::pair<Word,Word>>> writers_;
};
class CallEvidence {
public:
    void executable_function(const Function&);
    void open(const std::filesystem::path&);
    void close();
    std::optional<LogicAbiCall> export_call(const CallSource&) const;
    std::optional<LogicAbiCall> table_call(const CallSource&,std::uint32_t getter_slot) const;
    void register_provider(std::string_view hash,std::string symbol,Bytes text,const std::set<std::uint32_t>& functions,
                           std::uint32_t entry,const CallSources& flow,const std::map<std::uint32_t,std::uint32_t>& getters);
    void add_registered_cells(std::string_view hash,std::map<std::uint32_t,CallSources>&) const;
    std::optional<LogicAbiCall> named_call(const CallSource&,std::string_view name) const;
    std::string provider_tables() const;
    std::set<std::uint32_t> interface_tables(std::ostream& generated) const;
    static std::map<std::uint32_t,std::string_view> cache_names(Bytes,const std::map<std::uint32_t,CallSources>&,
                                                               const std::map<std::uint32_t,std::uint32_t>& getters);
    static std::set<std::uint32_t> interface_functions(const std::set<std::uint32_t>& exports,
            const std::set<std::uint32_t>& address_taken, const std::map<std::uint32_t,CallSources>&);
    static std::map<std::uint32_t,std::uint32_t> table_arguments(const std::set<std::uint32_t>& externally_callable,
            const std::map<std::uint32_t,std::uint32_t>& cells,const std::map<std::uint32_t,CallSources>&);
    void module(std::string_view hash, std::span<const std::byte> text,
                const std::set<std::uint32_t>& functions, const std::set<std::uint32_t>& entry_points,
                const std::map<std::uint32_t,CallSources>& sources);
    std::size_t installed_sites{}, named_sites{}, named_methods{};
private:
    struct Store { std::uint32_t pc, value; };
    struct Table { std::uint32_t size; Store fill; };
    std::map<std::uint32_t,std::vector<Store>> writes_;
    std::map<std::uint32_t,Table> tables_;
    std::map<std::uint32_t,std::uint32_t> getters_;
    std::set<std::uint32_t> functions_;
    struct Named { std::uint32_t index; std::set<std::pair<std::string,std::uint32_t>> providers; };
    std::map<std::pair<std::string,std::uint32_t>,Named> names_;
    std::map<std::string,std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t>,std::less<>> registrations_;
    std::ofstream bindings_, providers_;
};
}
