#include "boot.h"
#include "cpu.h"
#include "disc.h"
#include "guest_audio.h"
#include "guest_graphics.h"
#include "kernel_names.h"
#include "platform_bindings.h"
#include "play_diagnostics.h"
#include "play_window.h"
#include "sha1.h"
#include "shaders.h"
#include "unwind.h"
#include "xbe.h"
#include "../analysis_map.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <Windows.h>
#include <bcrypt.h>

namespace xbox_native {
namespace {
using Word = std::uint32_t;
constexpr Word guest_base = 0x10000, ram_end = 0x4000000, page = 4096;
constexpr Word kernel_base = 0x80010000, function_base = 0xFFF00000, return_address = 0xFFFFFFF0;
constexpr Word audio_register_page = 0xFE801000;
constexpr Word gamepad_type = 0x00144374;
constexpr Word gdt = kernel_base + page, shutdown_list = gdt + 64;
constexpr Word game_main = 0x000115D0; // Direct call from the verified XAPI startup at 0x00011C44.
constexpr Word bad_handle = 0xC0000008, bad_parameter = 0xC000000D, no_memory = 0xC0000017;
constexpr Word name_missing = 0xC0000034, name_conflict = 0xC0000035, address_conflict = 0xC0000018;
constexpr Word write_protected = 0xC00000A2, no_disc = 0xC0000013, end_of_file = 0xC0000011;
constexpr Word info_length_mismatch = 0xC0000004, access_denied = 0xC0000022, delete_pending = 0xC0000056;
constexpr Word delete_access = 0x10010000, share_delete = 4;
constexpr Word read_data = 0x90000001, write_data = 0x50000002, append_data = 0x50000004;
Word share_required(Word access) {
    return ((access & (read_data | 0x20000020U)) ? 1U : 0U) |
           ((access & (write_data | append_data)) ? 2U : 0U) |
           ((access & delete_access) ? share_delete : 0U);
}
constexpr Word wait_timeout = 0x00000102;
constexpr std::string_view cdrom = "\\DEVICE\\CDROM0", disk = "\\DEVICE\\HARDDISK0\\PARTITION1";
constexpr std::string_view disk_prefix = "\\DEVICE\\HARDDISK0\\PARTITION";
constexpr Word cache_partitions = 3, cache_capacity = 128 * 1024 * 1024;
constexpr Word file_limit = 8 * 1024 * 1024;
constexpr Word disk_cluster = 16384;
constexpr std::string_view legacy_save_magic = "SMMSAVE1", save_magic = "SMMSAVE2";

std::uint64_t file_time(FILETIME value) {
    return (std::uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime;
}

bool persistent_path(std::string_view path) {
    return path.starts_with("\\DEVICE\\HARDDISK0\\PARTITION1\\UDATA\\") ||
           path.starts_with("\\DEVICE\\HARDDISK0\\PARTITION1\\TDATA\\");
}

bool directory_match(std::string_view pattern, std::string_view name) {
    if (pattern == "*.*") pattern = "*";
    std::size_t p=0, n=0, star=std::string_view::npos, retry=0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) { ++p; ++n; }
        else if (p < pattern.size() && pattern[p] == '*') { star=p++; retry=n; }
        else if (star != std::string_view::npos) { p=star+1; n=++retry; }
        else return false;
    }
    return pattern.substr(p).find_first_not_of('*') == std::string_view::npos;
}

struct ThreadExit { Word status; };
struct KernelStop {
    unsigned ordinal;
    Word address;
    std::string reason;
};
struct DeviceStop { Word address, width; bool write; };
class OfflineBoot;
struct Thread {
    OfflineBoot* boot{};
    void* fiber{};
    Word id, object, routine, system, exit_status{};
    Word stack_bytes{}, waiters{};
    bool suspended{}, started{}, exited{}, returned{};
    Cpu cpu;
    std::function<bool()> wake;
    std::uint64_t deadline{UINT64_MAX};
};
struct Timer { std::uint64_t due; Word dpc; };
struct Event { std::string name; bool auto_reset, signaled; };
struct ModuleInstance { const CompiledModule* module; Word base, owner; };
struct FileTimes {
    std::uint64_t created{}, accessed{}, written{}, changed{};
};
static_assert(sizeof(FileTimes) == 32);
struct DirectoryItem {
    std::string name;
    Word size, allocation;
    bool directory;
    FileTimes times;
};
struct OpenFile {
    std::string path;
    std::optional<DiscEntry> disc_entry;
    Word access{}, share{};
    std::uint64_t offset{};
    bool directory{};
    bool raw{};
    bool modified{};
    std::vector<DirectoryItem> listing{};
    std::size_t listing_position{};
    bool listing_loaded{};
};
struct IoTrace {
    Word site, ordinal, status, detail, information;
    std::string path;
    std::optional<std::uint64_t> file_offset;
};

constexpr Word align_up(Word value, Word alignment) { return (value + alignment - 1) & ~(alignment - 1); }

class OfflineBoot {
public:
    OfflineBoot(Xbe& image, Disc* disc, std::uint64_t budget, Word breakpoint, bool audio_output,
                std::filesystem::path capture, Word capture_index, InputScript input,
                PlayWindow* window = nullptr, bool vsync = true, Word frames = 0, std::filesystem::path storage = {},
                const DiagnosticOptions* diagnostics = nullptr, Word language = 1)
        : image_(image), disc_(disc), ram_(ram_end - guest_base), kernel_(0x10000), memory_(guest_base, ram_),
          arena_start_(align_up(image.image_end, 0x10000)), stack_bottom_(ram_end),
          audio_(memory_, platform_allocator(), audio_output),
          graphics_(memory_, platform_allocator(), std::move(capture), capture_index, window ? window->native_handle() : nullptr, vsync),
          window_(window), frame_limit_(frames), input_(std::move(input)), storage_(std::move(storage)), language_(language) {
        if (compiled_batch.empty() || !std::ranges::all_of(compiled_batch, [&](const auto& function) {
                return function.source_sha256 == image.sha256;
            })) {
            throw std::runtime_error("Compiled functions do not match the verified XBE");
        }
        image.load_preloaded(guest_base, ram_);
        memory_.map(kernel_base, kernel_);
        memory_.guard_primary_pages(blocked_pages_);
        memory_.map(0x80020000, std::span(ram_).subspan(0x10000), 0x10000);
        // The SDK returns this write-combined alias when the movie decoder locks a surface.
        memory_.map(0xF0000000 + guest_base, ram_, 0);
        memory_.map_io(audio_register_page, page, nullptr,
            [](void*, Word offset, unsigned width) -> Word { throw DeviceStop{audio_register_page + offset, width, false}; },
            [](void*, Word offset, unsigned width, Word) { throw DeviceStop{audio_register_page + offset, width, true}; });
        diagnostic_.remaining = budget;
        diagnostic_.watch_address = game_main;
        diagnostic_.break_address = breakpoint;
        services_.context = this;
        services_.entries = dispatch_table(std::make_index_sequence<512>{});
        services_.platform = [](void* context, Cpu& cpu, Memory&, Word address) {
            auto& boot = *static_cast<OfflineBoot*>(context);
            const auto api = static_cast<PlatformApi>(address);
            boot.graphics_.advance_display(boot.interrupt_time());
            const auto handled = boot.input_api(api, cpu) || boot.graphics_.invoke(api, cpu);
            const auto ticks = boot.interrupt_time() / 100;
            if (boot.graphics_.captured()) boot.audio_.mark_capture(ticks);
            return handled || boot.audio_.invoke(api, cpu, ticks);
        };
        services_.resolve_module = [](void* context, Cpu& cpu, Word caller) {
            return static_cast<OfflineBoot*>(context)->resolve_module(cpu, caller);
        };
        directories_.emplace(disk);
        scratch_files_.emplace(std::string(disk_prefix) + '0', std::vector<std::byte>(0xA00));
        for (unsigned i = 0; i < cache_partitions; ++i) {
            scratch_files_.try_emplace(std::string(disk_prefix) + char('3' + i));
        }
        if (!storage_.empty()) open_storage();
        initialize_exports();
        if (window && diagnostics) {
            diagnostics_ = std::make_unique<PlayDiagnostics>(*diagnostics, *window, vsync,
                [this](const std::filesystem::path& dir) { capture_state(dir); },
                [this](FrameCounters& counters) {
                    counters.checkpoints = checkpoints();
                    counters.audio_frames = audio_.frames();
                    // The loop at 0x00018D25 increments this counter at 0x00018EC9.
                    constexpr Word engine_slot = 0x01EED350, updates_offset = 0x298;
                    const auto engine = memory_.load<Word>(engine_slot);
                    if (engine >= guest_base && engine <= ram_end - updates_offset - sizeof(Word)) {
                        counters.engine = engine;
                        std::memcpy(&counters.engine_updates, ram_.data() + engine - guest_base + updates_offset,
                                    sizeof(counters.engine_updates));
                    }
                });
            graphics_.diagnostics(diagnostics_.get());
        }
    }

    int run(std::ostream& output = std::cout) {
        const bool converted = !IsThreadAFiber();
        scheduler_ = converted ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
        if (!scheduler_) throw std::runtime_error("Cannot start the native guest scheduler");
        struct SchedulerScope {
            OfflineBoot& boot;
            bool converted;
            ~SchedulerScope() {
                boot.stop_threads();
                if (converted) ConvertFiberToThread();
            }
        } scheduler_scope{*this, converted};
        if (window_) {
            FILETIME now;
            GetSystemTimePreciseAsFileTime(&now);
            epoch_ = file_time(now);
            window_->begin();
        }
        auto& boot_thread = create(image_.entry, 0, 0, image_.stack_commit, 0, false);
        last_cpu_ = &boot_thread.cpu;
        std::string status = "returned", reason;
        try {
            schedule();
            if (quit_) status = "closed";
            else if (frame_limit_ && graphics_.scanouts() >= frame_limit_) status = "frame_limit";
        } catch (const KernelStop& stop) {
            failure_ordinal_ = stop.ordinal;
            stop_address_ = stop.address;
            reason = stop.reason;
            status = "kernel_boundary";
        } catch (const NativeBoundary& stop) {
            stop_address_ = stop.address;
            reason = stop.what();
            const std::string_view message = stop.what();
            status = message.ends_with("execution diagnostic budget exhausted") ? "budget" :
                     message.ends_with("execution diagnostic breakpoint reached") ? "breakpoint" : "native_boundary";
        } catch (const DeviceStop& stop) {
            device_stop_ = stop;
            status = "device_boundary";
            reason = "Xbox audio processing unit register access requires a device backend";
        } catch (const std::exception& stop) {
            reason = stop.what();
            status = "guest_error";
        }
        try {
            if (reason.empty() && !storage_writes_open()) flush_storage();
        } catch (const std::exception& stop) {
            if (!reason.empty()) reason += ". ";
            reason += stop.what();
            status = "guest_error";
        }
        const bool unfinished = diagnostics_ && !diagnostics_->finish();
        if (unfinished) {
            if (!reason.empty()) reason += ". ";
            reason += "A requested diagnostic capture did not complete";
        }
        if (graphics_.capture_pending()) {
            if (!reason.empty()) reason += ". ";
            reason += "The requested frame was never captured";
        }
        report(output, status, reason, boot_thread);
        if (unfinished || graphics_.capture_pending()) return 3;
        return status == "returned" || status == "breakpoint" || status == "closed" || status == "frame_limit" ? 0 : status == "budget" ? 4 : 3;
    }

private:
    Xbe& image_;
    Disc* disc_;
    std::vector<std::byte> ram_, kernel_;
    Memory memory_;
    Word arena_start_, stack_bottom_;
    std::map<Word, Word> allocations_;
    std::set<Word> virtual_allocations_;
    std::array<std::uint8_t, (ram_end - guest_base) / page> blocked_pages_{};
    std::deque<Thread> threads_;
    std::uint64_t completed_quanta_ = 0;
    std::map<Word,Word> free_stacks_;
    Word next_thread_id_ = 4;
    std::map<Word, Thread*> handles_;
    std::map<Word, std::shared_ptr<Event>> events_;
    std::map<Word, std::string> link_handles_;
    std::map<Word, OpenFile> files_;
    std::set<std::string> directories_;
    std::map<std::string, std::vector<std::byte>> scratch_files_;
    std::map<std::string, Word> file_reservations_;
    std::map<std::string, FileTimes> file_times_;
    std::set<std::string> pending_deletions_;
    std::map<std::string, std::string> links_;
    std::map<Word, Timer> timers_;
    std::deque<Word> dpcs_;
    std::vector<ModuleInstance> modules_;
    std::vector<Word> shutdown_registrations_;
    KernelServices services_;
    GuestClock clock_{this, [](void* context) {
        auto& boot = *static_cast<OfflineBoot*>(context);
        return boot.interrupt_time() / 10000000 * 733333333ULL +
               boot.interrupt_time() % 10000000 * 733333333ULL / 10000000;
    }};
    ExecutionDiagnostic diagnostic_;
    Thread* current_ = nullptr;
    Cpu* last_cpu_ = nullptr;
    Cpu callback_cpu_;
    void* scheduler_ = nullptr;
    std::exception_ptr thread_failure_;
    std::size_t next_thread_{};
    bool stopping_{};
    std::array<std::uint64_t, 512> service_calls_{};
    std::optional<unsigned> failure_ordinal_;
    std::optional<Word> stop_address_;
    std::optional<DeviceStop> device_stop_;
    std::string last_io_path_;
    std::deque<IoTrace> io_;
    Word last_service_site_ = 0, last_service_return_ = 0, next_handle_ = 4;
    GuestAudio audio_;
    GuestGraphics graphics_;
    std::unique_ptr<PlayDiagnostics> diagnostics_;
    PlayWindow* window_;
    Word frame_limit_;
    bool quit_{};
    InputScript input_;
    std::set<Word> input_handles_;
    Word input_packet_{}, start_presses_{}, confirm_presses_{}, b_presses_{};
    GamepadState last_input_;
    Word flicker_filter_{};
    bool luma_filter_{};
    std::uint64_t idle_time_{};
    std::uint64_t epoch_ = 133801632000000000ULL;
    std::filesystem::path storage_;
    Word language_;
    std::unique_ptr<void, decltype(&CloseHandle)> storage_lock_{nullptr, CloseHandle};
    std::array<std::byte,16> storage_key_{};
    Word storage_volume_serial_{};
    bool storage_dirty_{};

