#pragma once

#include "boot.h"
#include "gpu.h"

#include <functional>
#include <sstream>

namespace xbox_native {
class PlayWindow;

struct FrameCounters {
    std::uint32_t scanout{}, presentations{}, overlays{};
    std::uint64_t vblanks{}, checkpoints{}, audio_frames{};
    GpuStats gpu;
    std::uint32_t engine{}, engine_updates{};
    float engine_seconds{};
    std::uint64_t scheduler_idle_ticks{}, gpu_wait_ns{};
};

class PlayDiagnostics {
public:
    using WriteState = std::function<void(const std::filesystem::path&)>;
    using ReadCounters = std::function<void(FrameCounters&)>;

    PlayDiagnostics(const DiagnosticOptions&, PlayWindow&, WriteState, ReadCounters);
    ~PlayDiagnostics();
    void poll();
    bool capturing() const { return trace_ != nullptr; }
    bool measuring() const { return performance_ != nullptr; }
    void begin_gpu_work();
    void end_gpu_work();
    void capture_frame(Renderer&, Gpu&, const FrameCounters&, const Image* video, bool displayed,
                       std::span<const std::array<std::byte, 3>, 256> gamma);
    void scanout(Renderer&, Gpu&, FrameCounters, double present_ms, bool captured);
    bool finish();
    void report(std::ostream&) const;

private:
    bool recording_expired() const;
    using Path = std::filesystem::path;
    struct Recording;
    struct Result { Path directory; std::string error; };

    DiagnosticOptions options_;
    PlayWindow& window_;
    WriteState write_state_;
    ReadCounters read_counters_;
    std::unique_ptr<Recording> performance_;
    std::unique_ptr<std::ostringstream> trace_;
    std::uint64_t initial_draws_{};
    bool frame_requested_{}, toggle_requested_{}, frame_scripted_{}, performance_scripted_{};
    bool frame_completed_{}, performance_completed_{};
    Result frame_result_, performance_result_;

    Path directory(std::string_view prefix, const Path& requested);
    void stop_performance(std::string_view reason);
};
}
