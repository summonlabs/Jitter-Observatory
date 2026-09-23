// Jitter Observatory - engine ingest, fencing and authority integration.
// Copyright 2026 Summon Software Labs.
#include <string>
#include <vector>

#include <jitter/engine.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

JITTER_TEST(engine, initialize_verifies_the_metric_table_and_configuration) {
    EngineConfig config = fixture_config();
    Observatory observatory(config);
    CHECK(!observatory.initialized());
    auto summary = observatory.summarize(SummaryRequest{}, IngestContext{}, false);
    CHECK_ERR(summary, ErrorCode::NotReady);
    CHECK_OK(observatory.initialize());
    CHECK(observatory.initialized());
    CHECK(observatory.clocks().has_domain(LocalClockDomain::id()));

    EngineConfig bad = fixture_config();
    bad.window.capacity = 0;
    Observatory broken(bad);
    CHECK_ERR(broken.initialize(), ErrorCode::InvalidArgument);

    EngineConfig empty_metrics = fixture_config();
    empty_metrics.metrics.clear();
    Observatory defaults(empty_metrics);
    CHECK_OK(defaults.initialize());
    CHECK(!defaults.config().metrics.empty());
}

JITTER_TEST(engine, declared_identities_must_agree_with_their_content) {
    EngineConfig config = fixture_config();
    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());

    ClockDomainDescriptor clock;
    clock.name = "integration-clock";
    clock.kind = ClockKind::Synthetic;
    clock.unit = TimeUnit::Nanoseconds;
    clock.epoch_note = "integration";
    CHECK_OK(observatory.register_clock_domain(clock));

    ClockDomainDescriptor tampered = clock;
    tampered.id = synthetic_clock_id("not-the-same-clock");
    CHECK_ERR(observatory.register_clock_domain(tampered), ErrorCode::InvalidArgument);

    SourceDescriptor source;
    source.name = "integration-source";
    source.authority = SourceAuthority::Corroborating;
    source.origin = EvidenceOrigin::Synthetic;
    source.clock_domain = clock.id;
    CHECK_OK(observatory.register_source(source));

    SourceDescriptor missing_clock = source;
    missing_clock.name = "source-with-unknown-clock";
    missing_clock.clock_domain = synthetic_clock_id("ghost-clock");
    CHECK_ERR(observatory.register_source(missing_clock), ErrorCode::NotFound);

    SeriesDescriptor series;
    series.name = "integration-series";
    series.kind = MeasurementKind::OneWayDelay;
    series.unit = TimeUnit::Nanoseconds;
    series.path = synthetic_path_id("unregistered-path");
    series.clock_domain = clock.id;
    series.origin = EvidenceOrigin::Synthetic;
    CHECK_ERR(observatory.register_series(series), ErrorCode::NotFound);

    PathDescriptor path;
    path.name = "integration-path";
    path.origin = EvidenceOrigin::Synthetic;
    HopDescriptor hop;
    hop.index = 0;
    hop.name = "hop";
    hop.timing_clock = synthetic_clock_id("unregistered-hop-clock");
    path.hops.push_back(hop);
    CHECK_ERR(observatory.register_path(path), ErrorCode::NotFound);
}

JITTER_TEST(engine, ingest_admits_well_formed_batches) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);

    IngestOutcome first;
    std::uint64_t accepted = 0;
    for (const LatencyBatch& batch : fixture.value().batches) {
        auto outcome = fixture.value().observatory->ingest(batch, fixture.value().context);
        REQUIRE_OK(outcome);
        CHECK_EQ(outcome.value().verdict, SequenceVerdict::Accept);
        CHECK(outcome.value().admitted());
        CHECK_EQ(outcome.value().accepted, std::uint64_t{8});
        accepted += outcome.value().accepted;
        if (accepted == 8) {
            first = outcome.value();
        }
    }
    CHECK_EQ(accepted, std::uint64_t{64});
    CHECK(first.window_created);
    CHECK_EQ(fixture.value().observatory->counters().samples_stored, std::uint64_t{64});
    CHECK_EQ(fixture.value().observatory->counters().batches_accepted, std::uint64_t{8});
    CHECK_EQ(fixture.value().observatory->window_count(), std::size_t{1});
}