    GuestAllocator platform_allocator() {
        return {this, [](void* context, Word size) {
            auto& boot = *static_cast<OfflineBoot*>(context);
            return size && size <= ram_end ? boot.reserve(0, align_up(size, page)) : 0;
        }, [](void* context, Word address) {
            static_cast<OfflineBoot*>(context)->allocations_.erase(address);
        }};
    }

    std::uint64_t interrupt_time() const { return window_ ? window_->time() : idle_time_ + diagnostic_.visits * 100; }
    std::uint64_t checkpoints() const {
        auto count = diagnostic_.visits + completed_quanta_;
        for (const auto& thread : threads_)
            if (!thread.cpu.diagnostic && thread.stack_bytes) count += 1024 - thread.cpu.quantum_remaining;
        return count;
    }
    std::uint64_t system_time() const { return epoch_ + interrupt_time(); }

    template<std::size_t Ordinal>
    static void dispatch(void* context, Cpu& cpu, Memory&) {
        auto& boot = *static_cast<OfflineBoot*>(context);
        ++boot.service_calls_[Ordinal];
        boot.last_service_site_ = cpu.eip;
        boot.last_service_return_ = boot.memory_.load<Word>(cpu.registers[esp]);
        boot.update_clock();
        boot.service(Ordinal, cpu);
    }

    template<std::size_t... Ordinal>
    static auto dispatch_table(std::index_sequence<Ordinal...>) -> decltype(KernelServices::entries) {
        return {dispatch<Ordinal>...};
    }

    void initialize_exports() {
        // The CRT probes a PE header at the Xbox kernel base. This module only exports native data.
        memory_.store<std::uint16_t>(kernel_base, 0x5A4D);
        memory_.store<Word>(kernel_base + 0x3C, 0x100);
        memory_.store<Word>(kernel_base + 0x100, 0x4550);
        memory_.store<std::uint16_t>(kernel_base + 0x104, 0x14C);
        memory_.store<std::uint16_t>(kernel_base + 0x106, 1);
        memory_.store<std::uint16_t>(kernel_base + 0x114, 0xE0);
        memory_.store<std::uint16_t>(kernel_base + 0x118, 0x10B);
        memory_.store<Word>(kernel_base + 0x134, kernel_base);
        memory_.store<Word>(kernel_base + 0x138, page);
        memory_.store<Word>(kernel_base + 0x150, static_cast<Word>(kernel_.size()));
        std::memcpy(memory_.access(kernel_base + 0x1F8, 5), ".data", 5);
        memory_.store<std::uint64_t>(gdt + 8, 0x00CF9B000000FFFF);
        memory_.store<std::uint64_t>(gdt + 16, 0x00CF93000000FFFF);
        memory_.store<Word>(shutdown_list, shutdown_list);
        memory_.store<Word>(shutdown_list + 4, shutdown_list);
        for (const auto& import : image_.imports()) {
            Word address = function_base + import.ordinal * 16;
            switch (import.ordinal) {
            case 16: case 40: case 71: case 156: case 164: case 322: case 323: case 324: case 326: case 356:
                address = kernel_base + 2 * page + import.ordinal * 64;
                break;
            }
            memory_.store(import.address, address);
            if (import.ordinal == 323) {
                std::memcpy(memory_.access(address,storage_key_.size()),storage_key_.data(),storage_key_.size());
            } else if (import.ordinal == 324) {
                memory_.store<std::uint16_t>(address, 1);
                memory_.store<std::uint16_t>(address + 4, 5838);
            } else if (import.ordinal == 326) {
                constexpr char name[] = "\\Device\\CdRom0\\default.xbe";
                memory_.store<std::uint16_t>(address, sizeof(name) - 1);
                memory_.store<std::uint16_t>(address + 2, sizeof(name));
                memory_.store<Word>(address + 4, address + 8);
                std::memcpy(memory_.access(address + 8, sizeof(name)), name, sizeof(name));
            } else if (import.ordinal == 356) memory_.store<Word>(address, 6);
            else if (import.ordinal == 40) memory_.store(address, cache_partitions);
        }
    }

    Word argument(const Cpu& cpu, unsigned index) const {
        return memory_.load<Word>(cpu.registers[esp] + 4 + 4 * index);
    }

    void finish(Cpu& cpu, unsigned arguments, std::optional<Word> result = {}) {
        cpu.eip = memory_.load<Word>(cpu.registers[esp]);
        cpu.registers[esp] += 4 * (1 + arguments);
        if (result) cpu.registers[eax] = *result;
    }

    Thread& create(Word routine, Word context, Word system, Word stack_size, Word tls, bool suspended) {
        auto slot = std::ranges::find_if(threads_, [](const Thread& t){return t.exited && !t.stack_bytes;});
        if ((slot == threads_.end() && threads_.size() >= 64) || stack_size > ram_end - arena_start_ || tls >= ram_end - arena_start_) {
            throw std::runtime_error("Offline guest thread memory limit");
        }
        const auto size = align_up(stack_size ? stack_size : image_.stack_commit, page);
        if (size < 32 || tls > size - 32) throw std::runtime_error("Invalid guest stack or TLS size");
        const auto needed = size + page;
        const auto hole = std::ranges::find_if(free_stacks_,[&](const auto& h){return h.second >= needed;});
        Word base;
        if (hole != free_stacks_.end()) {
            base = hole->first;
            auto remaining = hole->second - needed;
            free_stacks_.erase(hole);
            if (remaining) free_stacks_.emplace(base + needed,remaining);
        } else {
            if (needed > stack_bottom_ - arena_start_ || (!allocations_.empty() &&
                allocations_.rbegin()->first + allocations_.rbegin()->second > stack_bottom_ - needed))
                throw std::runtime_error("Insufficient guest stack memory");
            base = stack_bottom_ -= needed;
        }
        std::memset(memory_.access(base,needed),0,needed);
        const auto stack_top = base + needed;
        const auto object = base | 0x80000000U;
        const auto tib = object + 0x400;
        Thread& thread = slot == threads_.end() ? threads_.emplace_back() : *slot;
        thread = Thread{};
        thread.stack_bytes = needed;
        thread.boot = this;
        thread.id = next_thread_id_;
        next_thread_id_ += 4;
        thread.routine = routine;
        thread.system = system;
        thread.suspended = suspended;
        thread.object = object;
        memory_.store<std::uint8_t>(object, 6);
        memory_.store<Word>(object + 0x1C, stack_top);
        memory_.store<Word>(object + 0x20, base + page);
        memory_.store<Word>(object + 0x28, tls ? stack_top - tls : 0);
        memory_.store<Word>(object + 0x12C, thread.id);
        memory_.store<Word>(tib, UINT32_MAX);
        memory_.store<Word>(tib + 4, stack_top);
        memory_.store<Word>(tib + 8, base + page);
        memory_.store<Word>(tib + 0x18, tib);
        memory_.store<Word>(tib + 0x20, object);
        memory_.store<Word>(tib + 0x28, object);
        auto& cpu = thread.cpu;
        cpu.kernel = &services_;
        cpu.clock = &clock_;
        cpu.diagnostic = &diagnostic_;
        cpu.preempt = [](Cpu& cpu) {
            auto& boot = *static_cast<OfflineBoot*>(cpu.kernel->context);
            if (!cpu.diagnostic) boot.completed_quanta_ += 1024;
            if (boot.window_ && boot.diagnostic_.reached_watch) cpu.diagnostic = nullptr;
            if ((cpu.flags & 0x200) && boot.memory_.load<std::uint8_t>(cpu.fs_base + 0x24) <= 1) {
                HostFloatingScope host(cpu.floating);
                boot.yield(cpu);
            }
        };
        cpu.fs_base = tib;
        cpu.fs_selector = 0x30;
        cpu.cs_selector = 8;
        cpu.ds_selector = cpu.ss_selector = 16;
        cpu.gdt_base = gdt;
        cpu.gdt_limit = 63;
        auto& sp = cpu.registers[esp];
        sp = (stack_top - tls - 16) & ~15U;
        memory_.store(sp, return_address);
        memory_.store(sp + 4, system ? routine : context);
        if (system) memory_.store(sp + 8, context);
        thread.fiber = CreateFiberEx(0, 8 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, &thread);
        if (!thread.fiber) {
            thread.exited = true;
            retire_thread(thread);
            throw std::runtime_error("Cannot allocate a guest thread fiber");
        }
        return thread;
    }

    static void WINAPI fiber_entry(void* parameter) {
        auto& thread = *static_cast<Thread*>(parameter);
        thread.boot->run_thread(thread);
        SwitchToFiber(thread.boot->scheduler_);
        std::terminate();
    }

    void run_thread(Thread& thread) {
        thread.started = true;
        try {
            invoke_native(thread.cpu, memory_, thread.system ? thread.system : thread.routine);
            if (thread.cpu.eip != return_address) throw NativeBoundary(thread.cpu.eip, "Unexpected thread return");
            thread.exit_status = thread.cpu.registers[eax];
            thread.returned = true;
        } catch (const ThreadExit& exit) {
            thread.exit_status = exit.status;
        } catch (...) {
            thread_failure_ = std::current_exception();
        }
        thread.exited = true;
        memory_.store<Word>(thread.object + 4, 1);
    }

    void yield(Cpu& cpu) {
        if (memory_.load<std::uint8_t>(cpu.fs_base + 0x24) > 1)
            throw NativeBoundary(cpu.eip, "Guest DPC attempted a blocking wait");
        SwitchToFiber(scheduler_);
        if (stopping_) throw ThreadExit{0xC0000120};
    }

    void schedule();
    void retire_thread(Thread& thread) {
        if (!thread.exited) return;
        if (thread.fiber) DeleteFiber(std::exchange(thread.fiber,nullptr));
        if (&thread == &threads_.front() || !thread.stack_bytes || thread.waiters ||
            std::ranges::any_of(handles_,[&](const auto& h){return h.second == &thread;})) return;
        auto base = thread.object & 0x7FFFFFFFU;
        auto bytes = std::exchange(thread.stack_bytes,0U);
        if (!thread.cpu.diagnostic) completed_quanta_ += 1024 - thread.cpu.quantum_remaining;
        auto after = free_stacks_.lower_bound(base);
        if (after != free_stacks_.end() && base + bytes == after->first) {
            bytes += after->second;
            after = free_stacks_.erase(after);
        }
        if (after != free_stacks_.begin()) {
            auto before = std::prev(after);
            if (before->first + before->second == base) {
                base = before->first;
                bytes += before->second;
                free_stacks_.erase(before);
            }
        }
        if (base == stack_bottom_) stack_bottom_ += bytes;
        else free_stacks_.emplace(base,bytes);
    }
    void capture_state(const std::filesystem::path&) const;
    void stop_threads() {
        stopping_ = true;
        for (auto& thread : threads_) {
            if (!thread.fiber) continue;
            if (thread.started && !thread.exited) {
                current_ = &thread;
                SwitchToFiber(thread.fiber);
            }
            DeleteFiber(thread.fiber);
        }
    }

