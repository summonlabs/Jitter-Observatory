// Jitter Observatory - metric identities, estimators and window semantics.
// Copyright 2026 Summon Software Labs.
#include <cmath>
#include <vector>

#include <jitter/metrics.hpp>
#include <jitter/window.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

const std::vector<std::int64_t> kSeries{100, 120, 90, 130, 110, 160, 70};

LatencySample window_sample(std::int64_t latency, std::int64_t observed_ticks,
                            std::uint64_t sequence, SeriesId series, PathId path,
                            GenerationId generation) {
    SampleSpec spec;
    spec.latency_ns = latency;
    spec.observed_ticks = observed_ticks;
    spec.received_ticks = observed_ticks + 10;
    spec.observation_clock = synthetic_clock_id("window-observation");
    spec.source = synthetic_source_id("window-source");
    spec.series = series;
    spec.path = path;
    spec.generation = generation;
    spec.sequence = SourceSequence(sequence);
    auto sample = build_sample(spec);
    return sample.ok() ? sample.value() : LatencySample{};
}

}  // namespace

JITTER_TEST(metrics, absolute_delta_estimators_are_exact) {
    // Successive absolute deltas of kSeries: 20, 30, 40, 20, 50, 90
    auto mean = metric_registry().compute(MetricKey::AbsoluteDeltaMean, kSeries);
    REQUIRE_OK(mean);
    CHECK_EQ(mean.value().value, 250.0 / 6.0);
    CHECK_EQ(mean.value().term_count, std::uint64_t{6});
    CHECK_EQ(mean.value().sample_count, std::uint64_t{7});

    auto maximum = metric_registry().compute(MetricKey::AbsoluteDeltaMax, kSeries);
    REQUIRE_OK(maximum);
    CHECK_EQ(maximum.value().value, 90.0);

    auto median = metric_registry().compute(MetricKey::AbsoluteDeltaMedian, kSeries);
    REQUIRE_OK(median);
    // sorted deltas: 20, 20, 30, 40, 50, 90 -> type 7 median of six points
    CHECK_EQ(median.value().value, 35.0);

    auto p95 = metric_registry().compute(MetricKey::AbsoluteDeltaP95, kSeries);
    REQUIRE_OK(p95);
    const double expected_p95 = 50.0 + (0.95 * 5.0 - 4.0) * (90.0 - 50.0);
    CHECK_EQ(p95.value().value, expected_p95);
}

JITTER_TEST(metrics, signed_and_absolute_delta_metrics_never_blur) {
    auto absolute = metric_registry().compute(MetricKey::AbsoluteDeltaMean, kSeries);
    auto signed_value = metric_registry().compute(MetricKey::IpdvMean, kSeries);
    REQUIRE_OK(absolute);
    REQUIRE_OK(signed_value);
    // The signed mean of the deltas is the total drift divided by the pair count.
    CHECK_EQ(signed_value.value().value, static_cast<double>(70 - 100) / 6.0);
    CHECK(absolute.value().value > signed_value.value().value);
    CHECK(absolute.value().id != signed_value.value().id);
    CHECK_ERR(check_comparable(absolute.value(), signed_value.value()), ErrorCode::MetricMismatch);
}

JITTER_TEST(metrics, variance_metrics_are_exactly_related) {
    auto variance = metric_registry().compute(MetricKey::SampleVariance, kSeries);
    auto stddev = metric_registry().compute(MetricKey::SampleStdDev, kSeries);
    auto cv = metric_registry().compute(MetricKey::CoefficientOfVariation, kSeries);
    auto mean = metric_registry().compute(MetricKey::LatencyMean, kSeries);
    REQUIRE_OK(variance);
    REQUIRE_OK(stddev);
    REQUIRE_OK(cv);
    REQUIRE_OK(mean);

    CHECK_EQ(stddev.value().value, std::sqrt(variance.value().value));
    CHECK_EQ(cv.value().value, stddev.value().value / mean.value().value);
    CHECK_EQ(variance.value().unit, MetricUnit::NanosecondsSquared);
    CHECK_EQ(stddev.value().unit, MetricUnit::Nanoseconds);
    CHECK_ERR(check_comparable(variance.value(), stddev.value()), ErrorCode::MetricMismatch);
}

