#include "play_diagnostics.h"
#include "file.h"
#include "play_window.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <Windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <wrl/client.h>

namespace xbox_native {
namespace {
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
constexpr auto recording_limit = std::chrono::seconds(60);
constexpr std::size_t frame_limit = 60 * 1000;

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::optional<std::uint64_t> thread_ticks() {
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return {};
    const auto ticks = [](const FILETIME& time) {
        return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}

void write_file(const std::filesystem::path& path, const auto& write) {
    std::ofstream file(path, std::ios::binary | std::ios::noreplace);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    write(file);
    file.close();
}

std::string processor_name() {
    char name[256]{};
    DWORD size = sizeof(name);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) return name;
    return {};
}

std::string dimensions(std::array<std::uint32_t, 2> size) {
    return std::format("[{},{}]", size[0], size[1]);
}

struct Distribution {
    double median{}, p99{}, average{};
};

Distribution summarize(std::vector<double> values) {
    if (values.empty()) return {};
    std::ranges::sort(values);
    double sum = 0;
    for (auto value : values) sum += value;
    const auto count = values.size();
    const auto median = (values[(count - 1) / 2] + values[count / 2]) / 2;
    return {median, values[count - count / 100 - 1], sum / count};
}

// Queries are collected without blocking the renderer. Each result belongs
// to the single scanout interval that issued it.
class GpuIntervals {
public:
    explicit GpuIntervals(Renderer& renderer) : context_(renderer.native_context()) {
        auto* device = renderer.native_device();
        for (auto& query : queries_) {
            const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
            if (FAILED(device->CreateQuery(&disjoint, &query.timer)) ||
                FAILED(device->CreateQuery(&stamp, &query.begin)) ||
                FAILED(device->CreateQuery(&stamp, &query.end))) return;
        }
        available_ = true;
    }

    ~GpuIntervals() { end(); }

    bool active() const { return active_ != nullptr; }

    bool start(std::size_t sample) {
        if (!available_) return false;
        for (auto& query : queries_) {
            if (query.sample) continue;
            active_ = &query;
            query.sample = sample;
            context_->Begin(query.timer.Get());
            context_->End(query.begin.Get());
            return true;
        }
        return false;
    }

    void end() {
        if (!active_) return;
        context_->End(active_->end.Get());
        context_->End(active_->timer.Get());
        active_ = nullptr;
    }

    void collect(const auto& record) {
        for (auto& query : queries_) {
            if (!query.sample || &query == active_) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT interval{};
            UINT64 begin = 0, end = 0;
            const auto data = [&](ID3D11Query* source, auto& value) {
                return context_->GetData(source, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            };
            const auto timer = data(query.timer.Get(), interval);
            const auto first = data(query.begin.Get(), begin), last = data(query.end.Get(), end);
            if (FAILED(timer) || FAILED(first) || FAILED(last)) {
                available_ = false;
                record(*query.sample, std::nullopt);
                query.sample.reset();
                continue;
            }
            if (timer != S_OK || first != S_OK || last != S_OK) continue;
            if (!interval.Disjoint && interval.Frequency && end >= begin)
                record(*query.sample, double(end - begin) * 1000 / double(interval.Frequency));
            else record(*query.sample, std::nullopt);
            query.sample.reset();
        }
    }

private:
    struct Query {
        ComPtr<ID3D11Query> timer, begin, end;
        std::optional<std::size_t> sample;
    };
    ID3D11DeviceContext* context_;
    std::array<Query, 16> queries_;
    Query* active_{};
    bool available_{};
};
}

struct PlayDiagnostics::Recording {
    struct GpuWork {
        double ms{};
        unsigned issued{}, resolved{};
        bool incomplete{};
        bool complete() const { return issued && issued == resolved && !incomplete; }
    };

    struct Sample {
        std::uint32_t scanout;
        std::array<std::uint32_t, 2> window;
        double elapsed_ms, present_ms;
        std::optional<double> cpu_ms;
        GpuWork gpu_work;
        FrameCounters delta;
        std::optional<std::uint32_t> engine_updates;
        bool skipped;
    };