    Word reserve(Word requested, Word size, Word alignment = page, Word first = 0, Word last = UINT32_MAX) {
        Word base = requested ? requested : align_up(std::max(arena_start_, first), alignment);
        if (base < arena_start_ || base >= stack_bottom_ || size > stack_bottom_ - base) return 0;
        for (const auto& [start, length] : allocations_) {
            if (start >= base && size <= start - base) break;
            if (start + length <= base) continue;
            if (requested) return 0;
            base = align_up(start + length, alignment);
        }
        if (base >= stack_bottom_ || size > stack_bottom_ - base || base > last || size - 1 > last - base) return 0;
        allocations_.emplace(base, size);
        std::memset(memory_.access(base, size), 0, size);
        return base;
    }

    void set_page_access(Word base, Word size, bool blocked) {
        const auto offset = base - guest_base;
        if (blocked) std::memset(ram_.data() + offset, 0, size);
        std::fill_n(blocked_pages_.begin() + offset / page, size / page, static_cast<std::uint8_t>(blocked));
    }

    std::string counted(Word address) const {
        const auto size = memory_.load<std::uint16_t>(address);
        if (!size) return {};
        if (size > memory_.load<std::uint16_t>(address + 2)) throw std::runtime_error("Invalid counted string");
        const auto buffer = memory_.load<Word>(address + 4);
        return {static_cast<const char*>(memory_.access(buffer, size)), size};
    }

    static std::string fold(std::string text) {
        for (auto& ch : text) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        return text;
    }

    Word new_handle() { return std::exchange(next_handle_, next_handle_ + 4); }
    std::string object_name(Word attributes, bool resolve = true) const;
    Word open_file(Cpu& cpu, bool create);
    Word set_file_disposition(const OpenFile& file, bool remove);
    void io_status(Word address, Word status, Word information = 0) {
        memory_.store(address, status);
        memory_.store(address + 4, information);
    }
    void trace_io(const Cpu& cpu, Word ordinal, Word status, Word detail, Word information,
                  std::optional<std::uint64_t> file_offset = {}) {
        if (window_ && !(diagnostics_ && (diagnostics_->measuring() || diagnostics_->capturing()))) return;
        if (io_.size() == 64) io_.pop_front();
        io_.push_back({cpu.eip, ordinal, status, detail, information, last_io_path_, file_offset});
    }
    void service(unsigned ordinal, Cpu& cpu);
    bool input_api(PlatformApi api, Cpu& cpu);
    NativeFunction resolve_module(Cpu& cpu, Word caller);
    std::uint64_t due_time(std::uint64_t raw) const {
        return interrupt_time() + ((raw >> 63) ? 0 - raw : raw > system_time() ? raw - system_time() : 0);
    }
    template<class Signaled>
    Word wait(Cpu& cpu, Word timeout, Signaled signaled, std::uint64_t next_signal = UINT64_MAX) {
        const auto deadline = timeout ? due_time(memory_.load<std::uint64_t>(timeout)) : UINT64_MAX;
        while (!signaled()) {
            if (interrupt_time() >= deadline) return wait_timeout;
            current_->wake = signaled;
            current_->deadline = std::min(deadline, next_signal);
            yield(cpu);
            current_->wake = {};
            current_->deadline = UINT64_MAX;
        }
        return 0;
    }
    bool queue_dpc(Word address, Word argument1, Word argument2);
    void dispatch_dpcs(Thread& thread);
    void update_clock() {
        graphics_.advance_display(interrupt_time());
        memory_.store<Word>(kernel_base + 2 * page + 156 * 64, static_cast<Word>(interrupt_time() / 10000));
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.due > interrupt_time()) {
                ++it;
                continue;
            }
            if (it->second.dpc) queue_dpc(it->second.dpc, 0, 0);
            memory_.store<std::uint8_t>(it->first + 3, 0);
            memory_.store<Word>(it->first + 4, 1);
            it = timers_.erase(it);
        }
    }
    void report(std::ostream&, std::string_view status, std::string_view reason, const Thread& boot_thread) const;
    void open_storage();
    void flush_storage();
    bool storage_writes_open() const {
        return std::ranges::any_of(files_, [](const auto& entry) {
            return entry.second.modified && persistent_path(entry.second.path);
        });
    }
    Word allocated_size(const std::string& path, Word size) const {
        const auto found = file_reservations_.find(path);
        return std::max(align_up(size, disk_cluster), found == file_reservations_.end() ? 0U : found->second);
    }
    FileTimes file_times(const std::string& path) const {
        const auto found = file_times_.find(path);
        return found == file_times_.end() ? FileTimes{} : found->second;
    }
    void file_written(OpenFile& file) {
        auto& times = file_times_[file.path];
        times.written = times.changed = system_time();
        file.modified = true;
        storage_changed(file.path);
    }
    void storage_changed(std::string_view path) { storage_dirty_ |= !storage_.empty() && persistent_path(path); }
};

