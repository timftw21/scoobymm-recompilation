#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace native_x86 {

enum class Alu { add, sub, bit_and, bit_or, bit_xor, increment, decrement };

constexpr std::uint32_t mask(unsigned bits) {
    return 0xffffffffU >> (32 - bits);
}

struct Cpu {
    // EAX, ECX, EDX, EBX, ESP, EBP, ESI and EDI.
    std::array<std::uint32_t, 8> gpr{};
    bool carry{}, parity{}, auxiliary{}, zero{}, sign{}, overflow{};
    std::span<std::byte> memory;
    std::uint32_t memory_base{};

    void write_register(unsigned index, unsigned bits, unsigned shift, std::uint32_t value) {
        const auto part = mask(bits);
        gpr[index] = (gpr[index] & ~(part << shift)) | ((value & part) << shift);
    }

    std::uint32_t load(std::uint32_t address, unsigned bits) const {
        const auto bytes = access(address, bits);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < bytes.size(); ++i) {
            value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
        }
        return value;
    }

    void store(std::uint32_t address, unsigned bits, std::uint32_t value) {
        const auto bytes = access(address, bits);
        for (unsigned i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffU);
        }
    }

    void push32(std::uint32_t value) {
        const auto address = gpr[4] - 4;
        store(address, 32, value);
        gpr[4] = address;
    }

    std::uint32_t pop32() {
        const auto value = load(gpr[4], 32);
        gpr[4] += 4;
        return value;
    }

    std::uint32_t alu(Alu operation, unsigned bits, std::uint32_t left, std::uint32_t right) {
        const auto width = mask(bits);
        const auto sign_bit = std::uint32_t{1} << (bits - 1);
        left &= width;
        right &= width;
        std::uint32_t result{};
        switch (operation) {
        case Alu::add:
        case Alu::increment: {
            const auto sum = std::uint64_t{left} + right;
            result = static_cast<std::uint32_t>(sum) & width;
            if (operation == Alu::add) carry = sum > width;
            overflow = (~(left ^ right) & (left ^ result) & sign_bit) != 0;
            auxiliary = ((left ^ right ^ result) & 0x10) != 0;
            break;
        }
        case Alu::sub:
        case Alu::decrement:
            result = (left - right) & width;
            if (operation == Alu::sub) carry = left < right;
            overflow = ((left ^ right) & (left ^ result) & sign_bit) != 0;
            auxiliary = ((left ^ right ^ result) & 0x10) != 0;
            break;
        case Alu::bit_and:
        case Alu::bit_or:
        case Alu::bit_xor:
            if (operation == Alu::bit_and) result = left & right;
            if (operation == Alu::bit_or) result = left | right;
            if (operation == Alu::bit_xor) result = left ^ right;
            carry = false;
            overflow = false;
            break;
        }
        zero = result == 0;
        sign = (result & sign_bit) != 0;
        parity = std::popcount(result & 0xffU) % 2 == 0;
        return result;
    }

private:
    std::span<std::byte> access(std::uint32_t address, unsigned bits) const {
        const auto offset = static_cast<std::uint64_t>(address) - memory_base;
        const auto size = bits / 8;
        if (address < memory_base || offset > memory.size() || size > memory.size() - offset ||
            std::uint64_t{address} + size > (std::uint64_t{1} << 32)) {
            throw std::out_of_range("x86 address is outside mapped memory");
        }
        return memory.subspan(static_cast<std::size_t>(offset), size);
    }
};

constexpr std::uint32_t sign_extend(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint32_t{1} << (bits - 1);
    return (value ^ sign) - sign;
}

std::uint32_t execute(Cpu& cpu, std::uint32_t entry);

} // namespace native_x86
