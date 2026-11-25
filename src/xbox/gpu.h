#pragma once
#include "render.h"
#include <functional>
#include <iosfwd>

namespace xbox_native {
class Memory;
struct GpuStats {
    std::uint64_t submissions=0,packets=0,methods=0,draws=0,vertices=0,clears=0,flips=0,fences=0,blits=0;
};
struct GraphicsObject {std::uint32_t handle,kind;};
// The original SDK produces NV2A packets. This bridge shares the native renderer
// with offline diagnostics; it does not decode or execute CPU instructions.
class Gpu {
public:
    enum class NotificationDelivery { on_idle, polled };
    Gpu(Renderer&,Memory&,std::uint32_t semaphore_base,std::span<const GraphicsObject>,
        NotificationDelivery = NotificationDelivery::on_idle);
    ~Gpu();
    // A FIFO position where HUD drawing changes: the widget's widescreen edge,
    // and whether draws from here on belong to HUD widgets (Upscaled HUD).
    struct HudAnchor { std::uint32_t address; int side; bool hud_art = false; };
    void submit(std::uint32_t begin,std::uint32_t end,std::span<const HudAnchor> anchors = {});
    void widescreen_buffers(std::array<std::uint32_t,3> color_addresses);
    bool busy();
    void poll_notifications(bool wait = false);
    void wait();
    void snapshot();
    GpuStats stats() const;
    std::uint64_t wait_ns() const;
    void trace_draws(std::ostream* output);
    static constexpr unsigned draw_trace_limit = 2048;
    std::function<void()> flip;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
