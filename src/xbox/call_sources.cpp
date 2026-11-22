#include "call_sources.h"

#include <algorithm>
#include <array>
#include <deque>
#include <set>

namespace xbox_native {
namespace {
using Word = std::uint32_t;
using Kind = PointerSource;
using Reg = ZydisRegister;
struct State {
    std::array<PointerValue,8> regs{};
    PointerValue top{}, second{}, third{}, nonzero{};
    unsigned overwritten_arguments{};
    std::map<Word,PointerValue> locals;
};
int index(Reg reg) {
    const auto full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,reg);
    return full >= ZYDIS_REGISTER_EAX && full <= ZYDIS_REGISTER_EDI ? full-ZYDIS_REGISTER_EAX : -1;
}
PointerValue plus(PointerValue value, Word offset) {
    if (!offset) return value;
    if (value.kind != Kind::constant && value.kind != Kind::relocated && value.kind != Kind::stack) return {};
    value.base += offset;
    return value;
}
PointerValue load(PointerValue base, Word displacement) {
    switch (base.kind) {
    case Kind::stack: {
        const auto offset = base.base + displacement;
        return offset >= 4 && offset <= 64 && !(offset & 3) ? PointerValue{Kind::argument,offset/4-1} : PointerValue{};
    }
    case Kind::constant: return {Kind::absolute_slot,base.base+displacement};
    case Kind::relocated: return {Kind::module_slot,base.base+displacement};
    case Kind::argument: return {Kind::argument_method,base.base,displacement};
    case Kind::module_slot: return {Kind::module_method,base.base,displacement};
    case Kind::absolute_slot: return {Kind::absolute_method,base.base,displacement};
    case Kind::getter_result: return {Kind::returned_method,base.base,displacement,base.producer};
    default: return {};
    }
}
PointerValue operand(const Instruction& ins,const ZydisDecodedOperand& op,const State& state,bool address=false) {
    if (!address && op.size != 32) return {};
    if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
        return {ins.relocated_immediate ? Kind::relocated : Kind::constant,static_cast<Word>(op.imm.value.u)};
    if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const auto r = index(op.reg.value);
        return r >= 0 ? state.regs[r] : PointerValue{};
    }
    if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || op.mem.index != ZYDIS_REGISTER_NONE ||
        (op.mem.segment != ZYDIS_REGISTER_DS && op.mem.segment != ZYDIS_REGISTER_SS)) return {};
    PointerValue base;
    if (op.mem.base == ZYDIS_REGISTER_NONE) base = {ins.relocated_displacement ? Kind::relocated : Kind::constant,0};
    else if (const auto r=index(op.mem.base); r>=0) base = state.regs[r];
    const auto displacement = static_cast<Word>(op.mem.disp.value);
    if (!address && base.kind==Kind::stack) {
        auto it=state.locals.find(base.base+displacement);
        if (it!=state.locals.end()) return it->second;
    }
    auto value=address ? plus(base,displacement) : load(base,displacement);
    if (value.kind==Kind::argument && (state.overwritten_arguments & (1u<<value.base))) return {};
    return value;
}

void stack_store(State& state,Word offset,unsigned bytes,PointerValue value) {
    if (offset<68 && std::uint64_t(offset)+bytes>4) {
        const auto end=std::min<std::uint64_t>(68,std::uint64_t(offset)+bytes);
        for (Word arg=(std::max(offset,4u)-4)/4;4+4*arg<end;++arg)
            state.overwritten_arguments|=1u<<arg;
    }
    std::erase_if(state.locals,[&](const auto& item) {return item.first-offset<bytes || offset-item.first<4;});
    if (bytes==4 && value.kind!=Kind::unknown && state.locals.size()<48) state.locals[offset]=value;
}