JITTER_TEST(engine, replaying_an_identical_batch_is_an_idempotent_duplicate) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);

    const LatencyBatch& batch = fixture.value().batches.front();
    auto first = fixture.value().observatory->ingest(batch, fixture.value().context);
    REQUIRE_OK(first);
    CHECK(first.value().admitted());

    auto replay = fixture.value().observatory->ingest(batch, fixture.value().context);
    REQUIRE_OK(replay);
    CHECK_EQ(replay.value().verdict, SequenceVerdict::DuplicateIdempotent);
    CHECK(!replay.value().admitted());
    CHECK_EQ(replay.value().duplicates, std::uint64_t{8});
    CHECK_EQ(fixture.value().observatory->counters().samples_stored, std::uint64_t{8});
    CHECK_EQ(fixture.value().observatory->counters().samples_duplicate, std::uint64_t{8});
}

JITTER_TEST(engine, a_rewound_or_reused_sequence_is_fenced) {
    ScenarioPlan plan;
    plan.samples = 24;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    REQUIRE_OK(observatory.ingest(fixture.value().batches[0], fixture.value().context));
    REQUIRE_OK(observatory.ingest(fixture.value().batches[1], fixture.value().context));

    // Replay of an older batch: the sequence has moved on, so it is fenced as a rewind
    // rather than accepted or reported as a fresh duplicate.
    auto stale = observatory.ingest(fixture.value().batches[0], fixture.value().context);
    REQUIRE_OK(stale);
    CHECK_EQ(stale.value().verdict, SequenceVerdict::StaleSequence);
    CHECK_EQ(stale.value().accepted, std::uint64_t{0});

    // A contradiction is a second, different claim about the newest position the guard
    // has already accepted.
    LatencyBatch reordered = fixture.value().batches[1];
    for (auto& sample : reordered.samples) {
        sample.id = MeasurementId{};
        auto rebuilt = make_sample(sample);
        REQUIRE_OK(rebuilt);
        sample = rebuilt.value();
    }
    reordered.samples.back().latency_ns += 1;
    reordered.samples.back().id = MeasurementId{};
    auto rebuilt_sample = make_sample(reordered.samples.back());
    REQUIRE_OK(rebuilt_sample);
    reordered.samples.back() = rebuilt_sample.value();
    auto rebuilt_batch = make_batch(reordered);
    REQUIRE_OK(rebuilt_batch);

    auto conflict = observatory.ingest(rebuilt_batch.value(), fixture.value().context);
    REQUIRE_OK(conflict);
    CHECK_EQ(conflict.value().verdict, SequenceVerdict::SequenceConflict);
    CHECK(conflict.value().conflict_recorded);
    CHECK_EQ(conflict.value().rejected, std::uint64_t{8});
    CHECK_EQ(observatory.conflict_log().size(), std::size_t{1});
    CHECK_EQ(observatory.counters().conflicts_recorded, std::uint64_t{1});
}

JITTER_TEST(engine, epoch_and_incarnation_regressions_are_fenced) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    ScenarioPlan advanced = fixture.value().plan;
    advanced.incarnation = SourceIncarnation(4);
    advanced.epoch = SourceEpoch(3);
    advanced.sequence_start = SourceSequence(50);
    const std::vector<LatencyBatch> newer = make_batches(advanced);
    REQUIRE_OK(observatory.ingest(newer.front(), fixture.value().context));

    // Evidence from an older incarnation is refused outright.
    ScenarioPlan older = fixture.value().plan;
    older.incarnation = SourceIncarnation(2);
    older.epoch = SourceEpoch(1);
    older.sequence_start = SourceSequence(1);
    const std::vector<LatencyBatch> stale_incarnation = make_batches(older);
    auto rejected = observatory.ingest(stale_incarnation.front(), fixture.value().context);
    REQUIRE_OK(rejected);
    CHECK_EQ(rejected.value().verdict, SequenceVerdict::StaleIncarnation);
    CHECK_EQ(rejected.value().rejected, std::uint64_t{8});
    CHECK(!rejected.value().notes.empty());

    // An older configuration epoch inside the current incarnation is refused too.
    ScenarioPlan older_epoch = advanced;
    older_epoch.epoch = SourceEpoch(2);
    older_epoch.sequence_start = SourceSequence(60);
    const std::vector<LatencyBatch> epoch_regression = make_batches(older_epoch);
    auto epoch_result = observatory.ingest(epoch_regression.front(), fixture.value().context);
    REQUIRE_OK(epoch_result);
    CHECK_EQ(epoch_result.value().verdict, SequenceVerdict::StaleEpoch);
    CHECK_EQ(fixture.value().observatory->counters().samples_stored, std::uint64_t{8});
}

