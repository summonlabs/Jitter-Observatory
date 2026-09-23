// Jitter Observatory - strongly typed content addressed identities.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/text.hpp>

namespace jitter {

// Every identity in the runtime is a 128-bit value derived from a SHA-256 digest of a
// canonical, domain separated encoding. Two identities can only collide if the digest
// collides, and an identity can only be reconstructed from the exact bytes that
// produced it, so "the same id" always means "the same definition".
template <class Tag>
class StrongId {
public:
    using tag_type = Tag;
    static constexpr std::size_t kBytes = 16;

    constexpr StrongId() noexcept = default;

    static StrongId from_bytes(const std::uint8_t* bytes) noexcept {
        StrongId id;
        for (std::size_t i = 0; i < kBytes; ++i) {
            id.bytes_[i] = bytes[i];
        }
        return id;
    }

    static StrongId from_array(const std::array<std::uint8_t, kBytes>& bytes) noexcept {
        StrongId id;
        id.bytes_ = bytes;
        return id;
    }

    static StrongId from_digest(const Digest& digest) noexcept {
        return from_bytes(digest.data());
    }

    static Result<StrongId> parse(std::string_view text) {
        std::string_view body = text;
        const std::size_t colon = text.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view prefix = text.substr(0, colon);
            if (prefix != Tag::name) {
                return Result<StrongId>::fail(
                    ErrorCode::InvalidArgument,
                    "identity prefix does not match expected tag",
                    std::string("expected=") + std::string(Tag::name) + " got=" + std::string(prefix));
            }
            body = text.substr(colon + 1);
        }
        if (body.size() != kBytes * 2) {
            return Result<StrongId>::fail(ErrorCode::InvalidArgument,
                                          "identity must be 32 hexadecimal characters",
                                          std::string(text));
        }
        std::array<std::uint8_t, kBytes> bytes{};
        for (std::size_t i = 0; i < kBytes; ++i) {
            const char high = body[i * 2];
            const char low = body[(i * 2) + 1];
            auto decode = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = decode(high);
            const int lo = decode(low);
            if (hi < 0 || lo < 0) {
                return Result<StrongId>::fail(ErrorCode::InvalidArgument,
                                              "identity contains a non hexadecimal character",
                                              std::string(text));
            }
            bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
        return from_array(bytes);
    }

    constexpr bool is_nil() const noexcept {
        for (const std::uint8_t byte : bytes_) {
            if (byte != 0u) {
                return false;
            }
        }
        return true;
    }

    const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }

    std::string hex() const { return jitter::text::hex_encode(bytes_); }

    std::string label() const {
        std::string out(Tag::name);
        out.push_back(':');
        out.append(hex());
        return out;
    }

    friend bool operator==(const StrongId& a, const StrongId& b) noexcept { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const StrongId& a, const StrongId& b) noexcept { return !(a == b); }
    friend bool operator<(const StrongId& a, const StrongId& b) noexcept { return a.bytes_ < b.bytes_; }
    friend bool operator>(const StrongId& a, const StrongId& b) noexcept { return b < a; }
    friend bool operator<=(const StrongId& a, const StrongId& b) noexcept { return !(b < a); }
    friend bool operator>=(const StrongId& a, const StrongId& b) noexcept { return !(a < b); }

private:
    std::array<std::uint8_t, kBytes> bytes_{};
};

struct SourceTag { static constexpr std::string_view name = "source"; };
struct SeriesTag { static constexpr std::string_view name = "series"; };
struct PathTag { static constexpr std::string_view name = "path"; };
struct HopTag { static constexpr std::string_view name = "hop"; };
struct GenerationTag { static constexpr std::string_view name = "generation"; };
struct ClockDomainTag { static constexpr std::string_view name = "clock"; };
struct WindowPolicyTag { static constexpr std::string_view name = "window"; };
struct MeasurementTag { static constexpr std::string_view name = "measurement"; };
struct MetricTag { static constexpr std::string_view name = "metric"; };
struct BaselineTag { static constexpr std::string_view name = "baseline"; };
struct EpisodeTag { static constexpr std::string_view name = "episode"; };
struct InstabilityPolicyTag { static constexpr std::string_view name = "policy"; };
struct SummaryTag { static constexpr std::string_view name = "summary"; };
struct BatchTag { static constexpr std::string_view name = "batch"; };
struct FreshnessPolicyTag { static constexpr std::string_view name = "freshness"; };

using SourceId = StrongId<SourceTag>;
using SeriesId = StrongId<SeriesTag>;
using PathId = StrongId<PathTag>;
using HopId = StrongId<HopTag>;
using GenerationId = StrongId<GenerationTag>;
using ClockDomainId = StrongId<ClockDomainTag>;
using WindowPolicyId = StrongId<WindowPolicyTag>;
using MeasurementId = StrongId<MeasurementTag>;
using MetricId = StrongId<MetricTag>;
using BaselineId = StrongId<BaselineTag>;
using EpisodeId = StrongId<EpisodeTag>;
using InstabilityPolicyId = StrongId<InstabilityPolicyTag>;
using SummaryId = StrongId<SummaryTag>;
using BatchId = StrongId<BatchTag>;
using FreshnessPolicyId = StrongId<FreshnessPolicyTag>;

}  // namespace jitter
