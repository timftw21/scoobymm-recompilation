#include "audio_effects.h"
#include <dsp56300.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

namespace xbox_native {
namespace {
using Word = std::uint32_t;
constexpr Word dma_end = 0x4000, dma_running = 0x10, dma_stopped = 0x20;

Dsp56300MemoryRegion buffer(Word start, Word end, Word* data, Word offset = 0) {
    Dsp56300MemoryRegion region{};
    region.start = start;
    region.end = end;
    region.kind = DSP56300_REGION_BUFFER;
    region.data.buffer = {data,offset};
    return region;
}
}

struct AudioEffects::Processor {
    std::array<Word,4096> x{}, program{};
    std::array<Word,2048> y{};
    std::vector<std::byte> scratch;
    std::unique_ptr<Dsp56300Jit,decltype(&dsp56300_destroy)> core{nullptr,dsp56300_destroy};
    Word interrupts{}, next = dma_end, start = dma_end, control{}, configuration{}, status_reads{};
    int cycle_credit = 500000;
    const char* fault = nullptr;

    Processor(std::span<const std::byte> kernel, std::span<const std::byte> image, std::size_t scratch_size)
        : scratch(scratch_size) {
        if (kernel.size() != 0x5cc || image.size() > scratch.size())
            throw std::runtime_error("Invalid Xbox global processor image");
        std::memcpy(program.data(),kernel.data(),kernel.size());
        std::ranges::copy(image,scratch.begin());
        auto io = Dsp56300MemoryRegion{0xffff80,0x1000000,DSP56300_REGION_CALLBACK,{}};
        io.data.callback = {this,
            [](void* context,Word reg) { return static_cast<Processor*>(context)->read(reg); },
            [](void* context,Word reg,Word value) { static_cast<Processor*>(context)->write(reg,value); }};
        const std::array xr{buffer(0,0x1000,x.data()),buffer(0x1400,0x1800,x.data(),0xc00),io};
        const auto yr = buffer(0,0x800,y.data()), pr = buffer(0,0x1000,program.data());
        const Dsp56300CreateInfo info{{xr.data(),static_cast<Word>(xr.size()),&yr,1,&pr,1}};
        core.reset(dsp56300_create(&info));
        if (!core) throw std::runtime_error("Could not create the Xbox effects processor");
        dsp56300_reset(core.get());
        run(); // Boot the control program. The first frame loads the signed effects image.
    }

    std::span<Word> memory(Word address) {
        if (address < 0x1000) return std::span(x).subspan(address);
        if (address >= 0x1400 && address < 0x1800) return std::span(x).subspan(address-0x800);
        if (address >= 0x1800 && address < 0x2000) return std::span(y).subspan(address-0x1800);
        if (address >= 0x2800 && address < 0x3800) return std::span(program).subspan(address-0x2800);
        return {};
    }

    // The verified programs use linear and circular scratch DMA, in 16- or 24-bit samples.
    void transfer() {
        for (unsigned chain = 0; !(next & dma_end); ++chain) {
            const auto node = memory(next & 0x3fff);
            if (chain == 128 || node.size() < 7) { fault = "Invalid DSP DMA chain"; return; }
            next = node[0];
            const auto flags = node[1], count = node[2], target = node[3], base = node[5];
            const auto id = (flags >> 5) & 15, format = (flags >> 10) & 7;
            const bool circular = id == 14;
            const Word bytes = format == 1 ? 2 : 4;
            const auto ram = memory(target);
            if ((flags & 13) || (!circular && id != 15) || (format != 1 && format != 2 && format != 6) ||
                count > ram.size() || base > scratch.size()) { fault = "Invalid DSP DMA descriptor"; return; }
            const auto space = std::span(scratch).subspan(base);
            const auto size = circular ? node[6]+1 : space.size();
            if (!size || size > space.size()) { fault = "Invalid DSP scratch buffer"; return; }
            auto offset = node[4];
            if (circular && offset >= size) offset = 0;
            for (Word i = 0; i < count; ++i) {
                Word value = bytes == 2 ? ram[i] >> 8 : ram[i];
                if (offset <= size && bytes <= size-offset) {
                    auto* sample = space.data()+offset;
                    if (flags & 2) std::memcpy(sample,&value,bytes);
                    else std::memcpy(&value,sample,bytes);
                    offset += bytes;
                    if (circular && offset == size) offset = 0;
                } else if (circular) {
                    auto* sample = reinterpret_cast<std::byte*>(&value);
                    for (Word b=0; b<bytes; ++b) {
                        if (flags & 2) space[offset] = sample[b];
                        else sample[b] = space[offset];
                        if (++offset == size) offset = 0;
                    }
                } else {
                    fault = "DSP transfer outside scratch buffer"; return;
                }
                if (!(flags & 2)) ram[i] = bytes == 2 ? (value & 0xffff) << 8 : value & 0xffffff;
            }
            if (flags & 16) node[4] = offset;
        }
        interrupts |= 0x80;
    }

    Word read(Word reg) {
        switch (reg) {
        case 0xffffb3: return dsp56300_cycle_count(core.get());
        case 0xffffc5: return interrupts;
        case 0xffffd4: return next;
        case 0xffffd5: return start;
        case 0xffffd6:
            if ((control & dma_running) && ++status_reads > 2) control = (control & ~dma_running) | dma_stopped;
            return control;
        case 0xffffd7: return configuration;
        default: fault = "Unknown DSP peripheral read"; return 0;
        }
    }

    void write(Word reg, Word value) {
        switch (reg) {
        case 0xffffb0: case 0xffffb1: case 0xffffb2: break; // Performance counters.
        case 0xffffc4: if (value & 1) dsp56300_set_halt_requested(core.get(),true); break;
        case 0xffffc5: interrupts &= ~value; break;
        case 0xffffd4: next = value; break;
        case 0xffffd5: start = value; break;
        case 0xffffd7: configuration = value; break;
        case 0xffffd6:
            switch (value & 7) {
            case 0: break;
            case 1: status_reads = 0; control = (control & ~dma_stopped) | dma_running; transfer(); break;
            case 2: control = (control & ~dma_running) | dma_stopped; break;
            case 3: control |= 8; break;
            case 4: control &= ~8U; break;
            default: fault = "Unknown DSP DMA command";
            }
            break;
        default: fault = "Unknown DSP peripheral write";
        }
    }

    void run() {
        dsp56300_set_halt_requested(core.get(),false);
        dsp56300_set_cycle_count(core.get(),0);
        // A hardware halt leaves the unused cycles in the core's budget. Replenish
        // only what it consumed, so each frame has the same bounded allowance.
        dsp56300_run(core.get(),cycle_credit);
        cycle_credit = static_cast<int>(dsp56300_cycle_count(core.get()));
        if (fault || !dsp56300_halt_requested(core.get())) {
            Dsp56300State state{};
            dsp56300_get_state(core.get(),&state);
            throw std::runtime_error(std::format("{} at DSP PC 0x{:X}",fault ? fault : "DSP missed end of frame",state.pc));
        }
    }
};

AudioEffects::AudioEffects(std::span<const std::byte> kernel,std::span<const std::byte> image,std::size_t scratch_size)
    : processor_(std::make_unique<Processor>(kernel,image,scratch_size)) {
    process(); // Load the effect code and state before the audio callback sees the image.
}
AudioEffects::~AudioEffects() = default;

std::span<std::uint32_t,1024> AudioEffects::mix_bins() { return std::span(processor_->x).subspan<0xc00,1024>(); }
void AudioEffects::process() {
    processor_->interrupts |= 2;
    processor_->run();
}
}
