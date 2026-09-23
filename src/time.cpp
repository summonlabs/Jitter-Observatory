// Jitter Observatory - clock domains, comparability and age accounting.
// Copyright 2026 Summon Software Labs.
#include <jitter/time.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::uint64_t kMaxClockNameBytes = 128;
constexpr std::uint64_t kMaxEpochNoteBytes = 256;
constexpr std::uint64_t kMaxJustificationBytes = 512;

Result<std::int64_t> scale_to_nanos(std::int64_t ticks, TimeUnit unit, std::string_view what) {
    const auto scale = nanos_per_unit(unit);
    if (!scale.has_value()) {
        return Result<std::int64_t>::fail(
            ErrorCode::Unsupported,
            std::string(what) + ": clock unit has no declared rate and cannot be converted to time",
            std::string(to_string(unit)));
    }
    std::int64_t out = 0;
    if (!checked_mul_i64(ticks, scale.value(), out)) {
        return Result<std::int64_t>::fail(ErrorCode::Overflow,
                                          std::string(what) + ": unit conversion overflowed",
                                          "ticks=" + text::i64_to_string(ticks));
    }
    return out;
}

}  // namespace

std::string_view to_string(TimeUnit unit) noexcept {
    switch (unit) {
        case TimeUnit::Unknown: return "unknown";
        case TimeUnit::Nanoseconds: return "nanoseconds";
        case TimeUnit::Microseconds: return "microseconds";
        case TimeUnit::Milliseconds: return "milliseconds";
        case TimeUnit::Seconds: return "seconds";
        case TimeUnit::CounterTicks: return "counter_ticks";
    }
    return "unknown";
}

bool parse_time_unit(std::string_view value, TimeUnit& out) noexcept {
    if (value == "unknown") { out = TimeUnit::Unknown; return true; }
    if (value == "nanoseconds" || value == "ns") { out = TimeUnit::Nanoseconds; return true; }
    if (value == "microseconds" || value == "us") { out = TimeUnit::Microseconds; return true; }
    if (value == "milliseconds" || value == "ms") { out = TimeUnit::Milliseconds; return true; }
    if (value == "seconds" || value == "s") { out = TimeUnit::Seconds; return true; }
    if (value == "counter_ticks" || value == "ticks") { out = TimeUnit::CounterTicks; return true; }
    return false;
}

std::optional<std::int64_t> nanos_per_unit(TimeUnit unit) noexcept {
    switch (unit) {
        case TimeUnit::Nanoseconds: return 1;
        case TimeUnit::Microseconds: return 1000;
        case TimeUnit::Milliseconds: return 1000000;
        case TimeUnit::Seconds: return 1000000000;
        case TimeUnit::Unknown:
        case TimeUnit::CounterTicks:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string_view to_string(ClockKind kind) noexcept {
    switch (kind) {
        case ClockKind::Unknown: return "unknown";
        case ClockKind::MonotonicLocal: return "monotonic_local";
        case ClockKind::WallClockUtc: return "wall_clock_utc";
        case ClockKind::HardwareCounter: return "hardware_counter";
        case ClockKind::Synchronized: return "synchronized";
        case ClockKind::SourceInternal: return "source_internal";
        case ClockKind::Synthetic: return "synthetic";
    }
    return "unknown";
}

bool parse_clock_kind(std::string_view value, ClockKind& out) noexcept {
    if (value == "unknown") { out = ClockKind::Unknown; return true; }
    if (value == "monotonic_local") { out = ClockKind::MonotonicLocal; return true; }
    if (value == "wall_clock_utc") { out = ClockKind::WallClockUtc; return true; }
    if (value == "hardware_counter") { out = ClockKind::HardwareCounter; return true; }
    if (value == "synchronized") { out = ClockKind::Synchronized; return true; }
    if (value == "source_internal") { out = ClockKind::SourceInternal; return true; }
    if (value == "synthetic") { out = ClockKind::Synthetic; return true; }
    return false;
}

std::string_view to_string(ComparabilityVerdict verdict) noexcept {
    switch (verdict) {
        case ComparabilityVerdict::Comparable: return "comparable";
        case ComparabilityVerdict::Incomparable: return "incomparable";
        case ComparabilityVerdict::Unknown: return "unknown";
    }
    return "unknown";
}

ClockDomainId make_clock_domain_id(const ClockDomainDescriptor& descriptor) {
    DigestBuilder builder(kDomainClockDomain);
    builder.str(descriptor.name);
    builder.u8(static_cast<std::uint8_t>(descriptor.kind));
    builder.u8(static_cast<std::uint8_t>(descriptor.unit));
    builder.str(descriptor.epoch_note);
    builder.i64(descriptor.declared_accuracy_ns);
    builder.boolean(descriptor.declared_utc_aligned);
    builder.id(descriptor.owner);
    return builder.as_id<ClockDomainTag>();
}

Status validate(const ClockDomainDescriptor& descriptor) {
    if (descriptor.name.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "clock domain name must not be empty");
    }
    if (descriptor.name.size() > kMaxClockNameBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "clock domain name is too long",
                               std::to_string(descriptor.name.size()));
    }
    if (descriptor.epoch_note.size() > kMaxEpochNoteBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "clock domain epoch note is too long",
                               std::to_string(descriptor.epoch_note.size()));
    }
    if (descriptor.declared_accuracy_ns < -1) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "declared accuracy must be -1 (undeclared) or non negative",
                               text::i64_to_string(descriptor.declared_accuracy_ns));
    }
    const ClockDomainId derived = make_clock_domain_id(descriptor);
    if (!descriptor.id.is_nil() && descriptor.id != derived) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "clock domain id does not match its content addressed identity",
                               "declared=" + descriptor.id.hex() + " derived=" + derived.hex());
    }
    return Status::success();
}