JITTER_TEST(metrics, dispersion_never_exceeds_standard_deviation) {
    const std::vector<std::int64_t> wide{10, 1000, 20, 2000, 30, 3000, 40};
    auto mad = metric_registry().compute(MetricKey::MeanAbsoluteDeviation, wide);
    auto stddev = metric_registry().compute(MetricKey::SampleStdDev, wide);
    REQUIRE_OK(mad);
    REQUIRE_OK(stddev);
    CHECK(mad.value().value <= stddev.value().value + 1e-9);
    CHECK(mad.value().value > 0.0);

    auto median_deviation = metric_registry().compute(MetricKey::MedianAbsoluteDeviation, wide);
    REQUIRE_OK(median_deviation);
    CHECK(median_deviation.value().value <= stddev.value().value + 1e-9);
}

JITTER_TEST(metrics, delta_metrics_are_exactly_translation_invariant) {
    constexpr std::int64_t shift = 1000000;
    std::vector<std::int64_t> shifted;
    shifted.reserve(kSeries.size());
    for (const std::int64_t value : kSeries) {
        shifted.push_back(value + shift);
    }
    for (const MetricKey key : {MetricKey::AbsoluteDeltaMean, MetricKey::AbsoluteDeltaMedian,
                                MetricKey::AbsoluteDeltaP95, MetricKey::AbsoluteDeltaMax,
                                MetricKey::AbsoluteDeltaEwma16}) {
        auto original = metric_registry().compute(key, kSeries);
        auto moved = metric_registry().compute(key, shifted);
        REQUIRE_OK(original);
        REQUIRE_OK(moved);
        CHECK_EQ(original.value().value, moved.value().value);
    }
}

JITTER_TEST(metrics, ewma_matches_its_declared_recurrence) {
    auto ewma = metric_registry().compute(MetricKey::AbsoluteDeltaEwma16, kSeries);
    REQUIRE_OK(ewma);
    const std::vector<double> deltas{20, 30, 40, 20, 50, 90};
    double expected = deltas.front();
    for (std::size_t i = 1; i < deltas.size(); ++i) {
        expected = expected + ((deltas[i] - expected) / 16.0);
    }
    CHECK_EQ(ewma.value().value, expected);
    CHECK_EQ(metric_registry().definition(MetricKey::AbsoluteDeltaEwma16).family, MetricFamily::Ewma);
}

JITTER_TEST(metrics, insufficient_evidence_is_reported_not_guessed) {
    const std::vector<std::int64_t> single{100};
    CHECK_ERR(metric_registry().compute(MetricKey::AbsoluteDeltaMean, single),
              ErrorCode::InsufficientEvidence);
    CHECK_ERR(metric_registry().compute(MetricKey::SampleVariance, single),
              ErrorCode::InsufficientEvidence);
    CHECK_ERR(metric_registry().compute(MetricKey::SampleStdDev, single),
              ErrorCode::InsufficientEvidence);

    const std::vector<std::int64_t> empty;
    CHECK_ERR(metric_registry().compute(MetricKey::LatencyMean, empty), ErrorCode::NoEvidence);

    // A mean of zero makes the coefficient of variation undefined rather than infinite.
    const std::vector<std::int64_t> cancelling{-100, 100, -100, 100};
    CHECK_ERR(metric_registry().compute(MetricKey::CoefficientOfVariation, cancelling),
              ErrorCode::Unsupported);
}

JITTER_TEST(metrics, compute_all_reports_unavailable_metrics) {
    const std::vector<std::int64_t> single{100};
    const auto computed = metric_registry().compute_all(default_metric_set(), single);
    // Only metrics whose declared minimum is one reading can exist here.
    CHECK(computed.values.size() >= std::size_t{2});
    CHECK(computed.values.size() < default_metric_set().size());
    CHECK(computed.unavailable.size() >= std::size_t{8});
    CHECK(computed.values.find(MetricKey::LatencyMean) != nullptr);
    CHECK(computed.values.find(MetricKey::AbsoluteDeltaMean) == nullptr);
    for (const std::string& entry : computed.unavailable) {
        CHECK(entry.find(':') != std::string::npos);
    }
}

