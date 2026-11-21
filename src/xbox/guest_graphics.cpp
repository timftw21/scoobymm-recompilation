#include "guest_graphics.h"
#include "platform_bindings.h"
#include "play_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace xbox_native {
namespace {
using Word = std::uint32_t;
constexpr Word device = 0x00123A50, device_slot = 0x00123A48;
constexpr Word push_start = device + 0x24, push_end = device + 0x28;
constexpr Word gpu_base = 0xFD000000, gpu_size = 0x00800000;
constexpr Word dac_write_index = 0x6813C8, dac_data = 0x6813C9;
constexpr Word sdk_return = 0xFFFFFFE0;
constexpr Word vblank_count = device + 0x19B8;
constexpr Word video_first = 0x8100, video_last = 0x8B00;
constexpr Word video_interrupt = 0x8100, video_buffer = 0x8700, video_stop = 0x8704;
constexpr Word video_address = 0x8920, video_source_size = 0x8928, video_source_point = 0x8930;
constexpr Word video_step_x = 0x8938, video_step_y = 0x8940, video_dest_point = 0x8948, video_dest_size = 0x8950;
constexpr Word video_format = 0x8958, video_color_key = 0x8B00;
constexpr std::uint64_t display_hz = 60, ticks_per_second = 10000000;
}

void GuestGraphics::finish(Cpu& cpu, unsigned arguments) {
    auto& sp = cpu.registers[esp];
    cpu.eip = memory_.load<Word>(sp);
    sp += 4 * (1 + arguments);
}

Word GuestGraphics::sdk_call(Cpu& cpu, Word address, std::span<const Word> args) {
    const auto registers = cpu.registers;
    const auto flags = cpu.flags, pc = cpu.eip;
    const auto stack = registers[esp] - static_cast<Word>(0x100 + 4 * args.size());
    memory_.store(stack, sdk_return);
    for (std::size_t i = 0; i < args.size(); ++i) memory_.store(stack + 4 + static_cast<Word>(4 * i), args[i]);
    cpu.registers[esp] = stack;
    cpu.registers[ecx] = device;
    invoke_native(cpu, memory_, address);
    if (cpu.eip != sdk_return || cpu.registers[esp] != stack + 4 * (args.size() + 1)) {
        throw NativeBoundary(address, "Original graphics helper returned with an unexpected stack");
    }
    const auto result = cpu.registers[eax];
    cpu.registers = registers;
    cpu.flags = flags;
    cpu.eip = pc;
    return result;
}

