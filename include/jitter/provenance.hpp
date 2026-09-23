// Jitter Observatory - provenance, authority and monotonic counters.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <jitter/checked.hpp>
#include <jitter/error.hpp>
#include <jitter/id.hpp>

namespace jitter {

// How the evidence behind a value was produced. This label travels with every
// observation, summary and export so that no consumer can mistake a synthetic
// measurement for a measurement of real fabric traffic.
enum class EvidenceOrigin : std::uint8_t {
    Unknown = 0,      // the source did not say
    Real = 1,         // measured on real hardware or a real transport path
    Synthetic = 2,    // generated, replayed, simulated, or injected by a test harness
    Unsupported = 3,  // the claim cannot be supported by this runtime at all
};

std::string_view to_string(EvidenceOrigin origin) noexcept;
bool parse_evidence_origin(std::string_view text, EvidenceOrigin& out) noexcept;

// Source authority is an ordered, explicitly assigned property. It is never
// inferred from arrival order or from the magnitude of a value.
enum class SourceAuthority : std::uint8_t {
    Unknown = 0,
    Simulated = 1,
    Advisory = 2,
    Corroborating = 3,
    Authoritative = 4,
};

std::string_view to_string(SourceAuthority authority) noexcept;
bool parse_source_authority(std::string_view text, SourceAuthority& out) noexcept;
std::uint8_t authority_rank(SourceAuthority authority) noexcept;

// Monotonic, non-wrapping-by-accident counters. Value 0 always means "unset", so a
// default constructed counter can never be mistaken for an observed position.
template <class Tag>
class Counter {
public:
    using tag_type = Tag;
    using value_type = std::uint64_t;

    constexpr Counter() noexcept = default;
    constexpr explicit Counter(std::uint64_t value) noexcept : value_(value) {}

    static Counter from_value(std::uint64_t value) noexcept { return Counter(value); }

    constexpr std::uint64_t value() const noexcept { return value_; }
    constexpr bool is_unset() const noexcept { return value_ == 0u; }
    constexpr bool is_set() const noexcept { return value_ != 0u; }

    // Saturating successor: a counter never silently wraps to zero.
    Counter next() const noexcept {
        if (value_ == kU64Max) {
            return Counter(kU64Max);
        }
        return Counter(value_ + 1u);
    }

    friend constexpr bool operator==(Counter a, Counter b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Counter a, Counter b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Counter a, Counter b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator>(Counter a, Counter b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator<=(Counter a, Counter b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>=(Counter a, Counter b) noexcept { return a.value_ >= b.value_; }

    std::string to_string_value() const;

private:
    std::uint64_t value_ = 0;
};

struct IncarnationTag { static constexpr std::string_view name = "incarnation"; };
struct EpochTag { static constexpr std::string_view name = "epoch"; };
struct SequenceTag { static constexpr std::string_view name = "sequence"; };
struct RevisionTag { static constexpr std::string_view name = "revision"; };
struct BootTag { static constexpr std::string_view name = "boot"; };
struct OrdinalTag { static constexpr std::string_view name = "ordinal"; };

using SourceIncarnation = Counter<IncarnationTag>;
using SourceEpoch = Counter<EpochTag>;
using SourceSequence = Counter<SequenceTag>;
using Revision = Counter<RevisionTag>;
using BootId = Counter<BootTag>;
using Ordinal = Counter<OrdinalTag>;

// A complete statement of where a piece of evidence came from.
struct Provenance {
    SourceId source;
    SourceIncarnation incarnation;
    SourceEpoch epoch;
    SourceSequence sequence;
    SourceAuthority authority = SourceAuthority::Unknown;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    std::string ingest_path;  // "in_process", "tcp:<peer>", "file:<digest>", "replay:<digest>"

    bool is_synthetic() const noexcept { return origin == EvidenceOrigin::Synthetic; }
    bool is_real() const noexcept { return origin == EvidenceOrigin::Real; }
    std::string describe() const;
};

}  // namespace jitter
