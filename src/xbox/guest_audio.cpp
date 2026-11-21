#include "guest_audio.h"
#include "file.h"
#include "platform_bindings.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace xbox_native {
namespace {
using Word = std::uint32_t;
constexpr Word ds_device_slot = 0x000EAD50; // CDirectSound constructor at 0x000CDC57.
constexpr Word ds_device_table = 0x00152DF4;
constexpr Word ds_buffer_table = 0x00152E18; // CDirectSoundBuffer constructor at 0x000CEF7B.
constexpr Word ds_stream_table = 0x00152E30; // CDirectSoundStream constructor at 0x000CF4AD.
constexpr Word invalid_argument = 0x80070057, no_memory = 0x8007000E;
constexpr Word packet_pending = 0x8000000A, packet_flushed = 0x80004004;
constexpr Word completion_return = 0xFFFFFFD0;
}

void GuestAudio::finish(Cpu& cpu, unsigned count, std::optional<Word> result) {
    auto& stack = cpu.registers[esp];
    cpu.eip = memory_.load<Word>(stack);
    stack += (count + 1) * 4;
    if (result) cpu.registers[eax] = *result;
}

AudioFormat GuestAudio::format(Word address) const {
    AudioFormat result;
    result.tag = memory_.load<std::uint16_t>(address);
    result.channels = memory_.load<std::uint16_t>(address + 2);
    result.rate = memory_.load<Word>(address + 4);
    result.block_align = memory_.load<std::uint16_t>(address + 12);
    result.bits = memory_.load<std::uint16_t>(address + 14);
    // The SDK's Xbox ADPCM path at 0x000D0C15 uses fixed 64-sample blocks.
    if (result.tag == 0x69) result.samples_per_block = 64;
    return result;
}

Word GuestAudio::configure_envelope(Word voice, Word descriptor, Word site) {
    if (!descriptor) return invalid_argument;
    const auto kind = memory_.load<Word>(descriptor), mode = memory_.load<Word>(descriptor + 4);
    if (mode > 3 || kind > 1) return invalid_argument;
    if (kind == 0) {
        if (mode && (memory_.load<Word>(descriptor + 32) || memory_.load<Word>(descriptor + 36)))
            throw NativeBoundary(site, "Multifunction sound envelope changes pitch or filter cutoff");
    } else {
        const auto frames = [&](Word offset) { return (memory_.load<Word>(descriptor + offset) & 0xFFF) * 512; };
        audio_->envelope(voice, mode ? AmplitudeEnvelope{
            mode == 1 ? frames(8) : 0, mode != 3 ? frames(12) : 0, frames(16), frames(20),
            float(memory_.load<Word>(descriptor + 28) & 0xFF) / 255.0f, frames(24)} : AmplitudeEnvelope{});
    }
    return 0;
}

void GuestAudio::mix_bins(Word id, Word address, bool volumes_only) {
    if (!address && !volumes_only) {
        constexpr std::array<MixBin, 2> stereo{{{0, 0}, {1, 0}}};
        audio_->mixbins(id, stereo);
        return;
    }
    const auto count = memory_.load<Word>(address);
    const auto pairs = memory_.load<Word>(address + 4);
    std::array<MixBin, 32> bins;
    if (count > (volumes_only ? bins.size() : 8)) throw std::runtime_error("Invalid sound mix bin count");
    for (Word i = 0; i < count; ++i) {
        bins[i] = {memory_.load<Word>(pairs + i * 8),
                   std::bit_cast<std::int32_t>(memory_.load<Word>(pairs + i * 8 + 4))};
    }
    audio_->mixbins(id, std::span(bins).first(count), volumes_only);
}

bool GuestAudio::set_data(Buffer& buffer, Word address, Word size) {
    const auto owned = !address && size ? allocate(size) : 0;
    if (!address && size && !owned) return false;
    if (owned) address = owned;
    try {
        const auto bytes = size ? std::span(static_cast<const std::byte*>(memory_.access(address, size)), size) : Bytes{};
        audio_->data(buffer.voice, bytes, address);
    } catch (...) {
        release(owned);
        throw;
    }
    release(std::exchange(buffer.owned, owned));
    buffer.data = address;
    buffer.size = size;
    return true;
}

