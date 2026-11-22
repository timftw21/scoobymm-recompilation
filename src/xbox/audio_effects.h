#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace xbox_native {
// The Xbox global processor exchanges one block of 32 samples in each of 32 mix bins.
class AudioEffects {
public:
    AudioEffects(std::span<const std::byte> kernel, std::span<const std::byte> image, std::size_t scratch_size);
    ~AudioEffects();
    std::span<std::uint32_t, 1024> mix_bins();
    void process();
private:
    struct Processor;
    std::unique_ptr<Processor> processor_;
};
}
