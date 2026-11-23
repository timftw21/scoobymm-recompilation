#include "call_evidence.h"
#include "file.h"

#include <algorithm>
#include <tuple>
#include <sstream>

namespace xbox_native {
namespace {
using Word=std::uint32_t;
using Kind=PointerSource;
// The loader supplies the same engine interface to Register and Constructor.
constexpr Word engine_api=logic_engine_api, lookup_slot=0x6c, registry_slot=0x78;

std::string_view disc_name(Bytes text, Word offset) {
    if (offset>=text.size()) return {};
    const std::string_view data(reinterpret_cast<const char*>(text.data()+offset),
                                std::min<std::size_t>(text.size()-offset,128));
    const auto end=data.find('\0');
    if (!end || end==data.npos || data.substr(0,end).find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=data.npos) return {};
    return data.substr(0,end);
}
}

InterfaceCells::InterfaceCells(const std::set<Word>& words,const std::map<Word,Word>& relocations,
                               const std::set<Word>& data_references):words_(words),relocations_(relocations) {
    for (auto [at,value]:relocations) {
        const auto it=words.upper_bound(value);
        if (it!=words.begin() && value-*std::prev(it)<4) unchecked_.emplace(at,*std::prev(it));
    }
    for (auto value:data_references) {
        const auto it=words.upper_bound(value);
        if (it!=words.begin() && value-*std::prev(it)<4) escaped_.insert(*std::prev(it));
    }
}

void InterfaceCells::instruction(Word function,const Instruction& ins) {
    if (ins.relocated_immediate) {
        const auto value=relocations_.at(ins.address+ins.decoded.raw.imm[0].offset);
        const auto it=words_.upper_bound(value);
        if (it!=words_.begin() && value-*std::prev(it)<4) escaped_.insert(*std::prev(it));
    }
    if (!ins.relocated_displacement) return;
    const auto at=ins.address+ins.decoded.raw.disp.offset,value=relocations_.at(at);
    unchecked_.erase(at);
    // Only the encoded operand owns this relocation. PUSH [symbol], for
    // example, also has an implicit stack write at an unrelated address.
    for (const auto& op:std::span(ins.operands).first(ins.decoded.operand_count_visible)) if (op.type==ZYDIS_OPERAND_TYPE_MEMORY) {
        for (auto it=words_.lower_bound(value>3?value-3:0);it!=words_.end() && *it<std::uint64_t(value)+std::max(1u,unsigned(op.size)/8);++it) {
            const auto cell=*it;
            if (value!=cell || op.size!=32 || op.mem.base!=ZYDIS_REGISTER_NONE || op.mem.index!=ZYDIS_REGISTER_NONE ||
                op.mem.segment!=ZYDIS_REGISTER_DS || ins.decoded.mnemonic==ZYDIS_MNEMONIC_LEA) {escaped_.insert(cell);continue;}
            if (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) writers_[cell].emplace(function,ins.address);
        }
    }
}

bool InterfaceCells::unchanged(Word slot) const {
    return writers_within(slot,{});
}

void InterfaceCells::output_argument(const Instruction& push) {
    unchecked_.erase(push.address+push.decoded.raw.imm[0].offset);
}

bool InterfaceCells::writers_within(Word slot,const std::set<Word>& sites) const {
    if (!words_.contains(slot) || escaped_.contains(slot) ||
        std::ranges::any_of(unchecked_,[&](const auto& ref){return ref.second==slot;})) return false;
    const auto writers=writers_.find(slot);
    return writers==writers_.end() || std::ranges::all_of(writers->second,[&](auto writer){return sites.contains(writer.second);});
}

std::map<Word,Word> InterfaceCells::verified_getters(const std::set<Word>& interface_functions,
                                                    const std::map<Word,CallSources>& sources,
                                                    std::map<Word,std::string_view>& missing) const {
    auto invalid=escaped_;
    for (auto [at,slot]:unchecked_) invalid.insert(slot);
    for (auto slot:invalid) missing[slot]="cell-address-escapes";
    std::map<Word,std::set<std::pair<Word,Word>>> explained;
    std::map<Word,Word> result;
    // Some libraries retain the loader argument itself and perform named
    // lookups on it later. First establish the full writer set of that cache.
    std::map<Word,std::set<std::pair<Word,Word>>> roots;
    for (const auto& [fn,flow]:sources) if (interface_functions.contains(fn))
        for (const auto& store:flow.getter_stores) if (store.getter==logic_root_argument)
            roots[store.slot].emplace(fn,store.site);
    std::erase_if(roots,[&](const auto& item) {
        const auto writes=writers_.find(item.first);
        return invalid.contains(item.first) || writes==writers_.end() || writes->second!=item.second;
    });
    for (const auto& [fn,flow]:sources) for (const auto& store:flow.getter_stores) {
        if (!writers_.contains(store.slot)) continue;
        const bool trusted=store.receiver==UINT32_MAX ? interface_functions.contains(fn) : roots.contains(store.receiver);
        if (!trusted) {invalid.insert(store.slot);missing.try_emplace(store.slot,"unproven-installer-argument");continue;}
        explained[store.slot].emplace(fn,store.site);
        const auto [it,added]=result.emplace(store.slot,store.getter);
        if (!added && it->second!=store.getter) {invalid.insert(store.slot);missing.try_emplace(store.slot,"multiple-interface-getters");}
    }
    std::erase_if(result,[&](const auto& entry) {
        if (explained.at(entry.first)!=writers_.at(entry.first)) missing.try_emplace(entry.first,"unexplained-cell-writer");
        return invalid.contains(entry.first) || explained.at(entry.first)!=writers_.at(entry.first);
    });
    return result;
}

void CallEvidence::executable_function(const Function& fn) {
    functions_.insert(fn.entry);
    functions_.insert(fn.alternate_entries.begin(),fn.alternate_entries.end());
    // The title's SDK tables start with three immediate pushes and a call to
    // the verified REP STOSD helper. Its byte count bounds every method lookup.
    auto pc=fn.entry;
    std::array<Word,3> args{};
    bool pushed=true;
    for (auto& arg:args) {
        auto it=fn.instructions.find(pc);
        if (it==fn.instructions.end() || it->second.decoded.mnemonic!=ZYDIS_MNEMONIC_PUSH ||
            it->second.operands[0].type!=ZYDIS_OPERAND_TYPE_IMMEDIATE) {pushed=false;break;}
        arg=static_cast<Word>(it->second.operands[0].imm.value.u);
        pc+=it->second.decoded.length;
    }
    if (pushed) {
        auto it=fn.instructions.find(pc);
        if (it!=fn.instructions.end() && it->second.decoded.mnemonic==ZYDIS_MNEMONIC_CALL && it->second.target()==0x68480)
            tables_.emplace(args[2], Table{args[1],{pc,args[0]}});
    }
    for (const auto& [pc,ins]:fn.instructions) {
        const auto& dst=ins.operands[0]; const auto& src=ins.operands[1];
        if (ins.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && dst.type==ZYDIS_OPERAND_TYPE_MEMORY && dst.size==32 &&
            dst.mem.base==ZYDIS_REGISTER_NONE && dst.mem.index==ZYDIS_REGISTER_NONE && dst.mem.segment==ZYDIS_REGISTER_DS &&
            src.type==ZYDIS_OPERAND_TYPE_IMMEDIATE)
            writes_[static_cast<Word>(dst.mem.disp.value)].push_back({pc,static_cast<Word>(src.imm.value.u)});
    }
    // These getters return an immediate table address without using guest memory.
    if (fn.instructions.size()!=2) return;
    const auto& first=fn.instructions.begin()->second;
    const auto& last=fn.instructions.rbegin()->second;
    if (first.address==fn.entry && first.decoded.mnemonic==ZYDIS_MNEMONIC_MOV &&
        first.operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && first.operands[0].reg.value==ZYDIS_REGISTER_EAX &&
        first.operands[1].type==ZYDIS_OPERAND_TYPE_IMMEDIATE && last.address==first.address+first.decoded.length &&
        last.decoded.mnemonic==ZYDIS_MNEMONIC_RET)
        getters_[fn.entry]=static_cast<Word>(first.operands[1].imm.value.u);
}

void CallEvidence::open(const std::filesystem::path& path) {
    bindings_.exceptions(std::ios::failbit|std::ios::badbit);
    providers_.exceptions(std::ios::failbit|std::ios::badbit);
    bindings_.open(path/"module-call-evidence.tsv");
    bindings_<<"object_sha256\tfunction\tcall\tmodule_slot\tmodule_store\tengine_slot\tengine_store\tgetter_or_target\ttable_or_name\tmethod_offset\tmethod_store\ttarget\n";
    providers_.open(path/"module-api-providers.tsv");
    providers_<<"name\tmethod_offset\tprovider_sha256\tregister_function\tallocation_call\tmethod_store\ttarget_offset\tallocation_bytes\n";
}

std::optional<LogicAbiCall> CallEvidence::export_call(const CallSource& call) const {
    const auto& source=call.target;
    if (call.direct || source.kind!=Kind::argument_method || source.base!=0 || (source.offset&3)) return {};
    const auto& table=tables_.at(engine_api);
    if (source.offset>=table.size) return {};
    const auto stores=writes_.find(engine_api+source.offset);
    if (stores==writes_.end() || stores->second.empty()) return {};
    const auto target=stores->second.front().value;
    if (!functions_.contains(target) || !std::ranges::all_of(stores->second,[&](Store s){return s.value==target;})) return {};
    return LogicAbiCall{call.site,source.offset,target};
}

std::optional<LogicAbiCall> CallEvidence::table_call(const CallSource& call,Word getter_slot) const {
    const auto method=call.target.offset;
    if (method&3) return {};
    CallSource lookup{};
    if (getter_slot==logic_root_argument) {
        lookup.target={Kind::argument_method,0,method};
        auto root=export_call(lookup);
        if (root) {
            root->site=call.site;
            root->source=call.target.base;
            root->table=engine_api;
            root->method=method;
        }
        return root;
    }
    lookup.target={Kind::argument_method,0,getter_slot};
    const auto root=export_call(lookup);
    if (!root) return {};
    const auto getter=getters_.find(root->target);
    if (getter==getters_.end()) return {};
    const auto table=tables_.find(getter->second);
    if (table==tables_.end() || method>=table->second.size) return {};
    const auto found=writes_.find(table->first+method);
    const auto stores=found==writes_.end() ? std::span(&table->second.fill,1) : std::span<const Store>(found->second);
    const auto target=stores.front().value;
    if (!functions_.contains(target) || !std::ranges::all_of(stores,[&](Store s){return s.value==target;})) return {};
    return LogicAbiCall{call.site,getter_slot,target,call.target.base,table->first,method,root->target};
}

void CallEvidence::register_provider(std::string_view hash,std::string symbol,Bytes text,const std::set<Word>& functions,
                                     Word entry,const CallSources& flow,const std::map<Word,Word>& getters) {
    for (const auto& call:flow.calls) {
        if (call.direct || call.target.offset || call.first_argument.kind!=Kind::relocated ||
            call.second_argument.kind!=Kind::constant) continue;
        auto source=call.target;
        if (source.kind==Kind::module_method) {
            auto it=getters.find(source.base);
            if (it==getters.end() || it->second!=registry_slot) continue;
        } else if (source.kind!=Kind::returned_method || source.base!=registry_slot) continue;
        const auto api=table_call(call,registry_slot);
        if (!api || api->target!=0x747d0 || api->table!=0x1f03f18) continue;
        const auto name=disc_name(text,call.first_argument.base);
        if (name.empty()) continue;
        registrations_[std::string(hash)][{entry,call.site}]=call.first_argument.base;
        for (const auto& field:flow.method_stores) {
            if (field.allocation!=call.site || std::uint64_t(field.field)+4>call.second_argument.base ||
                !functions.contains(field.target)) continue;
            auto [it,inserted]=names_.try_emplace({std::string(name),field.field},Named{static_cast<Word>(names_.size()),{}});
            if (!it->second.providers.emplace(symbol,field.target).second) continue;
            providers_<<name<<'\t'<<hex32(field.field)<<'\t'<<hash<<'\t'<<hex32(entry)<<'\t'<<hex32(call.site)
                      <<'\t'<<hex32(field.site)<<'\t'<<hex32(field.target)<<'\t'<<hex32(call.second_argument.base)<<'\n';
            ++named_methods;
        }
    }
}

void CallEvidence::add_registered_cells(std::string_view hash,std::map<Word,CallSources>& sources) const {
    const auto registrations=registrations_.find(hash);
    if (registrations==registrations_.end()) return;
    for (auto& [fn,flow]:sources) for (const auto& store:flow.allocation_stores) {
        auto it=registrations->second.find({fn,store.allocation});
        if (it!=registrations->second.end())
            flow.getter_stores.push_back({store.site,store.slot,lookup_slot,it->second});
    }
}

std::map<Word,std::string_view> CallEvidence::cache_names(Bytes text,const std::map<Word,CallSources>& sources,
                                                         const std::map<Word,Word>& getters) {
    std::map<Word,std::string_view> result;
    for (const auto& [fn,flow]:sources) for (const auto& s:flow.getter_stores) {
        if (s.getter!=lookup_slot || !getters.contains(s.slot)) continue;
        const auto name=disc_name(text,s.name_offset);
        const auto [it,added]=result.emplace(s.slot,name);
        if (!added && it->second!=name) it->second={};
    }
    return result;
}

std::optional<LogicAbiCall> CallEvidence::named_call(const CallSource& call,std::string_view name) const {
    const auto it=names_.find({std::string(name),call.target.offset});
    if (it==names_.end()) return {};
    return LogicAbiCall{call.site,lookup_slot,0,call.target.base,0,call.target.offset,0x47f30,it->second.index};
}

std::set<Word> CallEvidence::interface_tables(std::ostream& generated) const {
    std::set<Word> slots;
    const auto root=tables_.find(engine_api);
    if (root==tables_.end()) return slots;
    for (Word slot=0;slot<root->second.size;slot+=4) {
        CallSource call{};
        call.target={Kind::argument_method,0,slot};
        const auto source=export_call(call);
        if (!source || !getters_.contains(source->target)) continue;
        const auto table=getters_.at(source->target);
        if (!tables_.contains(table)) continue;
        generated<<"constexpr std::uint32_t methods_"<<slot<<"[]{";
        for (Word field=0;field<tables_.at(table).size;field+=4) {
            call.target={Kind::module_method,0,field};
            const auto method=table_call(call,slot);
            generated<<(method ? method->target : 0)<<',';
        }
        generated<<"};\nconstexpr LogicInterface iface_"<<slot<<"{"<<slot<<','<<source->target<<','<<table<<",methods_"<<slot<<"};\n";
        slots.insert(slot);
    }
    return slots;
}

std::map<Word,std::vector<Word>> CallEvidence::provided_methods(std::string_view symbol) const {
    std::map<Word,std::vector<Word>> result;
    for (const auto& [key,method]:names_) for (const auto& [object,offset]:method.providers)
        if (object==symbol) result[offset].push_back(method.index);
    return result;
}

std::string CallEvidence::provider_tables() const {
    std::ostringstream code;
    std::vector<std::string> entries(names_.size());
    for (const auto& [key,method]:names_) {
        code<<"constexpr LogicProvider providers_"<<method.index<<"[]{\n";
        for (const auto& [object,offset]:method.providers) code<<"    {&"<<object<<','<<offset<<"},\n";
        code<<"};\n";
        entries[method.index]="    {"+json(key.first)+","+std::to_string(key.second)+",providers_"+std::to_string(method.index)+"},\n";
    }
    code<<"const std::array<LogicNamedMethod,"<<entries.size()<<"> named_methods{{\n";
    for (const auto& entry:entries) code<<entry;
    code<<"}};\n";
    return code.str();
}

std::set<Word> CallEvidence::interface_functions(const std::set<Word>& exports,const std::set<Word>& address_taken,
                                                 const std::map<Word,CallSources>& sources) {
    // Start at the original loader. A helper shares the interface contract only
    // when every incoming call forwards argument zero and its address never escapes.
    std::map<Word,std::vector<Word>> forwarded;
    std::set<Word> invalid=address_taken;
    for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) {
        if (!call.direct) continue;
        if (call.first_argument==PointerValue{Kind::argument,0}) forwarded[fn].push_back(call.target.base);
        else invalid.insert(call.target.base);
    }
    std::set<Word> reached=exports;
    std::vector<Word> work(exports.begin(),exports.end());
    while (!work.empty()) {
        auto fn=work.back();work.pop_back();
        for (auto next:forwarded[fn]) if (reached.insert(next).second) work.push_back(next);
    }
    for (const auto& [fn,flow]:sources) if (!reached.contains(fn)) invalid.insert(fn);
    work.assign(invalid.begin(),invalid.end());
    while (!work.empty()) {
        auto fn=work.back();work.pop_back();
        reached.erase(fn);
        for (auto next:forwarded[fn]) if (invalid.insert(next).second) work.push_back(next);
    }
    return reached;
}