    Recording(Renderer& renderer, const FrameCounters& counters)
        : gpu(renderer), native(renderer.scanout_size()), adapter(renderer.adapter()), processor(processor_name()),
          first(counters), last(counters), started(Clock::now()) {
        samples.reserve(4096);
    }

    void restart(const FrameCounters& counters) {
        last = counters;
        previous_ticks = thread_ticks();
        boundary = Clock::now();
    }

    void begin_gpu_work() {
        if (gpu.active()) return;
        collect();
        if (gpu.start(samples.size())) ++gpu_work.issued;
        else gpu_work.incomplete = true;
    }

    void sample(const FrameCounters& counters, std::array<std::uint32_t, 2> window, double present_ms, bool skipped) {
        const auto now = Clock::now();
        const auto ticks = thread_ticks();
        std::optional<double> cpu;
        if (!skipped && ticks && previous_ticks) cpu = double(*ticks - *previous_ticks) / 10'000;
        const auto& a = counters.gpu;
        const auto& b = last.gpu;
        const GpuStats gpu_delta{a.submissions - b.submissions, a.packets - b.packets, a.methods - b.methods,
            a.draws - b.draws, a.vertices - b.vertices, a.clears - b.clears, a.flips - b.flips,
            a.fences - b.fences, a.blits - b.blits};
        std::optional<std::uint32_t> engine_updates;
        if (last.engine && last.engine == counters.engine && counters.engine_updates >= last.engine_updates)
            engine_updates = counters.engine_updates - last.engine_updates;
        samples.push_back({counters.scanout, window, skipped ? 0 : milliseconds(now - boundary), skipped ? 0 : present_ms,
            cpu, std::exchange(gpu_work, {}), {counters.scanout - last.scanout, counters.presentations - last.presentations,
                counters.overlays - last.overlays, counters.vblanks - last.vblanks, counters.checkpoints - last.checkpoints,
                counters.audio_frames - last.audio_frames, gpu_delta}, engine_updates, skipped});
        collect();
        if (now >= next_memory_sample) sample_memory(now);
    }

    void collect() {
        gpu.collect([&](std::size_t index, std::optional<double> ms) {
            auto& work = index == samples.size() ? gpu_work : samples.at(index).gpu_work;
            ++work.resolved;
            if (ms) work.ms += *ms;
            else work.incomplete = true;
        });
    }

    void sample_memory(Clock::time_point now = Clock::now()) {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            peak_working = std::max(peak_working, memory.PeakWorkingSetSize);
            peak_private = std::max(peak_private, memory.PrivateUsage);
        }
        next_memory_sample = now + std::chrono::milliseconds(500);
    }

