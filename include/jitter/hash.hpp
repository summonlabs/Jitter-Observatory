// Jitter Observatory - deterministic hashing and integrity checksums.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace jitter {

// SHA-256 (FIPS 180-4). Used for content addressed identities and for the
// persistence integrity chain. Verification vectors live in the unit tests.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;
    static constexpr std::size_t kBlockBytes = 64;

    Sha256() noexcept;

    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view data) noexcept;
    void update(const void* data, std::size_t size) noexcept;

    // Finalises and returns the digest. The object is left unusable until reset().
    std::array<std::uint8_t, kDigestBytes> finish() noexcept;
    void reset() noexcept;

    static std::array<std::uint8_t, kDigestBytes> hash(std::span<const std::uint8_t> data) noexcept;
    static std::array<std::uint8_t, kDigestBytes> hash(std::string_view data) noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, kBlockBytes> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78). Used for per-record
// corruption detection in the store and for wire frame validation.
std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;
std::uint32_t crc32c(std::string_view data) noexcept;

// A 256-bit digest value with stable ordering and lowercase hex rendering.
class Digest {
public:
    static constexpr std::size_t kBytes = Sha256::kDigestBytes;

    Digest() = default;
    explicit Digest(std::array<std::uint8_t, kBytes> bytes) : bytes_(bytes) {}

    static Digest of(std::string_view data) noexcept { return Digest(Sha256::hash(data)); }
    static Digest of(std::span<const std::uint8_t> data) noexcept { return Digest(Sha256::hash(data)); }

    const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    std::string hex() const;

    bool is_zero() const noexcept;
    bool operator==(const Digest& other) const noexcept { return bytes_ == other.bytes_; }
    bool operator!=(const Digest& other) const noexcept { return !(*this == other); }
    bool operator<(const Digest& other) const noexcept { return bytes_ < other.bytes_; }

private:
    std::array<std::uint8_t, kBytes> bytes_{};
};

Digest digest_of_hex(std::string_view hex);

}  // namespace jitter