Word GuestGraphics::initialize(Cpu& cpu, Word parameters) {
    if (renderer_ || cpu.registers[ecx] != device || memory_.load<Word>(device_slot) != device) {
        throw NativeBoundary(cpu.eip, "Unsupported graphics device context");
    }
    const auto width = memory_.load<Word>(parameters), height = memory_.load<Word>(parameters + 4);
    const auto format = memory_.load<Word>(parameters + 8), multisample = memory_.load<Word>(parameters + 16);
    const auto depth = memory_.load<Word>(parameters + 36);
    if (!width || !height || width > 4096 || height > 4096 ||
        (format != 0x11 && format != 0x12 && format != 0x1E) || (multisample && multisample != 0x11) ||
        (memory_.load<Word>(parameters + 32) && depth != 0x2A && depth != 0x2E)) {
        throw NativeBoundary(cpu.eip, std::format("Unsupported D3D presentation {}x{}, format {}, samples {}, depth {}",
            width, height, format, multisample, depth));
    }
    const auto notification_memory = allocator_.allocate(allocator_.context, 0x100);
    if (!notification_memory) return 0x8007000E;
    const auto notification = notification_memory | 0x80000000U;
    memory_.store(device + 0x2058, notification);
    memory_.store(device + 0x30, notification);
    memory_.store(device + 0x2060, notification + 32);
    memory_.store(device + 0x205C, notification + 64);
    memory_.store(device + 0x17F4, notification + 96);
    if (const auto status = sdk_call(cpu, 0x00118CC0); std::bit_cast<std::int32_t>(status) < 0) {
        allocator_.release(allocator_.context, notification_memory);
        memory_.store<Word>(device + 0x2058, 0);
        return status;
    }

    renderer_ = std::make_unique<Renderer>(width, height);
    if (window_) renderer_->attach_window(window_);
    cursor_ = memory_.load<Word>(push_start);
    // Object IDs and classes from the initializer at 0x0011A530–0x0011A5F1.
    constexpr std::array<GraphicsObject, 11> objects{{
        {0xD, 0x97}, {0xE, 0x39}, {0x10, 0x9F}, {0x11, 0x62}, {0x12, 0x44},
        {0x13, 0x57}, {0x14, 0x43}, {0x15, 0x12}, {0x16, 0x72}, {0x18, 0x19}, {0x19, 0x30}
    }};
    gpu_ = std::make_unique<Gpu>(*renderer_, memory_, notification, objects);
    gpu_->flip = [this] { scanout(); };
    for (unsigned i = 0; i < gamma_.size(); ++i) gamma_[i].fill(static_cast<std::byte>(i));
    memory_.map_io(gpu_base, gpu_size, this,
        [](void* graphics, Word offset, unsigned width) {
            return static_cast<GuestGraphics*>(graphics)->read_register(offset, width);
        },
        [](void* graphics, Word offset, unsigned width, Word value) {
            static_cast<GuestGraphics*>(graphics)->write_register(offset, width, value);
        });
    memory_.store<Word>(device + 0x17F8, gpu_base);
    memory_.store<Word>(device + 0x51C, gpu_base);

    memory_.store<Word>(0x001235A8, memory_.load<Word>(0x001235A8) | 0xFF7F7F);
    memory_.store<Word>(device + 0x534, 0x40);
    sdk_call(cpu, 0x00119A30);
    const std::array presentation{parameters};
    if (const auto status = sdk_call(cpu, 0x00119D10, presentation); std::bit_cast<std::int32_t>(status) < 0)
        return status;
    // Preserve the original setup of surfaces, shader selections and render state.
    memory_.store<Word>(device + 0x37C, 0x00125C98);
    const std::array shader{2U};
    sdk_call(cpu, 0x00118840, shader);
    memory_.fill_elements<Word>(0x00125B80, 0, 0x46, false);
    memory_.store<Word>(0x00125B84, 0x10);
    const std::array targets{memory_.load<Word>(device + 0x15F4), memory_.load<Word>(device + 0x1600)};
    sdk_call(cpu, 0x00113F30, targets);
    sdk_call(cpu, 0x00119B10);
    const std::array clear{0U, 0U, 3U, 0U, 0x3F800000U, 0U};
    sdk_call(cpu, 0x001153E0, clear);
    return 0;
}

Word GuestGraphics::read_register(Word offset, unsigned width) const {
    // The SDK reads this register to drain posted DAC writes. Host DAC writes are synchronous.
    if (offset == 0x3240 && width == 4) return 0;
    if (width == 4 && offset >= video_first && offset <= video_last && !(offset & 3))
        return offset == video_stop ? 0 : video_registers_[(offset - video_first) / 4];
    throw std::runtime_error(std::format("Unbound {}-bit NV2A register read at {}", width * 8, hex32(gpu_base + offset)));
}

void GuestGraphics::write_register(Word offset, unsigned width, Word value) {
    if (offset == dac_write_index && width == 1) {
        gamma_index_ = value & 0xFF;
        gamma_channel_ = 0;
    } else if (offset == dac_data && width == 1) {
        gamma_[gamma_index_][gamma_channel_++] = static_cast<std::byte>(value & 0xFF);
        if (gamma_channel_ == 3) {
            gamma_channel_ = 0;
            gamma_index_ = (gamma_index_ + 1) & 0xFF;
        }
        gamma_written_ = true;
        gamma_dirty_ = true;
    } else if (width == 4 && offset >= video_first && offset <= video_last && !(offset & 3)) {
        auto& reg = video_registers_[(offset - video_first) / 4];
        if (offset == video_interrupt) reg &= ~value;
        else reg = value;
        if (offset == video_buffer) overlay_pending_ = (value & 1) != 0;
        if (offset == video_stop && (value & 1)) {
            video_registers_[(video_buffer - video_first) / 4] = 0;
            overlay_pending_ = false;
        }
    } else {
        throw std::runtime_error(std::format("Unbound {}-bit NV2A register write at {}", width * 8, hex32(gpu_base + offset)));
    }
}

std::uint64_t GuestGraphics::next_vblank() const {
    return ((vblanks_ + 1) * ticks_per_second + display_hz - 1) / display_hz;
}

void GuestGraphics::advance_display(std::uint64_t now) {
    if (!gpu_) return;
    const auto elapsed = now * display_hz / ticks_per_second;
    if (elapsed <= vblanks_) return;
    vblanks_ = elapsed;
    memory_.store(vblank_count, static_cast<Word>(vblanks_));
    memory_.store<Word>(vblank_event + 4, 1);
    if (overlay_pending_) {
        overlay_pending_ = false;
        ++overlay_frames_;
        scanout();
    }
}

