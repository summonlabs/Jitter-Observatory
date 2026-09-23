// Jitter Observatory - persistence, restart and recovery semantics.
// Copyright 2026 Summon Software Labs.
#include <filesystem>
#include <string>
#include <vector>

#include <jitter/engine.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

std::string path_for(const std::string& name) {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "jitter-observatory-tests" / "restart";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return (base / name).string();
}

}  // namespace

JITTER_TEST(restart, persisted_evidence_never_silently_becomes_fresh) {
    const std::string path = path_for("withheld.jostore");
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;

    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    REQUIRE_OK(fixture.value().observatory->save(path, true, fixture.value().context));

    EngineConfig restored_config = fixture_config();
    restored_config.boot_id = 2;  // a different process incarnation
    Observatory restored(restored_config);
    REQUIRE_OK(restored.initialize());
    RestoreOptions options;
    auto outcome = restored.restore(path, options);
    REQUIRE_OK(outcome);
    CHECK(outcome.value().foreign_incarnation);
    CHECK_EQ(outcome.value().samples, std::uint64_t{64});
    CHECK_EQ(outcome.value().samples_withheld_from_current, std::uint64_t{64});
    CHECK_EQ(outcome.value().samples_admitted_as_current, std::uint64_t{0});
    CHECK_EQ(outcome.value().generations, std::uint64_t{1});
    CHECK_EQ(outcome.value().sources, std::uint64_t{1});
    CHECK_EQ(outcome.value().series, std::uint64_t{1});
    CHECK_EQ(outcome.value().paths, std::uint64_t{1});

    // Summarising immediately after the restart must not present the restored evidence
    // as current, even though it is well inside the freshness budget.
    const SummaryRequest request = fixture.value().summary_request();
    auto summary = restored.summarize(request, fixture.value().context, false);
    REQUIRE_OK(summary);
    CHECK_EQ(summary.value().summary.evidence.stale, std::uint64_t{64});
    CHECK_EQ(summary.value().summary.evidence.fresh, std::uint64_t{0});
    CHECK_EQ(summary.value().summary.classification.level, InstabilityLevel::Stale);
    CHECK(!summary.value().summary.classification.asserts_current_instability());
    CHECK(!asserts_stability(summary.value().summary.classification.level));
    CHECK(summary.value().summary.metrics.empty());
}

JITTER_TEST(restart, admitting_persisted_evidence_is_an_explicit_decision) {
    const std::string path = path_for("admitted.jostore");
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;

    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    REQUIRE_OK(fixture.value().observatory->save(path, true, fixture.value().context));

    EngineConfig config = fixture_config();
    config.admit_persisted_evidence = true;
    config.boot_id = 2;
    Observatory restored(config);
    REQUIRE_OK(restored.initialize());
    RestoreOptions options;
    options.admit_persisted_evidence = true;
    auto outcome = restored.restore(path, options);
    REQUIRE_OK(outcome);
    CHECK(outcome.value().foreign_incarnation);
    CHECK_EQ(outcome.value().samples_admitted_as_current, std::uint64_t{64});
    CHECK_EQ(outcome.value().samples_withheld_from_current, std::uint64_t{0});

    const SummaryRequest request = fixture.value().summary_request();
    auto summary = restored.summarize(request, fixture.value().context, false);
    REQUIRE_OK(summary);
    CHECK_EQ(summary.value().summary.evidence.fresh, std::uint64_t{64});
    CHECK(makes_positive_assertion(summary.value().summary.classification.level));

    // Admitting is not the same as trusting forever: the same evidence ages out.
    IngestContext late = fixture.value().context;
    late.now_utc_ns += 1000000000000ll;
    auto aged = restored.summarize(request, late, false);
    REQUIRE_OK(aged);
    CHECK_EQ(aged.value().summary.evidence.fresh, std::uint64_t{0});
    CHECK(!asserts_instability(aged.value().summary.classification.level));
}

JITTER_TEST(restart, the_source_guard_is_rebuilt_so_replays_stay_fenced) {
    const std::string path = path_for("guard.jostore");
    ScenarioPlan plan;
    plan.samples = 32;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    REQUIRE_OK(fixture.value().observatory->save(path, true, fixture.value().context));

    EngineConfig config = fixture_config();
    config.boot_id = 9;
    Observatory restored(config);
    REQUIRE_OK(restored.initialize());
    RestoreOptions options;
    REQUIRE_OK(restored.restore(path, options));

    // Replaying the newest stored batch is recognised, not re-admitted.
    auto replay = restored.ingest(fixture.value().batches.back(), fixture.value().context);
    REQUIRE_OK(replay);
    CHECK_EQ(replay.value().verdict, SequenceVerdict::DuplicateIdempotent);
    CHECK_EQ(replay.value().accepted, std::uint64_t{0});

    // Replaying an older position is fenced as a rewind rather than re-admitted.
    auto rewind = restored.ingest(fixture.value().batches.front(), fixture.value().context);
    REQUIRE_OK(rewind);
    CHECK_EQ(rewind.value().verdict, SequenceVerdict::StaleSequence);
    CHECK_EQ(rewind.value().accepted, std::uint64_t{0});

    // Newer evidence from the same incarnation is still accepted.
    ScenarioPlan forward = fixture.value().plan;
    forward.sequence_start = SourceSequence(500);
    const std::vector<LatencyBatch> newer = make_batches(forward);
    REQUIRE(!newer.empty());
    auto accepted = restored.ingest(newer.front(), fixture.value().context);
    REQUIRE_OK(accepted);
    CHECK(accepted.value().admitted());
}