JITTER_TEST(engine, historical_ingest_never_advances_the_guard) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    IngestContext historical = fixture.value().context;
    historical.historical = true;
    auto outcome = observatory.ingest(fixture.value().batches.front(), historical);
    REQUIRE_OK(outcome);
    CHECK_EQ(outcome.value().verdict, SequenceVerdict::Historical);
    CHECK(!outcome.value().admitted());
    CHECK_EQ(outcome.value().historical, std::uint64_t{8});
    CHECK_EQ(observatory.counters().batches_historical, std::uint64_t{1});
    CHECK_EQ(observatory.counters().samples_stored, std::uint64_t{8});

    // Because the guard did not move, the same batch is still new evidence later.
    auto live = observatory.ingest(fixture.value().batches.front(), fixture.value().context);
    REQUIRE_OK(live);
    CHECK(live.value().admitted());
}

JITTER_TEST(engine, authority_and_origin_cannot_be_self_asserted) {
    ScenarioPlan plan;
    plan.samples = 8;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    LatencyBatch claimed = fixture.value().batches.front();
    claimed.header.authority = SourceAuthority::Authoritative;
    CHECK_ERR(observatory.ingest(claimed, fixture.value().context), ErrorCode::PolicyViolation);

    LatencyBatch upgraded = fixture.value().batches.front();
    upgraded.header.origin = EvidenceOrigin::Real;
    for (auto& sample : upgraded.samples) {
        sample.provenance.origin = EvidenceOrigin::Real;
        sample.id = MeasurementId{};
        auto rebuilt = make_sample(sample);
        REQUIRE_OK(rebuilt);
        sample = rebuilt.value();
    }
    auto upgraded_batch = make_batch(upgraded);
    REQUIRE_OK(upgraded_batch);
    CHECK_ERR(observatory.ingest(upgraded_batch.value(), fixture.value().context),
              ErrorCode::PolicyViolation);

    LatencyBatch unknown_source = fixture.value().batches.front();
    unknown_source.header.source = synthetic_source_id("never-registered");
    for (auto& sample : unknown_source.samples) {
        sample.provenance.source = unknown_source.header.source;
        sample.id = MeasurementId{};
        auto rebuilt = make_sample(sample);
        REQUIRE_OK(rebuilt);
        sample = rebuilt.value();
    }
    auto unknown_batch = make_batch(unknown_source);
    REQUIRE_OK(unknown_batch);
    CHECK_ERR(observatory.ingest(unknown_batch.value(), fixture.value().context), ErrorCode::NotFound);

    LatencyBatch unsupported_origin = fixture.value().batches.front();
    unsupported_origin.header.origin = EvidenceOrigin::Unsupported;
    for (auto& sample : unsupported_origin.samples) {
        sample.provenance.origin = EvidenceOrigin::Unsupported;
        sample.id = MeasurementId{};
        auto rebuilt = make_sample(sample);
        REQUIRE_OK(rebuilt);
        sample = rebuilt.value();
    }
    auto unsupported_batch = make_batch(unsupported_origin);
    REQUIRE_OK(unsupported_batch);
    CHECK_ERR(observatory.ingest(unsupported_batch.value(), fixture.value().context),
              ErrorCode::Unsupported);

    EngineConfig no_synthetic = fixture_config();
    no_synthetic.accept_synthetic = false;
    Observatory strict(no_synthetic);
    REQUIRE_OK(strict.initialize());
    SourceDescriptor synthetic_source;
    synthetic_source.name = "synthetic.generator";
    synthetic_source.authority = SourceAuthority::Simulated;
    synthetic_source.origin = EvidenceOrigin::Synthetic;
    synthetic_source.description = "deterministic synthetic observation source used by tools and tests";
    CHECK_ERR(strict.register_source(synthetic_source), ErrorCode::PolicyViolation);
}