void OfflineBoot::open_storage() {
    storage_ = std::filesystem::absolute(storage_).lexically_normal();
    std::filesystem::create_directories(storage_);
    const auto lock = CreateFileW((storage_ / L"playtest.lock").c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                  0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock playtest storage: " + path_text(storage_));
    storage_lock_.reset(lock);
    const auto path = storage_ / "saves.bin";
    const auto key = storage_ / "storage.key";
    if (!std::filesystem::exists(key)) {
        if (std::filesystem::exists(path)) throw std::runtime_error("The playtest save archive requires its original storage.key");
        if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(storage_key_.data()),static_cast<ULONG>(storage_key_.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("Cannot create the playtest save identity");
        write_text(key,{reinterpret_cast<const char*>(storage_key_.data()),storage_key_.size()});
    } else {
        File identity(key);
        if (identity.size() != storage_key_.size()) throw std::runtime_error("Invalid storage.key in the playtest directory");
        identity.read(0,storage_key_);
    }
    Sha1 identity_hash;
    identity_hash.reset();
    identity_hash.update(storage_key_);
    storage_volume_serial_ = u32(identity_hash.finish(), 0);
    for (const auto folder : {"UDATA", "TDATA"}) {
        const auto parent = std::string(disk) + '\\' + folder;
        directories_.insert(parent);
        directories_.insert(parent + "\\5451000E"); // Title ID from the verified XBE certificate.
    }
    if (!std::filesystem::exists(path)) return;
    File file(path);
    if (file.size() < save_magic.size() + 4 || file.size() > cache_capacity) throw std::runtime_error("Invalid playtest save archive size");
    const auto header = file.read(0, save_magic.size() + 4);
    const std::string_view magic(reinterpret_cast<const char*>(header.data()), save_magic.size());
    const bool legacy = magic == legacy_save_magic;
    if (!legacy && magic != save_magic)
        throw std::runtime_error("Unknown playtest save archive format");
    FileTimes legacy_times;
    if (legacy) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
            throw std::runtime_error("Cannot read playtest save archive timestamps");
        const auto timestamp = file_time(attributes.ftLastWriteTime);
        legacy_times = {timestamp, timestamp, timestamp, timestamp};
    }
    const auto count = u32(header, save_magic.size());
    if (count > 4096) throw std::runtime_error("Playtest save archive contains too many entries");
    std::set<std::string> seen;
    std::uint64_t offset = header.size();
    for (Word i=0;i<count;++i) {
        const auto entry = file.read(offset,8);
        const auto length = u32(entry,0), size = u32(entry,4);
        offset += entry.size();
        if (!length || length > 1024 || (size != UINT32_MAX && size > file_limit)) throw std::runtime_error("Invalid playtest save entry");
        auto times = legacy_times;
        if (!legacy) {
            file.read(offset, std::as_writable_bytes(std::span(&times, 1)));
            offset += sizeof(times);
        }
        const auto bytes = file.read(offset,length);
        std::string name(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        offset += length;
        if (!persistent_path(name) || name.find('\0') != std::string::npos || !seen.insert(name).second)
            throw std::runtime_error("Invalid or duplicate playtest save path");
        file_times_.insert_or_assign(name, times);
        if (size == UINT32_MAX) directories_.insert(std::move(name));
        else {
            scratch_files_.insert_or_assign(std::move(name), file.read(offset,size));
            offset += size;
        }
    }
    if (offset != file.size()) throw std::runtime_error("Unexpected bytes in playtest save archive");
}

void OfflineBoot::flush_storage() {
    if (!storage_dirty_) return;
    const auto count = std::ranges::count_if(directories_,persistent_path) +
        std::ranges::count_if(scratch_files_,[](const auto& entry) { return persistent_path(entry.first); });
    if (count > 4096) throw std::runtime_error("Playtest saves exceed the entry limit");
    const auto pending = storage_ / std::format("saves-{}.tmp", GetCurrentProcessId());
    std::ofstream out(pending,std::ios::binary | std::ios::noreplace);
    if (!out) throw std::runtime_error("Cannot create a playtest save archive");
    const auto word = [&](Word value) { out.write(reinterpret_cast<const char*>(&value),sizeof(value)); };
    const auto entry = [&](const std::string& path, Word size) {
        if (path.size() > 1024) throw std::runtime_error("Playtest save path exceeds its limit");
        word(static_cast<Word>(path.size()));
        word(size);
        const auto times = file_times(path);
        out.write(reinterpret_cast<const char*>(&times), sizeof(times));
        out.write(path.data(),static_cast<std::streamsize>(path.size()));
    };
    try {
        out.write(save_magic.data(),save_magic.size());
        word(static_cast<Word>(count));
        for (const auto& path : directories_) if (persistent_path(path)) entry(path,UINT32_MAX);
        for (const auto& [path,data] : scratch_files_) if (persistent_path(path)) {
            entry(path,static_cast<Word>(data.size()));
            if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
        }
        const auto size = out.tellp();
        out.close();
        if (!out || size < 0 || size > cache_capacity ||
            !MoveFileExW(pending.c_str(),(storage_ / L"saves.bin").c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot commit playtest saves: " + path_text(storage_));
        }
    } catch (...) {
        if (out.is_open()) out.close();
        std::error_code error;
        std::filesystem::remove(pending,error);
        throw;
    }
    storage_dirty_ = false;
}

void OfflineBoot::schedule() {
    for (;;) {
        if (window_ && !window_->poll()) {quit_ = true; return;}
        if (diagnostics_) diagnostics_->poll();
        if (frame_limit_ && graphics_.scanouts() >= frame_limit_) return;
        update_clock();
        if (!dpcs_.empty()) {
            const auto thread = std::ranges::find_if(threads_, [](const Thread& thread) {
                return thread.started && !thread.exited && !thread.suspended && (thread.cpu.flags & 0x200);
            });
            if (thread != threads_.end()) dispatch_dpcs(*thread);
        }
        Thread* selected = nullptr;
        bool unfinished = false;
        auto next_due = UINT64_MAX;
        const auto now = interrupt_time();
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            const auto index = (next_thread_ + i) % threads_.size();
            auto& thread = threads_[index];
            if (thread.exited) continue;
            unfinished = true;
            if (thread.suspended) continue;
            if (thread.wake && thread.deadline > now && !thread.wake()) {
                next_due = std::min(next_due, thread.deadline);
                continue;
            }
            selected = &thread;
            next_thread_ = (index + 1) % threads_.size();
            break;
        }
        if (!unfinished) return;
        if (selected) {
            current_ = selected;
            last_cpu_ = &selected->cpu;
            SwitchToFiber(selected->fiber);
            if (selected->exited) retire_thread(*selected);
            if (thread_failure_) std::rethrow_exception(thread_failure_);
            continue;
        }
        for (const auto& [address, timer] : timers_) next_due = std::min(next_due, timer.due);
        if (next_due == UINT64_MAX)
            throw NativeBoundary(last_cpu_->eip, "All guest threads are waiting without a signal source");
        if (window_) Sleep(1);
        else idle_time_ += next_due - now;
    }
}

bool OfflineBoot::queue_dpc(Word address, Word argument1, Word argument2) {
    if (memory_.load<std::uint8_t>(address + 2)) return false;
    if (dpcs_.size() >= 1024) throw std::runtime_error("Guest DPC queue is full");
    memory_.store(address + 20, argument1);
    memory_.store(address + 24, argument2);
    memory_.store<std::uint8_t>(address + 2, 1);
    dpcs_.push_back(address);
    return true;
}

void OfflineBoot::dispatch_dpcs(Thread& thread) {
    current_ = &thread;
    while (!dpcs_.empty()) {
        const auto address = dpcs_.front();
        dpcs_.pop_front();
        memory_.store<std::uint8_t>(address + 2, 0);
        callback_cpu_ = thread.cpu;
        auto& cpu = callback_cpu_;
        cpu.preempt = nullptr;
        cpu.floating.active = false;
        cpu.floating.host_context = nullptr;
        last_cpu_ = &cpu;
        const auto irql = cpu.fs_base + 0x24;
        const auto prior = memory_.load<std::uint8_t>(irql);
        const auto stack = cpu.registers[esp] - 64;
        cpu.registers[esp] = stack;
        memory_.store(stack, return_address);
        memory_.store(stack + 4, address);
        for (Word i = 0; i < 3; ++i) memory_.store(stack + 8 + i * 4, memory_.load<Word>(address + 16 + i * 4));
        memory_.store<std::uint8_t>(irql, 2);
        try {
            invoke_native(cpu, memory_, memory_.load<Word>(address + 12));
            if (cpu.eip != return_address || cpu.registers[esp] != stack + 20)
                throw NativeBoundary(cpu.eip, "Guest DPC returned with an unexpected stack");
        } catch (...) {
            memory_.store(irql, prior);
            throw;
        }
        memory_.store(irql, prior);
        last_cpu_ = &thread.cpu;
    }
}

bool OfflineBoot::input_api(PlatformApi api, Cpu& cpu) {
    const auto scripted = [](const auto& buttons) {
        return std::ranges::any_of(buttons, [](const auto& pulses) { return !pulses.empty(); });
    };
    const bool connected = window_ || !input_.start.empty() || scripted(input_.face) ||
        scripted(input_.dpad) || !input_.stick.empty();
    const auto arg = [&](unsigned i) { return argument(cpu, i); };
    const auto done = [&](unsigned count, std::optional<Word> result = {}) {
        finish(cpu, count, result);
        return true;
    };
    if (api == PlatformApi::InputInitialize) {
        const auto count = arg(0), descriptors = arg(1);
        if (count > 32 || (count && !descriptors))
            throw NativeBoundary(cpu.eip, "Invalid Xbox input preallocation table");
        for (Word i = 0; i < count; ++i) {
            const auto type = memory_.load<Word>(descriptors + 8 * i);
            memory_.fill_elements<Word>(type, 0, 3, false);
        }
        if (connected) {
            memory_.store<Word>(gamepad_type, 1);
            memory_.store<Word>(gamepad_type + 4, 1);
        }
        return done(2);
    }
    if (!connected) return false;
    if (api == PlatformApi::InputOpen) {
        if (arg(0) != gamepad_type || arg(1) || arg(2)) return done(4, 0);
        const auto parameters = arg(3);
        if (parameters && !(memory_.load<std::uint8_t>(parameters) & 1))
            throw NativeBoundary(cpu.eip, "Native controller requires automatic polling");
        const auto handle = new_handle();
        input_handles_.insert(handle);
        return done(4, handle);
    }
    if (api == PlatformApi::InputClose) {
        input_handles_.erase(arg(0));
        return done(1);
    }
    if (api == PlatformApi::InputFeedback) {
        const auto feedback = arg(1);
        const auto event = memory_.load<Word>(feedback + 4);
        const auto found = events_.find(event);
        Word status = !input_handles_.contains(arg(0)) ? 1167 : event && found == events_.end() ? 6 : 0;
        if (!status && window_) status = window_->rumble(memory_.load<std::uint16_t>(feedback + 66),
                                                         memory_.load<std::uint16_t>(feedback + 68));
        memory_.store(feedback, status);
        if (event && found != events_.end()) found->second->signaled = true;
        return done(2, status);
    }
    if (api != PlatformApi::InputState) return false;
    if (!input_handles_.contains(arg(0))) return done(2, 1167); // ERROR_DEVICE_NOT_CONNECTED
    const auto scanout = graphics_.scanouts();
    const auto pulse = [&](const auto& inputs) {
        return std::ranges::any_of(inputs, [&](Word first) { return scanout >= first && scanout - first < 4; });
    };
    auto state = window_ ? window_->input() : GamepadState{};
    if (pulse(input_.start)) state.buttons |= 0x10;
    for (unsigned button = 0; button < input_.face.size(); ++button) {
        if (pulse(input_.face[button])) state.pressure[button] = 255;
    }
    for (unsigned direction = 0; direction < input_.dpad.size(); ++direction) {
        if (pulse(input_.dpad[direction])) state.buttons |= 1U << direction;
    }
    for (const auto& stick : input_.stick) {
        if (scanout < stick.first || scanout - stick.first >= stick.frames) continue;
        state.axes[0] = stick.x;
        state.axes[1] = stick.y;
    }
    if (state != last_input_) {
        ++input_packet_;
        if ((state.buttons & 0x10) && !(last_input_.buttons & 0x10)) ++start_presses_;
        if (state.pressure[0] && !last_input_.pressure[0]) ++confirm_presses_;
        if (state.pressure[1] && !last_input_.pressure[1]) ++b_presses_;
        last_input_ = state;
    }
    const auto output = arg(1);
    memory_.store(output, input_packet_);
    memory_.store(output + 4, state.buttons);
    for (unsigned i=0;i<state.pressure.size();++i) memory_.store(output + 6 + i, state.pressure[i]);
    for (unsigned i=0;i<state.axes.size();++i) memory_.store(output + 14 + i * 2, std::bit_cast<std::uint16_t>(state.axes[i]));
    return done(2, 0);
}

NativeFunction OfflineBoot::resolve_module(Cpu& cpu, Word caller) {
    // EF_LOGIC_CODE stores the loaded text base at +0x264 and clears it on release.
    constexpr Word text_field = 0x264;
    std::erase_if(modules_, [&](const ModuleInstance& instance) {
        return memory_.load<Word>(instance.owner + text_field) != instance.base;
    });
    const auto address = cpu.eip;
    const auto owner = cpu.registers[ecx];
    const auto active = [&](const ModuleInstance& instance) -> NativeFunction {
        const auto& [module, base, object] = instance;
        if (address < base || address - base >= module->code_hex.size() / 2) return nullptr;
        const auto offset = address - base;
        const auto found = std::ranges::lower_bound(module->functions, offset, {}, &CompiledFunction::address);
        return found != module->functions.end() && found->address == offset ? found->run : nullptr;
    };
    if (caller != 0x0003A6B5 && caller != 0x0003D42F) {
        for (const auto& module : modules_) if (const auto function = active(module)) return function;
        return nullptr;
    }
    std::string mismatch;
    std::size_t matched_bytes = 0;
    for (const auto& module : compiled_modules) {
        if (caller != module.registration_call || address < module.registration || owner < guest_base ||
            owner >= ram_end - text_field - 4) continue;
        const auto base = address - module.registration;
        const auto size = module.code_hex.size() / 2;
        if (base < guest_base || base > ram_end || size > ram_end - base ||
            memory_.load<Word>(owner + text_field) != base) continue;
        const auto* loaded = static_cast<const std::byte*>(memory_.access(base, size));
        const auto nibble = [](char digit) { return digit <= '9' ? digit - '0' : digit - 'A' + 10; };
        auto relocation = module.relocations.begin();
        bool identical = true;
        for (std::size_t offset = 0; offset < size && identical;) {
            if (relocation != module.relocations.end() && relocation->offset == offset) {
                const auto expected = base + relocation->value;
                const auto actual = memory_.load<Word>(base + static_cast<Word>(offset));
                identical = actual == expected;
                if (!identical && (mismatch.empty() || offset > matched_bytes)) {
                    matched_bytes = offset;
                    mismatch = std::format("Loaded {} relocation at {} differs. Expected {}, found {}",
                        module.name, hex32(static_cast<Word>(offset)), hex32(expected), hex32(actual));
                }
                ++relocation;
                offset += 4;
            } else {
                const auto expected = 16 * nibble(module.code_hex[2 * offset]) + nibble(module.code_hex[2 * offset + 1]);
                identical = std::to_integer<int>(loaded[offset]) == expected;
                if (!identical && (mismatch.empty() || offset > matched_bytes)) {
                    matched_bytes = offset;
                    mismatch = std::format("Loaded {} instruction byte at {} differs. Expected {:02X}, found {:02X}",
                        module.name, hex32(static_cast<Word>(offset)), expected, std::to_integer<unsigned>(loaded[offset]));
                }
                ++offset;
            }
        }
        if (!identical) continue;
        std::erase_if(modules_, [&](const ModuleInstance& instance) { return instance.owner == owner || instance.base == base; });
        modules_.push_back({&module, base, owner});
        return active(modules_.back());
    }
    if (!mismatch.empty()) throw NativeBoundary(address, std::move(mismatch));
    return nullptr;
}

std::string OfflineBoot::object_name(Word attributes, bool resolve) const {
    const auto name = memory_.load<Word>(attributes + 4);
    if (!name) return {};
    auto path = counted(name);
    const auto root = memory_.load<Word>(attributes);
    if (!path.starts_with('\\')) {
        if (const auto found = files_.find(root); found != files_.end() && found->second.directory) {
            path = found->second.path + '\\' + path;
        } else if (root == 0xFFFFFFFD || (root == 0 && path.size() > 1 && path[1] == ':')) {
            path = "\\??\\" + path;
        }
    }
    std::ranges::replace(path, '/', '\\');
    path = fold(std::move(path));
    for (unsigned depth = 0; depth < 16; ++depth) {
        path.erase(std::unique(path.begin(), path.end(), [](char a, char b) { return a == '\\' && b == '\\'; }), path.end());
        if (path.ends_with('\\')) path.pop_back();
        if (!resolve) return path;
        const auto link = std::ranges::find_if(links_, [&](const auto& entry) {
            const auto& key = entry.first;
            return path == key || (path.starts_with(key) && path[key.size()] == '\\');
        });
        if (link == links_.end()) return path;
        path = fold(link->second) + path.substr(link->first.size());
    }
    throw std::runtime_error("Guest symbolic links form a cycle");
}

Word OfflineBoot::open_file(Cpu& cpu, bool create) {
    const auto arg = [&](unsigned index) { return argument(cpu, index); };
    const auto path = object_name(arg(2));
    last_io_path_ = path;
    const auto access = arg(1), share = arg(create ? 6 : 4), options = arg(create ? 8 : 5), disposition = create ? arg(7) : 1;
    if (path.empty() || disposition > 5 || (options & 0x41) == 0x41) return bad_parameter;
    if (pending_deletions_.contains(path)) return delete_pending;
    for (const auto& [handle, open] : files_) {
        if (open.path == path && ((share_required(access) & ~open.share) || (share_required(open.access) & ~share)))
            return 0xC0000043; // STATUS_SHARING_VIOLATION
    }
    OpenFile file{path, {}, access, share};
    Word information = 1;
    if (path == cdrom || (path.starts_with(cdrom) && path[cdrom.size()] == '\\')) {
        if (!disc_) return no_disc;
        if ((access & 0x50010116U) || (disposition != 1 && disposition != 3)) return write_protected;
        const auto relative = path == cdrom ? std::string_view{} : std::string_view(path).substr(cdrom.size() + 1);
        file.disc_entry = disc_->lookup(relative);
        if (!file.disc_entry) return name_missing;
        file.directory = file.disc_entry->directory;
    } else if (path.starts_with(disk_prefix) && path.size() == disk_prefix.size() + 1 &&
               !path.ends_with('1') && scratch_files_.contains(path)) {
        file.raw = true;
        if (options & 1) {
            const auto& data = scratch_files_.at(path);
            if (data.size() < 16 || u32(data, 0) != 0x58544146U) return 0xC000014F;
            file.directory = true;
        }
    } else if (path == disk || (path.starts_with(disk) && path[disk.size()] == '\\')) {
        if (directories_.contains(path) || scratch_files_.contains(path)) {
            if (disposition == 2) return name_conflict;
            if (disposition == 0 || disposition == 4 || disposition == 5) {
                if (directories_.contains(path)) return 0xC00000BA;
                if (options & 1) return 0xC0000103;
                if (!(access & (disposition == 0 ? delete_access : write_data))) return access_denied;
                scratch_files_.at(path).clear();
                file_written(file);
                information = disposition == 0 ? 0 : 3;
            }
        } else {
            if (disposition == 1 || disposition == 4) return name_missing;
            const auto parent = path.substr(0, path.rfind('\\'));
            if (pending_deletions_.contains(parent)) return delete_pending;
            if (!directories_.contains(parent)) return 0xC000003A;
            if (options & 1) directories_.insert(path);
            else scratch_files_.try_emplace(path);
            const auto now = system_time();
            file_times_.insert_or_assign(path, FileTimes{now, now, now, now});
            storage_changed(path);
            file.modified = true;
            information = 2;
        }
        file.directory = directories_.contains(path);
    } else if (path.starts_with(disk_prefix) && path.size() > disk_prefix.size() + 1 &&
               path[disk_prefix.size() + 1] == '\\' &&
               scratch_files_.contains(path.substr(0, disk_prefix.size() + 1))) {
        return name_missing;
    } else return name_missing;
    if (file.directory && (options & 0x40)) return 0xC00000BA;
    if (!file.directory && (options & 1)) return 0xC0000103;
    const auto handle = new_handle();
    files_.emplace(handle, std::move(file));
    memory_.store(arg(0), handle);
    io_status(arg(3), 0, information);
    return 0;
}

Word OfflineBoot::set_file_disposition(const OpenFile& file, bool remove) {
    if (file.disc_entry) return write_protected;
    if (file.raw || file.path == disk || !(file.access & delete_access)) return access_denied;
    if (!remove) {
        pending_deletions_.erase(file.path);
        return 0;
    }
    if (file.directory) {
        const auto prefix = file.path + '\\';
        const auto folder = directories_.lower_bound(prefix);
        const auto child = scratch_files_.lower_bound(prefix);
        if ((folder != directories_.end() && folder->starts_with(prefix)) ||
            (child != scratch_files_.end() && child->first.starts_with(prefix))) {
            return 0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
        }
    }
    if (std::ranges::any_of(files_, [&](const auto& entry) {
        const auto& other = entry.second;
        return &other != &file && other.path == file.path && !(other.share & share_delete);
    })) return 0xC0000043; // STATUS_SHARING_VIOLATION
    pending_deletions_.insert(file.path);
    return 0;
}

void OfflineBoot::service(unsigned ordinal, Cpu& cpu) {
    const auto arg = [&](unsigned index) { return argument(cpu, index); };
    switch (ordinal) {
    case 2: {
        const auto option = arg(1), parameter = arg(2);
        if (option == 6) memory_.store<Word>(arg(3), 0x00400101); // Standard NTSC-M at 60 Hz.
        else if (option == 11 && parameter <= 5) flicker_filter_ = parameter;
        else if (option == 14) luma_filter_ = parameter != 0;
        else break;
        return finish(cpu, 4);
    }
    case 129: case 160: case 161: {
        const auto address = cpu.fs_base + 0x24;
        const auto previous = memory_.load<std::uint8_t>(address);
        const auto level = ordinal == 129 ? 2 : static_cast<std::uint8_t>(cpu.registers[ecx]);
        if (level > 31 || (ordinal == 161 ? level > previous : level < previous)) break;
        memory_.store<std::uint8_t>(address, level);
        if (ordinal != 161) cpu.registers[eax] = (cpu.registers[eax] & 0xFFFFFF00U) | previous;
        return finish(cpu, 0);
    }
    case 23: case 180: {
        const auto allocation = allocations_.find(arg(0) & 0x7FFFFFFFU);
        return finish(cpu, 1, allocation == allocations_.end() ? 0 : allocation->second);
    }
    case 165: case 166: case 15: {
        const auto size = arg(0);
        const auto alignment = ordinal == 166 && arg(3) ? arg(3) : page;
        if (!size || size > ram_end || alignment > ram_end || !std::has_single_bit(alignment)) {
            return finish(cpu, ordinal == 166 ? 5 : ordinal == 15 ? 2 : 1, 0);
        }
        const auto rounded = align_up(size, page);
        const auto low = ordinal == 166 ? arg(1) : 0U;
        const auto high = ordinal == 166 ? arg(2) : UINT32_MAX;
        if (low >= stack_bottom_) return finish(cpu, 5, 0);
        const auto first = align_up(std::max(low, arena_start_), std::max(alignment, page));
        const auto base = reserve(0, rounded, std::max(alignment, page), first, high);
        return finish(cpu, ordinal == 166 ? 5 : ordinal == 15 ? 2 : 1, base ? base | 0x80000000U : 0);
    }
    case 171: case 17:
        if (!allocations_.erase(arg(0) & 0x7FFFFFFFU)) break;
        return finish(cpu, 1);
    case 327: case 328: {
        const auto pointer = arg(0);
        const auto sections = image_.sections();
        const auto section = std::ranges::find(sections, pointer, &Section::header);
        if (section == sections.end()) return finish(cpu, 1, bad_parameter);
        const auto count = memory_.load<Word>(pointer + 24);
        const bool load = ordinal == 327;
        if (load ? count == UINT32_MAX : count == 0) return finish(cpu, 1, bad_parameter);
        if (load ? count == 0 : count == 1) {
            auto exposed = std::span(static_cast<std::byte*>(memory_.access(section->address, section->size)), section->size);
            if (load) image_.load_section(*section, exposed);
            else std::ranges::fill(exposed, std::byte{});
            for (const auto reference : {section->head_reference, section->tail_reference}) if (reference) {
                const auto old = memory_.load<std::uint16_t>(reference);
                memory_.store<std::uint16_t>(reference, load ? old + 1 : old - 1);
            }
        }
        memory_.store(pointer + 24, load ? count + 1 : count - 1);
        return finish(cpu, 1, 0);
    }
    case 279: {
        auto a = counted(arg(0)), b = counted(arg(1));
        if (arg(2)) {
            a = fold(std::move(a));
            b = fold(std::move(b));
        }
        return finish(cpu, 3, a == b);
    }
    case 260: { // RtlAnsiStringToUnicodeString
        const auto destination = arg(0), source = arg(1);
        if (!destination || !source) return finish(cpu, 3, bad_parameter);
        const auto length = memory_.load<std::uint16_t>(source);
        const Word bytes = 2U * length;
        if (bytes + 2 > UINT16_MAX) return finish(cpu, 3, 0xC00000F0);
        const auto input = memory_.load<Word>(source + 4);
        if (length) memory_.access(input, length);
        auto buffer = memory_.load<Word>(destination + 4);
        memory_.store<std::uint16_t>(destination, static_cast<std::uint16_t>(bytes));
        if (arg(2)) {
            buffer = reserve(0, align_up(bytes + 2, page));
            memory_.store<std::uint16_t>(destination + 2, static_cast<std::uint16_t>(bytes + 2));
            memory_.store(destination + 4, buffer ? buffer | 0x80000000U : 0);
            if (!buffer) return finish(cpu, 3, no_memory);
        } else if (bytes + 2 > memory_.load<std::uint16_t>(destination + 2)) {
            return finish(cpu, 3, 0x80000005); // STATUS_BUFFER_OVERFLOW
        }
        memory_.access(buffer, bytes + 2);
        for (Word i=0;i<length;++i) memory_.store<std::uint16_t>(buffer + 2*i, memory_.load<std::uint8_t>(input + i));
        memory_.store<std::uint16_t>(buffer + bytes, 0);
        return finish(cpu, 3, 0);
    }
    case 335: case 336: case 337: {
        Sha1 sha;
        const auto context = memory_.access(arg(0),sizeof(sha));
        if (ordinal == 335) sha.reset();
        else {
            std::memcpy(&sha,context,sizeof(sha));
            if (ordinal == 336 && arg(2)) sha.update(Bytes(static_cast<const std::byte*>(memory_.access(arg(1),arg(2))),arg(2)));
            if (ordinal == 337) {
                const auto digest = sha.finish();
                std::memcpy(memory_.access(arg(1),digest.size()),digest.data(),digest.size());
            }
        }
        std::memcpy(context,&sha,sizeof(sha));
        return finish(cpu, ordinal == 335 ? 1 : ordinal == 336 ? 3 : 2);
    }
    case 340: {
        const auto input = [&](unsigned pointer, unsigned length, Word limit = UINT32_MAX) {
            const auto size = std::min(arg(length),limit);
            return size ? Bytes(static_cast<const std::byte*>(memory_.access(arg(pointer),size)),size) : Bytes{};
        };
        const auto digest = xbox_hmac(input(0,1,64),input(2,3),input(4,5));
        std::memcpy(memory_.access(arg(6),digest.size()),digest.data(),digest.size());
        return finish(cpu, 7);
    }
    case 128:
        memory_.store(arg(0), system_time());
        return finish(cpu, 1);
    case 99: {
        if (!arg(2)) break;
        wait(cpu, arg(2), [] { return false; });
        return finish(cpu, 3, 0);
    }
    case 305: {
        const auto time = memory_.load<std::uint64_t>(arg(0));
        const FILETIME value{static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
        SYSTEMTIME fields{};
        if (!FileTimeToSystemTime(&value, &fields)) break;
        const std::uint16_t packed[]{fields.wYear, fields.wMonth, fields.wDay, fields.wHour,
                                     fields.wMinute, fields.wSecond, fields.wMilliseconds, fields.wDayOfWeek};
        std::memcpy(memory_.access(arg(1), sizeof(packed)), packed, sizeof(packed));
        return finish(cpu, 2);
    }
    case 149: {
        const auto timer = arg(0);
        const auto raw = std::uint64_t{arg(1)} | (std::uint64_t{arg(2)} << 32);
        const auto due = due_time(raw);
        if (arg(3)) memory_.access(arg(3), 28);
        const auto was_set = timers_.contains(timer);
        timers_.insert_or_assign(timer, Timer{due, arg(3)});
        memory_.store<std::uint8_t>(timer + 3, 1);
        memory_.store<Word>(timer + 4, 0);
        memory_.store(timer + 16, due);
        memory_.store(timer + 32, arg(3));
        return finish(cpu, 4, was_set);
    }
    case 119: return finish(cpu, 3, queue_dpc(arg(0), arg(1), arg(2)));
    case 137: {
        const auto address = arg(0);
        const bool queued = std::erase(dpcs_, address) != 0;
        if (queued) memory_.store<std::uint8_t>(address + 2, 0);
        return finish(cpu, 1, queued);
    }
    case 97: {
        const bool was_set = timers_.erase(arg(0)) != 0;
        memory_.store<std::uint8_t>(arg(0) + 3, 0);
        return finish(cpu, 1, was_set);
    }
    case 24: { // Offline NTSC-M retail profile, matching the reference boot.
        std::optional<Word> value;
        switch (arg(0)) {
        case 7: value = language_; break;
        case 0x104: value = 1; break;
        case 8: case 9: case 10: case 17: value = 0; break;
        case 0x103: value = 0x00400100; break;
        }
        if (!value) return finish(cpu, 5, name_missing);
        if (arg(4)) memory_.store<Word>(arg(4), 4);
        if (arg(3) < 4) return finish(cpu, 5, 0xC0000023);
        if (arg(1)) memory_.store<Word>(arg(1), 4);
        memory_.store(arg(2), *value);
        return finish(cpu, 5, 0);
    }
    case 301: {
        using Convert = ULONG (WINAPI*)(LONG);
        static const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
        if (!convert) break;
        return finish(cpu, 1, convert(std::bit_cast<LONG>(arg(0))));
    }
    case 312:
        return unwind_guest(cpu, memory_, arg(0), arg(1), arg(2), arg(3));
    case 255: { // PsCreateSystemThreadEx
        if (!arg(0) || arg(1) || arg(8)) break;
        auto& thread = create(arg(5), arg(6), arg(9), arg(2), arg(3), arg(7) != 0);
        const Word handle = new_handle();
        handles_.emplace(handle, &thread);
        memory_.store(arg(0), handle);
        if (arg(4)) memory_.store(arg(4), thread.id);
        return finish(cpu, 10, 0);
    }
    case 258: throw ThreadExit{arg(0)};
    case 187: { // NtClose
        if (const auto file = files_.find(arg(0)); file != files_.end()) {
            const bool modified = file->second.modified;
            auto path = std::move(file->second.path);
            files_.erase(file);
            const bool deleted = pending_deletions_.contains(path) && std::ranges::none_of(files_, [&](const auto& entry) {
                return entry.second.path == path;
            });
            if (deleted) {
                pending_deletions_.erase(path);
                scratch_files_.erase(path);
                directories_.erase(path);
                file_reservations_.erase(path);
                file_times_.erase(path);
                storage_changed(path);
                last_io_path_ = path;
                trace_io(cpu, ordinal, 0, 0, 0);
            }
            if ((modified || deleted) && persistent_path(path) && !storage_writes_open()) flush_storage();
            return finish(cpu, 1, 0);
        }
        if (const auto h = handles_.find(arg(0)); h != handles_.end()) {
            auto* thread = h->second;
            handles_.erase(h);
            retire_thread(*thread);
            return finish(cpu,1,0);
        }
        return finish(cpu, 1, (link_handles_.erase(arg(0)) ||
                              events_.erase(arg(0))) ? 0 : bad_handle);
    }
    case 189: {
        if (!arg(0) || arg(2) > 1) return finish(cpu, 4, bad_parameter);
        const auto name = arg(1) ? object_name(arg(1), false) : std::string{};
        std::shared_ptr<Event> event;
        if (!name.empty()) {
            const auto existing = std::ranges::find_if(events_, [&](const auto& pair) {
                return pair.second->name == name;
            });
            if (existing != events_.end()) event = existing->second;
        }
        const Word status = event ? 0x40000000 : 0;
        if (!event) event = std::make_shared<Event>(Event{name, arg(2) == 1, (arg(3) & 0xFF) != 0});
        const auto handle = new_handle();
        memory_.store(arg(0), handle);
        events_.emplace(handle, std::move(event));
        return finish(cpu, 4, status);
    }
    case 186: case 225: {
        const auto found = events_.find(arg(0));
        const auto count = ordinal == 225 ? 2 : 1;
        if (found == events_.end()) return finish(cpu, count, bad_handle);
        const auto previous = std::exchange(found->second->signaled, ordinal == 225);
        if (ordinal == 225 && arg(1)) memory_.store<Word>(arg(1), previous);
        return finish(cpu, count, 0);
    }
    case 108: {
        const auto object = arg(0);
        if (arg(1) > 1) break;
        memory_.store<Word>(object, (4U << 16) | arg(1));
        memory_.store<Word>(object + 4, (arg(2) & 0xFF) != 0);
        memory_.store(object + 8, object + 8);
        memory_.store(object + 12, object + 8);
        return finish(cpu, 3);
    }
    case 138: case 145: {
        const auto object = arg(0);
        const auto type = memory_.load<std::uint8_t>(object);
        if (type > 1 || (ordinal == 145 && (arg(2) & 0xFF))) break;
        const auto previous = memory_.load<Word>(object + 4);
        memory_.store<Word>(object + 4, ordinal == 145);
        return finish(cpu, ordinal == 145 ? 3 : 1, previous);
    }
    case 190: case 202: {
        const auto result = open_file(cpu, ordinal == 190);
        if (result) io_status(arg(3), result);
        trace_io(cpu, ordinal, result, arg(ordinal == 190 ? 8 : 5), memory_.load<Word>(arg(3) + 4));
        return finish(cpu, ordinal == 190 ? 9 : 6, result);
    }
    case 198: { // NtFlushBuffersFile
        const auto file = files_.find(arg(0));
        const auto result = file == files_.end() ? bad_handle : 0U;
        if (!result && persistent_path(file->second.path)) flush_storage();
        io_status(arg(1), result);
        return finish(cpu, 2, result);
    }
    case 207: { // NtQueryDirectoryFile returns one Xbox directory entry per call.
        const auto file = files_.find(arg(0));
        const auto event = events_.find(arg(1));
        const auto info = arg(7), output = arg(5), length = arg(6);
        if (arg(2) || (info != 1 && info != 12)) break;
        Word result = 0, bytes = 0;
        if (file == files_.end() || (arg(1) && event == events_.end())) result = bad_handle;
        else if (!file->second.directory) result = 0xC0000103;
        else {
            auto& opened = file->second;
            if (!opened.listing_loaded || (arg(9) & 0xFF)) {
                opened.listing.clear();
                const auto pattern = arg(8) ? fold(counted(arg(8))) : "*";
                const auto add = [&](std::string_view name, Word size, Word allocation, bool directory, FileTimes times) {
                    if (directory_match(pattern,fold(std::string(name)))) {
                        if (opened.listing.size() >= 4096) throw std::runtime_error("Guest directory listing exceeds its limit");
                        opened.listing.push_back({std::string(name),size,allocation,directory,times});
                    }
                };
                if (opened.disc_entry) {
                    disc_->visit_directory(*opened.disc_entry,[&](std::string_view name, DiscEntry member) {
                        add(name,member.size,align_up(member.size,2048),member.directory,{});
                        return true;
                    });
                } else {
                    const auto prefix = opened.path + '\\';
                    const auto name = [&](std::string_view path) { return path.substr(prefix.size()); };
                    for (auto item=directories_.lower_bound(prefix);item != directories_.end() && item->starts_with(prefix);++item) {
                        if (!name(*item).contains('\\')) add(name(*item),0,0,true,file_times(*item));
                    }
                    for (auto item=scratch_files_.lower_bound(prefix);item != scratch_files_.end() && item->first.starts_with(prefix);++item) {
                        if (!name(item->first).contains('\\')) {
                            const auto size = static_cast<Word>(item->second.size());
                            add(name(item->first), size, allocated_size(item->first, size), false, file_times(item->first));
                        }
                    }
                }
                std::ranges::sort(opened.listing,{},&DirectoryItem::name);
                opened.listing_position = 0;
                opened.listing_loaded = true;
            }
            if (opened.listing_position == opened.listing.size()) result = 0x80000006;
            else {
                const auto& item = opened.listing[opened.listing_position];
                const Word header = info == 1 ? 64 : 12;
                const auto name_bytes = static_cast<Word>(item.name.size());
                bytes = align_up(header + name_bytes,8);
                if (bytes > length) { result=0x80000005; bytes=0; }
                else {
                    std::memset(memory_.access(output,bytes),0,bytes);
                    memory_.store(output+4,static_cast<Word>(opened.listing_position++));
                    if (info == 1) {
                        std::memcpy(memory_.access(output+8, sizeof(item.times)), &item.times, sizeof(item.times));
                        memory_.store<std::uint64_t>(output+40,item.directory ? 0 : item.size);
                        memory_.store<std::uint64_t>(output+48,item.directory ? 0 : item.allocation);
                        memory_.store<Word>(output+56,(item.directory ? 0x10 : 0x80) | (opened.disc_entry ? 1 : 0));
                        memory_.store(output+60,name_bytes);
                    } else memory_.store(output+8,name_bytes);
                    std::memcpy(memory_.access(output+header,name_bytes),item.name.data(),name_bytes);
                }
            }
        }
        if (arg(1) && event != events_.end()) event->second->signaled = true;
        io_status(arg(4),result,bytes);
        return finish(cpu,10,result);
    }
    case 218: { // NtQueryVolumeInformationFile
        const auto file = files_.find(arg(0));
        const auto info = arg(4);
        const Word result = file == files_.end() ? bad_handle : arg(3) < 24 ? 0xC0000023 : 0;
        if (info != 1 && info != 3) break;
        if (!result) {
            const auto out = arg(2);
            const bool cd = file->second.disc_entry.has_value();
            constexpr std::uint64_t offline_capacity = 0x131F00000ULL;
            const auto& opened = file->second;
            const auto& data = opened.raw ? scratch_files_.at(opened.path) : kernel_;
            const bool fatx = !opened.raw || (data.size() >= 16 && u32(data, 0) == 0x58544146U);
            if (!fatx) {
                io_status(arg(1), 0xC000014F);
                trace_io(cpu, ordinal, 0xC000014F, info, data.size() < 4 ? 0 : u32(data, 0));
                return finish(cpu, 5, 0xC000014F);
            }
            last_io_path_ = opened.path;
            if (info == 1) {
                // Unlabeled volume. FATX stores the serial in its superblock.
                const Word serial = opened.raw ? u32(data, 4) : cd ? 0U : storage_volume_serial_;
                std::memset(memory_.access(out, 24), 0, 24);
                memory_.store(out + 8, serial);
                trace_io(cpu, ordinal, 0, info, 24);
            } else {
                const auto sectors = opened.raw ? u32(data, 8) : 32U;
                if (!sectors) break;
                const auto units = (opened.raw ? cache_capacity : offline_capacity) / (sectors * 512ULL);
                memory_.store(out, cd ? disc_->size() / 2048 : units);
                memory_.store(out + 8, cd ? std::uint64_t{0} : units);
                memory_.store<Word>(out + 16, cd ? 1 : sectors);
                memory_.store<Word>(out + 20, cd ? 2048 : 512);
                trace_io(cpu, ordinal, 0, info, (cd ? 1 : sectors) * (cd ? 2048 : 512));
            }
        }
        io_status(arg(1), result, result ? 0 : 24);
        return finish(cpu, 5, result);
    }
    case 196: case 200: {
        const auto file = files_.find(arg(0));
        if (file == files_.end() || !file->second.raw || arg(1) || arg(2)) break;
        const auto request = arg(5), output = arg(8);
        Word length = 0;
        if (ordinal == 196 && request == 0x70000) {
            length = 24;
            if (arg(9) < length) break;
            std::memset(memory_.access(output, length), 0, length);
            memory_.store<std::uint64_t>(output, cache_capacity / 512);
            memory_.store<Word>(output + 8, 12);
            memory_.store<Word>(output + 12, 1);
            memory_.store<Word>(output + 16, 1);
            memory_.store<Word>(output + 20, 512);
        } else if (ordinal == 196 && request == 0x74004) {
            length = 32;
            if (arg(9) < length) break;
            std::memset(memory_.access(output, length), 0, length);
            memory_.store<std::uint64_t>(output + 8, cache_capacity);
        } else if (!(ordinal == 200 && request == 0x90020)) break;
        io_status(arg(4), 0, length);
        trace_io(cpu, ordinal, 0, request, length);
        return finish(cpu, 10, 0);
    }
    case 211: { // NtQueryInformationFile
        const auto file = files_.find(arg(0));
        const auto info = arg(4);
        Word length;
        if (info == 4) length = 40;
        else if (info == 5) length = 24;
        else if (info == 6 || info == 14) length = 8;
        else if (info == 34) length = 56;
        else break;
        const Word result = file == files_.end() ? bad_handle : arg(3) < length ? 0xC0000023 : 0;
        if (!result) {
            const auto out = arg(2);
            auto& opened = file->second;
            const auto bytes = opened.directory ? std::uint64_t{0} :
                opened.disc_entry ? opened.disc_entry->size : scratch_files_.at(opened.path).size();
            const std::uint64_t allocated = opened.disc_entry ? (bytes + 2047) & ~2047ULL :
                allocated_size(opened.path, static_cast<Word>(bytes));
            std::memset(memory_.access(out, length), 0, length);
            if (info == 14) memory_.store(out, opened.offset);
            else if (info == 6) {
                Sha1 hash;
                hash.reset();
                hash.update(std::as_bytes(std::span(opened.path)));
                const auto digest = hash.finish();
                memory_.store(out, (std::uint64_t{u32(digest, 4)} << 32) | u32(digest, 0));
            } else if (info == 4 || info == 34) {
                const auto times = file_times(opened.path);
                std::memcpy(memory_.access(out, sizeof(times)), &times, sizeof(times));
                const Word attributes = (opened.directory ? 0x10U : 0x80U) | (opened.disc_entry ? 1U : 0U);
                if (info == 34) {
                    memory_.store(out + 32, allocated);
                    memory_.store(out + 40, bytes);
                    memory_.store(out + 48, attributes);
                } else memory_.store(out + 32, attributes);
            } else {
                memory_.store(out, allocated);
                memory_.store(out + 8, bytes);
                memory_.store<Word>(out + 16, 1);
                memory_.store<std::uint8_t>(out + 20, pending_deletions_.contains(opened.path));
                memory_.store<std::uint8_t>(out + 21, opened.directory);
            }
            if ((info == 6 || info == 34) && persistent_path(opened.path)) {
                last_io_path_ = opened.path;
                trace_io(cpu, ordinal, 0, info, length);
            }
        }
        io_status(arg(1), result, result ? 0 : length);
        return finish(cpu, 5, result);
    }
    case 226: { // NtSetInformationFile
        const auto file = files_.find(arg(0));
        if (file == files_.end()) {
            io_status(arg(1), bad_handle);
            return finish(cpu, 5, bad_handle);
        }
        const auto info = arg(4);
        auto& opened = file->second;
        Word result = 0;
        if (info == 13) {
            result = arg(3) < 1 ? info_length_mismatch :
                set_file_disposition(opened, memory_.load<std::uint8_t>(arg(2)) != 0);
        } else if (info == 14 || info == 19 || info == 20) {
            if (arg(3) < 8) result = info_length_mismatch;
            else {
                const auto size = memory_.load<std::uint64_t>(arg(2));
                if (size > INT64_MAX) result = bad_parameter;
                else if (info == 14) opened.offset = size;
                else if (opened.directory) result = bad_parameter;
                else if (opened.disc_entry) result = write_protected;
                else if (!(opened.access & 0x50000002U)) result = access_denied;
                else if (size > file_limit) result = 0xC000007F; // STATUS_DISK_FULL
                else {
                    const auto requested = static_cast<Word>(size);
                    auto& data = scratch_files_.at(opened.path);
                    // Allocation growth reserves space. Only EOF growth adds bytes.
                    if (requested != data.size() && (info == 20 || requested < data.size())) {
                        data.resize(requested);
                        file_written(opened);
                    }
                    if (info == 19) {
                        const auto allocation = align_up(requested, disk_cluster);
                        if (allocation == align_up(static_cast<Word>(data.size()), disk_cluster))
                            file_reservations_.erase(opened.path);
                        else file_reservations_.insert_or_assign(opened.path, allocation);
                    }
                }
            }
        } else if (info == 4 && arg(3) >= 36 && !opened.disc_entry) {
            const auto attributes = memory_.load<Word>(arg(2) + 32);
            if (attributes && attributes != 0x80 && attributes != 0x10) break;
            auto times = file_times(opened.path);
            std::uint64_t* fields[]{&times.created, &times.accessed, &times.written, &times.changed};
            bool changed = false;
            for (Word i = 0; i < std::size(fields); ++i) {
                const auto value = memory_.load<std::uint64_t>(arg(2) + 8 * i);
                if (!value || value > INT64_MAX || value == *fields[i]) continue;
                *fields[i] = value;
                changed = true;
            }
            if (changed) {
                file_times_.insert_or_assign(opened.path, times);
                opened.modified = true;
                storage_changed(opened.path);
            }
        } else break;
        io_status(arg(1), result);
        last_io_path_ = opened.path;
        trace_io(cpu, ordinal, result, info, 0);
        return finish(cpu, 5, result);
    }
    case 219: {
        const auto file = files_.find(arg(0));
        const bool valid = file != files_.end() && !file->second.directory;
        if (arg(2)) break;
        const auto event = events_.find(arg(1));
        if (arg(1) && event == events_.end()) return finish(cpu, 8, bad_handle);
        Word bytes = 0, result = valid ? 0 : bad_handle;
        std::optional<std::uint64_t> file_offset;
        if (valid && !(file->second.access & read_data)) result = access_denied;
        else if (valid) {
            auto& opened = file->second;
            const auto offset = arg(7) ? memory_.load<std::uint64_t>(arg(7)) : opened.offset;
            file_offset = offset;
            const auto size = opened.disc_entry ? opened.disc_entry->size : scratch_files_.at(opened.path).size();
            last_io_path_ = opened.path;
            if (offset >= size && arg(6)) result = end_of_file;
            else if (arg(6)) {
                bytes = static_cast<Word>(std::min<std::uint64_t>(arg(6), size - offset));
                auto output = std::span(static_cast<std::byte*>(memory_.access(arg(5), bytes)), bytes);
                if (opened.disc_entry) disc_->read(*opened.disc_entry, offset, output);
                else std::memcpy(output.data(), scratch_files_.at(opened.path).data() + offset, bytes);
                opened.offset = offset + bytes;
            }
        }
        io_status(arg(4), result, bytes);
        if (valid && event != events_.end()) event->second->signaled = true;
        trace_io(cpu, ordinal, result, arg(7) ? memory_.load<Word>(arg(7)) : UINT32_MAX, bytes, file_offset);
        return finish(cpu, 8, result);
    }
    case 236: {
        const auto file = files_.find(arg(0));
        if (arg(2)) break;
        const auto event = events_.find(arg(1));
        if (arg(1) && event == events_.end()) return finish(cpu, 8, bad_handle);
        Word bytes = 0, result = bad_handle;
        if (file != files_.end() && !file->second.directory) {
            auto& opened = file->second;
            last_io_path_ = opened.path;
            result = write_protected;
            if (!opened.disc_entry && !(opened.access & (write_data | append_data))) result = access_denied;
            else if (!opened.disc_entry) {
                auto& data = scratch_files_.at(opened.path);
                const auto requested = arg(7) ? memory_.load<std::uint64_t>(arg(7)) : opened.offset;
                const auto offset = !(opened.access & write_data) || requested == UINT64_MAX ? data.size() : requested;
                if (offset > file_limit || arg(6) > file_limit - offset) break;
                bytes = arg(6);
                if (bytes) {
                    const auto input = memory_.access(arg(5), bytes);
                    if (offset + bytes > data.size()) data.resize(static_cast<std::size_t>(offset + bytes));
                    std::memcpy(data.data() + offset, input, bytes);
                    opened.offset = offset + bytes;
                    file_written(opened);
                }
                result = 0;
            }
        }
        io_status(arg(4), result, bytes);
        if (file != files_.end() && !file->second.directory && event != events_.end()) event->second->signaled = true;
        trace_io(cpu, ordinal, result, arg(7) ? memory_.load<Word>(arg(7)) : UINT32_MAX, bytes);
        return finish(cpu, 8, result);
    }
    case 233: case 234: {
        const unsigned count = ordinal == 233 ? 3 : 4;
        const auto handle = arg(0), timeout = arg(count - 1);
        if (const auto event = events_.find(handle); event != events_.end()) {
            const auto object = event->second;
            const auto result = wait(cpu, timeout, [&] { return object->signaled; });
            if (!result && object->auto_reset) object->signaled = false;
            return finish(cpu, count, result);
        }
        if (const auto thread = handles_.find(handle); thread != handles_.end()) {
            auto* target = thread->second;
            ++target->waiters;
            const auto result = wait(cpu,timeout,[target]{return target->exited;});
            --target->waiters;
            retire_thread(*target);
            return finish(cpu,count,result);
        }
        return finish(cpu, count, bad_handle);
    }
    case 159: {
        const auto object = arg(0);
        const auto type = memory_.load<std::uint8_t>(object);
        if (type > 1 && type != 6 && type != 8 && type != 9) break;
        const auto next_signal = graphics_.ready() && object == GuestGraphics::vblank_event ? graphics_.next_vblank() : UINT64_MAX;
        Thread* target = nullptr;
        if (type == 6) {
            auto found = std::ranges::find_if(threads_,[&](const Thread& t){return t.stack_bytes && t.object == object;});
            if (found != threads_.end()) { target = &*found; ++target->waiters; }
        }
        const auto result = wait(cpu, arg(4), [&] { return memory_.load<Word>(object + 4) != 0; }, next_signal);
        if (target) { --target->waiters; retire_thread(*target); }
        if (!result && (type == 1 || type == 9)) memory_.store<Word>(object + 4, 0);
        return finish(cpu, 5, result);
    }
    case 47: { // HalRegisterShutdownNotification
        const auto registration = arg(0);
        memory_.access(registration, 16);
        std::erase(shutdown_registrations_, registration);
        if (arg(1)) shutdown_registrations_.push_back(registration);
        std::ranges::stable_sort(shutdown_registrations_, {}, [&](Word address) {
            return -static_cast<std::int64_t>(std::bit_cast<std::int32_t>(memory_.load<Word>(address + 4)));
        });
        auto previous = shutdown_list;
        for (const auto item : shutdown_registrations_) {
            const auto link = item + 8;
            memory_.store(previous, link);
            memory_.store(link + 4, previous);
            previous = link;
        }
        memory_.store(previous, shutdown_list);
        memory_.store(shutdown_list + 4, previous);
        return finish(cpu, 2);
    }
    case 67: {
        auto name = fold(counted(arg(0)));
        auto target = counted(arg(1));
        if (!name.starts_with('\\') || !target.starts_with('\\')) return finish(cpu, 2, bad_parameter);
        return finish(cpu, 2, links_.emplace(std::move(name), std::move(target)).second ? 0 : name_conflict);
    }
    case 69: return finish(cpu, 1, links_.erase(fold(counted(arg(0)))) ? 0 : name_missing);
    case 203: {
        const auto found = links_.find(object_name(arg(1), false));
        if (found == links_.end()) return finish(cpu, 2, name_missing);
        const auto handle = new_handle();
        link_handles_.emplace(handle, found->second);
        memory_.store(arg(0), handle);
        return finish(cpu, 2, 0);
    }
    case 215: {
        const auto found = link_handles_.find(arg(0));
        if (found == link_handles_.end()) return finish(cpu, 3, bad_handle);
        const auto& target = found->second;
        const auto output = arg(1), length = static_cast<Word>(target.size());
        const auto maximum = memory_.load<std::uint16_t>(output + 2);
        if (arg(2)) memory_.store(arg(2), length);
        if (maximum < length) return finish(cpu, 3, 0xC0000023);
        const auto size = std::min<Word>(maximum, length + 1);
        std::memcpy(memory_.access(memory_.load<Word>(output + 4), size), target.c_str(), size);
        memory_.store<std::uint16_t>(output, static_cast<std::uint16_t>(length));
        return finish(cpu, 3, 0);
    }
    case 289: {
        const auto source = arg(1);
        std::uint16_t length = 0;
        if (source) while (length < 65534 && memory_.load<std::uint8_t>(source + length)) ++length;
        memory_.store(arg(0), length);
        memory_.store<std::uint16_t>(arg(0) + 2, source ? length + 1 : 0);
        memory_.store(arg(0) + 4, source);
        return finish(cpu, 2);
    }
    case 107: case 113: {
        const auto address = arg(0);
        const bool timer = ordinal == 113;
        const auto kind = arg(1);
        if (timer && kind > 1) break;
        memory_.fill_elements<Word>(address, 0, timer ? 10 : 7, false);
        memory_.store<std::uint16_t>(address, timer ? 8 + kind : 19);
        if (timer) {
            timers_.erase(address);
            memory_.store<std::uint8_t>(address + 2, 10);
            memory_.store(address + 8, address + 8);
            memory_.store(address + 12, address + 8);
            memory_.store(address + 24, address + 24);
            memory_.store(address + 28, address + 24);
        } else {
            memory_.store(address + 4, address + 4);
            memory_.store(address + 8, address + 4);
            memory_.store(address + 12, kind);
            memory_.store(address + 16, arg(2));
        }
        return finish(cpu, timer ? 2 : 3);
    }
    case 184: { // NtAllocateVirtualMemory
        const auto base_pointer = arg(0), size_pointer = arg(2), flags = arg(3);
        const auto requested = memory_.load<Word>(base_pointer), size = memory_.load<Word>(size_pointer);
        if (arg(1) || !size || size > ram_end || (flags & ~0x3000U) || !(flags & 0x3000U)) break;
        const auto rounded = align_up(size + (requested & (page - 1)), page);
        const auto aligned = requested & ~(page - 1);
        auto base = aligned;
        if (aligned && !(flags & 0x2000)) {
            const auto limit = allocations_.upper_bound(aligned);
            if (limit == allocations_.begin()) return finish(cpu, 5, address_conflict);
            const auto [start, bytes] = *std::prev(limit);
            if (!virtual_allocations_.contains(start) || aligned - start >= bytes || rounded > bytes - (aligned - start))
                return finish(cpu, 5, address_conflict);
            set_page_access(aligned, rounded, false);
        } else {
            base = reserve(aligned, rounded, flags & 0x2000 ? 0x10000 : page);
            if (!base) return finish(cpu, 5, aligned ? address_conflict : no_memory);
            virtual_allocations_.insert(base);
            if (!(flags & 0x1000)) set_page_access(base, rounded, true);
        }
        memory_.store(base_pointer, base);
        memory_.store(size_pointer, rounded);
        return finish(cpu, 5, 0);
    }
    case 199: { // NtFreeVirtualMemory
        const auto base_pointer = arg(0), size_pointer = arg(1), flags = arg(2);
        const auto base = memory_.load<Word>(base_pointer), size = memory_.load<Word>(size_pointer);
        const auto limit = allocations_.upper_bound(base);
        if (limit == allocations_.begin()) return finish(cpu, 3, bad_parameter);
        const auto allocation = std::prev(limit);
        const auto [start, length] = *allocation;
        if (!virtual_allocations_.contains(start) || base - start >= length || size > length - (base - start))
            return finish(cpu, 3, bad_parameter);
        if (flags == 0x8000 && base == start && !size) {
            set_page_access(start, length, false);
            virtual_allocations_.erase(start);
            allocations_.erase(allocation);
            memory_.store<Word>(base_pointer, 0);
            memory_.store<Word>(size_pointer, 0);
        } else if (flags == 0x4000 && (size || base == start)) {
            const auto first = base & ~(page - 1);
            const auto end = size ? align_up(base + size, page) : start + length;
            memory_.store(base_pointer, first);
            memory_.store(size_pointer, end - first);
            set_page_access(first, end - first, true);
        } else return finish(cpu, 3, bad_parameter);
        return finish(cpu, 3, 0);
    }
    case 291: case 277: case 294: {
        const auto address = arg(0);
        if (ordinal == 291) {
            memory_.fill_elements<Word>(address, 0, 7, false);
            memory_.store<Word>(address, 0x00040001);
            memory_.store(address + 8, address + 8);
            memory_.store(address + 12, address + 8);
            memory_.store<Word>(address + 16, UINT32_MAX);
        } else {
            if (ordinal == 277) wait(cpu, 0, [&] {
                const auto owner = memory_.load<Word>(address + 24);
                return !owner || owner == current_->object;
            });
            const auto owner = memory_.load<Word>(address + 24);
            const auto recursion = memory_.load<Word>(address + 20);
            const auto lock = memory_.load<Word>(address + 16);
            if (owner && owner != current_->object) break;
            if (ordinal == 277) {
                memory_.store(address + 16, lock + 1);
                memory_.store(address + 20, recursion + 1);
                memory_.store(address + 24, current_->object);
            } else {
                if (!recursion || !owner) break;
                memory_.store(address + 16, lock - 1);
                memory_.store(address + 20, recursion - 1);
                if (recursion == 1) memory_.store<Word>(address + 24, 0);
            }
        }
        return finish(cpu, 1);
    }
    }
    throw KernelStop{ordinal, cpu.eip, std::format("Unbound Xbox kernel operation {} ({})", ordinal, kernel_name(ordinal))};
}

void OfflineBoot::capture_state(const std::filesystem::path& directory) const {
    std::ofstream state(directory / "game-state.json", std::ios::binary | std::ios::noreplace);
    state.exceptions(std::ios::badbit | std::ios::failbit);
    report(state, "captured", {}, threads_.front());
    state.close();
    std::ofstream memory(directory / "guest-memory.bin", std::ios::binary | std::ios::noreplace);
    memory.exceptions(std::ios::badbit | std::ios::failbit);
    memory.write(reinterpret_cast<const char*>(ram_.data()), ram_.size());
    memory.close();
    std::ofstream layout(directory / "guest-memory.json", std::ios::binary | std::ios::noreplace);
    layout.exceptions(std::ios::badbit | std::ios::failbit);
    layout << "{\"base\":" << guest_base << ",\"bytes\":" << ram_.size() << "}\n";
    layout.close();
}

void OfflineBoot::report(std::ostream& output, std::string_view status, std::string_view reason, const Thread& boot_thread) const {
    std::ostringstream out;
    const auto& cpu = *last_cpu_;
    const auto graphics = graphics_.stats();
    const bool main_reached = diagnostic_.reached_watch || (status == "breakpoint" && stop_address_ == game_main);
    out << "{\"format\":\"scoobymm-offline-boot-v1\",\"sha256\":" << json(image_.sha256)
        << ",\"entry\":" << json(hex32(image_.entry)) << ",\"status\":" << json(status)
        << ",\"entry_returned\":" << (boot_thread.returned ? "true" : "false")
        << ",\"game_main_reached\":" << (main_reached ? "true" : "false")
        << ",\"checkpoints\":" << checkpoints() << ",\"eip\":" << json(hex32(stop_address_.value_or(cpu.eip)))
        << ",\"reason\":" << json(reason);
    if (failure_ordinal_) {
        out << ",\"kernel_ordinal\":" << *failure_ordinal_ << ",\"kernel_name\":" << json(kernel_name(*failure_ordinal_));
    }
    if (device_stop_) {
        out << ",\"device_access\":{\"address\":" << json(hex32(device_stop_->address))
            << ",\"width_bytes\":" << device_stop_->width << ",\"write\":" << (device_stop_->write ? "true" : "false") << '}';
    }
    const auto stop = stop_address_.value_or(cpu.eip);
    if (status == "native_boundary" && stop >= guest_base && stop <= ram_end - 16) {
        const auto* bytes = static_cast<const std::byte*>(memory_.access(stop, 16));
        out << ",\"target_bytes\":" << json(hex_bytes(Bytes(bytes, 16)));
    }
    out << ",\"disc_mounted\":" << (disc_ ? "true" : "false")
        << ",\"start_presses\":" << start_presses_
        << ",\"confirm_presses\":" << confirm_presses_
        << ",\"b_presses\":" << b_presses_
        << ",\"audio\":{\"initialized\":" << (audio_.ready() ? "true" : "false")
        << ",\"host_output\":" << (audio_.output_ready() ? "true" : "false")
        << ",\"host_peak\":" << audio_.output_peak()
        << ",\"mixed_frames\":" << audio_.frames() << ",\"play_calls\":" << audio_.plays()
        << ",\"stream_packets\":" << audio_.stream_packets() << ",\"stream_completions\":" << audio_.stream_completions()
        << ",\"effects_loaded\":" << audio_.effect_count()
        << ",\"offline_peak\":" << audio_.peak()
        << ",\"offline_peak_since_effect_load\":" << audio_.peak_since_effect_load();
    if (const auto peak = audio_.peak_since_capture()) out << ",\"offline_peak_since_capture\":" << *peak;
    if (const auto block = audio_.first_invalid_adpcm_block()) {
        out << ",\"first_invalid_adpcm_block\":{\"voice\":" << block->voice
            << ",\"source_address\":" << json(hex32(block->source_address)) << ",\"offset\":" << block->offset
            << ",\"header\":" << json(hex_bytes(std::span(block->header).first(block->channels * 4))) << '}';
    }
    out << "},\"graphics\":{\"initialized\":" << (graphics_.ready() ? "true" : "false")
        << ",\"submissions\":" << graphics.submissions << ",\"draws\":" << graphics.draws
        << ",\"flips\":" << graphics.flips << ",\"clears\":" << graphics.clears
        << ",\"blits\":" << graphics.blits << ",\"overlay_frames\":" << graphics_.overlay_frames()
        << ",\"scanouts\":" << graphics_.scanouts() << ",\"presentations\":" << graphics_.presentations()
        << ",\"captured\":" << (graphics_.captured() ? "true" : "false") << '}'
        << ",\"last_kernel_site\":" << json(hex32(last_service_site_))
        << ",\"last_kernel_return\":" << json(hex32(last_service_return_)) << ",\"registers\":[";
    for (unsigned i = 0; i < cpu.registers.size(); ++i) {
        if (i) out << ',';
        out << json(hex32(cpu.registers[i]));
    }
    out << "],\"stack_words\":[";
    const auto stack = cpu.registers[esp];
    if (status != "returned" && stack >= guest_base && stack < ram_end) {
        const auto words = std::min<Word>(16, (ram_end - stack) / sizeof(Word));
        for (Word i = 0; i < words; ++i) {
            if (i) out << ',';
            out << json(hex32(memory_.load<Word>(stack + i * sizeof(Word))));
        }
    }
    out << "],\"logic_modules\":[";
    bool first_module = true;
    for (const auto& [module, base, owner] : modules_) {
        if (!std::exchange(first_module, false)) out << ',';
        out << "{\"name\":" << json(module->name) << ",\"base\":" << json(hex32(base)) << '}';
    }
    out << "],\"last_io_path\":" << json(last_io_path_) << ",\"threads\":[";
    for (const auto& thread : threads_) {
        if (thread.id != threads_.front().id) out << ',';
        out << "{\"id\":" << thread.id << ",\"start\":" << json(hex32(thread.routine))
            << ",\"state\":" << json(thread.exited ? "exited" : thread.suspended ? "suspended" : thread.wake ? "waiting" : "ready")
            << ",\"exit_status\":" << thread.exit_status << '}';
    }
    out << "],\"services\":[";
    bool first = true;
    for (unsigned i = 1; i < service_calls_.size(); ++i) if (service_calls_[i]) {
        if (!std::exchange(first, false)) out << ',';
        out << "{\"ordinal\":" << i << ",\"name\":" << json(kernel_name(i)) << ",\"calls\":" << service_calls_[i] << '}';
    }
    out << "],\"io\":[";
    first = true;
    for (const auto& entry : io_) {
        if (!std::exchange(first, false)) out << ',';
        out << "{\"site\":" << json(hex32(entry.site)) << ",\"ordinal\":" << entry.ordinal
            << ",\"status\":" << json(hex32(entry.status)) << ",\"detail\":" << entry.detail
            << ",\"information\":" << entry.information << ",\"path\":" << json(entry.path);
        if (entry.file_offset) out << ",\"file_offset\":" << *entry.file_offset;
        out << '}';
    }
    out << "],\"history\":[";
    const auto count = std::min<std::uint64_t>(diagnostic_.visits, diagnostic_.history.size());
    for (auto i = diagnostic_.visits - count; i < diagnostic_.visits; ++i) {
        if (i != diagnostic_.visits - count) out << ',';
        out << json(hex32(diagnostic_.history[i % diagnostic_.history.size()]));
    }
    out << ']';
    if (diagnostics_ && status != "captured") diagnostics_->report(out);
    output << out.str() << "}\n";
}
}

std::uint32_t xbox_language(std::string_view code) {
    // The XGetLanguage switch at 0x11480 indexes the title's EN/DE/FR strings.
    if (code == "en") return 1;
    if (code == "de") return 3;
    if (code == "fr") return 4;
    throw std::invalid_argument("Supported languages are en, fr and de");
}
int play_game(const PlayOptions& options, std::ostream& report) {
    Xbe image(options.xbe);
    Disc disc(options.disc);
    if (disc.executable_sha256() != image.sha256) throw std::runtime_error("XISO executable differs from the verified XBE");
    use_shader_cache(options.shader_cache);
    PlayWindow window(options.hidden, options.fullscreen);
    return OfflineBoot(image, &disc, UINT64_MAX, 0, options.audio, options.capture, options.capture_index,
                       options.input, &window, options.vsync, options.frames, options.storage, &options.diagnostics, options.language).run(report);
}

int boot_command(int argc, char* argv[]) {
    try {
        if (argc < 4) throw std::runtime_error("Usage: native-recompiler boot DEFAULT.xbe CHECKPOINT_BUDGET [STOP_ADDRESS] [--disc XISO] [--audio] [--frame PNG] [--frame-index N] [--start-at-scanout N] [--shader-cache DIR]");
        const auto budget = analysis_map::number(argv[3]);
        int next = 4;
        const auto breakpoint = argc > next && !std::string_view(argv[next]).starts_with("--") ?
            analysis_map::number(argv[next++]) : 0U;
        if (!budget || budget > 200000000) throw std::runtime_error("Boot checkpoint budget must be 1..200000000");
        Xbe image(argv[2]);
        std::optional<Disc> disc;
        bool audio_output = false;
        Word language = 1;
        std::filesystem::path capture;
        std::filesystem::path shader_cache;
        InputScript input;
        Word capture_index = 0;
        while (next < argc) {
            const std::string_view option = argv[next++];
            if (option == "--disc" && !disc && next < argc) {
                disc.emplace(argv[next++]);
                if (disc->executable_sha256() != image.sha256) throw std::runtime_error("XISO executable differs from the verified XBE");
            } else if (option == "--language" && next < argc) {
                language = xbox_language(argv[next++]);
            } else if (option == "--audio" && !audio_output) {
                audio_output = true;
            } else if (option == "--frame" && capture.empty() && next < argc) {
                capture = argv[next++];
                if (capture.extension() != ".png" || std::filesystem::exists(capture)) {
                    throw std::runtime_error("Frame output must be a new PNG file");
                }
            } else if (option == "--frame-index" && !capture_index && next < argc) {
                capture_index = analysis_map::number(argv[next++]);
                if (!capture_index) throw std::runtime_error("Frame index must be at least one");
            } else if (option == "--start-at-scanout" && next < argc) {
                const auto scanout = analysis_map::number(argv[next++]);
                if (!scanout || input.start.size() == 32)
                    throw std::runtime_error("Specify at most 32 nonzero Start scanout indices");
                input.start.push_back(scanout);
            } else if (option == "--shader-cache" && shader_cache.empty() && next < argc) {
                shader_cache = argv[next++];
            } else {
                throw std::runtime_error("Invalid or duplicate boot option: " + std::string(option));
            }
        }
        if (capture_index && capture.empty()) throw std::runtime_error("Frame index requires --frame");
        use_shader_cache(std::move(shader_cache));
        return OfflineBoot(image, disc ? &*disc : nullptr, budget, breakpoint, audio_output,
                           std::move(capture), capture_index ? capture_index : 1, std::move(input), nullptr,true,0,{},nullptr,language).run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
}