ClockModel::ClockModel(ComparabilityPolicy policy) : policy_(policy) {}

Status ClockModel::register_domain(ClockDomainDescriptor& descriptor) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_clock_domain_id(descriptor);
    }
    JITTER_TRY(validate(descriptor));
    for (const auto& existing : domains_) {
        if (existing.id == descriptor.id) {
            return Status::success();  // idempotent: identical content addressed domain
        }
    }
    if (domains_.size() >= Limits::kMaxClockDomains) {
        return Status::failure(ErrorCode::CapacityExceeded, "clock domain registry is full",
                               std::to_string(domains_.size()));
    }
    domains_.push_back(descriptor);
    return Status::success();
}

Status ClockModel::declare_equivalence(const ClockEquivalence& equivalence) {
    if (equivalence.a.is_nil() || equivalence.b.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "clock equivalence requires two named domains");
    }
    if (equivalence.a == equivalence.b) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "clock equivalence between a domain and itself is meaningless");
    }
    if (!has_domain(equivalence.a) || !has_domain(equivalence.b)) {
        return Status::failure(ErrorCode::NotFound,
                               "clock equivalence references an unregistered clock domain");
    }
    if (equivalence.max_offset_ns < 0) {
        return Status::failure(ErrorCode::InvalidArgument, "clock equivalence offset must be non negative");
    }
    if (equivalence.max_offset_ns > policy_.max_accepted_offset_ns) {
        return Status::failure(ErrorCode::PolicyViolation,
                               "clock equivalence claims an offset larger than the accepted bound",
                               "claimed=" + text::i64_to_string(equivalence.max_offset_ns) +
                                   " accepted=" + text::i64_to_string(policy_.max_accepted_offset_ns));
    }
    if (authority_rank(equivalence.declared_by_authority) <
        authority_rank(policy_.minimum_equivalence_authority)) {
        return Status::failure(ErrorCode::PolicyViolation,
                               "clock equivalence was declared with insufficient authority",
                               std::string(to_string(equivalence.declared_by_authority)));
    }
    if (equivalence.justification.empty()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "clock equivalence requires a justification");
    }
    if (equivalence.justification.size() > kMaxJustificationBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "clock equivalence justification is too long");
    }
    for (const auto& existing : equivalences_) {
        const bool same_pair = (existing.a == equivalence.a && existing.b == equivalence.b) ||
                               (existing.a == equivalence.b && existing.b == equivalence.a);
        if (same_pair) {
            return Status::failure(ErrorCode::Duplicate,
                                   "a clock equivalence for this pair is already declared",
                                   equivalence.a.hex() + "/" + equivalence.b.hex());
        }
    }
    if (equivalences_.size() >= Limits::kMaxClockDomains) {
        return Status::failure(ErrorCode::CapacityExceeded, "clock equivalence table is full");
    }
    equivalences_.push_back(equivalence);
    return Status::success();
}

bool ClockModel::has_domain(ClockDomainId id) const noexcept { return find(id) != nullptr; }

const ClockDomainDescriptor* ClockModel::find(ClockDomainId id) const noexcept {
    for (const auto& domain : domains_) {
        if (domain.id == id) {
            return &domain;
        }
    }
    return nullptr;
}

std::size_t ClockModel::domain_count() const noexcept { return domains_.size(); }

std::vector<ClockDomainDescriptor> ClockModel::domains() const {
    std::vector<ClockDomainDescriptor> copy = domains_;
    std::sort(copy.begin(), copy.end(), [](const ClockDomainDescriptor& a, const ClockDomainDescriptor& b) {
        return a.id < b.id;
    });
    return copy;
}