void GuestGraphics::composite_video(Image& image) const {
    const auto reg = [this](Word offset) { return video_registers_[(offset - video_first) / 4]; };
    if (!(reg(video_buffer) & 1)) return;
    const auto packed = reg(video_format), pitch = packed & 0x1FFF;
    const auto width = reg(video_source_size) & 0x7FF, height = reg(video_source_size) >> 16 & 0x7FF;
    const auto start_x = (reg(video_source_point) & 0x7FFF) >> 4;
    const auto start_y = reg(video_source_point) >> 20;
    if (!width || !height || pitch < ((width + 1) & ~1U) * 2 ||
        start_x >= width || start_y >= height || (packed & 0x30000) != 0x10000)
        throw std::runtime_error("Unsupported NV2A video overlay format");
    const auto address = reg(video_address);
    const auto* source = static_cast<const std::uint8_t*>(memory_.access(address, std::size_t(pitch) * (height - 1) + ((width + 1) & ~1U) * 2));
    const auto left = reg(video_dest_point) & 0xFFF, top = reg(video_dest_point) >> 16 & 0xFFF;
    const auto right = std::min(image.width, left + (reg(video_dest_size) & 0xFFF));
    const auto bottom = std::min(image.height, top + (reg(video_dest_size) >> 16 & 0xFFF));
    const auto to_byte = [](int value) { return static_cast<std::byte>(std::clamp((value + 128) >> 8, 0, 255)); };
    for (auto y = top; y < bottom; ++y) {
        const auto row = std::min<std::uint64_t>(height - 1, start_y + ((std::uint64_t{y - top} * reg(video_step_y)) >> 20));
        for (auto x = left; x < right; ++x) {
            auto* pixel = image.rgba.data() + (std::size_t(y) * image.width + x) * 4;
            const auto rgb = std::to_integer<Word>(pixel[0]) << 16 | std::to_integer<Word>(pixel[1]) << 8 | std::to_integer<Word>(pixel[2]);
            if ((packed & 0x100000) && rgb != (reg(video_color_key) & 0xFFFFFF)) continue;
            const auto col = std::min<std::uint64_t>(width - 1, start_x + ((std::uint64_t{x - left} * reg(video_step_x)) >> 20));
            const auto* pair = source + row * pitch + (col & ~1ULL) * 2;
            const auto luma = 298 * (pair[(col & 1) * 2] - 16);
            const auto cb = pair[1] - 128, cr = pair[3] - 128;
            pixel[0] = to_byte(luma + 409 * cr);
            pixel[1] = to_byte(luma - 100 * cb - 208 * cr);
            pixel[2] = to_byte(luma + 516 * cb);
            pixel[3] = std::byte{255};
        }
    }
}

void GuestGraphics::scanout() {
    using Clock = std::chrono::steady_clock;
    const bool capture = ++scanouts_ == capture_index_ && !capture_.empty();
    const bool diagnostic_capture = diagnostics_ && diagnostics_->capturing();
    const bool measuring = diagnostics_ && diagnostics_->measuring();
    if (measuring) diagnostics_->begin_gpu_work();
    const bool video = (video_registers_[(video_buffer - video_first) / 4] & 1) != 0;
    std::optional<Image> image;
    if (video && (window_ || capture || diagnostic_capture)) {
        image = renderer_->readback(true);
        composite_video(*image);
    }
    bool captured_display = false;
    bool displayed = false;
    if (window_) {
        if (std::exchange(gamma_dirty_, false)) renderer_->display_gamma(gamma_);
        displayed = renderer_->display(video ? &*image : nullptr);
    }
    if (measuring) diagnostics_->end_gpu_work();
    if (diagnostic_capture) {
        diagnostics_->capture_frame(*renderer_, *gpu_,
            {scanouts_, presentations_, overlay_frames_, vblanks_, 0, 0, gpu_->stats()},
            video ? &*image : nullptr, displayed, gamma_);
    }
    double present_ms = 0;
    if (displayed) {
        if (capture) {
            image = renderer_->read_display();
            captured_display = true;
        }
        const auto start = measuring ? Clock::now() : Clock::time_point{};
        renderer_->present(vsync_);
        if (measuring) present_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        ++presentations_;
    }
    if (capture) {
        if (!image) image = renderer_->readback(true);
        if (gamma_written_ && !captured_display) {
            for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
                for (unsigned channel = 0; channel < 3; ++channel) {
                    auto& sample = image->rgba[i + channel];
                    sample = gamma_[std::to_integer<unsigned>(sample)][channel];
                }
            }
        }
        save_png(*image, capture_);
        captured_ = true;
    }
    if (diagnostics_) {
        diagnostics_->scanout(*renderer_, *gpu_,
            {scanouts_, presentations_, overlay_frames_, vblanks_, 0, 0, gpu_->stats()},
            present_ms, capture || diagnostic_capture);
        if (submitting_) diagnostics_->begin_gpu_work();
    }
}