void CallEvidence::module(std::string_view hash,Bytes text,const std::set<Word>& functions,
                          const std::set<Word>& entry_points,const std::map<Word,CallSources>& sources) {
    const auto& root_table=tables_.at(engine_api);
    // Only propagate the engine interface through direct calls which forward its first argument.
    // This identifies a reachable installation chain. Writable slots can have other values.
    std::set<Word> api_functions=entry_points;
    std::vector<Word> pending(entry_points.begin(),entry_points.end());
    std::map<Word,std::vector<GetterStore>> installs;
    while (!pending.empty()) {
        auto fn=pending.back();pending.pop_back();
        const auto it=sources.find(fn);
        if (it==sources.end()) continue;
        for (const auto& store:it->second.getter_stores)
            if (store.receiver==UINT32_MAX && store.getter!=logic_root_argument) installs[store.slot].push_back(store);
        for (const auto& call:it->second.calls)
            if (call.direct && call.first_argument==PointerValue{Kind::argument,0} &&
                functions.contains(call.target.base) && api_functions.insert(call.target.base).second)
                pending.push_back(call.target.base);
    }
    for (const auto& [fn,flow]:sources) {
        for (const auto& call:flow.calls) {
            if (call.direct) continue;
            const auto target=call.target;
            if (target.kind==Kind::argument_method && !target.base && api_functions.contains(fn)) {
                const auto it=writes_.find(engine_api+target.offset);
                if (it==writes_.end() || (target.offset&3) || target.offset>=root_table.size) continue;
                bool found=false;
                for (auto root:it->second) if (functions_.contains(root.value)) {
                    bindings_<<hash<<'\t'<<hex32(fn)<<'\t'<<hex32(call.site)<<"\t-\t-\t"<<hex32(target.offset)
                             <<'\t'<<hex32(root.pc)<<'\t'<<hex32(root.value)<<"\t-\t-\t-\t"<<hex32(root.value)<<'\n';
                    found=true;
                }
                installed_sites+=found;
                continue;
            }
            const bool returned=target.kind==Kind::returned_method && api_functions.contains(fn);
            std::span<const GetterStore> chain;
            const GetterStore inline_getter{target.producer,0,target.base,UINT32_MAX};
            if (returned) chain={&inline_getter,1};
            else if (target.kind==Kind::module_method) {
                const auto slots=installs.find(target.base);
                if (slots!=installs.end()) chain=slots->second;
            }
            bool installed=false,named=false;
            for (auto store:chain) {
                const auto roots=writes_.find(engine_api+store.getter);
                if (store.getter>=root_table.size || (store.getter&3) || roots==writes_.end()) continue;
                for (auto root:roots->second) {
                    auto prefix=[&] {
                        bindings_<<hash<<'\t'<<hex32(fn)<<'\t'<<hex32(call.site)<<'\t'<<(returned ? "-" : hex32(target.base))<<'\t'
                                 <<hex32(store.site)<<'\t'<<hex32(store.getter)<<'\t'<<hex32(root.pc)<<'\t'<<hex32(root.value)<<'\t';
                    };
                    if (store.getter==lookup_slot && root.value==0x47f30) {
                        const auto name=disc_name(text,store.name_offset);
                        if (!name.empty()) {
                            prefix();bindings_<<name<<'\t'<<hex32(target.offset)<<"\tnamed-provider\t-\n";
                            named=true;
                        }
                        continue;
                    }
                    const auto getter=getters_.find(root.value);
                    if (getter==getters_.end() || (target.offset & 3)) continue;
                    const auto table=tables_.find(getter->second);
                    if (table==tables_.end() || target.offset>=table->second.size) continue;
                    const auto methods=writes_.find(getter->second+target.offset);
                    const auto entries=methods==writes_.end() ? std::span(&table->second.fill,1) : std::span<const Store>(methods->second);
                    for (auto method:entries) if (functions_.contains(method.value)) {
                        prefix();bindings_<<hex32(getter->second)<<'\t'<<hex32(target.offset)<<'\t'<<hex32(method.pc)<<'\t'<<hex32(method.value)<<'\n';
                        installed=true;
                    }
                }
            }
            installed_sites+=installed;
            named_sites+=named;
        }
    }
}

