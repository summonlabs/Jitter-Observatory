// Jitter Observatory - freshness and conflict accounting.
// Copyright 2026 Summon Software Labs.
#include <jitter/evidence.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::int64_t kMinFreshnessBudgetNs = 1;
constexpr std::int64_t kMaxFreshnessBudgetNs = 3600ll * 1000000000ll;  // one hour
constexpr std::uint64_t kMaxPolicyNameBytes = 64;

void add_reason(std::vector<std::string>& reasons, const std::string& reason) {
    if (reason.empty()) {
        return;
    }
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(reason);
    }
}

}  // namespace

std::string_view to_string(EvidenceState state) noexcept {
    switch (state) {
        case EvidenceState::Fresh: return "fresh";
        case EvidenceState::Stale: return "stale";
        case EvidenceState::Expired: return "expired";
        case EvidenceState::Conflicting: return "conflicting";
        case EvidenceState::Incomplete: return "incomplete";
        case EvidenceState::Unsupported: return "unsupported";
        case EvidenceState::Unknown: return "unknown";
        case EvidenceState::Missing: return "missing";
    }
    return "unknown";
}

bool parse_evidence_state(std::string_view value, EvidenceState& out) noexcept {
    if (value == "fresh") { out = EvidenceState::Fresh; return true; }
    if (value == "stale") { out = EvidenceState::Stale; return true; }
    if (value == "expired") { out = EvidenceState::Expired; return true; }
    if (value == "conflicting") { out = EvidenceState::Conflicting; return true; }
    if (value == "incomplete") { out = EvidenceState::Incomplete; return true; }
    if (value == "unsupported") { out = EvidenceState::Unsupported; return true; }
    if (value == "unknown") { out = EvidenceState::Unknown; return true; }
    if (value == "missing") { out = EvidenceState::Missing; return true; }
    return false;
}

bool is_positive_evidence(EvidenceState state) noexcept { return state == EvidenceState::Fresh; }

FreshnessPolicyId make_freshness_policy_id(const FreshnessPolicy& policy) {
    DigestBuilder builder(kDomainEvidence);
    builder.str(policy.name);
    builder.i64(policy.max_ingest_age_ns);
    builder.i64(policy.expiry_ingest_age_ns);
    builder.i64(policy.max_observation_age_ns);
    builder.i64(policy.expiry_observation_age_ns);
    builder.i64(policy.future_tolerance_ns);
    builder.boolean(policy.require_observation_age);
    return builder.as_id<FreshnessPolicyTag>();
}

Status validate_freshness_policy(const FreshnessPolicy& policy) {
    if (policy.name.empty() || policy.name.size() > kMaxPolicyNameBytes) {
        return Status::failure(ErrorCode::InvalidArgument, "freshness policy needs a short, non empty name");
    }
    if (policy.max_ingest_age_ns < kMinFreshnessBudgetNs ||
        policy.max_ingest_age_ns > kMaxFreshnessBudgetNs) {
        return Status::failure(ErrorCode::OutOfRange, "ingest freshness budget is outside the supported range",
                               text::i64_to_string(policy.max_ingest_age_ns));
    }
    if (policy.expiry_ingest_age_ns < policy.max_ingest_age_ns) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "ingest expiry must not be shorter than the ingest freshness budget");
    }
    if (policy.max_observation_age_ns < kMinFreshnessBudgetNs ||
        policy.max_observation_age_ns > kMaxFreshnessBudgetNs) {
        return Status::failure(ErrorCode::OutOfRange,
                               "observation freshness budget is outside the supported range",
                               text::i64_to_string(policy.max_observation_age_ns));
    }
    if (policy.expiry_observation_age_ns < policy.max_observation_age_ns) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "observation expiry must not be shorter than the observation freshness budget");
    }
    if (policy.future_tolerance_ns < 0 || policy.future_tolerance_ns > kMaxFreshnessBudgetNs) {
        return Status::failure(ErrorCode::OutOfRange, "future tolerance is outside the supported range");
    }
    return Status::success();
}

