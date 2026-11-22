#include "cpu.h"
#include "logic_exports.h"
#include "unwind.h"
#include <algorithm>
#include <atomic>

namespace xbox_native {
std::uint64_t Memory::load_mapped(std::uint32_t address,unsigned width) const {
    if (const auto* io = address >= io_floor_ ? find_io(address, width) : nullptr) {
        const auto offset = address - io->base;
        const auto low = io->read(io->context, offset, std::min(width, 4U));
        if (width <= 4) return low;
        return low | (std::uint64_t(io->read(io->context, offset + 4, 4)) << 32);
    }
    std::uint64_t value{};
    std::memcpy(&value,mapped_pointer(address,width),width);
    if constexpr (std::endian::native==std::endian::big) value=std::byteswap(value);
    return value;
}
void Memory::store_mapped(std::uint32_t address,unsigned width,std::uint64_t value) {
    if (const auto* io = address >= io_floor_ ? find_io(address, width) : nullptr) {
        const auto offset = address - io->base;
        io->write(io->context, offset, std::min(width, 4U), static_cast<std::uint32_t>(value));
        if (width == 8) io->write(io->context, offset + 4, 4, static_cast<std::uint32_t>(value >> 32));
        return;
    }
    if constexpr (std::endian::native==std::endian::big) value=std::byteswap(value);
    std::memcpy(mapped_pointer(address,width),&value,width);
}
std::byte* Memory::mapped_pointer(std::uint32_t address, std::size_t size) const {
    const auto primary = [&](std::uint32_t offset) {
        if (!blocked_pages_.empty() && size) {
            const auto first = offset >> 12, last = static_cast<std::uint32_t>((offset + size - 1) >> 12);
            if (std::ranges::any_of(blocked_pages_.subspan(first, last - first + 1), [](auto blocked) { return blocked != 0; }))
                throw std::runtime_error(std::format("Guest memory access at 0x{:08X} crosses an uncommitted page", address));
        }
        return bytes_.data() + offset;
    };
    if(address>=base_ && address-base_<=bytes_.size() && size<=bytes_.size()-(address-base_))
        return primary(address - base_);
    for(unsigned i=0;i<region_count_;++i) {
        const auto& region=regions_[i];
        if(address>=region.base && address-region.base<=region.bytes.size() && size<=region.bytes.size()-(address-region.base)) {
            if (region.primary_offset != UINT32_MAX)
                return primary(region.primary_offset + address - region.base);
            return region.bytes.data()+(address-region.base);
        }
    }
    throw std::runtime_error(std::format("Guest memory access at 0x{:08X}, {} bytes, outside the mapped regions", address, size));
}

void writeback_guest_cache(const Cpu& cpu) {
    if (cpu.cs_selector & 3) throw GuestTrap(13, cpu.eip, cpu.eip);
    // Guest RAM and its aliases share ordinary coherent host memory. Publish CPU writes before the next device access.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

bool invoke_platform(Cpu& cpu, Memory& memory, std::uint32_t address) {
    if (!cpu.kernel || !cpu.kernel->platform) return false;
    cpu.eip = address;
    HostFloatingScope host(cpu.floating);
    return cpu.kernel->platform(cpu.kernel->context, cpu, memory, address);
}
std::uint32_t read_port(Cpu& cpu,std::uint16_t port,unsigned width) {
    if(!cpu.kernel || !cpu.kernel->read_port) throw NativeBoundary(cpu.eip,std::format("unbound {}-bit input port 0x{:04X}",width,port));
    HostFloatingScope host(cpu.floating);
    return cpu.kernel->read_port(cpu.kernel->context,port,width);
}
void write_port(Cpu& cpu,std::uint16_t port,unsigned width,std::uint32_t value) {
    if(!cpu.kernel || !cpu.kernel->write_port) throw NativeBoundary(cpu.eip,std::format("unbound {}-bit output port 0x{:04X}",width,port));
    HostFloatingScope host(cpu.floating);
    cpu.kernel->write_port(cpu.kernel->context,port,width,value);
}
void invoke_native(Cpu& cpu, Memory& memory, std::uint32_t address) {
    const auto caller = cpu.eip;
    cpu.eip = address;
    // Loaded logic objects own per-call ABI bindings. Check the caller before
    // selecting any guest, cached XBE, or kernel destination.
    if (cpu.kernel && cpu.kernel->resolve_module) {
        if (const auto fn = cpu.kernel->resolve_module(cpu.kernel->context,cpu,caller)) {
            fn(cpu,memory);
            return;
        }
    }
    if (logic_export_field(caller))
        throw NativeBoundary(address,std::format("No matching disc module export for call at 0x{:08X}",caller));
    if(address>0xFFF00000U && address<0xFFF02000U && !(address&15U)) {
        const auto ordinal=(address-0xFFF00000U)/16;
        if(!cpu.kernel || !cpu.kernel->entries[ordinal]) throw NativeBoundary(address,"kernel function is unbound");
        HostFloatingScope host(cpu.floating);
        cpu.kernel->entries[ordinal](cpu.kernel->context,cpu,memory);
        return;
    }
    // Only the verified executable's table is immutable. A disc module can unload
    // and another module can later use the same address.
    static thread_local std::array<const CompiledFunction*,256> recent{};
    auto& cached = recent[((address >> 4) ^ (address >> 12)) & (recent.size() - 1)];
    if (cached && cached->address == address) {
        cached->run(cpu, memory);
        return;
    }
    const auto found = std::ranges::lower_bound(compiled_batch, address, {}, &CompiledFunction::address);
    if (found != compiled_batch.end() && found->address == address) {
        cached = &*found;
        found->run(cpu, memory);
        return;
    }
    throw NativeBoundary(address, std::format("call or jump from 0x{:08X} has no mapped entry", caller));
}

namespace {
using Word = std::uint32_t;
struct Context32 {
    Word flags;
    std::array<Word, 34> debug_and_x87;
    Word gs, fs, es, ds, edi, esi, ebx, edx, ecx, eax, ebp, eip, cs, eflags, esp, ss;
    std::array<std::byte, 512> extended;
};
static_assert(sizeof(Context32) == 716 && offsetof(Context32, eip) == 184);
}

void dispatch_divide_fault(Cpu& cpu, Memory& memory, Word site) {
    cpu.eip = site;
    if (!cpu.fs_base) throw GuestTrap(0, site, site);
    const auto stack = cpu.registers[esp], low = memory.load<Word>(cpu.fs_base + 8), high = memory.load<Word>(cpu.fs_base + 4);
    constexpr Word scratch = 80 + sizeof(Context32) + 24;
    if (stack > high || stack < low || stack - low < scratch) throw GuestTrap(0,site,site);
    const Word record = stack - 80, context = record - static_cast<Word>(sizeof(Context32)), call = context - 24;
    const auto saved = cpu.registers;
    const auto flags = cpu.flags;
    Context32 state{0x10007, {}, cpu.gs_selector,cpu.fs_selector,cpu.ds_selector,cpu.ds_selector,
        saved[edi],saved[esi],saved[ebx],saved[edx],saved[ecx],saved[eax],saved[ebp],site,cpu.cs_selector,flags,stack,cpu.ss_selector,{}};
    std::memcpy(memory.access(static_cast<Word>(context),sizeof(state)), &state,sizeof(state));
    std::memset(memory.access(record,80),0,80);
    memory.store(record,0xC0000094U);
    memory.store(record+12,site);
    for (auto frame = memory.load<Word>(cpu.fs_base); frame != UINT32_MAX;) {
        if ((frame & 3) || frame < stack || frame > high - 8) throw GuestTrap(0,site,site);
        const auto next = memory.load<Word>(frame);
        if (next <= frame) throw GuestTrap(0,site,site);
        constexpr Word returned = 0xFFFFFFD4;
        const Word args[]{returned, record, frame, static_cast<Word>(context), call + 20, frame};
        std::memcpy(memory.access(call,sizeof(args)),args,sizeof(args));
        cpu.registers[esp] = call;
        invoke_native(cpu,memory,memory.load<Word>(frame+4));
        const auto disposition = cpu.registers[eax];
        if (cpu.eip != returned || cpu.registers[esp] != call + 4) throw NativeBoundary(site,"Invalid exception handler return");
        cpu.registers = saved;
        cpu.flags = flags;
        cpu.eip = site;
        if (disposition == 0) {
            std::memcpy(&state,memory.access(static_cast<Word>(context),sizeof(state)),sizeof(state));
            cpu.registers = {state.eax,state.ecx,state.edx,state.ebx,state.esp,state.ebp,state.esi,state.edi};
            cpu.flags = state.eflags;
            cpu.eip = state.eip;
            return;
        }
        if (disposition != 1) throw NativeBoundary(site,"Invalid divide exception disposition");
        frame = next;
    }
    throw GuestTrap(0,site,site);
}

void unwind_guest(Cpu& cpu, Memory& memory, std::uint32_t target, std::uint32_t resume,
                  std::uint32_t record, std::uint32_t value) {
    using Word = std::uint32_t;
    constexpr Word chain_end = UINT32_MAX;
    constexpr Word handler_return = 0xFFFFFFD4;
    const auto registers = cpu.registers;
    const auto flags = cpu.flags, site = cpu.eip, stack = registers[esp], tib = cpu.fs_base;
    if (!target || resume != memory.load<Word>(stack))
        throw NativeBoundary(site, "RtlUnwind requires a target frame and the CRT return address");
    const auto low = memory.load<Word>(tib + 8), high = memory.load<Word>(tib + 4);
    if (high < 20 || stack < low || stack > high - 20)
        throw NativeBoundary(site, "RtlUnwind entered with an invalid stack");
    const auto next_frame = [&](Word frame) {
        if (frame == chain_end || (frame & 3) || frame < stack || frame > high - 8)
            throw NativeBoundary(site, "RtlUnwind encountered an invalid exception frame");
        const auto next = memory.load<Word>(frame);
        if (next <= frame || (target != chain_end && next > target))
            throw NativeBoundary(site, "RtlUnwind target is outside the registered exception chain");
        return next;
    };
    auto frame = memory.load<Word>(tib);
    for (auto current = frame; current != target; current = next_frame(current)) {}
    if (target != chain_end && ((target & 3) || target < stack || target > high - 8))
        throw NativeBoundary(site, "RtlUnwind target is outside the guest stack");

    constexpr Word unwinding = 2; // EXCEPTION_UNWINDING
    if (record) memory.store(record + 4, memory.load<Word>(record + 4) | unwinding);
    if (frame != target) {
        constexpr Word record_size = 80, context_size = sizeof(Context32), dispatch_size = 4, call_size = 20;
        constexpr Word scratch_size = record_size + context_size + dispatch_size + call_size;
        if (stack - low < scratch_size) throw NativeBoundary(site, "RtlUnwind ran out of guest stack");
        const auto context = stack - record_size - context_size;
        const auto dispatch = context - dispatch_size, call = dispatch - call_size;
        if (!record) {
            record = stack - record_size;
            std::memset(memory.access(record, record_size), 0, record_size);
            memory.store<Word>(record, 0xC0000027); // STATUS_UNWIND
            memory.store(record + 4, unwinding);
            memory.store(record + 12, resume);
        }
        const Context32 original{
            0x00010007, {}, cpu.gs_selector, cpu.fs_selector, cpu.ds_selector, cpu.ds_selector,
            registers[edi], registers[esi], registers[ebx], registers[edx], registers[ecx], value,
            registers[ebp], resume, cpu.cs_selector, flags, stack + 20, cpu.ss_selector, {}
        };
        std::memcpy(memory.access(context, sizeof(original)), &original, sizeof(original));
        while (frame != target) {
            const auto next = next_frame(frame);
            memory.store(dispatch, frame);
            const Word arguments[]{handler_return, record, frame, context, dispatch};
            std::memcpy(memory.access(call, sizeof(arguments)), arguments, sizeof(arguments));
            cpu.registers = registers;
            cpu.registers[esp] = call;
            cpu.flags = flags;
            cpu.eip = site;
            invoke_native(cpu, memory, memory.load<Word>(frame + 4));
            if (cpu.eip != handler_return || cpu.registers[esp] != call + 4 ||
                cpu.registers[eax] != 1 || memory.load<Word>(tib) != frame) {
                throw NativeBoundary(site, "Exception handler did not finish the requested unwind");
            }
            memory.store(tib, next);
            frame = next;
        }
    }
    cpu.registers = registers;
    cpu.registers[esp] = stack + 20;
    cpu.registers[eax] = value;
    cpu.flags = flags;
    cpu.eip = resume;
}
}
