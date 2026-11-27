#pragma once
#include "cpu.h"
#include "gpu.h"
#include "guest_allocator.h"
#include "settings.h"

namespace xbox_native {
enum class PlatformApi : std::uint32_t;
class PlayDiagnostics;
class PlayWindow;

class GuestGraphics {
public:
    GuestGraphics(Memory& memory, GuestAllocator allocator, std::filesystem::path capture, std::uint32_t capture_index,
                  PlayWindow* window = nullptr)
        : memory_(memory), allocator_(allocator), capture_(std::move(capture)), capture_index_(capture_index),
          window_(window) {}
    bool invoke(PlatformApi api, Cpu& cpu);
    bool ready() const { return renderer_ != nullptr; }
    bool captured() const { return captured_; }
    bool capture_pending() const { return !capture_.empty() && !captured_; }
    void diagnostics(PlayDiagnostics* value) { diagnostics_ = value; }
    RenderStats render_stats() const { return renderer_ ? renderer_->stats() : RenderStats{}; }
    GpuStats stats() const { return gpu_ ? gpu_->stats() : GpuStats{}; }
    void advance_display(std::uint64_t now);
    void movie_opened(std::string_view disc_path);
    void name_widget(std::uint32_t handle,std::uint32_t name);
    void draw_widget(std::uint32_t widget);
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
    PlayWindow* window_;
    PlayDiagnostics* diagnostics_{};
    std::unique_ptr<Renderer> offscreen_;
    Renderer* renderer_{};
    std::unique_ptr<Gpu> gpu_;
    std::uint32_t cursor_{};
    // The HUD registry at 0x49DF0 has 512 slots and generation-tagged handles.
    struct WidgetAnchor { std::uint32_t handle = UINT32_MAX; int side{}; bool gameplay_root{}, title_backdrop{}; };
    std::array<WidgetAnchor,512> widget_anchors_{};
    std::uint32_t pause_menu_handle_ = UINT32_MAX;
    std::vector<Gpu::HudAnchor> hud_markers_;
    int hud_side_{};
    bool hud_art_{}, title_backdrop_{};
    AspectRatio presentation_aspect_{}, last_aspect_{};
    std::uint32_t widget_from_handle(std::uint32_t hud, std::uint32_t handle) const;
    bool pause_menu_visible() const;
    std::array<std::array<std::byte, 3>, 256> gamma_{};
    unsigned gamma_index_{}, gamma_channel_{};
    bool gamma_written_{}, gamma_dirty_{};
    bool captured_{};
    std::array<std::uint32_t, (0x8B00 - 0x8100) / 4 + 1> video_registers_{};
    static constexpr std::uint64_t ntsc_field_ns = 16'683'350; // SDK constant at 0x11EC16.
    std::uint64_t field_period_ns_ = ntsc_field_ns;
    std::uint64_t vblanks_{};
    std::uint32_t scanouts_{}, overlay_frames_{}, presentations_{};
    bool overlay_pending_{}, submitting_{};
    unsigned movie_matte_rows_{};
    unsigned movie_id_{};

    std::uint32_t initialize(Cpu& cpu, std::uint32_t parameters);
    std::uint32_t sdk_call(Cpu& cpu, std::uint32_t address, std::span<const std::uint32_t> args = {},
                           std::uint32_t device_offset = 0);
    void submit();
    std::uint32_t read_register(std::uint32_t offset, unsigned width) const;
    void write_register(std::uint32_t offset, unsigned width, std::uint32_t value);
    void scanout();
    VideoOverlay video_overlay();
    static void composite_video(Image&, const VideoOverlay&);
    void finish(Cpu& cpu, unsigned arguments);
};
}