    void write(const Path& directory, const DiagnosticOptions& options, std::string_view reason, bool vsync) const {
        std::vector<double> elapsed, cpu, gpu_times, present;
        std::uint64_t flips = 0, presentations = 0, overlays = 0, vblanks = 0, checkpoints = 0, audio_frames = 0;
        std::uint64_t updates = 0, update_samples = 0;
        double measured_ms = 0;
        for (const auto& sample : samples) {
            if (!sample.skipped) {
                elapsed.push_back(sample.elapsed_ms);
                measured_ms += sample.elapsed_ms;
                present.push_back(sample.present_ms);
                if (sample.cpu_ms) cpu.push_back(*sample.cpu_ms);
                if (sample.gpu_work.complete()) gpu_times.push_back(sample.gpu_work.ms);
            }
            flips += sample.delta.gpu.flips;
            presentations += sample.delta.presentations;
            overlays += sample.delta.overlays;
            vblanks += sample.delta.vblanks;
            checkpoints += sample.delta.checkpoints;
            audio_frames += sample.delta.audio_frames;
            if (sample.engine_updates) {
                updates += *sample.engine_updates;
                ++update_samples;
            }
        }
        const auto timing = [](const std::vector<double>& values) {
            if (values.empty()) return std::string("{\"samples\":0}");
            const auto stats = summarize(values);
            return std::format("{{\"samples\":{},\"median_ms\":{:.4f},\"p99_ms\":{:.4f},\"average_ms\":{:.4f}}}",
                               values.size(), stats.median, stats.p99, stats.average);
        };
        write_file(directory / "performance.csv", [&](std::ostream& out) {
            out << "scanout,window_width,window_height,excluded,frame_ms,game_thread_cpu_ms,gpu_command_span_ms,present_ms,"
                   "guest_flips,presentations,overlay_scanouts,guest_vblanks,guest_checkpoints,audio_frames,draws,vertices,gpu_submissions,engine_updates\n";
            for (const auto& sample : samples) {
                const auto& d = sample.delta;
                out << sample.scanout << ',' << sample.window[0] << ',' << sample.window[1] << ',' << sample.skipped << ',';
                if (sample.skipped) out << ",,,";
                else {
                    out << sample.elapsed_ms << ',';
                    if (sample.cpu_ms) out << *sample.cpu_ms;
                    out << ',';
                    if (sample.gpu_work.complete()) out << sample.gpu_work.ms;
                    out << ',' << sample.present_ms;
                }
                out << ',' << d.gpu.flips << ',' << d.presentations << ',' << d.overlays << ',' << d.vblanks
                    << ',' << d.checkpoints << ',' << d.audio_frames << ',' << d.gpu.draws << ',' << d.gpu.vertices
                    << ',' << d.gpu.submissions << ',';
                if (sample.engine_updates) out << *sample.engine_updates;
                out << '\n';
            }
        });
        write_file(directory / "performance.txt", [&](std::ostream& out) {
            const auto stats = summarize(elapsed);
            out << std::format("Scooby-Doo! Mystery Mayhem\n{} scanouts measured, {} excluded for captures\n"
                               "Median {:.2f} ms, 99th percentile {:.2f} ms, {:.2f} scanouts per second\n"
                               "{} game flips, {} window presentations\n"
                               "GPU command timings available for {} of {} samples\nCSV contains one row per scanout.\n",
                               elapsed.size(), samples.size() - elapsed.size(), stats.median, stats.p99,
                               measured_ms ? 1000 * elapsed.size() / measured_ms : 0,
                               flips, presentations, gpu_times.size(), elapsed.size());
        });
        write_file(directory / "performance.json", [&](std::ostream& out) {
            out << "{\"format\":\"scoobymm-performance-v1\",\"reason\":" << json(reason)
                << ",\"session_report\":" << json(path_text(options.session_report)) << ",\"processor\":" << json(processor)
                << ",\"graphics_adapter\":" << json(adapter) << ",\"native_size\":" << dimensions(native)
                << ",\"window_size\":" << dimensions(samples.empty() ? std::array<std::uint32_t, 2>{} : samples.back().window)
                << ",\"vsync\":" << (vsync ? "true" : "false")
                << ",\"start_scanout\":" << first.scanout << ",\"scanouts\":" << samples.size()
                << ",\"excluded_capture_scanouts\":" << samples.size() - elapsed.size()
                << ",\"measured_scanouts_per_second\":" << (measured_ms ? 1000 * elapsed.size() / measured_ms : 0)
                << ",\"measured_ms\":" << measured_ms << ",\"guest_flips\":" << flips << ",\"presentations\":" << presentations
                << ",\"engine_updates\":" << (update_samples ? std::to_string(updates) : "null")
                << ",\"engine_update_samples\":" << update_samples
                << ",\"overlay_scanouts\":" << overlays << ",\"guest_vblanks\":" << vblanks
                << ",\"guest_checkpoints\":" << checkpoints << ",\"audio_frames\":" << audio_frames
                << ",\"process_lifetime_peak_working_set_bytes\":" << peak_working
                << ",\"recording_sampled_peak_private_bytes\":" << peak_private
                << ",\"frame\":" << timing(elapsed) << ",\"game_thread_cpu\":" << timing(cpu)
                << ",\"gpu_command_span\":" << timing(gpu_times) << ",\"present\":" << timing(present) << "}\n";
        });
    }

