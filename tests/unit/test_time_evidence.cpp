// Jitter Observatory - clock comparability, freshness and conflict semantics.
// Copyright 2026 Summon Software Labs.
#include <jitter/evidence.hpp>
#include <jitter/time.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

ClockDomainDescriptor nanos_clock(std::string name, ClockKind kind = ClockKind::Synthetic,
                                 TimeUnit unit = TimeUnit::Nanoseconds) {
    ClockDomainDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.kind = kind;
    descriptor.unit = unit;
    descriptor.epoch_note = "unit test clock";
    descriptor.declared_utc_aligned = false;
    // Identities are content addressed, so a caller that wants to reference a
    // descriptor derives its identity the same way the registry does.
    descriptor.id = make_clock_domain_id(descriptor);
    return descriptor;
}

constexpr std::int64_t kNow = 1000000000ll;

}  // namespace

JITTER_TEST(time, registration_is_idempotent_and_content_addressed) {
    ClockModel model;
    ClockDomainDescriptor descriptor = nanos_clock("alpha");
    CHECK_OK(model.register_domain(descriptor));
    CHECK_OK(model.register_domain(descriptor));
    CHECK_EQ(model.domain_count(), std::size_t{1});
    CHECK(model.has_domain(descriptor.id));

    ClockDomainDescriptor tampered = descriptor;
    tampered.id = synthetic_clock_id("something-else");
    CHECK_ERR(model.register_domain(tampered), ErrorCode::InvalidArgument);

    ClockDomainDescriptor empty = descriptor;
    empty.name.clear();
    empty.id = ClockDomainId{};
    CHECK_ERR(model.register_domain(empty), ErrorCode::InvalidArgument);
}

JITTER_TEST(time, comparability_requires_the_same_or_a_declared_domain) {
    ClockModel model;
    ClockDomainDescriptor alpha = nanos_clock("alpha");
    ClockDomainDescriptor beta = nanos_clock("beta");
    CHECK_OK(model.register_domain(alpha));
    CHECK_OK(model.register_domain(beta));

    const ComparabilityAssessment same = model.assess(alpha.id, alpha.id, kNow);
    CHECK_EQ(same.verdict, ComparabilityVerdict::Comparable);
    CHECK_EQ(same.reason, std::string("same_domain"));

    const ComparabilityAssessment undeclared = model.assess(alpha.id, beta.id, kNow);
    CHECK_EQ(undeclared.verdict, ComparabilityVerdict::Incomparable);
    CHECK_EQ(undeclared.reason, std::string("distinct_undeclared_domains"));

    const ComparabilityAssessment unknown = model.assess(synthetic_clock_id("ghost"), alpha.id, kNow);
    CHECK_EQ(unknown.verdict, ComparabilityVerdict::Unknown);
    CHECK_EQ(unknown.reason, std::string("domain_unknown"));

    ClockEquivalence weak;
    weak.a = alpha.id;
    weak.b = beta.id;
    weak.max_offset_ns = 10;
    weak.declared_by_authority = SourceAuthority::Advisory;
    weak.justification = "not authoritative enough";
    CHECK_ERR(model.declare_equivalence(weak), ErrorCode::PolicyViolation);
    CHECK_EQ(model.assess(alpha.id, beta.id, kNow).verdict, ComparabilityVerdict::Incomparable);

    weak.declared_by_authority = SourceAuthority::Authoritative;
    weak.justification.clear();
    CHECK_ERR(model.declare_equivalence(weak), ErrorCode::InvalidArgument);

    weak.justification = "declared by the operator for the unit test";
    CHECK_OK(model.declare_equivalence(weak));
    const ComparabilityAssessment declared = model.assess(alpha.id, beta.id, kNow);
    CHECK_EQ(declared.verdict, ComparabilityVerdict::Comparable);
    CHECK_EQ(declared.reason, std::string("declared_equivalence"));
    CHECK_EQ(declared.max_offset_ns, std::int64_t{10});
    CHECK_ERR(model.declare_equivalence(weak), ErrorCode::Duplicate);
}