void GuestAudio::mark_capture(std::uint64_t ticks) {
    if (!output_ && !capture_sample_) capture_sample_ = ticks * 48 / 100;
}

void GuestAudio::tick(std::uint64_t checkpoints) {
    if (const auto failure = audio_->failure(); !failure.empty()) throw std::runtime_error(failure);
    if (output_) return;
    const auto target = checkpoints * 48 / 100; // 48 kHz with the boot harness's 10 us ticks.
    std::array<float, 1024> samples;
    while (mixed_until_ < target) {
        const auto count = std::min<std::uint64_t>(target - mixed_until_, samples.size() / 2);
        const auto chunk = std::span(samples).first(count * 2);
        audio_->mix(chunk);
        float chunk_peak = 0;
        for (const float sample : chunk) chunk_peak = std::max(chunk_peak, std::abs(sample));
        peak_ = std::max(peak_, chunk_peak);
        if (effect_count_) peak_since_effect_load_ = std::max(peak_since_effect_load_, chunk_peak);
        if (capture_sample_ && mixed_until_ + count > *capture_sample_) {
            if (mixed_until_ >= *capture_sample_) peak_since_capture_ = std::max(peak_since_capture_, chunk_peak);
            else for (const float sample : chunk.subspan((*capture_sample_ - mixed_until_) * 2))
                peak_since_capture_ = std::max(peak_since_capture_, std::abs(sample));
        }
        mixed_until_ += count;
    }
}

Word GuestAudio::call_guest(Cpu& cpu, Word address, std::span<const Word> arguments) {
    const auto registers = cpu.registers;
    const auto flags = cpu.flags, pc = cpu.eip;
    const auto stack = registers[esp] - static_cast<Word>((arguments.size() + 1) * 4);
    memory_.store(stack, completion_return);
    for (std::size_t i = 0; i < arguments.size(); ++i)
        memory_.store(stack + 4 + static_cast<Word>(i * 4), arguments[i]);
    cpu.registers[esp] = stack;
    invoke_native(cpu, memory_, address);
    if (cpu.eip != completion_return || cpu.registers[esp] != registers[esp]) {
        throw NativeBoundary(address, "Original sound routine returned with an unexpected stack");
    }
    const auto result = cpu.registers[eax];
    cpu.registers = registers;
    cpu.flags = flags;
    cpu.eip = pc;
    return result;
}

