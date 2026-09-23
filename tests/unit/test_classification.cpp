// Jitter Observatory - policy driven instability classification.
// Copyright 2026 Summon Software Labs.
#include <vector>

#include <jitter/classification.hpp>
#include <jitter/episode.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

InstabilityPolicy quiet_policy() {
    InstabilityPolicy policy;
    policy.name = "quiet";
    policy.min_fresh_samples = 4;
    policy.min_terms = 2;
    ThresholdRule rule;
    rule.metric = MetricKey::AbsoluteDeltaMean;
    rule.direction = ThresholdDirection::AtLeast;
    rule.elevated_at = 1000.0;
    rule.unstable_at = 10000.0;
    rule.justification = "unit test threshold";
    policy.rules.push_back(rule);
    return policy;
}

ClassificationInput make_input(const InstabilityPolicy& policy, const EvidenceSummary& evidence,
                               const std::vector<std::int64_t>& latencies) {
    ClassificationInput input;
    input.series = synthetic_series_id("classification-series");
    input.path = synthetic_path_id("classification-path");
    input.generation = synthetic_generation_id("classification-generation");
    input.generation_ordinal = Ordinal(1);
    input.policy = policy;
    input.window = make_window_policy_id(WindowPolicy{});
    input.evidence = evidence;
    input.metrics = metric_registry().compute_all(default_metric_set(), latencies).values;
    input.metric_diagnostics = metric_registry().compute_all(default_metric_set(), latencies).unavailable;
    return input;
}

EvidenceSummary fresh_evidence(std::uint64_t count) {
    EvidenceSummary evidence;
    evidence.total = count;
    evidence.fresh = count;
    evidence.recompute_dominant();
    return evidence;
}

}  // namespace

JITTER_TEST(classification, a_policy_without_rules_never_asserts_anything) {
    InstabilityPolicy empty;
    empty.name = "empty";
    const ClassificationInput input = make_input(empty, fresh_evidence(16), {100, 110, 105, 115});
    const Classification classification = classify(input);
    CHECK_EQ(classification.level, InstabilityLevel::Unsupported);
    CHECK(!asserts_stability(classification.level));
    CHECK(!asserts_instability(classification.level));
    CHECK(!makes_positive_assertion(classification.level));
    CHECK(!classification.reasons.empty());
    CHECK_EQ(classification.reasons.front(), std::string("no_threshold_rules_declared"));
}

JITTER_TEST(classification, absent_evidence_is_missing_not_stable) {
    EvidenceSummary empty;
    empty.recompute_dominant();
    const Classification classification = classify(make_input(quiet_policy(), empty, {}));
    CHECK_EQ(classification.level, InstabilityLevel::Missing);
    CHECK_EQ(classification.evidence, EvidenceState::Missing);
    CHECK(!asserts_stability(classification.level));
    CHECK(!classification.asserts_current_instability());
}

JITTER_TEST(classification, stale_evidence_cannot_establish_current_instability) {
    EvidenceSummary stale;
    stale.total = 32;
    stale.stale = 32;
    stale.recompute_dominant();

    // The readings themselves are wildly unstable; admissibility, not magnitude, decides.
    const std::vector<std::int64_t> wild{100, 100000, 200, 900000, 100, 500000};
    const Classification classification = classify(make_input(quiet_policy(), stale, wild));
    CHECK_EQ(classification.level, InstabilityLevel::Stale);
    CHECK(!asserts_instability(classification.level));
    CHECK(!asserts_stability(classification.level));
    CHECK(!classification.asserts_current_instability());
    CHECK_EQ(classification.evidence, EvidenceState::Stale);

    EvidenceSummary expired;
    expired.total = 8;
    expired.expired = 8;
    expired.recompute_dominant();
    CHECK_EQ(classify(make_input(quiet_policy(), expired, wild)).level, InstabilityLevel::Expired);
}

