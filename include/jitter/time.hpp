// Jitter Observatory - clock domains, comparability and age accounting.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/provenance.hpp>

namespace jitter {

enum class TimeUnit : std::uint8_t {
    Unknown = 0,
    Nanoseconds = 1,
    Microseconds = 2,
    Milliseconds = 3,
    Seconds = 4,
    // Opaque counter ticks with no declared rate. Values in this unit can be
    // differenced inside their own domain but can never be converted to time.
    CounterTicks = 5,
};

std::string_view to_string(TimeUnit unit) noexcept;
bool parse_time_unit(std::string_view text, TimeUnit& out) noexcept;

// Nanoseconds per unit. Returns nullopt for Unknown and CounterTicks: those can be
// differenced but never converted into a wall-clock duration.
std::optional<std::int64_t> nanos_per_unit(TimeUnit unit) noexcept;

enum class ClockKind : std::uint8_t {
    Unknown = 0,
    MonotonicLocal = 1,   // local monotonic clock, resets on boot
    WallClockUtc = 2,     // civil time referenced to the Unix epoch
    HardwareCounter = 3,  // free running device counter
    Synchronized = 4,     // externally disciplined (for example PTP-disciplined)
    SourceInternal = 5,   // private to one source, origin unknown
    Synthetic = 6,        // produced by a generator or a test harness
};

std::string_view to_string(ClockKind kind) noexcept;
bool parse_clock_kind(std::string_view text, ClockKind& out) noexcept;

struct ClockDomainDescriptor {
    ClockDomainId id;
    std::string name;
    ClockKind kind = ClockKind::Unknown;
    TimeUnit unit = TimeUnit::Nanoseconds;
    // Declared by the source: the origin of the tick counter, in prose.
    std::string epoch_note;
    // Declared accuracy of the domain relative to UTC; -1 means "not declared".
    std::int64_t declared_accuracy_ns = -1;
    bool declared_utc_aligned = false;
    SourceId owner;
};

// Content addressed identity of a clock domain descriptor.
ClockDomainId make_clock_domain_id(const ClockDomainDescriptor& descriptor);
Status validate(const ClockDomainDescriptor& descriptor);

// A reading on an explicitly named clock domain. A TimePoint without its domain is
// meaningless, so the domain is a required field rather than an optional one.
struct TimePoint {
    ClockDomainId domain;
    std::int64_t ticks = 0;

    bool is_nil() const noexcept { return domain.is_nil(); }
    friend bool operator==(const TimePoint& a, const TimePoint& b) noexcept {
        return a.domain == b.domain && a.ticks == b.ticks;
    }
    friend bool operator!=(const TimePoint& a, const TimePoint& b) noexcept { return !(a == b); }
};

struct Duration {
    std::int64_t ns = 0;
    friend bool operator==(const Duration& a, const Duration& b) noexcept { return a.ns == b.ns; }
    friend bool operator!=(const Duration& a, const Duration& b) noexcept { return a.ns != b.ns; }
};

enum class ComparabilityVerdict : std::uint8_t {
    Comparable = 0,    // the two domains may be differenced, directly or through a declaration
    Incomparable = 1,  // the domains are known and must not be differenced
    Unknown = 2,       // at least one domain was never registered
};

std::string_view to_string(ComparabilityVerdict verdict) noexcept;

struct ComparabilityAssessment {
    ComparabilityVerdict verdict = ComparabilityVerdict::Unknown;
    // Upper bound on the offset between the two domains when comparable.
    std::int64_t max_offset_ns = 0;
    // Stable machine readable reason. One of: same_domain, declared_equivalence,
    // distinct_undeclared_domains, domain_unknown, unit_not_convertible,
    // authority_insufficient, declaration_expired.
    std::string reason;