JITTER_TEST(engine, a_closed_generation_cannot_receive_current_evidence) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    REQUIRE_OK(observatory.ingest(fixture.value().batches.front(), fixture.value().context));

    const PathDescriptor* path = observatory.paths().find(effective.path);
    REQUIRE(path != nullptr);
    PathDescriptor changed = *path;
    changed.hops.pop_back();  // the route changed: one hop fewer
    CHECK_OK(observatory.observe_topology(effective.path, make_topology(changed),
                                          fixture.value().context.now_utc_ns, "route_change"));

    // A batch that still names the old generation is refused as stale, whatever its
    // sequence number says.
    CHECK_ERR(observatory.ingest(fixture.value().batches[1], fixture.value().context),
              ErrorCode::StaleGeneration);
    CHECK_EQ(fixture.value().observatory->counters().generations_opened, std::uint64_t{1});

    const PathGeneration* current = observatory.paths().current(effective.path);
    REQUIRE(current != nullptr);
    CHECK(current->ordinal.value() >= 2);
    CHECK_EQ(current->id, observatory.paths().current(effective.path)->id);

    // Evidence generated for the new generation is accepted.
    ScenarioPlan rerouted = effective;
    rerouted.generation = current->id;
    rerouted.ordinal = current->ordinal;
    rerouted.sequence_start = SourceSequence(100);
    const std::vector<LatencyBatch> rerouted_batches = make_batches(rerouted);
    REQUIRE(!rerouted_batches.empty());
    auto accepted = observatory.ingest(rerouted_batches.front(), fixture.value().context);
    REQUIRE_OK(accepted);
    CHECK(accepted.value().admitted());
    // A request that still names the closed generation is answered with an explicit
    // refusal instead of being presented as the current state.
    SummaryRequest historical = fixture.value().summary_request();
    auto historical_summary = observatory.summarize(historical, fixture.value().context, true);
    REQUIRE_OK(historical_summary);
    CHECK_EQ(historical_summary.value().summary.classification.level, InstabilityLevel::Unsupported);
    CHECK(!historical_summary.value().summary.classification.asserts_current_instability());
}

JITTER_TEST(engine, unknown_series_path_and_generation_are_rejected) {
    ScenarioPlan plan;
    plan.samples = 8;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    LatencyBatch unknown_series = fixture.value().batches.front();
    unknown_series.header.series = synthetic_series_id("unknown-series");
    for (auto& sample : unknown_series.samples) {
        sample.series = unknown_series.header.series;
        sample.id = MeasurementId{};
        auto rebuilt = make_sample(sample);
        REQUIRE_OK(rebuilt);
        sample = rebuilt.value();
    }
    auto unknown_series_batch = make_batch(unknown_series);
    REQUIRE_OK(unknown_series_batch);
    CHECK_ERR(observatory.ingest(unknown_series_batch.value(), fixture.value().context),
              ErrorCode::NotFound);

    LatencyBatch unknown_generation = fixture.value().batches.front();
    unknown_generation.header.generation = synthetic_generation_id("unknown-generation");
    for (auto& sample : unknown_generation.samples) {
        sample.generation = unknown_generation.header.generation;
        sample.id = MeasurementId{};
        auto rebuilt = make_sample(sample);
        REQUIRE_OK(rebuilt);
        sample = rebuilt.value();
    }
    auto rebuilt_batch = make_batch(unknown_generation);
    REQUIRE_OK(rebuilt_batch);
    CHECK_ERR(observatory.ingest(rebuilt_batch.value(), fixture.value().context),
              ErrorCode::StaleGeneration);

    LatencyBatch bad_revision = fixture.value().batches.front();
    bad_revision.header.protocol_revision = 7;
    CHECK_ERR(observatory.ingest(bad_revision, fixture.value().context),
              ErrorCode::VersionUnsupported);
}