JITTER_TEST(classification, contradicted_evidence_blocks_a_positive_level) {
    EvidenceSummary conflicting;
    conflicting.total = 16;
    conflicting.fresh = 15;
    conflicting.conflicting = 1;
    conflicting.recompute_dominant();
    const Classification classification =
        classify(make_input(quiet_policy(), conflicting, {100, 110, 120, 130}));
    CHECK_EQ(classification.level, InstabilityLevel::Conflicting);
    CHECK(!classification.asserts_current_instability());

    // A policy that explicitly tolerates a contradiction may continue.
    InstabilityPolicy tolerant = quiet_policy();
    tolerant.max_conflicts_tolerated = 1;
    const Classification allowed =
        classify(make_input(tolerant, conflicting, {100, 110, 120, 130}));
    CHECK(makes_positive_assertion(allowed.level));
}

JITTER_TEST(classification, too_few_fresh_observations_is_insufficient) {
    const Classification classification =
        classify(make_input(quiet_policy(), fresh_evidence(3), {100, 110, 105}));
    CHECK_EQ(classification.level, InstabilityLevel::Insufficient);
    CHECK(!classification.asserts_current_instability());
    CHECK(!asserts_stability(classification.level));
}

JITTER_TEST(classification, thresholds_produce_stable_elevated_and_unstable) {
    InstabilityPolicy policy = quiet_policy();

    const Classification stable =
        classify(make_input(policy, fresh_evidence(16), {1000, 1000, 1000, 1000, 1000, 1000}));
    CHECK_EQ(stable.level, InstabilityLevel::Stable);
    CHECK(stable.has_driver == false);

    // Successive deltas of five thousand nanoseconds sit between the two thresholds.
    const Classification elevated =
        classify(make_input(policy, fresh_evidence(16), {0, 5000, 10000, 15000, 20000, 25000}));
    CHECK_EQ(elevated.level, InstabilityLevel::Elevated);
    CHECK(elevated.has_driver);
    CHECK_EQ(elevated.driver_key, MetricKey::AbsoluteDeltaMean);
    CHECK_EQ(elevated.driver_value, 5000.0);

    const Classification unstable =
        classify(make_input(policy, fresh_evidence(16), {0, 20000, 0, 20000, 0, 20000}));
    CHECK_EQ(unstable.level, InstabilityLevel::Unstable);
    CHECK(unstable.asserts_current_instability());
    CHECK_EQ(unstable.driver_key, MetricKey::AbsoluteDeltaMean);
    CHECK_EQ(unstable.driver_id, metric_registry().definition(MetricKey::AbsoluteDeltaMean).id);
}

JITTER_TEST(classification, require_all_rules_changes_the_combination_rule) {
    InstabilityPolicy policy = quiet_policy();
    ThresholdRule second;
    second.metric = MetricKey::SampleStdDev;
    second.direction = ThresholdDirection::AtLeast;
    second.elevated_at = 1000.0;    // the raw readings deviate by about 10954
    second.unstable_at = 1000000.0;  // but never by a million
    second.justification = "second unit test threshold";
    policy.rules.push_back(second);

    const std::vector<std::int64_t> series{0, 20000, 0, 20000, 0, 20000};
    const Classification any_rule = classify(make_input(policy, fresh_evidence(16), series));
    CHECK_EQ(any_rule.level, InstabilityLevel::Unstable);

    policy.require_all_rules = true;
    const Classification all_rules = classify(make_input(policy, fresh_evidence(16), series));
    CHECK_EQ(all_rules.level, InstabilityLevel::Elevated);
    CHECK(all_rules.has_driver);
}

