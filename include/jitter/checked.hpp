// Jitter Observatory - checked arithmetic for externally derived sizes.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <jitter/error.hpp>

namespace jitter {

inline constexpr std::uint64_t kU64Max = 0xFFFFFFFFFFFFFFFFull;
inline constexpr std::int64_t kI64Max = 9223372036854775807ll;
inline constexpr std::int64_t kI64Min = -9223372036854775807ll - 1ll;

inline bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a > kU64Max - b) {
        return false;
    }
    out = a + b;
    return true;
}

inline bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > kU64Max / a) {
        return false;
    }
    out = a * b;
    return true;
}

inline bool checked_add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a > kI64Max - b) {
        return false;
    }
    if (b < 0 && a < kI64Min - b) {
        return false;
    }
    out = a + b;
    return true;
}

inline bool checked_sub_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a < kI64Min + b) {
        return false;
    }
    if (b < 0 && a > kI64Max + b) {
        return false;
    }
    out = a - b;
    return true;
}

inline bool checked_mul_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a == -1 && b == kI64Min) {
        return false;
    }
    if (b == -1 && a == kI64Min) {
        return false;
    }
    const std::int64_t product = a * b;
    if (product / b != a) {
        return false;
    }
    out = product;
    return true;
}

// Narrowing helpers used at every point where an external size meets an internal type.
inline bool checked_to_size(std::uint64_t value, std::size_t& out) noexcept {
    if (value > static_cast<std::uint64_t>(SIZE_MAX)) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

inline bool checked_to_u32(std::uint64_t value, std::uint32_t& out) noexcept {
    if (value > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

inline std::uint64_t to_u64(std::size_t value) noexcept { return static_cast<std::uint64_t>(value); }
inline std::int64_t to_i64(std::uint64_t value) noexcept { return static_cast<std::int64_t>(value); }

// Throwing-free checked operations that report a structured error.
Result<std::uint64_t> add_u64(std::uint64_t a, std::uint64_t b, std::string_view context);
Result<std::uint64_t> mul_u64(std::uint64_t a, std::uint64_t b, std::string_view context);
Result<std::int64_t> add_i64(std::int64_t a, std::int64_t b, std::string_view context);
Result<std::int64_t> sub_i64(std::int64_t a, std::int64_t b, std::string_view context);
Result<std::size_t> to_size(std::uint64_t value, std::string_view context);
Status require_within(std::uint64_t value, std::uint64_t bound, std::string_view what);

}  // namespace jitter
