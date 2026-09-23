// Jitter Observatory - baseline comparison and per hop attribution.
// Copyright 2026 Summon Software Labs.
#include <vector>

#include <jitter/attribution.hpp>
#include <jitter/baseline.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

Baseline make_baseline_for(SeriesId series, PathId path, GenerationId generation, Ordinal ordinal,
                           WindowPolicyId window, InstabilityPolicyId policy,
                           const std::vector<std::int64_t>& latencies, double scalar) {
    Baseline baseline;
    baseline.name = "unit-baseline";
    baseline.series = series;
    baseline.path = path;
    baseline.generation = generation;
    baseline.generation_ordinal = ordinal;
    baseline.window = window;
    baseline.policy = policy;
    baseline.sample_count = static_cast<std::uint64_t>(latencies.size());
    baseline.fresh_sample_count = baseline.sample_count;
    baseline.first_observation_utc_ns = 100;
    baseline.last_observation_utc_ns = 200;
    const auto computed = metric_registry().compute_all(default_metric_set(), latencies);
    for (const MetricValue& value : computed.values.ordered()) {
        MetricValue scaled = value;
        scaled.value = value.value * scalar;
        baseline.metrics.push_back(scaled);
    }
    auto made = make_baseline(std::move(baseline));
    return made.ok() ? made.value() : Baseline{};
}

CurrentSnapshot make_snapshot(SeriesId series, PathId path, GenerationId generation, Ordinal ordinal,
                              WindowPolicyId window, InstabilityPolicyId policy,
                              const std::vector<std::int64_t>& latencies, bool fresh) {
    CurrentSnapshot snapshot;
    snapshot.series = series;
    snapshot.path = path;
    snapshot.generation = generation;
    snapshot.generation_ordinal = ordinal;
    snapshot.window = window;
    snapshot.policy = policy;
    snapshot.sample_count = static_cast<std::uint64_t>(latencies.size());
    snapshot.evidence.total = snapshot.sample_count;
    if (fresh) {
        snapshot.evidence.fresh = snapshot.sample_count;
    } else {
        snapshot.evidence.stale = snapshot.sample_count;
    }
    snapshot.evidence.recompute_dominant();
    const auto computed = metric_registry().compute_all(default_metric_set(), latencies);
    for (const MetricValue& value : computed.values.ordered()) {
        snapshot.metrics.insert(value);
    }
    return snapshot;
}

}  // namespace

JITTER_TEST(baseline, identical_scope_compares_and_reports_deltas) {
    const SeriesId series = synthetic_series_id("baseline-series");
    const PathId path = synthetic_path_id("baseline-path");
    const GenerationId generation = synthetic_generation_id("baseline-generation");
    const Ordinal ordinal(1);
    const WindowPolicy policy;
    const InstabilityPolicy instability = default_instability_policy();
    const WindowPolicyId window_id = make_window_policy_id(policy);
    const InstabilityPolicyId policy_id = make_instability_policy_id(instability);

    const std::vector<std::int64_t> baseline_series{100, 110, 120, 130, 140, 150};
    const std::vector<std::int64_t> current_series{100, 160, 120, 260, 140, 620};

    const Baseline baseline = make_baseline_for(series, path, generation, ordinal, window_id,
                                                policy_id, baseline_series, 1.0);
    CHECK(!baseline.id.is_nil());
    CHECK_EQ(baseline.metrics.size(), default_metric_set().size());
    CHECK(baseline.span_ns().has_value());
    CHECK_EQ(baseline.span_ns().value(), std::int64_t{100});

    const CurrentSnapshot snapshot =
        make_snapshot(series, path, generation, ordinal, window_id, policy_id, current_series, true);
    const BaselineComparisonReport report = compare_to_baseline(baseline, snapshot);
    CHECK_EQ(report.overall, ComparisonVerdict::Comparable);
    CHECK(!report.generation_changed);
    CHECK(report.comparisons.size() >= std::size_t{10});

    bool saw_delta = false;
    for (const MetricComparison& comparison : report.comparisons) {
        CHECK_EQ(comparison.verdict, ComparisonVerdict::Comparable);
        CHECK(comparison.delta.has_value());
        // A ratio is only defined against a non zero baseline; a zero baseline is
        // reported as such instead of producing an infinity.
        if (comparison.baseline_value.has_value() && comparison.baseline_value.value() != 0.0) {
            CHECK(comparison.ratio.has_value());
        } else {
            CHECK(!comparison.ratio.has_value());
            CHECK(comparison.reason.find("zero_baseline") != std::string::npos);
        }
        if (comparison.name == std::string_view("jitter.absolute_delta.mean")) {
            saw_delta = true;
            CHECK(comparison.delta.value() > 0.0);
        }
    }
    CHECK(saw_delta);
}