JITTER_TEST(time, equivalence_expiry_and_offset_bounds_are_enforced) {
    ClockModel model;
    ClockDomainDescriptor alpha = nanos_clock("alpha");
    ClockDomainDescriptor beta = nanos_clock("beta");
    CHECK_OK(model.register_domain(alpha));
    CHECK_OK(model.register_domain(beta));

    ClockEquivalence equivalence;
    equivalence.a = alpha.id;
    equivalence.b = beta.id;
    equivalence.max_offset_ns = 10;
    equivalence.declared_by_authority = SourceAuthority::Authoritative;
    equivalence.justification = "expiring declaration";
    equivalence.valid_until_utc_ns = kNow - 1;
    CHECK_OK(model.declare_equivalence(equivalence));
    const ComparabilityAssessment expired = model.assess(alpha.id, beta.id, kNow);
    CHECK_EQ(expired.verdict, ComparabilityVerdict::Incomparable);
    CHECK_EQ(expired.reason, std::string("declaration_expired"));

    ClockModel bounded;
    CHECK_OK(bounded.register_domain(alpha));
    CHECK_OK(bounded.register_domain(beta));
    ClockEquivalence huge = equivalence;
    huge.valid_until_utc_ns.reset();
    huge.max_offset_ns = 1000000000;
    CHECK_ERR(bounded.declare_equivalence(huge), ErrorCode::PolicyViolation);
}

JITTER_TEST(time, units_without_a_declared_rate_are_never_converted) {
    ClockModel model;
    ClockDomainDescriptor counters = nanos_clock("counters", ClockKind::HardwareCounter,
                                                       TimeUnit::CounterTicks);
    ClockDomainDescriptor alpha = nanos_clock("alpha");
    CHECK_OK(model.register_domain(counters));
    CHECK_OK(model.register_domain(alpha));

    ClockEquivalence equivalence;
    equivalence.a = counters.id;
    equivalence.b = alpha.id;
    equivalence.max_offset_ns = 5;
    equivalence.declared_by_authority = SourceAuthority::Authoritative;
    equivalence.justification = "declared anyway";
    CHECK_OK(model.declare_equivalence(equivalence));

    const ComparabilityAssessment assessment = model.assess(counters.id, alpha.id, kNow);
    CHECK_EQ(assessment.verdict, ComparabilityVerdict::Incomparable);
    CHECK_EQ(assessment.reason, std::string("unit_not_convertible"));

    TimePoint left;
    left.domain = counters.id;
    left.ticks = 10;
    TimePoint right;
    right.domain = alpha.id;
    right.ticks = 20;
    CHECK_ERR(model.difference(left, right, kNow), ErrorCode::Incomparable);
    CHECK(!nanos_per_unit(TimeUnit::CounterTicks).has_value());
    CHECK(!nanos_per_unit(TimeUnit::Unknown).has_value());
    CHECK_EQ(nanos_per_unit(TimeUnit::Microseconds).value(), std::int64_t{1000});
}

JITTER_TEST(time, in_domain_difference_is_always_available) {
    TimePoint a;
    a.domain = synthetic_clock_id("alpha");
    a.ticks = 100;
    TimePoint b;
    b.domain = a.domain;
    b.ticks = 250;
    CHECK_EQ(ClockModel::in_domain_delta(a, b).value(), std::int64_t{150});

    TimePoint other;
    other.domain = synthetic_clock_id("beta");
    other.ticks = 10;
    CHECK_ERR(ClockModel::in_domain_delta(a, other), ErrorCode::Incomparable);
}

JITTER_TEST(evidence, freshness_states_are_explicit_and_ordered) {
    ClockModel model;
    ClockDomainDescriptor local = LocalClockDomain::descriptor();
    CHECK_OK(model.register_domain(local));
    ClockDomainDescriptor observation = nanos_clock("observation");
    CHECK_OK(model.register_domain(observation));

    ClockEquivalence equivalence;
    equivalence.a = observation.id;
    equivalence.b = local.id;
    equivalence.max_offset_ns = 0;
    equivalence.declared_by_authority = SourceAuthority::Authoritative;
    equivalence.justification = "the observation clock is the local clock";
    CHECK_OK(model.declare_equivalence(equivalence));

    FreshnessPolicy policy;
    policy.max_ingest_age_ns = 1000;
    policy.expiry_ingest_age_ns = 10000;
    policy.max_observation_age_ns = 1000;
    policy.expiry_observation_age_ns = 10000;
    policy.future_tolerance_ns = 100;

    SampleSpec spec;
    spec.observation_clock = observation.id;
    spec.source = synthetic_source_id("evidence-source");
    spec.series = synthetic_series_id("evidence-series");
    spec.path = synthetic_path_id("evidence-path");
    spec.generation = synthetic_generation_id("evidence-generation");
    spec.observed_ticks = 1000;
    spec.received_ticks = 1000;

    auto sample = build_sample(spec);
    REQUIRE_OK(sample);

    const SampleAssessment fresh = assess_sample(sample.value(), model, 1500, policy);
    CHECK_EQ(fresh.state, EvidenceState::Fresh);

    const SampleAssessment stale = assess_sample(sample.value(), model, 5000, policy);
    CHECK_EQ(stale.state, EvidenceState::Stale);
    CHECK_EQ(stale.reason, std::string("ingest_age_exceeds_budget"));

    const SampleAssessment expired = assess_sample(sample.value(), model, 50000, policy);
    CHECK_EQ(expired.state, EvidenceState::Expired);

    SampleSpec future = spec;
    future.received_ticks = 1000;
    future.observed_ticks = 5000;
    auto future_sample = build_sample(future);
    REQUIRE_OK(future_sample);
    const SampleAssessment ahead = assess_sample(future_sample.value(), model, 1000, policy);
    CHECK_EQ(ahead.state, EvidenceState::Unknown);
    CHECK_EQ(ahead.reason, std::string("observation_time_is_in_the_future"));
    CHECK(ahead.ages.observation_age_ns.has_value());
    CHECK(ahead.ages.observation_age_ns.value() < 0);
}

