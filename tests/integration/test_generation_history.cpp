// Jitter Observatory - route generation segmentation and history.
// Copyright 2026 Summon Software Labs.
#include <string>
#include <vector>

#include <jitter/engine.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

// Rebuilds a path descriptor with one hop removed, which is exactly what a route
// change looks like from the outside.
PathDescriptor shorten(const PathDescriptor& original) {
    PathDescriptor changed = original;
    if (!changed.hops.empty()) {
        changed.hops.pop_back();
        for (std::size_t i = 0; i < changed.hops.size(); ++i) {
            changed.hops[i].index = static_cast<std::uint16_t>(i);
        }
    }
    return changed;
}

}  // namespace

JITTER_TEST(generation, an_unchanged_topology_does_not_open_a_generation) {
    ScenarioPlan plan;
    plan.samples = 32;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    const PathDescriptor* path = observatory.paths().find(effective.path);
    REQUIRE(path != nullptr);
    const PathTopology topology = make_topology(*path);

    auto repeat = observatory.observe_topology(effective.path, topology,
                                               fixture.value().context.now_utc_ns + 1000,
                                               "periodic_check");
    REQUIRE_OK(repeat);
    CHECK(!repeat.value().created_new);
    CHECK_EQ(repeat.value().generation.id, effective.generation);
    CHECK_EQ(observatory.paths().generations(effective.path).size(), std::size_t{1});
    CHECK_EQ(observatory.counters().generations_opened, std::uint64_t{0});
}

JITTER_TEST(generation, a_revision_bump_never_segments_history) {
    ScenarioPlan plan;
    plan.samples = 32;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    CHECK_OK(observatory.bump_path_revision(effective.path, "declared_clock_annotation_update"));
    const PathGeneration* current = observatory.paths().current(effective.path);
    REQUIRE(current != nullptr);
    CHECK_EQ(current->id, effective.generation);
    CHECK_EQ(current->revision.value(), std::uint64_t{1});
    CHECK_EQ(observatory.paths().generations(effective.path).size(), std::size_t{1});
    CHECK_EQ(observatory.counters().generations_opened, std::uint64_t{0});
}

JITTER_TEST(generation, a_route_change_segments_history_and_closes_episodes) {
    ScenarioPlan plan;
    plan.samples = 96;
    plan.batch_size = 16;
    plan.spike_every = 2;
    plan.spike_ns = 5000000;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    auto unstable = observatory.summarize(fixture.value().summary_request(),
                                          fixture.value().context, true);
    REQUIRE_OK(unstable);
    REQUIRE(asserts_instability(unstable.value().summary.classification.level));
    REQUIRE(unstable.value().episodes.opened.has_value());
    CHECK_EQ(observatory.episode_log().open_episodes().size(), std::size_t{1});

    const PathDescriptor* path = observatory.paths().find(effective.path);
    REQUIRE(path != nullptr);
    const PathDescriptor changed = shorten(*path);
    auto observed = observatory.observe_topology(effective.path, make_topology(changed),
                                                 fixture.value().context.now_utc_ns + 5000,
                                                 "hop_removed");
    REQUIRE_OK(observed);
    CHECK(observed.value().created_new);
    CHECK(observed.value().generation.ordinal.value() >= 2);
    CHECK_EQ(observatory.counters().generations_opened, std::uint64_t{1});

    // The open episode was closed by the route change rather than extended across it.
    CHECK(observatory.episode_log().open_episodes().empty());
    const std::vector<Episode> episodes = observatory.episode_log().episodes(effective.series);
    REQUIRE(episodes.size() == 1);
    CHECK_EQ(episodes.front().close_reason, EpisodeCloseReason::GenerationChanged);
    CHECK_EQ(episodes.front().generation, effective.generation);

    // The old generation is closed and the new one is open.
    const PathGeneration* current = observatory.paths().current(effective.path);
    REQUIRE(current != nullptr);
    CHECK_EQ(current->id, observed.value().generation.id);
    const PathGeneration* previous = observatory.paths().find_generation(effective.generation);
    REQUIRE(previous != nullptr);
    CHECK(!previous->is_open());
    CHECK_EQ(previous->close_reason, std::string("topology_changed"));
}