JITTER_TEST(baseline, a_route_generation_change_refuses_the_comparison) {
    const SeriesId series = synthetic_series_id("generation-series");
    const PathId path = synthetic_path_id("generation-path");
    const WindowPolicy policy;
    const InstabilityPolicy instability = default_instability_policy();
    const WindowPolicyId window_id = make_window_policy_id(policy);
    const InstabilityPolicyId policy_id = make_instability_policy_id(instability);

    const std::vector<std::int64_t> series_values{100, 110, 120, 130, 140, 150};
    const Baseline baseline = make_baseline_for(series, path, synthetic_generation_id("gen-1"),
                                                Ordinal(1), window_id, policy_id, series_values, 1.0);
    const CurrentSnapshot snapshot =
        make_snapshot(series, path, synthetic_generation_id("gen-2"), Ordinal(2), window_id,
                      policy_id, series_values, true);
    const BaselineComparisonReport report = compare_to_baseline(baseline, snapshot);
    CHECK_EQ(report.overall, ComparisonVerdict::Incomparable);
    CHECK(report.generation_changed);
    for (const MetricComparison& comparison : report.comparisons) {
        CHECK_EQ(comparison.verdict, ComparisonVerdict::Incomparable);
        CHECK_EQ(comparison.reason, std::string("generation_changed"));
        CHECK(!comparison.delta.has_value());
    }
}

JITTER_TEST(baseline, different_window_policies_are_incomparable) {
    const SeriesId series = synthetic_series_id("window-series");
    const PathId path = synthetic_path_id("window-path");
    const GenerationId generation = synthetic_generation_id("window-generation");
    const InstabilityPolicy instability = default_instability_policy();
    const InstabilityPolicyId policy_id = make_instability_policy_id(instability);

    WindowPolicy small;
    small.kind = WindowKind::Count;
    small.count = 64;
    small.capacity = 64;
    WindowPolicy large;
    large.kind = WindowKind::Count;
    large.count = 128;
    large.capacity = 128;

    const std::vector<std::int64_t> values{100, 110, 120, 130};
    const Baseline baseline = make_baseline_for(series, path, generation, Ordinal(1),
                                                make_window_policy_id(small), policy_id, values, 1.0);
    const CurrentSnapshot snapshot = make_snapshot(series, path, generation, Ordinal(1),
                                                   make_window_policy_id(large), policy_id, values, true);
    const BaselineComparisonReport report = compare_to_baseline(baseline, snapshot);
    CHECK_EQ(report.overall, ComparisonVerdict::Incomparable);
    CHECK(report.window_changed);
}

JITTER_TEST(baseline, non_fresh_evidence_yields_no_delta) {
    const SeriesId series = synthetic_series_id("stale-series");
    const PathId path = synthetic_path_id("stale-path");
    const GenerationId generation = synthetic_generation_id("stale-generation");
    const WindowPolicy policy;
    const InstabilityPolicy instability = default_instability_policy();
    const WindowPolicyId window_id = make_window_policy_id(policy);
    const InstabilityPolicyId policy_id = make_instability_policy_id(instability);
    const std::vector<std::int64_t> values{100, 900, 120, 1500, 140, 3000};

    const Baseline baseline = make_baseline_for(series, path, generation, Ordinal(1), window_id,
                                                policy_id, values, 1.0);
    const CurrentSnapshot snapshot = make_snapshot(series, path, generation, Ordinal(1), window_id,
                                                   policy_id, values, false);
    const BaselineComparisonReport report = compare_to_baseline(baseline, snapshot);
    CHECK_EQ(report.overall, ComparisonVerdict::Stale);
    for (const MetricComparison& comparison : report.comparisons) {
        CHECK_EQ(comparison.verdict, ComparisonVerdict::Stale);
        CHECK(!comparison.delta.has_value());
    }
}

