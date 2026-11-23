#pragma once
#include "audio.h"
#include "cpu.h"
#include "guest_allocator.h"

#include <map>
#include <deque>
#include <memory>
#include <optional>

namespace xbox_native {
class Disc;
enum class PlatformApi : std::uint32_t;

class GuestAudio {
public:
    GuestAudio(Memory& memory, GuestAllocator allocator, bool output)
        : memory_(memory), allocator_(allocator), output_(output) {}
    void prepare_disc_effects(Disc& disc);
    std::uint64_t gameplay_effect_blocks() const { return audio_ ? audio_->gameplay_effect_blocks() : 0; }
    std::uint64_t prepared_effect_installs() const { return prepared_installs_; }
    std::uint64_t startup_effect_blocks() const { return startup_effect_blocks_; }
    bool invoke(PlatformApi api, Cpu& cpu, std::uint64_t checkpoints);
    void output_gain(float gain) {
        if (output_gain_ == gain) return;
        output_gain_ = gain;
        if (audio_) audio_->output_gain(gain);
    }
    bool ready() const { return audio_ != nullptr; }
    bool output_ready() const { return audio_ && audio_->output_ready(); }
    std::uint64_t frames() const { return audio_ ? audio_->sample_clock() : 0; }
    std::uint64_t plays() const { return plays_; }
    std::uint64_t stream_packets() const { return stream_packets_; }
    std::uint64_t stream_completions() const { return stream_completions_; }
    std::uint32_t effect_count() const { return effect_count_; }
    float peak() const { return peak_; }
    float output_peak() const { return audio_ ? audio_->output_peak() : 0; }
    float peak_since_effect_load() const { return peak_since_effect_load_; }
    std::optional<AdpcmBlockError> first_invalid_adpcm_block() const {
        return audio_ ? audio_->first_invalid_adpcm_block() : std::nullopt;
    }
    std::optional<float> peak_since_capture() const {
        if (!capture_sample_ || mixed_until_ <= *capture_sample_) return {};
        return peak_since_capture_;
    }
    void mark_capture(std::uint64_t ticks);
private:
    struct Buffer {
        std::uint32_t voice{}, owned{}, data{}, size{};
        AudioFormat format;
    };
    struct Packet {
        std::uint32_t size, completed_size, status, context;
    };
    struct Stream {
        std::uint32_t voice{}, rate{}, capacity{}, callback{}, context{};
        std::uint64_t reported{};
        std::deque<Packet> pending;
        bool flushing{};
    };

    struct PreparedEffect {
        std::vector<std::byte> kernel, image;
        std::size_t scratch_size;
        std::shared_ptr<AudioEffects> processor;
    };
    std::vector<PreparedEffect> prepared_effects_;
    std::uint64_t prepared_installs_{}, startup_effect_blocks_{};
    Memory& memory_;
    GuestAllocator allocator_;
    bool output_;
    float output_gain_ = 1;
    std::unique_ptr<Audio> audio_;
    std::map<std::uint32_t, Buffer> buffers_;
    std::map<std::uint32_t, Stream> streams_;
    std::uint32_t device_{};
    std::uint32_t effect_image_{}, effect_count_{};
    std::uint64_t mixed_until_{}, plays_{};
    std::uint64_t stream_packets_{}, stream_completions_{};
    std::optional<std::uint64_t> capture_sample_;
    float peak_{}, peak_since_effect_load_{}, peak_since_capture_{};

    std::uint32_t allocate(std::uint32_t size) { return allocator_.allocate(allocator_.context, size); }
    void release(std::uint32_t address) { if (address) allocator_.release(allocator_.context, address); }
    AudioFormat format(std::uint32_t address) const;
    std::uint32_t configure_envelope(std::uint32_t voice, std::uint32_t descriptor, std::uint32_t site);
    void mix_bins(std::uint32_t id, std::uint32_t address, bool volumes_only);
    bool set_data(Buffer& buffer, std::uint32_t address, std::uint32_t size);
    void tick(std::uint64_t checkpoints);
    std::uint32_t call_guest(Cpu& cpu, std::uint32_t address, std::span<const std::uint32_t> arguments);
    std::uint32_t load_effects(Cpu& cpu, std::uint32_t image, std::uint32_t size,
                               std::uint32_t locations, std::uint32_t output, bool prepare = false);
    void complete_packet(Cpu& cpu, std::uint32_t callback, std::uint32_t context, const Packet& packet, std::uint32_t status);
    void service_streams(Cpu& cpu, bool work = false);
    void finish(Cpu& cpu, unsigned count, std::optional<std::uint32_t> result = {});
};
}