JITTER_TEST(generation, history_is_reported_per_segment) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    const PathDescriptor* path = observatory.paths().find(effective.path);
    REQUIRE(path != nullptr);
    PathDescriptor changed = shorten(*path);
    REQUIRE_OK(observatory.observe_topology(effective.path, make_topology(changed),
                                            fixture.value().context.now_utc_ns + 1000, "route_change"));

    HistoryRequest request;
    request.series = effective.series;
    request.path = effective.path;
    auto report = observatory.history(request);
    REQUIRE_OK(report);
    CHECK_EQ(report.value().segments.size(), std::size_t{2});
    CHECK_EQ(report.value().segments[0].ordinal.value(), std::uint64_t{1});
    CHECK_EQ(report.value().segments[1].ordinal.value(), std::uint64_t{2});
    CHECK(!report.value().segments[0].current);
    CHECK(report.value().segments[1].current);
    CHECK_EQ(report.value().segments[0].retained_observations, std::uint64_t{64});
    CHECK_EQ(report.value().segments[1].retained_observations, std::uint64_t{0});
    CHECK_EQ(report.value().segments[0].hop_count, std::uint32_t{3});
    CHECK_EQ(report.value().segments[1].hop_count, std::uint32_t{2});
    CHECK(!report.value().digest.is_zero());

    bool segmented_reason = false;
    for (const std::string& reason : report.value().reasons) {
        if (reason == "history_is_segmented_by_route_generation") {
            segmented_reason = true;
        }
    }
    CHECK(segmented_reason);

    // Rows are bounded by the request.
    request.max_rows = 1;
    auto bounded = observatory.history(request);
    REQUIRE_OK(bounded);
    CHECK(bounded.value().segments.size() == 2);
}

JITTER_TEST(generation, a_baseline_cannot_be_compared_across_a_route_change) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    auto baseline = observatory.capture_baseline("before-route-change", fixture.value().summary_request(),
                                                 fixture.value().context, "unit test baseline");
    REQUIRE_OK(baseline);
    CHECK_EQ(observatory.baselines(effective.series).size(), std::size_t{1});
    CHECK(observatory.find_baseline(baseline.value().id).has_value());

    const PathDescriptor* path = observatory.paths().find(effective.path);
    REQUIRE(path != nullptr);
    const PathDescriptor changed = shorten(*path);
    REQUIRE_OK(observatory.observe_topology(effective.path, make_topology(changed),
                                            fixture.value().context.now_utc_ns + 1000, "route_change"));

    // The request names the current generation, so the comparison sees a different
    // generation and refuses to produce a delta.
    const PathGeneration* current = observatory.paths().current(effective.path);
    REQUIRE(current != nullptr);
    SummaryRequest current_request = fixture.value().summary_request();
    current_request.generation = current->id;
    current_request.generation_ordinal = current->ordinal;

    // Evidence for the new generation has to exist before a comparison can even be
    // attempted; the comparison is then refused because the generation changed.
    ScenarioPlan rerouted = effective;
    rerouted.generation = current->id;
    rerouted.ordinal = current->ordinal;
    rerouted.sequence_start = SourceSequence(200);
    const std::vector<LatencyBatch> rerouted_batches = make_batches(rerouted);
    REQUIRE(!rerouted_batches.empty());
    for (const LatencyBatch& batch : rerouted_batches) {
        auto ingested = observatory.ingest(batch, fixture.value().context);
        REQUIRE_OK(ingested);
    }

    CompareRequest compare_request;
    compare_request.current = current_request;
    compare_request.baseline = baseline.value().id;
    auto comparison = observatory.compare(compare_request, fixture.value().context);
    REQUIRE_OK(comparison);
    CHECK_EQ(comparison.value().overall, ComparisonVerdict::Incomparable);
    CHECK(comparison.value().generation_changed);
    for (const MetricComparison& metric : comparison.value().comparisons) {
        CHECK(!metric.delta.has_value());
    }
    CHECK_ERR(observatory.compare(CompareRequest{current_request, BaselineId{}}, fixture.value().context),
              ErrorCode::NotFound);
}

JITTER_TEST(generation, attribution_follows_the_current_generation) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    AttributionRequest request;
    request.series = effective.series;
    request.metric = MetricKey::AbsoluteDeltaMean;
    request.min_arrivals = 4;
    auto report = observatory.attribute(request, fixture.value().context);
    REQUIRE_OK(report);
    // The synthetic path declares one hop that emits no timing at all, so attribution
    // is incomplete and the missing hop has no value.
    CHECK(!report.value().complete);
    CHECK_EQ(report.value().total_hops, std::uint64_t{3});
    CHECK_EQ(report.value().attributed_hops, std::uint64_t{2});
    REQUIRE(report.value().hops.size() == 3);
    CHECK_EQ(report.value().hops[2].state, AttributionState::Absent);
    CHECK(!report.value().hops[2].value_present);
}