    GpuIntervals gpu;
    std::array<std::uint32_t, 2> native;
    std::string adapter, processor;
    FrameCounters first, last;
    Clock::time_point started, boundary, next_memory_sample{};
    std::optional<std::uint64_t> previous_ticks;
    std::vector<Sample> samples;
    GpuWork gpu_work;
    std::size_t peak_working{}, peak_private{};
};

PlayDiagnostics::PlayDiagnostics(const DiagnosticOptions& options, PlayWindow& window, bool vsync,
                                 WriteState write_state, ReadCounters read_counters)
    : options_(options), window_(window), vsync_(vsync), write_state_(std::move(write_state)),
      read_counters_(std::move(read_counters)) {}

PlayDiagnostics::~PlayDiagnostics() = default;

PlayDiagnostics::Path PlayDiagnostics::directory(std::string_view prefix, const Path& requested) {
    if (!requested.empty()) {
        auto path = std::filesystem::absolute(requested);
        std::filesystem::create_directories(path.parent_path());
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Diagnostic directory already exists");
        return path;
    }
    std::filesystem::create_directories(options_.root);
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    for (unsigned suffix = 0;; ++suffix) {
        auto path = options_.root / std::format("{}-{}-{}-{}", prefix, stamp, GetCurrentProcessId(), suffix);
        if (std::filesystem::create_directory(path)) return path;
    }
}

void PlayDiagnostics::title(std::string_view status) {
    std::string text(status);
    if (performance_) {
        if (!text.empty()) text += " | ";
        text += "F9 recording";
    }
    window_.diagnostic_title(text);
}

void PlayDiagnostics::poll() {
    const auto keys = window_.take_diagnostic_keys();
    if (keys.capture && !frame_requested_ && !capturing()) {
        frame_requested_ = true;
        title("F8 capture queued");
    }
    if (keys.toggle_performance) {
        if (performance_) stop_performance("hotkey");
        else toggle_requested_ = !toggle_requested_;
    }
    if (performance_ && Clock::now() - performance_->started >= recording_limit)
        stop_performance("time_limit");
}

void PlayDiagnostics::begin_gpu_work() {
    if (performance_) performance_->begin_gpu_work();
}

void PlayDiagnostics::end_gpu_work() {
    if (performance_) performance_->gpu.end();
}

void PlayDiagnostics::capture_frame(Renderer& renderer, Gpu& gpu, const FrameCounters& counters, const Image* video,
                                    bool displayed, std::span<const std::array<std::byte, 3>, 256> gamma) {
    gpu.trace_draws(nullptr);
    auto trace = std::move(trace_);
    frame_result_ = {};
    try {
        frame_result_.directory = directory("frame", frame_scripted_ ? options_.frame : Path{});
        const auto& dir = frame_result_.directory;
        auto native = video ? *video : renderer.readback(true);
        for (std::size_t i = 0; i < native.rgba.size(); i += 4) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                auto& value = native.rgba[i + channel];
                value = gamma[std::to_integer<unsigned>(value)][channel];
            }
        }
        save_png(native, dir / "frame.png");
        if (displayed) save_png(renderer.read_display(), dir / "window.png");
        write_file(dir / "draws.jsonl", [&](std::ostream& out) { out << trace->view(); });
        write_state_(dir);
        if (performance_) {
            performance_->collect();
            performance_->write(dir, options_, "frame_snapshot", vsync_);
        }
        const auto draws = gpu.stats().draws - initial_draws_;
        write_file(dir / "capture.json", [&](std::ostream& out) {
            out << "{\"format\":\"scoobymm-frame-v1\",\"scanout\":" << counters.scanout
                << ",\"session_report\":" << json(path_text(options_.session_report))
                << ",\"graphics_adapter\":" << json(renderer.adapter()) << ",\"native_size\":" << dimensions(renderer.scanout_size())
                << ",\"window_size\":" << dimensions(displayed ? renderer.display_size() : std::array<std::uint32_t, 2>{})
                << ",\"vsync\":" << (vsync_ ? "true" : "false") << ",\"movie_overlay\":" << (video ? "true" : "false")
                << ",\"draws\":" << draws << ",\"draw_trace_limit\":" << Gpu::draw_trace_limit
                << ",\"draw_trace_truncated\":" << (draws > Gpu::draw_trace_limit ? "true" : "false")
                << ",\"window_png\":" << (displayed ? "true" : "false") << "}\n";
        });
        if (frame_scripted_) frame_completed_ = true;
        std::cout << "Frame capture " << path_text(dir) << '\n';
        title("F8 saved to " + path_text(dir.filename()));
    } catch (const std::exception& error) {
        frame_result_.error = error.what();
        std::cerr << "Frame capture failed: " << error.what() << '\n';
        title("F8 capture failed");
    }
    frame_scripted_ = false;
}