JITTER_TEST(evidence, incomparable_clocks_yield_unknown_not_fresh) {
    ClockModel model;
    ClockDomainDescriptor local_domain = LocalClockDomain::descriptor();
    CHECK_OK(model.register_domain(local_domain));
    ClockDomainDescriptor observation = nanos_clock("isolated");
    CHECK_OK(model.register_domain(observation));

    FreshnessPolicy policy;
    policy.require_observation_age = true;

    SampleSpec spec;
    spec.observation_clock = observation.id;
    spec.source = synthetic_source_id("isolated-source");
    spec.series = synthetic_series_id("isolated-series");
    spec.path = synthetic_path_id("isolated-path");
    spec.generation = synthetic_generation_id("isolated-generation");
    spec.observed_ticks = 1000;
    spec.received_ticks = 1000;
    auto sample = build_sample(spec);
    REQUIRE_OK(sample);

    const SampleAssessment assessment = assess_sample(sample.value(), model, 1100, policy);
    CHECK_EQ(assessment.state, EvidenceState::Unsupported);
    CHECK_EQ(assessment.ages.comparability, ComparabilityVerdict::Incomparable);
    CHECK(!assessment.ages.observation_age_ns.has_value());
    CHECK(assessment.ages.ingest_age_ns.has_value());
    CHECK(!is_positive_evidence(assessment.state));

    // A known incomparable clock is a structural limit, not a policy choice: relaxing
    // the requirement cannot make an unmeasurable age measurable.
    FreshnessPolicy lax = policy;
    lax.require_observation_age = false;
    const SampleAssessment permissive = assess_sample(sample.value(), model, 1100, lax);
    CHECK_EQ(permissive.state, EvidenceState::Unsupported);

    // The policy choice does govern the case where the domain is simply not registered.
    ClockModel partial;
    ClockDomainDescriptor partial_local = LocalClockDomain::descriptor();
    REQUIRE_OK(partial.register_domain(partial_local));
    SampleSpec unknown_clock = spec;
    unknown_clock.observation_clock = synthetic_clock_id("never-registered-domain");
    auto unknown_sample = build_sample(unknown_clock);
    REQUIRE_OK(unknown_sample);
    const SampleAssessment strict_unknown = assess_sample(unknown_sample.value(), partial, 1100, policy);
    CHECK_EQ(strict_unknown.state, EvidenceState::Unknown);
    CHECK_EQ(strict_unknown.ages.comparability, ComparabilityVerdict::Unknown);
    const SampleAssessment relaxed_unknown = assess_sample(unknown_sample.value(), partial, 1100, lax);
    CHECK_EQ(relaxed_unknown.state, EvidenceState::Fresh);
}

JITTER_TEST(evidence, conflict_retention_is_bounded_and_counted) {
    ConflictLog log;
    const SeriesId series = synthetic_series_id("retention-series");
    for (std::uint64_t i = 0; i < Limits::kMaxConflictsRetained + 4; ++i) {
        ConflictRecord record;
        record.series = series;
        record.path = synthetic_path_id("retention-path");
        record.generation = synthetic_generation_id("retention-generation");
        record.sequence = SourceSequence(i + 1);
        record.source_a = synthetic_source_id("retention-a");
        record.source_b = synthetic_source_id("retention-b");
        record.authority_a = SourceAuthority::Advisory;
        record.authority_b = SourceAuthority::Advisory;
        record = make_conflict_record(record);
        CHECK(log.append(record));
    }
    CHECK_EQ(log.size(), static_cast<std::size_t>(Limits::kMaxConflictsRetained));
    CHECK_EQ(log.dropped(), std::uint64_t{4});
    CHECK_EQ(log.for_series(series).size(), static_cast<std::size_t>(Limits::kMaxConflictsRetained));
}

