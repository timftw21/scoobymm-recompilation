#pragma once
#include "cpu.h"
#include "gpu.h"
#include "guest_allocator.h"

namespace xbox_native {
enum class PlatformApi : std::uint32_t;
class PlayDiagnostics;

class GuestGraphics {
public:
    GuestGraphics(Memory& memory, GuestAllocator allocator, std::filesystem::path capture, std::uint32_t capture_index,
                  void* window = nullptr, bool vsync = true)
        : memory_(memory), allocator_(allocator), capture_(std::move(capture)), capture_index_(capture_index),
          window_(window), vsync_(vsync) {}
    bool invoke(PlatformApi api, Cpu& cpu);
    bool ready() const { return renderer_ != nullptr; }
    bool captured() const { return captured_; }
    void diagnostics(PlayDiagnostics* value) { diagnostics_ = value; }
    GpuStats stats() const { return gpu_ ? gpu_->stats() : GpuStats{}; }
    void advance_display(std::uint64_t now);
    std::uint64_t next_vblank() const;
    std::uint32_t overlay_frames() const { return overlay_frames_; }
    std::uint32_t scanouts() const { return scanouts_; }
    std::uint32_t presentations() const { return presentations_; }
    static constexpr std::uint32_t vblank_event = 0x001253DC;
private:
    Memory& memory_;
    GuestAllocator allocator_;
    std::filesystem::path capture_;
    std::uint32_t capture_index_;
    void* window_;
    bool vsync_;
    PlayDiagnostics* diagnostics_{};
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Gpu> gpu_;
    std::uint32_t cursor_{};
    std::array<std::array<std::byte, 3>, 256> gamma_{};
    unsigned gamma_index_{}, gamma_channel_{};
    bool gamma_written_{}, gamma_dirty_{};
    bool captured_{};
    std::array<std::uint32_t, (0x8B00 - 0x8100) / 4 + 1> video_registers_{};
    std::uint64_t vblanks_{};
    std::uint32_t scanouts_{}, overlay_frames_{}, presentations_{};
    bool overlay_pending_{}, submitting_{};

    std::uint32_t initialize(Cpu& cpu, std::uint32_t parameters);
    std::uint32_t sdk_call(Cpu& cpu, std::uint32_t address, std::span<const std::uint32_t> args = {});
    void submit();
    std::uint32_t read_register(std::uint32_t offset, unsigned width) const;
    void write_register(std::uint32_t offset, unsigned width, std::uint32_t value);
    void scanout();
    void composite_video(Image& image) const;
    void finish(Cpu& cpu, unsigned arguments);
};
}