JITTER_TEST(metrics, repeated_computation_is_bit_identical) {
    const auto first = metric_registry().compute_all(default_metric_set(), kSeries);
    const auto second = metric_registry().compute_all(default_metric_set(), kSeries);
    CHECK_EQ(first.values.size(), second.values.size());
    for (const MetricValue& value : first.values.ordered()) {
        const MetricValue* other = second.values.find(value.id);
        REQUIRE(other != nullptr);
        CHECK_EQ(value.value, other->value);
        CHECK_EQ(value.term_count, other->term_count);
    }
}

JITTER_TEST(window, count_window_keeps_the_most_recent_observations) {
    WindowPolicy policy;
    policy.kind = WindowKind::Count;
    policy.count = 3;
    policy.capacity = 5;
    CHECK_OK(validate_window_policy(policy));

    const SeriesId series = synthetic_series_id("w-series");
    const PathId path = synthetic_path_id("w-path");
    const GenerationId generation = synthetic_generation_id("w-generation");
    SampleWindow window(policy);
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK_OK(window.add(window_sample(static_cast<std::int64_t>(100 + i),
                                          static_cast<std::int64_t>(1000 + i), i + 1, series, path,
                                          generation)));
    }
    CHECK_EQ(window.retained(), std::size_t{5});
    const WindowSelection selection = window.select();
    CHECK_EQ(selection.samples.size(), std::size_t{3});
    CHECK_EQ(selection.excluded_by_policy, std::uint64_t{2});
    CHECK_EQ(selection.latencies_ns(), (std::vector<std::int64_t>{102, 103, 104}));
    CHECK_EQ(selection.selection_reason, std::string("most_recent_count"));
}

JITTER_TEST(window, capacity_is_a_hard_bound_and_evictions_are_counted) {
    WindowPolicy policy;
    policy.kind = WindowKind::Count;
    policy.count = 4;
    policy.capacity = 4;
    CHECK_OK(validate_window_policy(policy));

    const SeriesId series = synthetic_series_id("bound-series");
    const PathId path = synthetic_path_id("bound-path");
    const GenerationId generation = synthetic_generation_id("bound-generation");
    SampleWindow window(policy);
    for (std::uint64_t i = 0; i < 10; ++i) {
        CHECK_OK(window.add(window_sample(1000, static_cast<std::int64_t>(i), i + 1, series, path,
                                          generation)));
    }
    CHECK_EQ(window.retained(), std::size_t{4});
    CHECK_EQ(window.evicted_by_capacity(), std::uint64_t{6});
}

JITTER_TEST(window, time_window_selects_relative_to_the_newest_observation) {
    WindowPolicy policy;
    policy.kind = WindowKind::Time;
    policy.duration_ns = 100;
    policy.capacity = 16;
    CHECK_OK(validate_window_policy(policy));

    const SeriesId series = synthetic_series_id("time-series");
    const PathId path = synthetic_path_id("time-path");
    const GenerationId generation = synthetic_generation_id("time-generation");
    SampleWindow window(policy);
    const std::int64_t ticks[] = {0, 50, 100, 150, 400};
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK_OK(window.add(window_sample(100, ticks[i], i + 1, series, path, generation)));
    }
    const WindowSelection selection = window.select();
    CHECK_EQ(selection.samples.size(), std::size_t{1});
    CHECK_EQ(selection.latencies_ns(), (std::vector<std::int64_t>{100}));
    CHECK_EQ(selection.selection_reason, std::string("within_duration_of_newest_observation"));
}