JITTER_TEST(baseline, baseline_construction_rejects_duplicates_and_empty_identity) {
    Baseline baseline;
    baseline.name = "duplicate";
    baseline.series = synthetic_series_id("dup-series");
    baseline.generation = synthetic_generation_id("dup-generation");
    MetricValue value;
    value.id = metric_registry().definition(MetricKey::LatencyMean).id;
    value.key = MetricKey::LatencyMean;
    value.name = metric_registry().definition(MetricKey::LatencyMean).name;
    value.value = 1.0;
    baseline.metrics.push_back(value);
    baseline.metrics.push_back(value);
    CHECK_ERR(make_baseline(baseline), ErrorCode::Duplicate);

    Baseline nameless;
    nameless.series = synthetic_series_id("nameless");
    nameless.generation = synthetic_generation_id("nameless-generation");
    CHECK_ERR(make_baseline(nameless), ErrorCode::InvalidArgument);
}

JITTER_TEST(attribution, timing_absent_hops_are_never_fabricated) {
    AttributionRequest request;
    request.series = synthetic_series_id("attr-series");
    request.path = synthetic_path_id("attr-path");
    request.generation = synthetic_generation_id("attr-generation");
    request.generation_ordinal = Ordinal(1);
    request.series_clock = synthetic_clock_id("attr-series-clock");
    request.metric = MetricKey::AbsoluteDeltaMean;
    request.min_arrivals = 4;
    request.current_generation = true;

    ClockModel clocks;
    ClockDomainDescriptor series_clock;
    series_clock.name = "attr-series-clock";
    series_clock.kind = ClockKind::Synthetic;
    series_clock.unit = TimeUnit::Nanoseconds;
    series_clock.epoch_note = "test";
    CHECK_OK(clocks.register_domain(series_clock));
    request.series_clock = series_clock.id;
    request.clocks = &clocks;

    PathDescriptor path;
    path.name = "attr-path";
    path.origin = EvidenceOrigin::Synthetic;
    HopDescriptor first;
    first.index = 0;
    first.name = "first";
    first.timing_clock = series_clock.id;
    first.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(first);
    HopDescriptor second;
    second.index = 1;
    second.name = "second";
    second.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(second);
    path.id = make_path_id(path);
    for (HopDescriptor& hop : path.hops) {
        hop.id = make_hop_id(hop, path.id);
    }
    request.path_descriptor = &path;
    request.path = path.id;

    std::vector<TimePoint> arrivals;
    for (int i = 0; i < 8; ++i) {
        TimePoint point;
        point.domain = series_clock.id;
        point.ticks = 1000 + (i * 10) + ((i % 3) * 40);
        arrivals.push_back(point);
    }
    request.arrivals_by_hop[path.hops[0].id] = arrivals;
    // The second hop emitted nothing at all.

    const PathAttributionReport report = attribute_hops(request);
    CHECK(!report.complete);
    CHECK_EQ(report.total_hops, std::uint64_t{2});
    CHECK_EQ(report.attributed_hops, std::uint64_t{1});
    REQUIRE(report.hops.size() == 2);
    CHECK_EQ(report.hops[0].state, AttributionState::Attributed);
    CHECK(report.hops[0].value_present);
    CHECK(report.hops[0].terms > 0);
    CHECK_EQ(report.hops[1].state, AttributionState::Absent);
    CHECK_EQ(report.hops[1].reason, std::string("hop_timing_absent"));
    CHECK(!report.hops[1].value_present);
    CHECK_EQ(report.hops[1].value, 0.0);
    CHECK(report.limitation_note.find("not a causal decomposition") != std::string::npos);
}