SampleAssessment assess_sample(const LatencySample& sample, const ClockModel& clocks,
                               std::int64_t now_utc_ns, const FreshnessPolicy& policy) {
    SampleAssessment assessment;
    assessment.sample = sample.id;

    // Ingest age is measured entirely on the runtime's own clock domain. A subtraction
    // that would overflow is treated as "not establishable" rather than wrapping.
    std::int64_t ingest_age = 0;
    const bool ingest_age_known = checked_sub_i64(now_utc_ns, sample.received_at.ticks, ingest_age);
    if (ingest_age_known) {
        assessment.ages.ingest_age_ns = ingest_age;
    }

    // The observation age needs a comparable clock domain; without one it is unknown,
    // never assumed to be zero and never assumed to be small.
    const ComparabilityAssessment comparability =
        clocks.assess(sample.observed_at.domain, LocalClockDomain::id(), now_utc_ns);
    assessment.ages.comparability = comparability.verdict;
    assessment.ages.reason = comparability.reason;
    if (comparability.comparable()) {
        std::int64_t observation_age = 0;
        std::int64_t span = 0;
        if (checked_sub_i64(now_utc_ns, sample.observed_at.ticks, observation_age)) {
            assessment.ages.observation_age_ns = observation_age;
        }
        if (checked_sub_i64(sample.received_at.ticks, sample.observed_at.ticks, span)) {
            assessment.ages.in_domain_span_ns = span;
        }
    }

    if (!ingest_age_known) {
        assessment.state = EvidenceState::Unknown;
        assessment.reason = "ingest_age_overflowed_the_supported_range";
        return assessment;
    }
    if (ingest_age > policy.expiry_ingest_age_ns) {
        assessment.state = EvidenceState::Expired;
        assessment.reason = "ingest_age_exceeds_expiry";
        return assessment;
    }
    if (assessment.ages.observation_age_ns.has_value() &&
        assessment.ages.observation_age_ns.value() > policy.expiry_observation_age_ns) {
        assessment.state = EvidenceState::Expired;
        assessment.reason = "observation_age_exceeds_expiry";
        return assessment;
    }
    if (ingest_age > policy.max_ingest_age_ns) {
        assessment.state = EvidenceState::Stale;
        assessment.reason = "ingest_age_exceeds_budget";
        return assessment;
    }
    if (assessment.ages.observation_age_ns.has_value() &&
        assessment.ages.observation_age_ns.value() > policy.max_observation_age_ns) {
        assessment.state = EvidenceState::Stale;
        assessment.reason = "observation_age_exceeds_budget";
        return assessment;
    }
    if (assessment.ages.observation_age_ns.has_value() &&
        assessment.ages.observation_age_ns.value() < -policy.future_tolerance_ns) {
        assessment.state = EvidenceState::Unknown;
        assessment.reason = "observation_time_is_in_the_future";
        return assessment;
    }
    if (!assessment.ages.observation_age_ns.has_value()) {
        if (comparability.verdict == ComparabilityVerdict::Incomparable) {
            assessment.state = EvidenceState::Unsupported;
            assessment.reason = "observation_clock_incomparable:" + comparability.reason;
            return assessment;
        }
        if (policy.require_observation_age) {
            assessment.state = EvidenceState::Unknown;
            assessment.reason = "observation_age_not_establishable";
            return assessment;
        }
    }

    assessment.state = EvidenceState::Fresh;
    assessment.reason = "within_all_declared_budgets";
    return assessment;
}

void EvidenceSummary::recompute_dominant() {
    if (total == 0) {
        dominant = EvidenceState::Missing;
        return;
    }
    if (conflicting > 0) {
        dominant = EvidenceState::Conflicting;
        return;
    }
    if (fresh > 0) {
        dominant = EvidenceState::Fresh;
        return;
    }
    if (stale > 0) {
        dominant = EvidenceState::Stale;
        return;
    }
    if (expired > 0) {
        dominant = EvidenceState::Expired;
        return;
    }
    if (unsupported > 0) {
        dominant = EvidenceState::Unsupported;
        return;
    }
    if (unknown > 0) {
        dominant = EvidenceState::Unknown;
        return;
    }
    dominant = EvidenceState::Incomplete;
}