std::map<CallEvidence::Parameter,Word> CallEvidence::table_arguments(const std::set<Word>& externally_callable,
        const std::map<Word,Word>& cells,const std::map<Word,CallSources>& sources) {
    // Join each parameter independently. A caller can reorder parameters when forwarding them.
    std::map<Parameter,Word> incoming;
    std::map<Parameter,std::vector<Parameter>> forwarded;
    std::vector<Parameter> work;
    const auto offer=[&](Parameter fn,Word cell) {
        auto [it,added]=incoming.emplace(fn,cell);
        if (added) work.push_back(fn);
        else if (it->second!=cell && it->second!=UINT32_MAX) {it->second=UINT32_MAX;work.push_back(fn);}
    };
    for (const auto& [fn,flow]:sources) for (const auto& call:flow.calls) if (call.direct) {
        const std::array args{call.first_argument,call.second_argument,call.third_argument};
        for (Word i=0;i<args.size();++i) {
            const auto src=args[i];
            const Parameter dest{call.target.base,i};
            if (externally_callable.contains(dest.first)) offer(dest,UINT32_MAX);
            else if (src.kind==Kind::argument && src.base<args.size()) forwarded[{fn,src.base}].push_back(dest);
            else offer(dest,src.kind==Kind::module_slot && cells.contains(src.base) ? src.base : UINT32_MAX);
        }
    }
    auto propagate=[&] {
        while (!work.empty()) {
            auto fn=work.back();work.pop_back();
            const auto value=incoming.at(fn);
            if (auto edges=forwarded.find(fn);edges!=forwarded.end())
                for (auto next:edges->second) offer(next,value);
        }
    };
    propagate();
    for (const auto& [parameter,edges]:forwarded) if (!incoming.contains(parameter)) offer(parameter,UINT32_MAX);
    propagate();
    std::erase_if(incoming,[](const auto& kv){return kv.second==UINT32_MAX;});
    return incoming;
}
void CallEvidence::close() {
    bindings_.close();
    providers_.close();
}