void GuestGraphics::submit() {
    const auto flags = memory_.load<Word>(device + 8);
    const auto end = memory_.load<Word>(device + ((flags & 4) ? 0x358 : 0));
    const auto start = memory_.load<Word>(push_start), limit = memory_.load<Word>(push_end);
    if (cursor_ < start || end < cursor_ || end > limit) throw std::runtime_error("D3D submission leaves the push buffer");
    if (cursor_ != end) {
        submitting_ = true;
        if (diagnostics_) diagnostics_->begin_gpu_work();
        gpu_->submit(cursor_, end);
        if (diagnostics_) diagnostics_->end_gpu_work();
        submitting_ = false;
        cursor_ = end;
    }
    const auto channel = memory_.load<Word>(device + 0x17F4);
    memory_.store<Word>(channel + 0x40, end & 0x0FFFFFFF);
    memory_.store<Word>(channel + 0x44, end & 0x0FFFFFFF);
}

bool GuestGraphics::invoke(PlatformApi api, Cpu& cpu) {
    const auto arg = [&](unsigned i) { return memory_.load<Word>(cpu.registers[esp] + 4 * (1 + i)); };
    const auto done = [&](unsigned count) { finish(cpu, count); return true; };
    if (api == PlatformApi::GraphicsInitialize) {
        cpu.registers[eax] = initialize(cpu, arg(0));
        return done(1);
    }
    if (!gpu_) return false;
    switch (api) {
    case PlatformApi::GraphicsKickOff:
    case PlatformApi::GraphicsKickOffWait:
        if (cpu.registers[ecx] != device) return false;
        submit();
        if (api == PlatformApi::GraphicsKickOffWait) gpu_->wait();
        return done(0);
    case PlatformApi::GraphicsBusy:
        cpu.registers[eax] = gpu_->busy();
        return done(0);
    case PlatformApi::GraphicsWait:
        submit();
        gpu_->wait();
        return done(2);
    case PlatformApi::GraphicsReserve: {
        const auto flags = memory_.load<Word>(device + 8), cursor = memory_.load<Word>(device);
        if (flags & 4) {
            const auto buffer = memory_.load<Word>(device + 0x34C), base = memory_.load<Word>(buffer + 4);
            memory_.store(device + 0x350, memory_.load<Word>(device + 0x350) + cursor - base);
            memory_.store(device, base);
            cpu.registers[eax] = base;
        } else {
            const auto minimum = arg(0), maximum = arg(1);
            const auto begin = memory_.load<Word>(push_start), end = memory_.load<Word>(push_end);
            if (cursor < begin || cursor > end || end - begin <= 0x204 || minimum > maximum || maximum > end - begin - 0x204) {
                throw NativeBoundary(cpu.eip, "Invalid D3D push buffer reservation");
            }
            auto next = cursor;
            if (maximum + 0x204 > end - cursor) {
                submit();
                gpu_->wait();
                next = cursor_ = begin;
                memory_.store(device, next);
                memory_.store<Word>(device + 0x40, cursor - begin);
            }
            memory_.store(device + 4, next + maximum - 0x204);
            cpu.registers[eax] = next;
        }
        return done(2);
    }
    case PlatformApi::GraphicsTile: {
        const auto index = arg(0), descriptor = arg(1);
        if (index >= 8) throw NativeBoundary(cpu.eip, "Invalid D3D tile index");
        const auto target = device + 0x1694 + 24 * index;
        if (!descriptor || !memory_.load<Word>(descriptor + 4)) {
            memory_.fill_elements<Word>(target, 0, 6, false);
        } else {
            const auto address = memory_.load<Word>(descriptor + 4);
            const auto size = memory_.load<Word>(descriptor + 8), pitch = memory_.load<Word>(descriptor + 12);
            if (!size || !pitch || (pitch & 63)) throw NativeBoundary(cpu.eip, "Invalid tiled surface allocation");
            memory_.access(address, size);
            std::memmove(memory_.access(target, 24), memory_.access(descriptor, 24), 24);
            if (!(memory_.load<Word>(descriptor) & 0x80000000U)) {
                memory_.store<Word>(target + 16, 0);
                memory_.store<Word>(target + 20, 0);
            }
        }
        return done(2);
    }
    default: return false;
    }
}
}