JITTER_TEST(classification, rules_that_cannot_be_evaluated_are_named) {
    InstabilityPolicy policy = quiet_policy();
    policy.min_terms = 1000;  // no rule can ever satisfy this
    const Classification classification =
        classify(make_input(policy, fresh_evidence(16), {100, 110, 120, 130}));
    CHECK_EQ(classification.level, InstabilityLevel::Insufficient);
    REQUIRE(!classification.reasons.empty());
    bool found = false;
    for (const std::string& reason : classification.reasons) {
        if (reason.find("no_rule_could_be_evaluated") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

JITTER_TEST(classification, a_closed_generation_is_never_current) {
    ClassificationInput input = make_input(quiet_policy(), fresh_evidence(16), {0, 20000, 0, 20000});
    input.current_generation = false;
    const Classification classification = classify(input);
    CHECK_EQ(classification.level, InstabilityLevel::Unsupported);
    CHECK(!classification.asserts_current_instability());
    CHECK(!asserts_instability(classification.level));
}

JITTER_TEST(classification, classification_is_deterministic) {
    const std::vector<std::int64_t> series{100, 1000, 120, 4000, 90, 2200, 500};
    const Classification first = classify(make_input(quiet_policy(), fresh_evidence(16), series));
    const Classification second = classify(make_input(quiet_policy(), fresh_evidence(16), series));
    CHECK_EQ(first.level, second.level);
    CHECK_EQ(first.driver_key, second.driver_key);
    CHECK_EQ(first.driver_value, second.driver_value);
    CHECK_EQ(first.reasons, second.reasons);
    CHECK_EQ(first.policy, second.policy);
    CHECK_EQ(first.rules.size(), second.rules.size());
}

JITTER_TEST(classification, policy_validation_rejects_undeclared_or_impossible_rules) {
    InstabilityPolicy policy = quiet_policy();
    CHECK_OK(validate_instability_policy(policy));

    InstabilityPolicy unjustified = policy;
    unjustified.rules[0].justification.clear();
    CHECK_ERR(validate_instability_policy(unjustified), ErrorCode::InvalidArgument);

    InstabilityPolicy inverted = policy;
    inverted.rules[0].elevated_at = 500.0;
    inverted.rules[0].unstable_at = 100.0;
    CHECK_ERR(validate_instability_policy(inverted), ErrorCode::InvalidArgument);

    InstabilityPolicy duplicated = policy;
    duplicated.rules.push_back(policy.rules[0]);
    CHECK_ERR(validate_instability_policy(duplicated), ErrorCode::Duplicate);

    InstabilityPolicy nonfinite = policy;
    nonfinite.rules[0].elevated_at = std::numeric_limits<double>::quiet_NaN();
    CHECK_ERR(validate_instability_policy(nonfinite), ErrorCode::InvalidArgument);

    InstabilityPolicy nameless = policy;
    nameless.name.clear();
    CHECK_ERR(validate_instability_policy(nameless), ErrorCode::InvalidArgument);

    InstabilityPolicy identified_a = quiet_policy();
    InstabilityPolicy identified_b = quiet_policy();
    CHECK_EQ(make_instability_policy_id(identified_a), make_instability_policy_id(identified_b));
    identified_b.rules[0].elevated_at += 1.0;
    CHECK(make_instability_policy_id(identified_a) != make_instability_policy_id(identified_b));
}

JITTER_TEST(episode, episodes_open_extend_and_recover) {
    InstabilityPolicy policy = quiet_policy();
    policy.episode_close_windows = 2;
    EpisodeLog log;

    const ClassificationInput unstable_input =
        make_input(policy, fresh_evidence(16), {0, 20000, 0, 20000, 0, 20000});
    const Classification unstable = classify(unstable_input);
    REQUIRE(asserts_instability(unstable.level));

    const EpisodeUpdate opened = log.observe(unstable, 1000);
    REQUIRE(opened.opened.has_value());
    CHECK(opened.opened.value().is_open());
    CHECK_EQ(opened.opened.value().observations, std::uint64_t{1});
    CHECK_EQ(log.open_episodes().size(), std::size_t{1});

    const EpisodeUpdate extended = log.observe(unstable, 2000);
    CHECK(extended.extended);
    CHECK(!extended.opened.has_value());
    CHECK_EQ(log.open_episodes().front().observations, std::uint64_t{2});

    const Classification stable =
        classify(make_input(policy, fresh_evidence(16), {1000, 1000, 1000, 1000, 1000, 1000}));
    const EpisodeUpdate first_recovery = log.observe(stable, 3000);
    CHECK(!first_recovery.closed.has_value());
    CHECK_EQ(log.open_episodes().size(), std::size_t{1});

    const EpisodeUpdate second_recovery = log.observe(stable, 4000);
    REQUIRE(second_recovery.closed.has_value());
    CHECK_EQ(second_recovery.closed.value().close_reason, EpisodeCloseReason::Recovered);
    CHECK_EQ(second_recovery.closed.value().closed_at_utc_ns.value(), std::int64_t{4000});
    CHECK(log.open_episodes().empty());
    CHECK_EQ(log.episodes(unstable.series).size(), std::size_t{1});
}

JITTER_TEST(episode, losing_the_evidence_closes_an_episode_without_claiming_recovery) {
    InstabilityPolicy policy = quiet_policy();
    EpisodeLog log;
    const Classification unstable =
        classify(make_input(policy, fresh_evidence(16), {0, 20000, 0, 20000}));
    REQUIRE(log.observe(unstable, 1000).opened.has_value());

    EvidenceSummary stale;
    stale.total = 16;
    stale.stale = 16;
    stale.recompute_dominant();
    const Classification lost = classify(make_input(policy, stale, {0, 20000, 0, 20000}));
    const EpisodeUpdate update = log.observe(lost, 2000);
    REQUIRE(update.closed.has_value());
    CHECK_EQ(update.closed.value().close_reason, EpisodeCloseReason::EvidenceLost);
    CHECK(log.open_episodes().empty());
}

JITTER_TEST(episode, a_generation_change_segments_history) {
    InstabilityPolicy policy = quiet_policy();
    EpisodeLog log;
    const Classification unstable =
        classify(make_input(policy, fresh_evidence(16), {0, 20000, 0, 20000}));
    REQUIRE(log.observe(unstable, 1000).opened.has_value());
    CHECK_EQ(log.open_episodes().size(), std::size_t{1});

    const GenerationId newer = synthetic_generation_id("newer-generation");
    const std::vector<Episode> closed = log.retire(unstable.series, newer, unstable.policy,
                                                   unstable.window, 2000);
    REQUIRE(closed.size() == 1);
    CHECK_EQ(closed.front().close_reason, EpisodeCloseReason::GenerationChanged);
    CHECK(log.open_episodes().empty());
    CHECK_EQ(log.episodes(unstable.series).size(), std::size_t{1});

    // A classification that targets the closed generation must not reopen anything.
    ClassificationInput historical = make_input(policy, fresh_evidence(16), {0, 20000, 0, 20000});
    historical.current_generation = false;
    const EpisodeUpdate update = log.observe(classify(historical), 3000);
    CHECK(!update.opened.has_value());
    CHECK(log.open_episodes().empty());
}

JITTER_TEST(episode, episode_retention_is_bounded_and_counted) {
    InstabilityPolicy policy = quiet_policy();
    EpisodeLog log;
    const Classification unstable =
        classify(make_input(policy, fresh_evidence(16), {0, 20000, 0, 20000}));

    // A distinct generation per episode, so that each one is closed and retained
    // separately until the retention bound starts dropping the oldest.
    for (std::uint64_t i = 0; i < Limits::kMaxEpisodesPerSeries + 8; ++i) {
        Episode episode;
        episode.series = unstable.series;
        episode.path = unstable.path;
        episode.generation = synthetic_generation_id("retention-" + std::to_string(i));
        episode.generation_ordinal = Ordinal(i + 1);
        episode.policy = unstable.policy;
        episode.window = unstable.window;
        episode.opened_at_utc_ns = static_cast<std::int64_t>(i) * 1000;
        episode.closed_at_utc_ns = static_cast<std::int64_t>(i) * 1000 + 10;
        episode.close_reason = EpisodeCloseReason::Recovered;
        episode.id = make_episode_id(episode);
        CHECK_OK(log.import_episode(episode, 0));
    }
    CHECK_EQ(log.episodes(unstable.series).size(),
             static_cast<std::size_t>(Limits::kMaxEpisodesPerSeries));
    CHECK_EQ(log.dropped(), std::uint64_t{8});
}

JITTER_TEST(episode, imported_open_episodes_are_closed_as_evidence_lost) {
    EpisodeLog log;
    Episode episode;
    episode.series = synthetic_series_id("import-series");
    episode.path = synthetic_path_id("import-path");
    episode.generation = synthetic_generation_id("import-generation");
    episode.generation_ordinal = Ordinal(1);
    episode.policy = make_instability_policy_id(quiet_policy());
    episode.window = make_window_policy_id(WindowPolicy{});
    episode.opened_at_utc_ns = 500;
    episode.id = make_episode_id(episode);
    CHECK_OK(log.import_episode(episode, 900));
    CHECK(log.open_episodes().empty());
    REQUIRE(log.episodes(episode.series).size() == 1);
    CHECK_EQ(log.episodes(episode.series).front().close_reason, EpisodeCloseReason::EvidenceLost);
}
