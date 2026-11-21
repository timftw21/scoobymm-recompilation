#include "file.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#ifdef _WIN32
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace xbox_native {
File::File(const std::filesystem::path& path) : stream_(path, std::ios::binary) {
    if (!stream_) throw std::runtime_error("Cannot open " + path_text(path));
    size_ = std::filesystem::file_size(path);
}

void File::check(std::uint64_t offset, std::uint64_t count) const {
    if (offset > size_ || count > size_ - offset)
        throw std::runtime_error(std::format("Read outside file: offset {}, size {}", offset, count));
}

void File::read(std::uint64_t offset, std::span<std::byte> output) {
    check(offset, output.size());
    if (output.empty()) return;
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("Read is too large");
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!stream_) throw std::runtime_error("File read failed");
}

std::vector<std::byte> File::read(std::uint64_t offset, std::size_t count) {
    check(offset, count);
    std::vector<std::byte> bytes(count);
    read(offset, bytes);
    return bytes;
}

template<class T> T little(Bytes bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("Truncated binary field");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return value;
}
std::uint32_t u32(Bytes bytes, std::size_t offset) { return little<std::uint32_t>(bytes, offset); }
std::uint16_t u16(Bytes bytes, std::size_t offset) { return little<std::uint16_t>(bytes, offset); }

std::string path_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string json(std::string_view text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) result += std::format("\\u{:04X}", c);
            else result += static_cast<char>(c);
        }
    }
    return result + '"';
}

std::string hex32(std::uint32_t value) { return std::format("0x{:08X}", value); }
std::string hex_bytes(Bytes bytes) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned>(bytes[i]);
        result[i * 2] = digits[value >> 4];
        result[i * 2 + 1] = digits[value & 15];
    }
    return result;
}

namespace {
#ifdef _WIN32
void crypto_check(NTSTATUS status) {
    if (status < 0) throw std::runtime_error(std::format("Windows hash error: 0x{:08X}",
                                                        static_cast<std::uint32_t>(status)));
}
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Hash {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~Hash() { if (handle) BCryptDestroyHash(handle); }
};
#endif
constexpr std::size_t chunk_size = 256 * 1024;
}

std::string sha256(File& file, std::uint64_t offset, std::uint64_t length) {
    file.check(offset, length);
    const auto end = offset + length;
#ifdef _WIN32
    Algorithm algorithm;
    crypto_check(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    Hash hash;
    crypto_check(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0));
#else
    struct Hash {
        EVP_MD_CTX* handle = EVP_MD_CTX_new();
        ~Hash() { EVP_MD_CTX_free(handle); }
    } hash;
    const auto check = [](int result) {
        if (result != 1) throw std::runtime_error("Native hash failed");
    };
    if (!hash.handle) throw std::runtime_error("Cannot allocate native hash");
    check(EVP_DigestInit_ex(hash.handle, EVP_sha256(), nullptr));
#endif
    std::vector<std::byte> buffer(chunk_size);
    while (offset < end) {
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(end - offset, buffer.size()));
        file.read(offset, std::span(buffer).first(size));
#ifdef _WIN32
        crypto_check(BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(buffer.data()),
                                    static_cast<ULONG>(size), 0));
#else
        check(EVP_DigestUpdate(hash.handle, buffer.data(), size));
#endif
        offset += size;
    }
    std::array<std::byte, 32> digest{};
#ifdef _WIN32
    crypto_check(BCryptFinishHash(hash.handle, reinterpret_cast<PUCHAR>(digest.data()), digest.size(), 0));
#else
    unsigned written = 0;
    check(EVP_DigestFinal_ex(hash.handle, reinterpret_cast<unsigned char*>(digest.data()), &written));
    if (written != digest.size()) throw std::runtime_error("Invalid native hash size");
#endif
    return hex_bytes(digest);
}

void write_text(const std::filesystem::path& output, std::string_view text) {
    std::ofstream stream(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!stream) throw std::runtime_error("Cannot create " + path_text(output));
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream) throw std::runtime_error("Could not write " + path_text(output));
}
}