JITTER_TEST(generation, incomparable_hop_clocks_block_per_hop_attribution_end_to_end) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    plan.incomparable_second_hop = true;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    AttributionRequest request;
    request.series = effective.series;
    request.metric = MetricKey::AbsoluteDeltaMean;
    request.min_arrivals = 4;
    auto report = observatory.attribute(request, fixture.value().context);
    REQUIRE_OK(report);
    CHECK(!report.value().complete);
    CHECK_EQ(report.value().attributed_hops, std::uint64_t{1});
    REQUIRE(report.value().hops.size() == 3);
    CHECK_EQ(report.value().hops[0].state, AttributionState::Attributed);
    CHECK_EQ(report.value().hops[1].state, AttributionState::Unsupported);
    CHECK(!report.value().hops[1].value_present);
    CHECK(report.value().hops[1].reason.find("hop_clock_incomparable") != std::string::npos);
}

JITTER_TEST(generation, revision_and_lookup_errors_are_explicit) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    CHECK_ERR(observatory.bump_path_revision(synthetic_path_id("unregistered-path"), "test"),
              ErrorCode::NotFound);

    HistoryRequest request;
    request.series = fixture.value().plan.series;
    request.path = synthetic_path_id("unregistered-path");
    auto report = observatory.history(request);
    REQUIRE_OK(report);
    CHECK(report.value().segments.empty());

    // The generation history is bounded and the bound is reported rather than hidden.
    ClockModel clocks;
    PathCatalog catalog(clocks);
    PathDescriptor path;
    path.name = "bounded-history";
    path.origin = EvidenceOrigin::Synthetic;
    REQUIRE_OK(catalog.register_path(path));
    for (std::uint64_t i = 0; i < Limits::kMaxGenerationHistoryPerPath + 3; ++i) {
        PathGeneration generation;
        generation.path = path.id;
        generation.ordinal = Ordinal(i + 1);
        generation.topology = Digest::of("topology-" + std::to_string(i));
        generation.id = make_generation_id(generation.path, generation.ordinal, generation.topology);
        REQUIRE_OK(catalog.import_generation(generation));
    }
    CHECK_EQ(catalog.generations(path.id).size(),
             static_cast<std::size_t>(Limits::kMaxGenerationHistoryPerPath));
    CHECK_EQ(catalog.generations(path.id).front().ordinal.value(), std::uint64_t{4});
    CHECK_EQ(catalog.generations(path.id).back().ordinal.value(),
             Limits::kMaxGenerationHistoryPerPath + 3);
}

JITTER_TEST(generation, generation_import_preserves_order_and_refuses_rewinds) {
    ClockModel clocks;
    PathCatalog catalog(clocks);
    PathDescriptor path;
    path.name = "history-path";
    path.origin = EvidenceOrigin::Synthetic;
    REQUIRE_OK(catalog.register_path(path));

    PathGeneration first;
    first.path = path.id;
    first.ordinal = Ordinal(1);
    first.topology = Digest::of("topology-one");
    first.opened_at_utc_ns = 100;
    first.hop_count = 1;
    first.id = make_generation_id(first.path, first.ordinal, first.topology);
    CHECK_OK(catalog.import_generation(first));

    PathGeneration duplicate;
    duplicate.path = path.id;
    duplicate.ordinal = Ordinal(1);
    duplicate.topology = Digest::of("topology-one");
    duplicate.id = make_generation_id(duplicate.path, duplicate.ordinal, duplicate.topology);
    CHECK_ERR(catalog.import_generation(duplicate), ErrorCode::Conflict);

    PathGeneration second = duplicate;
    second.ordinal = Ordinal(2);
    second.topology = Digest::of("topology-two");
    second.id = make_generation_id(second.path, second.ordinal, second.topology);
    CHECK_OK(catalog.import_generation(second));
    CHECK_EQ(catalog.generations(path.id).size(), std::size_t{2});
    CHECK_EQ(catalog.current(path.id)->ordinal.value(), std::uint64_t{2});

    PathGeneration unknown_path = second;
    unknown_path.path = synthetic_path_id("never-registered");
    CHECK_ERR(catalog.import_generation(unknown_path), ErrorCode::NotFound);
}