Word GuestAudio::load_effects(Cpu& cpu, Word image, Word size, Word locations, Word output) {
    // The original SDK reads the image header at 0x800 and locates the map after its code and state.
    constexpr Word payload = 0x818, map_size = 32, scratch_base = 0xC000;
    if (!image || size < payload + 8 || size > 1024 * 1024) return invalid_argument;
    const auto bytes = Bytes(static_cast<const std::byte*>(memory_.access(image, size)), size);
    const auto map_offset = std::uint64_t{payload} + (std::uint64_t{u32(bytes, 0x804)} + u32(bytes, 0x80C)) * 4;
    if (map_offset > size - 8) return invalid_argument;
    const auto count = u32(bytes, map_offset), scratch = u32(bytes, map_offset + 4);
    if (!count || count > 32 || scratch > 2 * 1024 * 1024 || count * (map_size + 8) > size - map_offset - 8)
        return invalid_argument;
    const auto reverb = locations ? memory_.load<Word>(locations) : UINT32_MAX;
    const auto crosstalk = locations ? memory_.load<Word>(locations + 4) : UINT32_MAX;
    if ((reverb != UINT32_MAX && reverb >= count) || (crosstalk != UINT32_MAX && crosstalk >= count))
        return invalid_argument;
    Word y_bytes = 0, code_end = payload;
    for (Word i = 0; i < count; ++i) {
        const auto entry = map_offset + 8 + i * map_size;
        const auto code = u32(bytes, entry), length = u32(bytes, entry + 4);
        const auto state = u32(bytes, entry + 8), state_size = u32(bytes, entry + 12);
        const auto y = u32(bytes, entry + 16), y_length = u32(bytes, entry + 20);
        const auto scratch_at = u32(bytes, entry + 24), scratch_size = u32(bytes, entry + 28);
        if (code != code_end || code > map_offset || length > map_offset - code ||
            state > map_offset || state_size > map_offset - state || y > 65536 || y_length > 65536 - y ||
            scratch_at < scratch_base || scratch_at - scratch_base > scratch || scratch_size > scratch - (scratch_at - scratch_base))
            return invalid_argument;
        code_end = code + length;
        y_bytes = std::max(y_bytes, y + y_length);
    }
    const auto aligned_size = (size + 15) & ~15U;
    const auto base = allocate(aligned_size + 8 + y_bytes + scratch);
    if (!base) return no_memory;
    const auto key = base + aligned_size;
    const auto descriptor = base + static_cast<Word>(map_offset);
    const auto keys = descriptor + 8 + count * map_size;
    const auto y_base = key + 8, scratch_at = y_base + y_bytes;
    try {
        std::memcpy(memory_.access(base, size), bytes.data(), size);
        const std::array init_key{key};
        call_guest(cpu, 0x000D6440, init_key);
        const std::array decode_keys{key, keys, count * 8, keys, 0U};
        auto result = call_guest(cpu, 0x000D646D, decode_keys);
        for (Word i = 0; !result && i < count; ++i) {
            const auto entry = descriptor + 8 + i * map_size;
            const auto code = base + memory_.load<Word>(entry);
            const std::array decode{keys + i * 8, code, memory_.load<Word>(entry + 4), code, 0U};
            result = call_guest(cpu, 0x000D646D, decode);
            memory_.store(entry, code);
            memory_.store(entry + 8, base + memory_.load<Word>(entry + 8));
            memory_.store(entry + 16, y_base + memory_.load<Word>(entry + 16));
            memory_.store(entry + 24, scratch_at + memory_.load<Word>(entry + 24) - scratch_base);
        }
        if (result) {
            release(base);
            return result;
        }
        if (output) memory_.store(output, descriptor);
    } catch (...) {
        release(base);
        throw;
    }
    release(std::exchange(effect_image_, base));
    effect_count_ = count;
    peak_since_effect_load_ = 0;
    return 0;
}

void GuestAudio::complete_packet(Cpu& cpu, Word callback, Word context, const Packet& packet, Word status) {
    if (packet.completed_size) memory_.store(packet.completed_size, status ? 0 : packet.size);
    if (packet.status) memory_.store(packet.status, status);
    if (!status) ++stream_completions_;
    if (callback) {
        const std::array arguments{context, packet.context, status};
        call_guest(cpu, callback, arguments);
    } else if (packet.context) {
        const std::array arguments{packet.context,0U};
        call_guest(cpu, 0xFFF00000U + 225 * 16, arguments); // NtSetEvent
    }
}

void GuestAudio::service_streams(Cpu& cpu, bool work) {
    struct Completion { Word callback, context; Packet packet; Word status; };
    std::vector<Completion> completions;
    for (auto& [address, stream] : streams_) {
        const bool flush = work && stream.flushing && !(audio_->status(stream.voice) & 1U);
        const auto completed = flush ? audio_->flush_packets(stream.voice) : audio_->completed_packets(stream.voice);
        while (stream.reported < completed) {
            completions.push_back({stream.callback, stream.context, stream.pending.front(), 0});
            stream.pending.pop_front();
            ++stream.reported;
        }
        if (flush) {
            stream.flushing = false;
            for (const auto& packet : stream.pending) completions.push_back({stream.callback, stream.context, packet, packet_flushed});
            stream.pending.clear();
        }
    }
    for (const auto& completion : completions)
        complete_packet(cpu, completion.callback, completion.context, completion.packet, completion.status);
}