JITTER_TEST(window, tumbling_window_uses_its_anchor) {
    WindowPolicy policy;
    policy.kind = WindowKind::Tumbling;
    policy.duration_ns = 100;
    policy.anchor_ns = 0;
    policy.capacity = 16;
    CHECK_OK(validate_window_policy(policy));

    const SeriesId series = synthetic_series_id("tumble-series");
    const PathId path = synthetic_path_id("tumble-path");
    const GenerationId generation = synthetic_generation_id("tumble-generation");
    SampleWindow window(policy);
    const std::int64_t ticks[] = {10, 20, 30, 110, 120};
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK_OK(window.add(window_sample(100, ticks[i], i + 1, series, path, generation)));
    }
    const WindowSelection selection = window.select();
    CHECK_EQ(selection.samples.size(), std::size_t{2});
    CHECK_EQ(selection.latencies_ns(), (std::vector<std::int64_t>{100, 100}));
}

JITTER_TEST(window, duplicate_observations_are_ignored_and_out_of_order_is_counted) {
    WindowPolicy policy;
    policy.kind = WindowKind::Count;
    policy.count = 16;
    policy.capacity = 16;

    const SeriesId series = synthetic_series_id("order-series");
    const PathId path = synthetic_path_id("order-path");
    const GenerationId generation = synthetic_generation_id("order-generation");
    SampleWindow window(policy);
    const LatencySample first = window_sample(100, 300, 3, series, path, generation);
    CHECK_OK(window.add(first));
    CHECK_OK(window.add(first));
    CHECK_EQ(window.duplicate_ignored(), std::uint64_t{1});
    CHECK_EQ(window.retained(), std::size_t{1});

    // An older observation arriving later is accepted and counted but never rewinds the
    // window contents.
    CHECK_OK(window.add(window_sample(200, 100, 1, series, path, generation)));
    CHECK_EQ(window.out_of_order_accepted(), std::uint64_t{1});
    CHECK_EQ(window.newest_observation_ticks().value(), std::int64_t{300});
    const WindowSelection selection = window.select();
    REQUIRE(selection.samples.size() == 2);
    CHECK_EQ(selection.samples.front()->observed_at.ticks, std::int64_t{100});
    CHECK_EQ(selection.samples.back()->observed_at.ticks, std::int64_t{300});
}

JITTER_TEST(window, a_window_never_mixes_series_or_generations) {
    WindowPolicy policy;
    policy.kind = WindowKind::Count;
    policy.count = 8;
    policy.capacity = 8;

    const SeriesId series = synthetic_series_id("mix-series");
    const PathId path = synthetic_path_id("mix-path");
    const GenerationId generation = synthetic_generation_id("mix-generation");
    SampleWindow window(policy);
    CHECK_OK(window.add(window_sample(100, 1, 1, series, path, generation)));

    CHECK_ERR(window.add(window_sample(100, 2, 2, series, path,
                                      synthetic_generation_id("other-generation"))),
              ErrorCode::Conflict);
    CHECK_ERR(window.add(window_sample(100, 3, 3, synthetic_series_id("other-series"), path,
                                      generation)),
              ErrorCode::Conflict);
    CHECK_EQ(window.rejected_mismatch(), std::uint64_t{2});
    CHECK_EQ(window.retained(), std::size_t{1});
}

JITTER_TEST(window, window_policy_validation_rejects_impossible_policies) {
    WindowPolicy policy;
    policy.count = 0;
    CHECK_ERR(validate_window_policy(policy), ErrorCode::InvalidArgument);

    policy.count = 10;
    policy.capacity = 5;
    CHECK_ERR(validate_window_policy(policy), ErrorCode::InvalidArgument);

    policy.capacity = Limits::kMaxWindowSamples + 1;
    policy.count = 1;
    CHECK_ERR(validate_window_policy(policy), ErrorCode::LimitExceeded);

    WindowPolicy timed;
    timed.kind = WindowKind::Time;
    timed.duration_ns = 0;
    CHECK_ERR(validate_window_policy(timed), ErrorCode::InvalidArgument);

    WindowPolicy identified_a;
    identified_a.kind = WindowKind::Count;
    identified_a.count = 4;
    identified_a.capacity = 8;
    WindowPolicy identified_b = identified_a;
    identified_b.count = 5;
    CHECK(make_window_policy_id(identified_a) != make_window_policy_id(identified_b));
}