    bool comparable() const noexcept { return verdict == ComparabilityVerdict::Comparable; }
};

// An explicit, authority backed statement that two clock domains may be treated as
// sharing a timeline within a bounded offset. Equivalences are declared by an
// operator or an authoritative source; the runtime never infers them.
struct ClockEquivalence {
    ClockDomainId a;
    ClockDomainId b;
    std::int64_t max_offset_ns = 0;
    SourceAuthority declared_by_authority = SourceAuthority::Unknown;
    std::string justification;
    std::optional<std::int64_t> valid_until_utc_ns;
};

struct ComparabilityPolicy {
    // Minimum authority required to accept a declared equivalence.
    SourceAuthority minimum_equivalence_authority = SourceAuthority::Corroborating;
    // Declarations claiming a larger offset than this are refused outright.
    std::int64_t max_accepted_offset_ns = 1000000;  // 1 ms
    // When true a clock domain with an unknown unit can never be comparable with
    // anything except itself.
    bool require_convertible_units = true;
};

// Age of a piece of evidence, in the only two ways it can honestly be measured.
struct AgeAssessment {
    // Local-clock age: now_utc - received_at_utc. Always computable when both are known.
    std::optional<std::int64_t> ingest_age_ns;
    // Source-clock age: now_utc - observed_at, which requires that the observation
    // clock domain is comparable with the local reference domain. Nullopt when the
    // clocks are not comparable; that is "unknown", never zero.
    std::optional<std::int64_t> observation_age_ns;
    // Same-domain span between observation and receipt, expressed in the source
    // clock's own units when those units are convertible. This is a transport
    // observation, not an age, and it never substitutes for observation_age_ns.
    std::optional<std::int64_t> in_domain_span_ns;
    ComparabilityVerdict comparability = ComparabilityVerdict::Unknown;
    std::string reason;
};

class ClockModel {
public:
    explicit ClockModel(ComparabilityPolicy policy = {});

    // Registers a descriptor and fills in its content addressed identity when the
    // caller left it nil, so that a registered descriptor can always be referenced.
    // Registration is idempotent for identical content.
    Status register_domain(ClockDomainDescriptor& descriptor);
    Status declare_equivalence(const ClockEquivalence& equivalence);

    bool has_domain(ClockDomainId id) const noexcept;
    const ClockDomainDescriptor* find(ClockDomainId id) const noexcept;
    std::size_t domain_count() const noexcept;
    std::vector<ClockDomainDescriptor> domains() const;

    ComparabilityAssessment assess(ClockDomainId a, ClockDomainId b,
                                   std::int64_t now_utc_ns) const;

    // Signed difference b - a, expressed in nanoseconds. Fails with Incomparable when
    // the domains cannot be compared and with OutOfRange when the units cannot be
    // converted to time.
    Result<Duration> difference(const TimePoint& a, const TimePoint& b,
                                std::int64_t now_utc_ns) const;

    // Difference inside a single domain, in that domain's ticks. Always available.
    static Result<std::int64_t> in_domain_delta(const TimePoint& a, const TimePoint& b);

    const ComparabilityPolicy& policy() const noexcept { return policy_; }
    std::vector<ClockEquivalence> equivalences() const { return equivalences_; }
    void set_policy(const ComparabilityPolicy& policy) { policy_ = policy; }

private:
    ComparabilityPolicy policy_;
    std::vector<ClockDomainDescriptor> domains_;
    std::vector<ClockEquivalence> equivalences_;
};

// Local reference clock: the runtime's own view of wall-clock UTC, used for ingest
// ages and for ordering events the runtime itself witnessed.
struct LocalClockDomain {
    static ClockDomainId id();
    static ClockDomainDescriptor descriptor();
};

// Deterministic clock used by tests, examples and the generator. It never reads the
// ambient system clock, so callers must advance it explicitly.
class ManualClock {
public:
    explicit ManualClock(std::int64_t start_utc_ns = 1700000000000000000ll)
        : now_utc_ns_(start_utc_ns) {}

    std::int64_t now_utc_ns() const noexcept { return now_utc_ns_; }

    void advance(std::int64_t delta_ns) {
        if (delta_ns < 0) {
            // Time in this runtime never runs backwards; a negative advance is clamped.
            return;
        }
        now_utc_ns_ += delta_ns;
    }

    void set(std::int64_t now_utc_ns) {
        if (now_utc_ns >= now_utc_ns_) {
            now_utc_ns_ = now_utc_ns;
        }
    }

private:
    std::int64_t now_utc_ns_;
};

}  // namespace jitter