bool object_accesses(const Function& fn,Word load,ObjectUses& uses) {
    const auto& seed=fn.instructions.at(load);
    const auto& dst=seed.operands[0];
    if (seed.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV || dst.type!=ZYDIS_OPERAND_TYPE_REGISTER || dst.size!=32) return false;
    return object_accesses_after(fn,load+seed.decoded.length,dst.reg.value,0,uses);
}

bool object_accesses_after(const Function& fn,Word start,ZydisRegister base,Word displacement,ObjectUses& uses) {
    using Token=std::tuple<Word,ZydisRegister,Word>;
    std::set<Token> seen;
    std::vector<Token> work{{start,base,displacement}};
    const auto full=[](ZydisRegister r){return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,r);};
    while (!work.empty()) {
        const auto token=work.back();work.pop_back();
        if (!seen.insert(token).second) continue;
        const auto [pc,reg,offset]=token;
        if (seen.size()>4096 || !fn.instructions.contains(pc)) return false;
        const auto& ins=fn.instructions.at(pc);
        const auto& d=ins.decoded;
        const auto& a=ins.operands[0];
        const auto& b=ins.operands[1];
        const Word next=pc+d.length;
        if (d.meta.category==ZYDIS_CATEGORY_RET) {
            if (reg==ZYDIS_REGISTER_EAX || reg==ZYDIS_REGISTER_EDX) uses.returns.emplace(reg,offset);
            continue;
        }
        if (d.meta.category==ZYDIS_CATEGORY_CALL) {
            if (ins.call_next_instruction || (a.type==ZYDIS_OPERAND_TYPE_REGISTER && full(a.reg.value)==reg) ||
                (a.type==ZYDIS_OPERAND_TYPE_MEMORY && (a.mem.base==reg || a.mem.index==reg))) return false;
            if (reg!=ZYDIS_REGISTER_EAX && reg!=ZYDIS_REGISTER_ECX && reg!=ZYDIS_REGISTER_EDX)
                work.emplace_back(next,reg,offset);
            continue;
        }
        bool keep=true;
        if ((d.mnemonic==ZYDIS_MNEMONIC_XOR || d.mnemonic==ZYDIS_MNEMONIC_SUB) &&
            a.type==ZYDIS_OPERAND_TYPE_REGISTER && b.type==ZYDIS_OPERAND_TYPE_REGISTER &&
            a.reg.value==reg && b.reg.value==reg && a.size==32) continue;
        for (unsigned i=0;i<d.operand_count;++i) {
            const auto& op=ins.operands[i];
            if (op.type==ZYDIS_OPERAND_TYPE_MEMORY && (op.mem.base==reg || op.mem.index==reg)) {
                if (op.mem.base!=reg || op.mem.index!=ZYDIS_REGISTER_NONE ||
                    (op.mem.segment!=ZYDIS_REGISTER_DS && op.mem.segment!=ZYDIS_REGISTER_SS) || ins.relocated_displacement) return false;
                const Word at=offset+static_cast<Word>(op.mem.disp.value);
                if (d.mnemonic==ZYDIS_MNEMONIC_LEA) {
                    if (a.type!=ZYDIS_OPERAND_TYPE_REGISTER || a.size!=32) return false;
                    work.emplace_back(next,a.reg.value,at);
                } else {
                    if (!op.size || (op.size%8)) return false;
                    const ObjectAccess access{pc,at,static_cast<Word>(op.size/8)};
                    if (op.actions&ZYDIS_OPERAND_ACTION_MASK_READ) uses.reads.push_back(access);
                    if (op.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) uses.writes.push_back(access);
                }
            }
            if (op.type!=ZYDIS_OPERAND_TYPE_REGISTER || full(op.reg.value)!=reg) continue;
            if (op.actions&ZYDIS_OPERAND_ACTION_MASK_READ) {
                if (d.mnemonic==ZYDIS_MNEMONIC_PUSH && i==0 && op.size==32)
                    uses.pushes.emplace_back(pc,offset);
                else if (d.mnemonic==ZYDIS_MNEMONIC_MOV && i==1 && a.type==ZYDIS_OPERAND_TYPE_REGISTER && a.size==32 && op.size==32)
                    work.emplace_back(next,a.reg.value,offset);
                else if ((d.mnemonic==ZYDIS_MNEMONIC_ADD || d.mnemonic==ZYDIS_MNEMONIC_SUB) && !i &&
                         op.size==32 && b.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && !ins.relocated_immediate) {
                    const Word delta=static_cast<Word>(b.imm.value.u);
                    work.emplace_back(next,reg,d.mnemonic==ZYDIS_MNEMONIC_ADD ? offset+delta : offset-delta);
                } else if (d.mnemonic!=ZYDIS_MNEMONIC_TEST && d.mnemonic!=ZYDIS_MNEMONIC_CMP) return false;
            }
            if (op.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) {
                if (op.size!=32 || op.actions&ZYDIS_OPERAND_ACTION_CONDWRITE) return false;
                keep=false;
            }
        }
        if (!keep) continue;
        if (ins.cross_function_branch || ins.cross_function_fallthrough || !ins.external_jump_targets.empty()) return false;
        if (d.meta.category==ZYDIS_CATEGORY_COND_BR || d.meta.category==ZYDIS_CATEGORY_UNCOND_BR) {
            if (const auto to=ins.target()) work.emplace_back(*to,reg,offset);
            else {
                if (ins.jump_targets.empty()) return false;
                for (Word to:ins.jump_targets) work.emplace_back(to,reg,offset);
            }
            if (d.meta.category==ZYDIS_CATEGORY_UNCOND_BR) continue;
        }
        work.emplace_back(next,reg,offset);
    }
    return true;
}
}
