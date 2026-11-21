#pragma once

#include "file.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace xbox_native {
using ShaDigest = std::array<std::byte, 20>;

// The original SDK copies the hash context, so it holds guest data only.
struct Sha1 {
    std::array<std::uint32_t, 6> reserved{};
    std::array<std::uint32_t, 5> state{};
    std::array<std::uint32_t, 2> count{};
    std::array<std::byte, 64> buffer{};

    void reset() {
        state = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        count = {};
        buffer = {};
    }
    void update(Bytes input) {
        const auto previous = bit_count();
        const auto next = previous + 8 * static_cast<std::uint64_t>(input.size());
        count = {static_cast<std::uint32_t>(next), static_cast<std::uint32_t>(next >> 32)};
        const auto used = static_cast<std::size_t>((previous >> 3) & 63);
        if (used) {
            const auto copied = std::min(buffer.size() - used, input.size());
            std::ranges::copy(input.first(copied), buffer.begin() + used);
            input = input.subspan(copied);
            if (used + copied < buffer.size()) return;
            block(buffer);
        }
        while (input.size() >= buffer.size()) {
            block(input.first(buffer.size()));
            input = input.subspan(buffer.size());
        }
        std::ranges::copy(input, buffer.begin());
    }
    ShaDigest finish() {
        std::array<std::byte, 72> pad{std::byte{0x80}};
        const auto occupied = static_cast<unsigned>((bit_count() >> 3) & 63);
        const auto length = (occupied < 56 ? 56 : 120) - occupied;
        const auto bits = std::byteswap(bit_count());
        std::memcpy(pad.data() + length, &bits, sizeof(bits));
        update(Bytes(pad).first(length + sizeof(bits)));
        ShaDigest digest{};
        for (unsigned i=0;i<state.size();++i) {
            const auto word = std::byteswap(state[i]);
            std::memcpy(digest.data() + i * sizeof(word), &word, sizeof(word));
        }
        state = {};
        count = {};
        buffer = {};
        return digest;
    }
private:
    std::uint64_t bit_count() const { return (std::uint64_t{count[1]} << 32) | count[0]; }
    void block(Bytes bytes) {
        std::array<std::uint32_t, 80> words;
        for (unsigned i=0;i<16;++i) {
            std::uint32_t word;
            std::memcpy(&word, bytes.data() + i * sizeof(word), sizeof(word));
            words[i] = std::byteswap(word);
        }
        for (unsigned i=16;i<words.size();++i) words[i] = std::rotl(words[i-3] ^ words[i-8] ^ words[i-14] ^ words[i-16], 1);
        auto [a,b,c,d,e] = state;
        for (unsigned i=0;i<words.size();++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const auto next = std::rotl(a,5) + f + e + k + words[i];
            e=d; d=c; c=std::rotl(b,30); b=a; a=next;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
    }
};
static_assert(sizeof(Sha1) == 116);

inline ShaDigest xbox_hmac(Bytes key, Bytes first, Bytes second) {
    std::array<std::byte, 64> pad{};
    std::ranges::copy(key.first(std::min(pad.size(), key.size())), pad.begin());
    for (auto& byte : pad) byte ^= std::byte{0x36};
    Sha1 sha;
    sha.reset();
    sha.update(pad);
    sha.update(first);
    sha.update(second);
    const auto inner = sha.finish();
    for (auto& byte : pad) byte ^= std::byte{0x36 ^ 0x5C};
    sha.reset();
    sha.update(pad);
    sha.update(inner);
    return sha.finish();
}
}