void PlayDiagnostics::scanout(Renderer& renderer, Gpu& gpu, FrameCounters counters, double present_ms, bool captured) {
    const bool scripted_performance = !options_.performance.empty() && counters.scanout == options_.performance_at;
    if (performance_ || toggle_requested_ || scripted_performance)
        read_counters_(counters);
    if (performance_) {
        performance_->sample(counters, renderer.display_size(), present_ms, captured);
        if (performance_scripted_ && options_.performance_frames &&
            performance_->samples.size() >= options_.performance_frames)
            stop_performance("frame_limit");
        else if (performance_->samples.size() >= frame_limit) stop_performance("sample_limit");
        else if (Clock::now() - performance_->started >= recording_limit) stop_performance("time_limit");
        else performance_->restart(counters);
    }
    if (scripted_performance) {
        if (performance_) stop_performance("scripted_recording");
        performance_scripted_ = true;
        toggle_requested_ = true;
    }
    if (std::exchange(toggle_requested_, false)) {
        if (performance_) stop_performance("hotkey");
        else {
            try {
                performance_result_ = {};
                performance_ = std::make_unique<Recording>(renderer, counters);
                performance_->sample_memory();
                title();
                performance_->restart(counters);
            } catch (const std::exception& error) {
                performance_result_.error = error.what();
                performance_.reset();
                std::cerr << "Performance recording failed: " << error.what() << '\n';
                title("F9 recording failed");
            }
        }
    }
    if (!options_.frame.empty() && counters.scanout == options_.capture_at) {
        frame_scripted_ = true;
        frame_requested_ = true;
    }
    if (std::exchange(frame_requested_, false)) {
        trace_ = std::make_unique<std::ostringstream>();
        initial_draws_ = gpu.stats().draws;
        gpu.trace_draws(trace_.get());
        title("F8 capturing next frame");
    }
}

void PlayDiagnostics::stop_performance(std::string_view reason) {
    auto recording = std::move(performance_);
    recording->gpu.end();
    recording->collect();
    try {
        performance_result_.directory = directory("performance", performance_scripted_ ? options_.performance : Path{});
        recording->write(performance_result_.directory, options_, reason, vsync_);
        if (performance_scripted_) {
            performance_completed_ = !options_.performance_frames || recording->samples.size() >= options_.performance_frames;
            if (!performance_completed_) performance_result_.error = "Recording stopped before the requested scanout count";
        }
        std::cout << "Performance capture " << path_text(performance_result_.directory) << '\n';
        title("F9 saved to " + path_text(performance_result_.directory.filename()));
    } catch (const std::exception& error) {
        performance_result_.error = error.what();
        std::cerr << "Performance capture failed: " << error.what() << '\n';
        title("F9 capture failed");
    }
    performance_scripted_ = false;
}

bool PlayDiagnostics::finish() {
    if (performance_) stop_performance("session_end");
    if (!options_.frame.empty() && !frame_completed_ && frame_result_.error.empty())
        frame_result_.error = "Session ended before the requested frame capture";
    if (!options_.performance.empty() && !performance_completed_ && performance_result_.error.empty())
        performance_result_.error = "Session ended before the requested recording completed";
    return (options_.frame.empty() || frame_completed_) && (options_.performance.empty() || performance_completed_);
}

void PlayDiagnostics::report(std::ostream& out) const {
    const auto result = [&](std::string_view name, const Result& value) {
        if (!value.directory.empty() || !value.error.empty())
            out << ',' << json(name) << ":{\"directory\":" << json(path_text(value.directory))
                << ",\"error\":" << json(value.error) << '}';
    };
    result("frame_capture", frame_result_);
    result("performance_capture", performance_result_);
}
}
