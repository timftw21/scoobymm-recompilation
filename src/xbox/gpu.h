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
    // Apply the widget's placement and artwork settings at its FIFO position.
    struct HudAnchor { std::uint32_t address; int side; bool hud_art = false, title_backdrop = false; };
    void submit(std::uint32_t begin,std::uint32_t end,std::span<const HudAnchor> anchors = {});
    // Draws to these color surfaces narrow by the scale, 4:3 over the display
    // aspect, and HUD widgets keep to their edge.
    void widescreen_buffers(std::array<std::uint32_t,3> color_addresses, float scale);
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