ComparabilityAssessment ClockModel::assess(ClockDomainId a, ClockDomainId b,
                                           std::int64_t now_utc_ns) const {
    ComparabilityAssessment assessment;
    if (a.is_nil() || b.is_nil()) {
        assessment.verdict = ComparabilityVerdict::Unknown;
        assessment.reason = "domain_unknown";
        return assessment;
    }
    if (a == b) {
        if (find(a) == nullptr) {
            assessment.verdict = ComparabilityVerdict::Unknown;
            assessment.reason = "domain_unknown";
            return assessment;
        }
        assessment.verdict = ComparabilityVerdict::Comparable;
        assessment.max_offset_ns = 0;
        assessment.reason = "same_domain";
        return assessment;
    }

    const ClockDomainDescriptor* left = find(a);
    const ClockDomainDescriptor* right = find(b);
    if (left == nullptr || right == nullptr) {
        assessment.verdict = ComparabilityVerdict::Unknown;
        assessment.reason = "domain_unknown";
        return assessment;
    }

    if (policy_.require_convertible_units &&
        (!nanos_per_unit(left->unit).has_value() || !nanos_per_unit(right->unit).has_value())) {
        assessment.verdict = ComparabilityVerdict::Incomparable;
        assessment.reason = "unit_not_convertible";
        return assessment;
    }

    bool saw_expired = false;
    bool saw_low_authority = false;
    for (const auto& equivalence : equivalences_) {
        const bool matches = (equivalence.a == a && equivalence.b == b) ||
                             (equivalence.a == b && equivalence.b == a);
        if (!matches) {
            continue;
        }
        if (equivalence.valid_until_utc_ns.has_value() &&
            now_utc_ns > equivalence.valid_until_utc_ns.value()) {
            saw_expired = true;
            continue;
        }
        if (authority_rank(equivalence.declared_by_authority) <
            authority_rank(policy_.minimum_equivalence_authority)) {
            saw_low_authority = true;
            continue;
        }
        assessment.verdict = ComparabilityVerdict::Comparable;
        assessment.max_offset_ns = equivalence.max_offset_ns;
        assessment.reason = "declared_equivalence";
        return assessment;
    }

    assessment.verdict = ComparabilityVerdict::Incomparable;
    if (saw_expired) {
        assessment.reason = "declaration_expired";
    } else if (saw_low_authority) {
        assessment.reason = "authority_insufficient";
    } else {
        assessment.reason = "distinct_undeclared_domains";
    }
    return assessment;
}

Result<Duration> ClockModel::difference(const TimePoint& a, const TimePoint& b,
                                        std::int64_t now_utc_ns) const {
    const ComparabilityAssessment assessment = assess(a.domain, b.domain, now_utc_ns);
    if (!assessment.comparable()) {
        return Result<Duration>::fail(
            ErrorCode::Incomparable,
            "clock domains are not comparable, so no time difference may be formed",
            "reason=" + assessment.reason + " left=" + a.domain.hex() + " right=" + b.domain.hex());
    }
    const ClockDomainDescriptor* left = find(a.domain);
    const ClockDomainDescriptor* right = find(b.domain);
    if (left == nullptr || right == nullptr) {
        return Result<Duration>::fail(ErrorCode::NotFound, "clock domain is not registered");
    }
    Result<std::int64_t> a_ns = scale_to_nanos(a.ticks, left->unit, "left reading");
    if (!a_ns.ok()) {
        return a_ns.error();
    }
    Result<std::int64_t> b_ns = scale_to_nanos(b.ticks, right->unit, "right reading");
    if (!b_ns.ok()) {
        return b_ns.error();
    }
    std::int64_t delta = 0;
    if (!checked_sub_i64(b_ns.value(), a_ns.value(), delta)) {
        return Result<Duration>::fail(ErrorCode::Overflow, "clock difference overflowed");
    }
    Duration duration;
    duration.ns = delta;
    return duration;
}

Result<std::int64_t> ClockModel::in_domain_delta(const TimePoint& a, const TimePoint& b) {
    if (a.domain != b.domain) {
        return Result<std::int64_t>::fail(ErrorCode::Incomparable,
                                          "in-domain delta requires readings on the same clock domain");
    }
    std::int64_t delta = 0;
    if (!checked_sub_i64(b.ticks, a.ticks, delta)) {
        return Result<std::int64_t>::fail(ErrorCode::Overflow, "in-domain delta overflowed");
    }
    return delta;
}

ClockDomainDescriptor LocalClockDomain::descriptor() {
    ClockDomainDescriptor descriptor;
    descriptor.name = "local.utc";
    descriptor.kind = ClockKind::WallClockUtc;
    descriptor.unit = TimeUnit::Nanoseconds;
    descriptor.epoch_note = "unix epoch, runtime local reference clock";
    descriptor.declared_accuracy_ns = -1;
    descriptor.declared_utc_aligned = true;
    descriptor.id = make_clock_domain_id(descriptor);
    return descriptor;
}

ClockDomainId LocalClockDomain::id() { return descriptor().id; }

}  // namespace jitter