bool GuestAudio::invoke(PlatformApi api, Cpu& cpu, std::uint64_t checkpoints) {
    const auto arg = [&](unsigned index) { return memory_.load<Word>(cpu.registers[esp] + 4 + index * 4); };
    const auto done = [&](unsigned count, std::optional<Word> result = {}) {
        finish(cpu, count, result);
        return true;
    };
    if (api == PlatformApi::SoundDeviceAcquire) {
        const auto output = arg(0);
        if (!output) return done(1, invalid_argument);
        if (!audio_) {
            audio_ = std::make_unique<Audio>(output_);
            mixed_until_ = checkpoints * 48 / 100;
        }
        if (!device_) {
            device_ = allocate(44);
            if (!device_) return done(1, no_memory);
            memory_.store(device_, ds_device_table);
            memory_.store<Word>(device_ + 4, 1);
            memory_.store(device_ + 16, device_ + 16);
            memory_.store(device_ + 20, device_ + 16);
            memory_.store(ds_device_slot, device_);
        }
        memory_.store(device_ + 4, memory_.load<Word>(device_ + 4) + 1);
        memory_.store(output, device_);
        return done(1, 0);
    }
    if (!audio_) return false;
    tick(checkpoints);
    if (api == PlatformApi::SoundDeviceWork) {
        if (arg(0) != device_) return false;
        service_streams(cpu, true);
        return done(1);
    }
    if (api == PlatformApi::SoundDeviceSynchronize) {
        if (arg(0) != device_) return false;
        audio_->synchronize_playback();
        return done(1, 0);
    }
    if (api == PlatformApi::SoundDeviceLoadEffects) {
        if (arg(0) != device_) return false;
        return done(5, load_effects(cpu, arg(1), arg(2), arg(3), arg(4)));
    }
    if (api == PlatformApi::SoundStreamCreate) {
        const auto descriptor = arg(1), output = arg(2);
        if (arg(0) != device_ || !descriptor || !output || arg(3)) return done(4, invalid_argument);
        const auto capacity = memory_.load<Word>(descriptor + 4);
        const auto wave = memory_.load<Word>(descriptor + 8);
        if (!wave || !capacity) return done(4, invalid_argument);
        const auto raw = allocate(40);
        if (!raw) return done(4, no_memory);
        Stream stream;
        try {
            stream.capacity = capacity;
            stream.callback = memory_.load<Word>(descriptor + 12);
            stream.context = memory_.load<Word>(descriptor + 16);
            const auto pcm = format(wave);
            stream.rate = pcm.rate;
            stream.voice = audio_->create(pcm);
            audio_->headroom(stream.voice, 600);
            const auto bins = memory_.load<Word>(descriptor + 20);
            if (bins) mix_bins(stream.voice, bins, false);
            streams_.emplace(raw, std::move(stream));
        } catch (...) {
            if (stream.voice) audio_->release(stream.voice);
            release(raw);
            throw;
        }
        memory_.store(raw, ds_stream_table);
        memory_.store<Word>(raw + 4, 0x00152E24);
        memory_.store<Word>(raw + 8, 1);
        memory_.store(raw + 12, device_);
        memory_.store(output, raw);
        return done(4, 0);
    }
    if (api == PlatformApi::SoundBufferCreate) {
        const auto descriptor = arg(1), output = arg(2);
        if (arg(0) != device_ || !descriptor || !output || arg(3) || memory_.load<Word>(descriptor) != 24) {
            return done(4, invalid_argument);
        }
        const auto flags = memory_.load<Word>(descriptor + 4);
        const auto wave = memory_.load<Word>(descriptor + 12);
        if (!wave || (flags & 0x182000U)) throw NativeBoundary(cpu.eip, "Sound buffer needs an external format or effects routing");
        const auto pcm = format(wave);
        const auto raw = allocate(36);
        if (!raw) return done(4, no_memory);
        Buffer buffer{};
        try {
            buffer.format = pcm;
            buffer.voice = audio_->create(pcm);
            audio_->headroom(buffer.voice, (flags & 0x10) ? 0 : 600);
            const auto bins = memory_.load<Word>(descriptor + 16);
            if (bins) mix_bins(buffer.voice, bins, false);
            const auto size = memory_.load<Word>(descriptor + 8);
            if (size && !set_data(buffer, 0, size)) {
                audio_->release(buffer.voice);
                release(raw);
                return done(4, no_memory);
            }
        } catch (...) {
            if (buffer.voice) audio_->release(buffer.voice);
            release(raw);
            throw;
        }
        memory_.store(raw, ds_buffer_table);
        memory_.store<Word>(raw + 4, 1);
        memory_.store(raw + 8, device_);
        buffers_.emplace(raw, buffer);
        memory_.store(output, raw + 28);
        return done(4, 0);
    }
    const auto object = arg(0);
    if (api == PlatformApi::SoundStreamFrequency || api == PlatformApi::SoundStreamMixBins ||
        api == PlatformApi::SoundStreamMixVolumes || api == PlatformApi::SoundStreamEnvelope) {
        const auto found = streams_.find(object);
        if (found == streams_.end()) return false;
        const auto& stream = found->second;
        const auto value = arg(1);
        if (api == PlatformApi::SoundStreamFrequency) audio_->frequency(stream.voice, value ? value : stream.rate);
        else if (api == PlatformApi::SoundStreamEnvelope) return done(2, configure_envelope(stream.voice, value, cpu.eip));
        else mix_bins(stream.voice, value, api == PlatformApi::SoundStreamMixVolumes);
        return done(2, 0);
    }
    if (api == PlatformApi::SoundStreamProcess || api == PlatformApi::SoundStreamRelease ||
        api == PlatformApi::SoundStreamDiscontinuity || api == PlatformApi::SoundStreamPause ||
        api == PlatformApi::SoundStreamFlush || api == PlatformApi::SoundStreamFlushEx ||
        api == PlatformApi::SoundStreamStatus) {
        if (!streams_.contains(object)) return false;
        service_streams(cpu);
        const auto found = streams_.find(object);
        if (found == streams_.end()) throw NativeBoundary(cpu.eip, "Audio completion callback released the active stream");
        auto& stream = found->second;
        if (api == PlatformApi::SoundStreamStatus) {
            if (!arg(1)) return done(2, invalid_argument);
            memory_.store(arg(1), audio_->stream_status(stream.voice) | (stream.pending.size() < stream.capacity ? 1U : 0U) |
                                       (stream.flushing ? 0x80000U : 0));
            return done(2, 0);
        }
        if (api == PlatformApi::SoundStreamPause) {
            const auto mode = arg(1);
            if (mode > 2) throw NativeBoundary(cpu.eip, "Unsupported sound stream pause mode");
            audio_->pause(stream.voice, mode);
            return done(2, 0);
        }
        if (api == PlatformApi::SoundStreamDiscontinuity) {
            audio_->end_segment(stream.voice);
            return done(1, 0);
        }
        if (api == PlatformApi::SoundStreamFlush || api == PlatformApi::SoundStreamFlushEx) {
            const auto extended = api == PlatformApi::SoundStreamFlushEx;
            const auto flags = extended ? arg(3) : 0;
            if (extended && (arg(1) || arg(2) || (flags & ~3U)))
                throw NativeBoundary(cpu.eip, "Unsupported timed audio stream flush or sealed release segment");
            if (flags) {
                if (flags & 2) audio_->release_envelope(stream.voice, false);
                else audio_->stop(stream.voice);
                stream.flushing = true;
                return done(4, 0);
            }
            stream.flushing = false;
            const auto completed = audio_->flush_packets(stream.voice);
            auto position = std::exchange(stream.reported, completed);
            const auto callback = stream.callback, context = stream.context;
            auto packets = std::exchange(stream.pending, {});
            for (const auto& packet : packets)
                complete_packet(cpu, callback, context, packet, position++ < completed ? 0 : packet_flushed);
            return done(extended ? 4 : 1, 0);
        }
        if (api == PlatformApi::SoundStreamRelease) {
            const auto references = memory_.load<Word>(object + 8) - 1;
            memory_.store(object + 8, references);
            if (!references) {
                const auto retired = std::move(stream);
                streams_.erase(found);
                const auto completed = audio_->flush_packets(retired.voice);
                audio_->release(retired.voice);
                auto position = retired.reported;
                for (const auto& packet : retired.pending)
                    complete_packet(cpu, retired.callback, retired.context, packet, position++ < completed ? 0 : packet_flushed);
                release(object);
            }
            return done(1, references);
        }
        const auto input = arg(1);
        if (!input || arg(2)) return done(3, invalid_argument);
        if (stream.pending.size() >= stream.capacity) return done(3, no_memory);
        const auto data = memory_.load<Word>(input);
        const Packet packet{memory_.load<Word>(input + 4), memory_.load<Word>(input + 8),
                            memory_.load<Word>(input + 12), memory_.load<Word>(input + 16)};
        if (!data || !packet.size) return done(3, invalid_argument);
        const auto bytes = std::span(static_cast<const std::byte*>(memory_.access(data, packet.size)), packet.size);
        stream.pending.push_back(packet);
        try {
            audio_->queue_packet(stream.voice, bytes, data);
        } catch (...) {
            stream.pending.pop_back();
            throw;
        }
        if (packet.completed_size) memory_.store<Word>(packet.completed_size, 0);
        if (packet.status) memory_.store(packet.status, packet_pending);
        ++stream_packets_;
        return done(3, 0);
    }
    const auto found = buffers_.find(object);
    if (found == buffers_.end()) return false;
    auto& buffer = found->second;
    const auto id = buffer.voice;
    switch (api) {
    case PlatformApi::SoundBufferRelease: {
        const auto references = memory_.load<Word>(object + 4) - 1;
        memory_.store(object + 4, references);
        if (!references) {
            audio_->release(id);
            release(buffer.owned);
            buffers_.erase(found);
            release(object);
        }
        return done(1, references);
    }
    case PlatformApi::SoundBufferData:
        return done(3, set_data(buffer, arg(1), arg(2)) ? 0 : no_memory);
    case PlatformApi::SoundBufferPlay:
    case PlatformApi::SoundBufferPlayEx: {
        const auto flags = arg(3);
        if (arg(1) || arg(2) || (flags & ~7U)) throw NativeBoundary(cpu.eip, "Unsupported timed or flagged sound buffer play");
        audio_->play(id, (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0);
        ++plays_;
        return done(4, 0);
    }
    case PlatformApi::SoundBufferStop:
        audio_->stop(id);
        return done(1, 0);
    case PlatformApi::SoundBufferStopEx: {
        const auto flags = arg(3);
        if (arg(1) || arg(2) || (flags & ~3U)) throw NativeBoundary(cpu.eip, "Unsupported timed or flagged sound buffer stop");
        if (flags & 1) audio_->release_envelope(id, (flags & 2) != 0);
        else audio_->stop(id);
        return done(4, 0);
    }
    case PlatformApi::SoundBufferPause:
        return done(2, audio_->pause_buffer(id, arg(1)));
    case PlatformApi::SoundBufferStatus:
        memory_.store(arg(1), audio_->status(id));
        return done(2, 0);
    case PlatformApi::SoundBufferFrequency:
        audio_->frequency(id, arg(1) ? arg(1) : buffer.format.rate);
        return done(2, 0);
    case PlatformApi::SoundBufferEnvelope:
        return done(2, configure_envelope(id, arg(1), cpu.eip));
    case PlatformApi::SoundBufferPlayRegion:
        audio_->region(id, false, arg(1), arg(2));
        return done(3, 0);
    case PlatformApi::SoundBufferMixBins:
    case PlatformApi::SoundBufferMixVolumes:
        mix_bins(id, arg(1), api == PlatformApi::SoundBufferMixVolumes);
        return done(2, 0);
    default: return false;
    }
}
}