JITTER_TEST(attribution, incomparable_hop_clocks_yield_no_value) {
    ClockModel clocks;
    ClockDomainDescriptor series_clock;
    series_clock.name = "series-clock";
    series_clock.kind = ClockKind::Synthetic;
    series_clock.unit = TimeUnit::Nanoseconds;
    series_clock.epoch_note = "test";
    CHECK_OK(clocks.register_domain(series_clock));

    ClockDomainDescriptor counter_clock;
    counter_clock.name = "counter-clock";
    counter_clock.kind = ClockKind::HardwareCounter;
    counter_clock.unit = TimeUnit::CounterTicks;
    counter_clock.epoch_note = "free running counter, no declared relation";
    CHECK_OK(clocks.register_domain(counter_clock));

    PathDescriptor path;
    path.name = "mixed-path";
    path.origin = EvidenceOrigin::Synthetic;
    HopDescriptor first;
    first.index = 0;
    first.name = "comparable";
    first.timing_clock = series_clock.id;
    first.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(first);
    HopDescriptor second;
    second.index = 1;
    second.name = "incomparable";
    second.timing_clock = counter_clock.id;
    second.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(second);
    path.id = make_path_id(path);
    for (HopDescriptor& hop : path.hops) {
        hop.id = make_hop_id(hop, path.id);
    }

    AttributionRequest request;
    request.series = synthetic_series_id("mixed-series");
    request.path = path.id;
    request.generation = synthetic_generation_id("mixed-generation");
    request.generation_ordinal = Ordinal(1);
    request.series_clock = series_clock.id;
    request.metric = MetricKey::AbsoluteDeltaMean;
    request.min_arrivals = 4;
    request.current_generation = true;
    request.clocks = &clocks;
    request.path_descriptor = &path;

    std::vector<TimePoint> comparable;
    std::vector<TimePoint> incomparable;
    for (int i = 0; i < 8; ++i) {
        TimePoint nanos_point;
        nanos_point.domain = series_clock.id;
        nanos_point.ticks = 1000 + (i * 10);
        comparable.push_back(nanos_point);
        TimePoint counter_point;
        counter_point.domain = counter_clock.id;
        counter_point.ticks = 5000 + (i * 7);
        incomparable.push_back(counter_point);
    }
    request.arrivals_by_hop[path.hops[0].id] = comparable;
    request.arrivals_by_hop[path.hops[1].id] = incomparable;

    const PathAttributionReport report = attribute_hops(request);
    CHECK(!report.complete);
    REQUIRE(report.hops.size() == 2);
    CHECK_EQ(report.hops[0].state, AttributionState::Attributed);
    CHECK_EQ(report.hops[1].state, AttributionState::Unsupported);
    CHECK(!report.hops[1].value_present);
    CHECK(report.hops[1].reason.find("hop_clock_incomparable") != std::string::npos);
    CHECK(!report.hops[1].clock_comparable);
}

JITTER_TEST(attribution, a_declared_equivalence_enables_attribution) {
    ClockModel clocks;
    ClockDomainDescriptor series_clock;
    series_clock.name = "declared-series-clock";
    series_clock.kind = ClockKind::Synthetic;
    series_clock.unit = TimeUnit::Nanoseconds;
    series_clock.epoch_note = "test";
    CHECK_OK(clocks.register_domain(series_clock));

    ClockDomainDescriptor hop_clock;
    hop_clock.name = "declared-hop-clock";
    hop_clock.kind = ClockKind::Synthetic;
    hop_clock.unit = TimeUnit::Nanoseconds;
    hop_clock.epoch_note = "test";
    CHECK_OK(clocks.register_domain(hop_clock));

    ClockEquivalence equivalence;
    equivalence.a = series_clock.id;
    equivalence.b = hop_clock.id;
    equivalence.max_offset_ns = 100;
    equivalence.declared_by_authority = SourceAuthority::Authoritative;
    equivalence.justification = "unit test declaration";
    CHECK_OK(clocks.declare_equivalence(equivalence));

    PathDescriptor path;
    path.name = "declared-path";
    path.origin = EvidenceOrigin::Synthetic;
    HopDescriptor hop;
    hop.index = 0;
    hop.name = "declared-hop";
    hop.timing_clock = hop_clock.id;
    hop.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(hop);
    path.id = make_path_id(path);
    path.hops[0].id = make_hop_id(path.hops[0], path.id);

    AttributionRequest request;
    request.series = synthetic_series_id("declared-series");
    request.path = path.id;
    request.generation = synthetic_generation_id("declared-generation");
    request.generation_ordinal = Ordinal(1);
    request.series_clock = series_clock.id;
    request.metric = MetricKey::AbsoluteDeltaMean;
    request.min_arrivals = 4;
    request.current_generation = true;
    request.clocks = &clocks;
    request.path_descriptor = &path;

    std::vector<TimePoint> arrivals;
    for (int i = 0; i < 8; ++i) {
        TimePoint point;
        point.domain = hop_clock.id;
        point.ticks = 2000 + (i * 25) + ((i % 2) * 100);
        arrivals.push_back(point);
    }
    request.arrivals_by_hop[path.hops[0].id] = arrivals;

    const PathAttributionReport report = attribute_hops(request);
    CHECK(report.complete);
    CHECK_EQ(report.attributed_hops, std::uint64_t{1});
    REQUIRE(report.hops.size() == 1);
    CHECK_EQ(report.hops[0].state, AttributionState::Attributed);
    CHECK(report.hops[0].value_present);
    CHECK_EQ(report.hops[0].clock_max_offset_ns, std::int64_t{100});
}