JITTER_TEST(engine, summarize_reports_every_evidence_class) {
    ScenarioPlan plan;
    plan.samples = 128;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(256, 512), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    for (const LatencyBatch& batch : fixture.value().batches) {
        REQUIRE_OK(observatory.ingest(batch, fixture.value().context));
    }

    const SummaryRequest request = fixture.value().summary_request();
    auto outcome = observatory.summarize(request, fixture.value().context, true);
    REQUIRE_OK(outcome);
    const SeriesSummary& summary = outcome.value().summary;
    CHECK_EQ(summary.selected, std::uint64_t{128});
    CHECK_EQ(summary.fresh_samples, std::uint64_t{128});
    CHECK_EQ(summary.evidence.dominant, EvidenceState::Fresh);
    CHECK_EQ(summary.dominant_origin, EvidenceOrigin::Synthetic);
    CHECK_EQ(summary.evidence.synthetic_origin, std::uint64_t{128});
    CHECK_EQ(summary.evidence.real_origin, std::uint64_t{0});
    CHECK(makes_positive_assertion(summary.classification.level));
    CHECK(!summary.id.is_nil());
    CHECK(!summary.digest.is_zero());
    CHECK(summary.find(MetricKey::AbsoluteDeltaMean) != nullptr);
    CHECK(summary.find(MetricKey::SampleVariance) != nullptr);
    CHECK_EQ(summary.metrics.size(), std::size_t{16});

    // Every metric value carries the identity of its own definition.
    for (const MetricValue& value : summary.metrics) {
        CHECK_EQ(value.id, metric_registry().definition(value.key).id);
    }

    // Reading a series with no window is an explicit miss, not an empty summary.
    SummaryRequest unknown = request;
    unknown.generation = synthetic_generation_id("no-window-generation");
    CHECK_ERR(observatory.summarize(unknown, fixture.value().context, false), ErrorCode::NotFound);
}

JITTER_TEST(engine, stale_observations_cannot_establish_current_instability) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    plan.spike_every = 4;
    plan.spike_ns = 5000000;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    for (const LatencyBatch& batch : fixture.value().batches) {
        REQUIRE_OK(observatory.ingest(batch, fixture.value().context));
    }

    // Summarising long after the observations were received turns them into history.
    IngestContext late = fixture.value().context;
    late.now_utc_ns += 1000000000000ll;
    auto outcome = observatory.summarize(fixture.value().summary_request(), late, false);
    REQUIRE_OK(outcome);
    CHECK_EQ(outcome.value().summary.fresh_samples, std::uint64_t{0});
    CHECK_EQ(outcome.value().summary.classification.level, InstabilityLevel::Expired);
    CHECK(!asserts_instability(outcome.value().summary.classification.level));
    CHECK(!outcome.value().summary.classification.asserts_current_instability());
    CHECK(outcome.value().summary.metrics.empty());
}

JITTER_TEST(engine, export_documents_are_byte_stable) {
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    auto fixture = build_engine(plan, fixture_config(), false);
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    for (const LatencyBatch& batch : fixture.value().batches) {
        REQUIRE_OK(observatory.ingest(batch, fixture.value().context));
    }
    const SummaryRequest request = fixture.value().summary_request();

    auto first = observatory.export_summary(request, fixture.value().context, false);
    auto second = observatory.export_summary(request, fixture.value().context, false);
    REQUIRE_OK(first);
    REQUIRE_OK(second);
    CHECK_EQ(first.value(), second.value());
    CHECK(first.value().find("\"schema\":1") != std::string::npos);

    auto pretty = observatory.export_summary(request, fixture.value().context, true);
    REQUIRE_OK(pretty);
    CHECK(pretty.value().size() > first.value().size());

    auto catalog = observatory.export_catalog(false);
    REQUIRE_OK(catalog);
    CHECK(catalog.value().find("metric_definitions") != std::string::npos);
    CHECK(catalog.value().find("not a causal") == std::string::npos);

    auto explanation = observatory.explain(request, fixture.value().context);
    REQUIRE_OK(explanation);
    CHECK(explanation.value().lines.size() > std::size_t{20});
    CHECK(!explanation.value().digest.is_zero());
    bool saw_boundary = false;
    for (const ExplanationLine& line : explanation.value().lines) {
        if (line.code == "boundary.authority") {
            saw_boundary = true;
        }
    }
    CHECK(saw_boundary);
}