void execute(const Instruction& ins,State& state,CallSources* report,bool cdecl) {
    const auto op = ins.decoded.mnemonic;
    const auto& dst = ins.operands[0];
    const auto& src = ins.operands[1];
    auto& sp = state.regs[ZYDIS_REGISTER_ESP-ZYDIS_REGISTER_EAX];
    if (op == ZYDIS_MNEMONIC_PUSH) {
        state.third = state.second;
        state.second = state.top;
        state.top = operand(ins,dst,state);
        if (dst.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && ins.decoded.operand_width==32 && !ins.relocated_immediate)
            state.top={Kind::constant,static_cast<Word>(dst.imm.value.u)};
        if (ins.decoded.operand_width!=32) state.top=state.second=state.third={};
        sp = plus(sp,-static_cast<Word>(ins.decoded.operand_width/8));
        if (sp.kind==Kind::stack) stack_store(state,sp.base,ins.decoded.operand_width/8,state.top);
        return;
    }
    if (op==ZYDIS_MNEMONIC_POP && dst.type==ZYDIS_OPERAND_TYPE_REGISTER && dst.reg.value!=ZYDIS_REGISTER_ESP) {
        const auto r=index(dst.reg.value);
        if (r>=0) state.regs[r]=dst.size==32?state.top:PointerValue{};
        sp=plus(sp,ins.decoded.operand_width/8);
        state.top=dst.size==32?state.second:PointerValue{};
        state.second=dst.size==32?state.third:PointerValue{};
        state.third={};
        return;
    }
    if (ins.decoded.meta.category == ZYDIS_CATEGORY_CALL) {
        if (ins.call_next_instruction) {
            state.third=state.second;
            state.second=state.top;
            state.top={Kind::relocated,ins.address+ins.decoded.length};
            sp=plus(sp,-4u);
            if (sp.kind==Kind::stack) stack_store(state,sp.base,4,state.top);
            return;
        }
        const auto direct = ins.target();
        const auto target = direct ? PointerValue{Kind::relocated,*direct} : operand(ins,dst,state);
        if (report) {
            const auto frame=state.regs[ZYDIS_REGISTER_EBP-ZYDIS_REGISTER_EAX];
            const auto intact=frame==PointerValue{Kind::stack,0u-4} ? (~state.overwritten_arguments & 7u) : 0u;
            report->calls.push_back({ins.address,target,state.top,state.second,state.third,direct.has_value(),
                                     ins.address+ins.decoded.length,intact});
        }
        if (report && !direct && state.nonzero.kind==Kind::module_slot)
            report->nonzero_cells.emplace(ins.address,state.nonzero.base);
        for (unsigned i=0; i<state.regs.size(); ++i) {
            if (i == ZYDIS_REGISTER_ESP-ZYDIS_REGISTER_EAX) continue;
            // Only ABI provenance uses nonvolatile registers across a call. A fixed
            // target is reported as such only from instructions in this function.
            if (i < 3 || state.regs[i].kind == Kind::constant || state.regs[i].kind == Kind::relocated ||
                state.regs[i].kind == Kind::absolute_slot)
                state.regs[i] = {};
        }
        const Word name = state.top.kind == Kind::relocated ? state.top.base : UINT32_MAX;
        if (target.kind == Kind::argument_method && target.base == 0)
            state.regs[0] = {Kind::getter_result,target.offset,name,ins.address};
        else if (target.kind==Kind::module_method && target.offset==0x6c && name!=UINT32_MAX)
            state.regs[0]={Kind::cached_lookup,target.base,name,ins.address};
        else if ((target.kind == Kind::module_method || target.kind == Kind::returned_method) && target.offset == 0 && name != UINT32_MAX)
            state.regs[0] = {Kind::method_result,ins.address};
        state.top = state.second = state.third = {};
        state.locals.clear();
        if (!cdecl) sp={};
        return;
    }
    PointerValue result;
    if (op == ZYDIS_MNEMONIC_MOV || op == ZYDIS_MNEMONIC_LEA) {
        result = operand(ins,src,state,op == ZYDIS_MNEMONIC_LEA);
        if (op==ZYDIS_MNEMONIC_LEA && result.kind==Kind::stack && result.base>=4 && result.base<68) {
            // An addressable input can be changed through a pointer given to another routine.
            state.overwritten_arguments=0xffff;
            std::erase_if(state.locals,[](const auto& item){return item.first>=4 && item.first<68;});
        }
        if (dst.type == ZYDIS_OPERAND_TYPE_MEMORY && (dst.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            const auto addr = operand(ins,dst,state,true);
            if (report && addr.kind == Kind::relocated && result.kind == Kind::getter_result)
                report->getter_stores.push_back({ins.address,addr.base,result.base,result.offset});
            if (report && addr.kind==Kind::relocated && result==PointerValue{Kind::argument,0} && dst.size==32)
                report->getter_stores.push_back({ins.address,addr.base,UINT32_MAX,UINT32_MAX});
            if (report && addr.kind==Kind::relocated && result.kind==Kind::cached_lookup && dst.size==32)
                report->getter_stores.push_back({ins.address,addr.base,0x6c,result.offset,result.base});
            if (report && addr.kind==Kind::relocated && result.kind==Kind::method_result)
                report->allocation_stores.push_back({ins.address,addr.base,result.base});
            if (report && result.kind == Kind::relocated && dst.size==32 && dst.mem.index==ZYDIS_REGISTER_NONE) {
                const auto r=index(dst.mem.base);
                if (r>=0 && state.regs[r].kind==Kind::method_result)
                    report->method_stores.push_back({state.regs[r].base,ins.address,static_cast<Word>(dst.mem.disp.value),result.base});
            }
        }
    } else if ((op == ZYDIS_MNEMONIC_ADD || op == ZYDIS_MNEMONIC_SUB) && src.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        auto amount=static_cast<Word>(src.imm.value.u);
        result=plus(operand(ins,dst,state),op == ZYDIS_MNEMONIC_ADD ? amount : -amount);
    }
    for (unsigned i=0; i<ins.decoded.operand_count; ++i) {
        const auto& item=ins.operands[i];
        if (item.type==ZYDIS_OPERAND_TYPE_MEMORY && (item.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            const auto location=operand(ins,item,state,true);
            if (location.kind==Kind::stack) {
                if (location==sp) state.top=result;
                if (location==plus(sp,4)) state.second=result;
                if (location==plus(sp,8)) state.third=result;
                stack_store(state,location.base,item.size/8,item.size==32?result:PointerValue{});
            } else if (location.kind!=Kind::constant && location.kind!=Kind::relocated) {
                state.top=state.second=state.third={};
                state.locals.clear();
            }
        }
        if (item.type != ZYDIS_OPERAND_TYPE_REGISTER || !(item.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) continue;
        const auto r=index(item.reg.value);
        if (r<0) continue;
        if (item.reg.value==ZYDIS_REGISTER_ESP) state.top=state.second=state.third={};
        state.regs[r] = i==0 && item.size==32 && !(item.actions & ZYDIS_OPERAND_ACTION_CONDWRITE) ? result : PointerValue{};
    }
}

bool merge(PointerValue& into,const PointerValue& from) {
    if (into == from || into.kind == Kind::unknown) return false;
    into = {};
    return true;
}
}

CallSources trace_call_sources(const Function& fn,const std::map<Word,Word>& relocations) {
    std::set<Word> leaders{fn.entry};
    leaders.insert(fn.alternate_entries.begin(),fn.alternate_entries.end());
    for (const auto& [pc,ins] : fn.instructions) {
        const auto cat=ins.decoded.meta.category;
        if (cat!=ZYDIS_CATEGORY_COND_BR && cat!=ZYDIS_CATEGORY_UNCOND_BR) continue;
        if (const auto target=ins.target()) leaders.insert(*target);
        leaders.insert(ins.jump_targets.begin(),ins.jump_targets.end());
        leaders.insert(pc+ins.decoded.length);
    }
    std::map<Word,State> states;
    std::deque<Word> work;
    State entry{};
    entry.regs[ZYDIS_REGISTER_ESP-ZYDIS_REGISTER_EAX]={Kind::stack};
    std::map<Word,std::vector<State>> predecessors;
    bool recording_edges=false;
    auto visit=[&](Word pc,const State& state) {
        if (!fn.instructions.contains(pc)) return;
        if (recording_edges) {
            const auto it=predecessors.find(pc);
            if (it!=predecessors.end() && it->second.size()<=16) it->second.push_back(state);
            return;
        }
        auto [it,added]=states.try_emplace(pc,state);
        bool changed=added;
        if (!added) {
            for (unsigned i=0;i<state.regs.size();++i) changed |= merge(it->second.regs[i],state.regs[i]);
            changed |= merge(it->second.top,state.top);
            changed |= merge(it->second.second,state.second);
            changed |= merge(it->second.third,state.third);
            changed |= merge(it->second.nonzero,state.nonzero);
            changed |= std::erase_if(it->second.locals,[&](const auto& item) {
                const auto next=state.locals.find(item.first);
                return next==state.locals.end() || next->second!=item.second;
            })!=0;
            const auto written=state.overwritten_arguments | it->second.overwritten_arguments;
            changed |= written!=it->second.overwritten_arguments;
            it->second.overwritten_arguments=written;
        }
        if (changed) work.push_back(pc);
    };
    visit(fn.entry,entry);
    for (const auto pc:fn.alternate_entries) visit(pc,State{});
    auto block=[&](Word start,State state,CallSources* report) {
        auto it=fn.instructions.find(start);
        PointerValue tested;
        while (it!=fn.instructions.end()) {
            const auto& [pc,ins]=*it++;
            const auto next=pc+ins.decoded.length,cat=static_cast<unsigned>(ins.decoded.meta.category);
            if (ins.relocated_displacement || ins.relocated_immediate) {
                // COFF stores the addend in the instruction and the symbol offset in its relocation.
                // Leave the decoder's original operands intact for lowering.
                auto resolved=ins;
                for (auto& op:resolved.operands) {
                    if (ins.relocated_immediate && op.type==ZYDIS_OPERAND_TYPE_IMMEDIATE)
                        op.imm.value.u=relocations.at(pc+ins.decoded.raw.imm[0].offset);
                    if (ins.relocated_displacement && op.type==ZYDIS_OPERAND_TYPE_MEMORY)
                        op.mem.disp.value=relocations.at(pc+ins.decoded.raw.disp.offset);
                }
                execute(resolved,state,report,fn.relocatable);
            } else execute(ins,state,report,fn.relocatable);
            if (cat==ZYDIS_CATEGORY_COND_BR || cat==ZYDIS_CATEGORY_UNCOND_BR) {
                if (!report) {
                    if (const auto target=ins.target()) {
                        if (ins.decoded.mnemonic==ZYDIS_MNEMONIC_JNZ && tested.kind==Kind::module_slot) {
                            auto taken=state; taken.nonzero=tested; visit(*target,taken);
                        } else visit(*target,state);
                    }
                    for (auto target:ins.jump_targets) visit(target,state);
                }
                if (cat==ZYDIS_CATEGORY_UNCOND_BR) break;
                if (ins.decoded.mnemonic==ZYDIS_MNEMONIC_JZ && tested.kind==Kind::module_slot) state.nonzero=tested;
            }
            const auto& lhs=ins.operands[0]; const auto& rhs=ins.operands[1];
            tested=ins.decoded.mnemonic==ZYDIS_MNEMONIC_TEST && lhs.type==ZYDIS_OPERAND_TYPE_REGISTER &&
                rhs.type==ZYDIS_OPERAND_TYPE_REGISTER && lhs.reg.value==rhs.reg.value ? operand(ins,lhs,state) : PointerValue{};
            if (cat==ZYDIS_CATEGORY_RET || it==fn.instructions.end() || it->first!=next) break;
            if (leaders.contains(next)) {
                if (!report) visit(next,state);
                break;
            }
        }
    };
    while (!work.empty()) {
        const auto start=work.front();
        work.pop_front();
        block(start,states.at(start),nullptr);
    }
    CallSources report;
    for (const auto& [start,state]:states) {
        const auto first=report.calls.size();
        block(start,state,&report);
        if (std::ranges::any_of(std::span(report.calls).subspan(first),[](const auto& c){return !c.direct && c.target.kind==Kind::unknown;}))
            predecessors.try_emplace(start);
    }
    if (!predecessors.empty()) {
        // Revisit a join separately along every incoming edge. A different method
        // selected on each branch can still have a complete, finite target set.
        recording_edges=true;
        visit(fn.entry,entry);
        for (auto pc:fn.alternate_entries) visit(pc,State{});
        for (const auto& [start,state]:states) block(start,state,nullptr);
        for (const auto& [start,inputs]:predecessors) {
            if (inputs.size()<2 || inputs.size()>16) continue;
            for (const auto& in:inputs) {
                CallSources path;
                block(start,in,&path);
                for (const auto& c:path.calls) if (!c.direct) {
                    auto& values=report.alternatives[c.site];
                    if (std::ranges::find(values,c.target)==values.end()) values.push_back(c.target);
                }
            }
        }
        std::erase_if(report.alternatives,[](const auto& item) {
            return item.second.empty() || std::ranges::any_of(item.second,[](auto v){return v.kind==Kind::unknown;});
        });
    }
    return report;
}
std::string_view source_name(PointerSource source) {
    switch (source) {
    case Kind::unknown: return "unknown";
    case Kind::stack: return "stack";
    case Kind::argument: return "argument";
    case Kind::constant: return "constant";
    case Kind::relocated: return "module-address";
    case Kind::absolute_slot: return "absolute-slot";
    case Kind::module_slot: return "module-slot";
    case Kind::argument_method: return "argument-method";
    case Kind::module_method: return "module-method";
    case Kind::absolute_method: return "absolute-method";
    case Kind::getter_result: return "getter-result";
    case Kind::returned_method: return "returned-method";
    case Kind::method_result: return "method-result";
    case Kind::cached_lookup: return "cached-lookup";
    }
    return "unknown";
}
}