JITTER_TEST(attribution, too_few_arrivals_is_insufficient_not_attributed) {
    ClockModel clocks;
    ClockDomainDescriptor series_clock;
    series_clock.name = "few-series-clock";
    series_clock.kind = ClockKind::Synthetic;
    series_clock.unit = TimeUnit::Nanoseconds;
    series_clock.epoch_note = "test";
    CHECK_OK(clocks.register_domain(series_clock));

    PathDescriptor path;
    path.name = "few-path";
    path.origin = EvidenceOrigin::Synthetic;
    HopDescriptor hop;
    hop.index = 0;
    hop.name = "few-hop";
    hop.timing_clock = series_clock.id;
    hop.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(hop);
    path.id = make_path_id(path);
    path.hops[0].id = make_hop_id(path.hops[0], path.id);

    AttributionRequest request;
    request.series = synthetic_series_id("few-series");
    request.path = path.id;
    request.generation = synthetic_generation_id("few-generation");
    request.generation_ordinal = Ordinal(1);
    request.series_clock = series_clock.id;
    request.metric = MetricKey::AbsoluteDeltaMean;
    request.min_arrivals = 8;
    request.current_generation = true;
    request.clocks = &clocks;
    request.path_descriptor = &path;

    std::vector<TimePoint> arrivals;
    for (int i = 0; i < 4; ++i) {
        TimePoint point;
        point.domain = series_clock.id;
        point.ticks = 100 + (i * 30);
        arrivals.push_back(point);
    }
    request.arrivals_by_hop[path.hops[0].id] = arrivals;

    const PathAttributionReport report = attribute_hops(request);
    CHECK(!report.complete);
    REQUIRE(report.hops.size() == 1);
    CHECK_EQ(report.hops[0].state, AttributionState::Insufficient);
    CHECK_EQ(report.hops[0].reason, std::string("fewer_arrivals_than_required"));
}

JITTER_TEST(attribution, mixing_clock_domains_inside_one_hop_is_refused) {
    ClockModel clocks;
    ClockDomainDescriptor series_clock;
    series_clock.name = "mixin-series-clock";
    series_clock.kind = ClockKind::Synthetic;
    series_clock.unit = TimeUnit::Nanoseconds;
    series_clock.epoch_note = "test";
    CHECK_OK(clocks.register_domain(series_clock));

    PathDescriptor path;
    path.name = "mix-in-path";
    path.origin = EvidenceOrigin::Synthetic;
    HopDescriptor hop;
    hop.index = 0;
    hop.name = "mixed-hop";
    hop.timing_clock = series_clock.id;
    hop.origin = EvidenceOrigin::Synthetic;
    path.hops.push_back(hop);
    path.id = make_path_id(path);
    path.hops[0].id = make_hop_id(path.hops[0], path.id);

    AttributionRequest request;
    request.series = synthetic_series_id("mix-in-series");
    request.path = path.id;
    request.generation = synthetic_generation_id("mix-in-generation");
    request.generation_ordinal = Ordinal(1);
    request.series_clock = series_clock.id;
    request.min_arrivals = 2;
    request.current_generation = true;
    request.clocks = &clocks;
    request.path_descriptor = &path;

    std::vector<TimePoint> arrivals;
    TimePoint first;
    first.domain = series_clock.id;
    first.ticks = 10;
    arrivals.push_back(first);
    TimePoint second;
    second.domain = synthetic_clock_id("a-different-domain");
    second.ticks = 20;
    arrivals.push_back(second);
    request.arrivals_by_hop[path.hops[0].id] = arrivals;

    const PathAttributionReport report = attribute_hops(request);
    REQUIRE(report.hops.size() == 1);
    CHECK_EQ(report.hops[0].state, AttributionState::Unsupported);
    CHECK_EQ(report.hops[0].reason, std::string("hop_timing_mixes_clock_domains"));
}