JITTER_TEST(restart, baselines_episodes_and_conflicts_survive_a_restart) {
    const std::string path = path_for("history.jostore");
    ScenarioPlan plan;
    plan.samples = 96;
    plan.batch_size = 16;
    plan.spike_every = 2;
    plan.spike_ns = 6000000;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    Observatory& original = *fixture.value().observatory;
    const ScenarioPlan effective = fixture.value().plan;

    auto baseline = original.capture_baseline("restart-baseline", fixture.value().summary_request(),
                                              fixture.value().context, "before restart");
    REQUIRE_OK(baseline);

    auto summary =
        original.summarize(fixture.value().summary_request(), fixture.value().context, true);
    REQUIRE_OK(summary);
    REQUIRE(asserts_instability(summary.value().summary.classification.level));
    REQUIRE(summary.value().episodes.opened.has_value());
    CHECK_EQ(original.episode_log().open_episodes().size(), std::size_t{1});

    // Force a contradiction so that a conflict record exists as well.
    // A contradiction is two claims about the same sequence position, so the newest
    // batch is the one to tamper with.
    LatencyBatch tampered = fixture.value().batches.back();
    tampered.samples.back().latency_ns += 7;
    tampered.samples.back().id = MeasurementId{};
    auto rebuilt_sample = make_sample(tampered.samples.back());
    REQUIRE_OK(rebuilt_sample);
    tampered.samples.back() = rebuilt_sample.value();
    auto rebuilt_batch = make_batch(tampered);
    REQUIRE_OK(rebuilt_batch);
    auto conflict = original.ingest(rebuilt_batch.value(), fixture.value().context);
    REQUIRE_OK(conflict);
    CHECK_EQ(conflict.value().verdict, SequenceVerdict::SequenceConflict);

    REQUIRE_OK(original.save(path, true, fixture.value().context));

    EngineConfig config = fixture_config();
    config.boot_id = 77;
    Observatory restored(config);
    REQUIRE_OK(restored.initialize());
    RestoreOptions options;
    REQUIRE_OK(restored.restore(path, options));

    CHECK_EQ(restored.baselines(effective.series).size(), std::size_t{1});
    CHECK(restored.find_baseline(baseline.value().id).has_value());
    CHECK_EQ(restored.conflict_log().size(), std::size_t{1});

    // An episode that was open when it was persisted cannot be confirmed after a
    // restart, so it is closed as evidence lost rather than left running.
    CHECK(restored.episode_log().open_episodes().empty());
    const std::vector<Episode> episodes = restored.episode_log().episodes(effective.series);
    REQUIRE(episodes.size() == 1);
    CHECK_EQ(episodes.front().close_reason, EpisodeCloseReason::EvidenceLost);
    CHECK(episodes.front().closed_at_utc_ns.has_value());

    HistoryRequest request;
    request.series = effective.series;
    request.path = effective.path;
    auto history = restored.history(request);
    REQUIRE_OK(history);
    CHECK_EQ(history.value().episodes.size(), std::size_t{1});
    CHECK_EQ(history.value().conflicts.size(), std::size_t{1});
    CHECK_EQ(history.value().segments.size(), std::size_t{1});
}

JITTER_TEST(restart, a_damaged_store_is_refused_until_recovery_is_authorised) {
    const std::string path = path_for("damaged.jostore");
    ScenarioPlan plan;
    plan.samples = 32;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    REQUIRE_OK(fixture.value().observatory->save(path, true, fixture.value().context));

    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 6);

    EngineConfig config = fixture_config();
    Observatory restored(config);
    REQUIRE_OK(restored.initialize());

    RestoreOptions strict;
    CHECK_ERR(restored.restore(path, strict), ErrorCode::IntegrityFailure);

    RestoreOptions authorised;
    authorised.repair_truncated_tail = true;
    auto outcome = restored.restore(path, authorised);
    REQUIRE_OK(outcome);
    CHECK(!outcome.value().recovery.clean);
    CHECK(outcome.value().recovery.truncated_tail);
    CHECK(!outcome.value().notes.empty());
    // The damaged bytes were the trailer, so every observation survived and the store is
    // reported as having lost its closing record.
    CHECK_EQ(outcome.value().samples, std::uint64_t{32});
    CHECK(!outcome.value().recovery.trailer_present);
}

JITTER_TEST(restart, a_restored_engine_can_be_saved_again) {
    const std::string first = path_for("chain-a.jostore");
    const std::string second = path_for("chain-b.jostore");
    ScenarioPlan plan;
    plan.samples = 48;
    plan.batch_size = 12;
    auto fixture = build_engine(plan, fixture_config(), true);
    REQUIRE_OK(fixture);
    REQUIRE_OK(fixture.value().observatory->save(first, true, fixture.value().context));

    EngineConfig config = fixture_config();
    config.boot_id = 5;
    Observatory restored(config);
    REQUIRE_OK(restored.initialize());
    RestoreOptions options;
    REQUIRE_OK(restored.restore(first, options));
    REQUIRE_OK(restored.save(second, true, fixture.value().context));

    auto loaded = load_store(second);
    REQUIRE_OK(loaded);
    CHECK(loaded.value().recovery.clean);
    CHECK(loaded.value().recovery.trailer_present);
    CHECK(loaded.value().recovery.records_recovered > 0);
    CHECK_EQ(loaded.value().header.boot_id, std::uint64_t{5});
}

JITTER_TEST(restart, a_missing_store_is_an_explicit_error) {
    EngineConfig config = fixture_config();
    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());
    RestoreOptions options;
    CHECK_ERR(observatory.restore(path_for("absent.jostore"), options), ErrorCode::IoFailure);
}
