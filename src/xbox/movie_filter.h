#pragma once
#include "render.h"

namespace xbox_native {
// Stabilize isolated one-pixel shifts of previously still XMV blocks.
// The player retains one decoded frame. There is no presentation delay.
class MovieFilter {
public:
    Bytes frame(const VideoOverlay& video);
    void reset() { width_=0; }
private:
    struct Block {
        unsigned char still{}, held{};
        bool quiet{},hold{};
        signed char shift{};
    };
    unsigned width_{},height_{},pitch_{},movie_{};
    std::vector<std::byte> reference_;
    std::vector<Block> blocks_;
};
}
