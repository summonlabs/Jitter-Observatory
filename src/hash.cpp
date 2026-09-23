// Jitter Observatory - SHA-256 and CRC-32C.
// Copyright 2026 Summon Software Labs.
#include <jitter/hash.hpp>

#include <jitter/error.hpp>

#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t count) noexcept {
    const std::uint32_t shift = count & 31u;
    return (value >> shift) | (value << ((32u - shift) & 31u));
}

constexpr std::uint32_t big_endian_u32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void store_big_endian_u32(std::uint8_t* p, std::uint32_t value) noexcept {
    p[0] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    p[1] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    p[2] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    p[3] = static_cast<std::uint8_t>(value & 0xffu);
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256u; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = ((c & 1u) != 0u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace

Sha256::Sha256() noexcept { reset(); }

void Sha256::reset() noexcept {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    buffer_.fill(0);
    buffered_ = 0;
    total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::uint32_t w[64]{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = big_endian_u32(block + (i * 4));
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    total_bytes_ += static_cast<std::uint64_t>(size);

    if (buffered_ > 0) {
        while (buffered_ < kBlockBytes && offset < size) {
            buffer_[buffered_++] = bytes[offset++];
        }
        if (buffered_ == kBlockBytes) {
            compress(buffer_.data());
            buffered_ = 0;
        }
    }

    while (offset + kBlockBytes <= size) {
        compress(bytes + offset);
        offset += kBlockBytes;
    }

    while (offset < size) {
        buffer_[buffered_++] = bytes[offset++];
    }
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
    update(data.data(), data.size());
}

void Sha256::update(std::string_view data) noexcept {
    update(data.data(), data.size());
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::finish() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8ull;
    const std::uint8_t pad = 0x80u;
    update(&pad, 1);

    const std::uint8_t zero = 0x00u;
    while (buffered_ != 56) {
        update(&zero, 1);
    }

    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>((bit_length >> ((7 - i) * 8)) & 0xffull);
    }
    update(length_bytes, 8);

    std::array<std::uint8_t, kDigestBytes> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        store_big_endian_u32(digest.data() + (i * 4), state_[i]);
    }
    reset();
    return digest;
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::hash(std::span<const std::uint8_t> data) noexcept {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::hash(std::string_view data) noexcept {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) {
        crc = kCrc32cTable[(crc ^ byte) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
    return crc32c_extend(0u, data);
}

std::uint32_t crc32c(std::string_view data) noexcept {
    return crc32c_extend(0u, std::span<const std::uint8_t>(
                                 reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

bool Digest::is_zero() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0u) {
            return false;
        }
    }
    return true;
}

std::string Digest::hex() const { return jitter::text::hex_encode(bytes_); }

Digest digest_of_hex(std::string_view hex) {
    Digest digest;
    if (hex.size() != Digest::kBytes * 2) {
        return digest;
    }
    std::array<std::uint8_t, Digest::kBytes> bytes{};
    for (std::size_t i = 0; i < Digest::kBytes; ++i) {
        const auto decoded = jitter::text::hex_nibble(hex[i * 2]);
        const auto low = jitter::text::hex_nibble(hex[(i * 2) + 1]);
        if (!decoded.has_value() || !low.has_value()) {
            return Digest{};
        }
        bytes[i] = static_cast<std::uint8_t>((decoded.value() << 4) | low.value());
    }
    return Digest(bytes);
}

}  // namespace jitter