JITTER_TEST(evidence, summary_dominance_and_positive_assertion_rule) {
    EvidenceSummary empty;
    empty.recompute_dominant();
    CHECK_EQ(empty.dominant, EvidenceState::Missing);
    CHECK(!empty.admits_positive_assertion());

    EvidenceSummary stale_only;
    stale_only.total = 4;
    stale_only.stale = 4;
    stale_only.recompute_dominant();
    CHECK_EQ(stale_only.dominant, EvidenceState::Stale);
    CHECK(!stale_only.admits_positive_assertion());

    EvidenceSummary mixed;
    mixed.total = 10;
    mixed.fresh = 9;
    mixed.conflicting = 1;
    mixed.recompute_dominant();
    CHECK_EQ(mixed.dominant, EvidenceState::Conflicting);
    CHECK(!mixed.admits_positive_assertion());

    EvidenceSummary healthy;
    healthy.total = 10;
    healthy.fresh = 10;
    healthy.recompute_dominant();
    CHECK_EQ(healthy.dominant, EvidenceState::Fresh);
    CHECK(healthy.admits_positive_assertion());

    std::vector<SampleAssessment> assessments;
    SampleAssessment one;
    one.state = EvidenceState::Stale;
    one.reason = "ingest_age_exceeds_budget";
    assessments.push_back(one);
    SampleAssessment two;
    two.state = EvidenceState::Fresh;
    two.reason = "within_all_declared_budgets";
    assessments.push_back(two);
    const EvidenceSummary summary = summarize_evidence(assessments);
    CHECK_EQ(summary.total, std::uint64_t{2});
    CHECK_EQ(summary.fresh, std::uint64_t{1});
    CHECK_EQ(summary.stale, std::uint64_t{1});
    CHECK_EQ(summary.dominant, EvidenceState::Fresh);
    CHECK_EQ(summary.reasons.size(), std::size_t{2});
}

JITTER_TEST(evidence, conflicts_resolve_by_authority_only) {
    const SourceId weak = synthetic_source_id("weak");
    const SourceId strong = synthetic_source_id("strong");
    std::optional<SourceId> winner;

    CHECK_EQ(resolve_conflict(SourceAuthority::Advisory, SourceAuthority::Authoritative, weak, strong,
                              winner),
             ConflictOutcome::ResolvedByAuthority);
    REQUIRE(winner.has_value());
    CHECK_EQ(winner.value(), strong);

    winner.reset();
    CHECK_EQ(resolve_conflict(SourceAuthority::Advisory, SourceAuthority::Advisory, weak, strong,
                              winner),
             ConflictOutcome::UnresolvedEqualAuthority);
    CHECK(!winner.has_value());

    winner.reset();
    CHECK_EQ(resolve_conflict(SourceAuthority::Authoritative, SourceAuthority::Advisory, weak, weak,
                              winner),
             ConflictOutcome::UnresolvedSameSource);
    CHECK(!winner.has_value());

    winner.reset();
    CHECK_EQ(resolve_conflict(SourceAuthority::Unknown, SourceAuthority::Authoritative, weak, strong,
                              winner),
             ConflictOutcome::RefusedUnknownAuthority);

    ConflictRecord record;
    record.series = synthetic_series_id("conflict-series");
    record.path = synthetic_path_id("conflict-path");
    record.generation = synthetic_generation_id("conflict-generation");
    record.sequence = SourceSequence(7);
    record.source_a = weak;
    record.source_b = strong;
    record.authority_a = SourceAuthority::Advisory;
    record.authority_b = SourceAuthority::Authoritative;
    record = make_conflict_record(record);
    CHECK_EQ(record.outcome, ConflictOutcome::ResolvedByAuthority);
    CHECK(record.resolved());

    ConflictLog log;
    CHECK(log.append(record));
    CHECK(!log.append(record));  // the same contradiction is recorded once
    CHECK_EQ(log.size(), std::size_t{1});
    CHECK_EQ(log.for_series(record.series).size(), std::size_t{1});
    CHECK(log.has_conflict(record.series, record.generation));
    CHECK_EQ(log.dropped(), std::uint64_t{0});
}