EvidenceSummary summarize_evidence(const std::vector<SampleAssessment>& assessments) {
    EvidenceSummary summary;
    summary.total = static_cast<std::uint64_t>(assessments.size());
    for (const SampleAssessment& assessment : assessments) {
        switch (assessment.state) {
            case EvidenceState::Fresh: ++summary.fresh; break;
            case EvidenceState::Stale: ++summary.stale; break;
            case EvidenceState::Expired: ++summary.expired; break;
            case EvidenceState::Conflicting: ++summary.conflicting; break;
            case EvidenceState::Incomplete: ++summary.incomplete; break;
            case EvidenceState::Unsupported: ++summary.unsupported; break;
            case EvidenceState::Unknown: ++summary.unknown; break;
            case EvidenceState::Missing: break;
        }
        add_reason(summary.reasons, std::string(to_string(assessment.state)) + ":" + assessment.reason);
    }
    std::sort(summary.reasons.begin(), summary.reasons.end());
    summary.recompute_dominant();
    return summary;
}

std::string_view to_string(ConflictOutcome outcome) noexcept {
    switch (outcome) {
        case ConflictOutcome::ResolvedByAuthority: return "resolved_by_authority";
        case ConflictOutcome::UnresolvedEqualAuthority: return "unresolved_equal_authority";
        case ConflictOutcome::UnresolvedSameSource: return "unresolved_same_source";
        case ConflictOutcome::RefusedUnknownAuthority: return "refused_unknown_authority";
    }
    return "unresolved_equal_authority";
}

ConflictOutcome resolve_conflict(SourceAuthority a, SourceAuthority b, SourceId source_a,
                                 SourceId source_b, std::optional<SourceId>& winner) {
    winner.reset();
    if (a == SourceAuthority::Unknown || b == SourceAuthority::Unknown) {
        return ConflictOutcome::RefusedUnknownAuthority;
    }
    if (source_a == source_b) {
        return ConflictOutcome::UnresolvedSameSource;
    }
    if (authority_rank(a) > authority_rank(b)) {
        winner = source_a;
        return ConflictOutcome::ResolvedByAuthority;
    }
    if (authority_rank(b) > authority_rank(a)) {
        winner = source_b;
        return ConflictOutcome::ResolvedByAuthority;
    }
    return ConflictOutcome::UnresolvedEqualAuthority;
}

ConflictRecord make_conflict_record(ConflictRecord record) {
    const auto outcome = resolve_conflict(record.authority_a, record.authority_b, record.source_a,
                                          record.source_b, record.winner);
    record.outcome = outcome;
    DigestBuilder builder(kDomainEvidence);
    builder.id(record.series);
    builder.id(record.path);
    builder.id(record.generation);
    builder.u64(record.sequence.value());
    builder.id(record.source_a);
    builder.id(record.source_b);
    builder.raw(std::span<const std::uint8_t>(record.content_a.data(), Digest::kBytes));
    builder.raw(std::span<const std::uint8_t>(record.content_b.data(), Digest::kBytes));
    record.identity = builder.digest();
    record.reason = std::string(to_string(outcome));
    return record;
}

bool ConflictLog::append(ConflictRecord record) {
    for (const ConflictRecord& existing : records_) {
        if (existing.identity == record.identity) {
            return false;  // idempotent: the same contradiction is recorded once
        }
    }
    records_.push_back(std::move(record));
    std::sort(records_.begin(), records_.end(),
              [](const ConflictRecord& a, const ConflictRecord& b) { return a.identity < b.identity; });
    if (records_.size() > Limits::kMaxConflictsRetained) {
        records_.erase(records_.begin());
        ++dropped_;
    }
    return true;
}

std::vector<ConflictRecord> ConflictLog::for_series(SeriesId series) const {
    std::vector<ConflictRecord> out;
    for (const ConflictRecord& record : records_) {
        if (record.series == series) {
            out.push_back(record);
        }
    }
    return out;
}

bool ConflictLog::has_conflict(SeriesId series, GenerationId generation) const noexcept {
    for (const ConflictRecord& record : records_) {
        if (record.series == series && record.generation == generation) {
            return true;
        }
    }
    return false;
}

void ConflictLog::clear() noexcept {
    records_.clear();
    dropped_ = 0;
}

}  // namespace jitter
