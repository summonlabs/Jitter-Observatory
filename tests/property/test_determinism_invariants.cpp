// Jitter Observatory - property and seeded randomised invariant tests.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include <jitter/engine.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

private:
    std::uint64_t state_;
};

// Rebuilds a batch with its samples in a different order. The sample set is identical,
// so every window result must be identical as well.
LatencyBatch reordered_batch(const LatencyBatch& original, std::uint64_t seed) {
    LatencyBatch copy = original;
    std::mt19937 generator(static_cast<std::mt19937::result_type>(seed));
    std::shuffle(copy.samples.begin(), copy.samples.end(), generator);
    auto rebuilt = make_batch(copy);
    return rebuilt.ok() ? rebuilt.value() : LatencyBatch{};
}

}  // namespace

JITTER_TEST(property, arrival_order_within_a_batch_never_changes_a_summary) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        ScenarioPlan plan;
        plan.samples = 96;
        plan.batch_size = 12;
        plan.seed = 1000 + seed;
        plan.spike_every = static_cast<std::int64_t>(seed % 5);

        auto first = build_engine(plan, fixture_config(128, 256), false, "property/forward");
        REQUIRE_OK(first);
        auto second = build_engine(plan, fixture_config(128, 256), false, "property/forward");
        REQUIRE_OK(second);

        const ScenarioPlan effective = first.value().plan;
        ScenarioPlan other = plan;
        // The second engine must declare the same identities to accept the same samples.
        std::uint64_t index = 0;
        for (const LatencyBatch& batch : first.value().batches) {
            auto direct = first.value().observatory->ingest(batch, first.value().context);
            REQUIRE_OK(direct);
            const LatencyBatch shuffled = reordered_batch(batch, seed * 31 + index);
            auto reordered = second.value().observatory->ingest(shuffled, second.value().context);
            REQUIRE_OK(reordered);
            CHECK(reordered.value().admitted());
            ++index;
        }
        static_cast<void>(other);

        SummaryRequest request = first.value().summary_request();
        request.metrics = default_metric_set();
        auto parsed_a = first.value().observatory->summarize(request, first.value().context, false);
        auto parsed_b = second.value().observatory->summarize(request, second.value().context, false);
        REQUIRE_OK(parsed_a);
        REQUIRE_OK(parsed_b);

        // The measurement content is identical no matter how the observations arrived.
        CHECK_EQ(parsed_a.value().summary.measurement_digest,
                 parsed_b.value().summary.measurement_digest);
        CHECK_EQ(parsed_a.value().summary.metrics.size(), parsed_b.value().summary.metrics.size());
        for (const MetricValue& value : parsed_a.value().summary.metrics) {
            const MetricValue* counterpart = parsed_b.value().summary.find(value.id);
            REQUIRE(counterpart != nullptr);
            CHECK_EQ(value.value, counterpart->value);
            CHECK_EQ(value.term_count, counterpart->term_count);
        }
        CHECK_EQ(parsed_a.value().summary.classification.level,
                 parsed_b.value().summary.classification.level);
        CHECK_EQ(parsed_a.value().summary.evidence.fresh, parsed_b.value().summary.evidence.fresh);
        CHECK_EQ(parsed_a.value().summary.selected, parsed_b.value().summary.selected);

        // The operational counters may differ, and they record exactly why.
        auto summary_a = first.value().observatory->export_summary(request, first.value().context, false);
        auto summary_b =
            second.value().observatory->export_summary(request, second.value().context, false);
        REQUIRE_OK(summary_a);
        REQUIRE_OK(summary_b);
        CHECK(summary_a.value() != summary_b.value() ||
              parsed_a.value().summary.out_of_order_accepted == 0);
        CHECK(parsed_b.value().summary.out_of_order_accepted > 0);
        CHECK_EQ(parsed_a.value().summary.selected, std::uint64_t{96});
        CHECK_EQ(parsed_a.value().summary.evidence.fresh, std::uint64_t{96});
        static_cast<void>(effective);
    }
}

JITTER_TEST(property, batch_size_never_changes_a_summary) {
    ScenarioPlan small_plan;
    small_plan.samples = 120;
    small_plan.batch_size = 1;
    small_plan.seed = 77;

    ScenarioPlan large_plan = small_plan;
    large_plan.batch_size = 40;

    auto small = build_engine(small_plan, fixture_config(256, 512), false, "property/batch");
    REQUIRE_OK(small);
    auto large = build_engine(large_plan, fixture_config(256, 512), false, "property/batch");
    REQUIRE_OK(large);

    for (const LatencyBatch& batch : small.value().batches) {
        REQUIRE_OK(small.value().observatory->ingest(batch, small.value().context));
    }
    for (const LatencyBatch& batch : large.value().batches) {
        REQUIRE_OK(large.value().observatory->ingest(batch, large.value().context));
    }

    const SummaryRequest request = small.value().summary_request();
    auto a = small.value().observatory->export_summary(request, small.value().context, false);
    auto b = large.value().observatory->export_summary(request, large.value().context, false);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK_EQ(a.value(), b.value());
}

JITTER_TEST(property, window_bounds_hold_under_randomised_loads) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        SplitMix64 random(seed * 7919);
        const std::uint64_t capacity = 8 + random.below(40);
        EngineConfig config = fixture_config(capacity, capacity);
        config.window.kind = static_cast<WindowKind>(random.below(3));
        config.window.count = capacity;
        config.window.duration_ns = static_cast<std::int64_t>(1000 + random.below(100000));
        config.window.anchor_ns = static_cast<std::int64_t>(random.below(500));
        if (config.window.kind == WindowKind::Time && config.window.count == 0) {
            config.window.count = 1;
        }

        ScenarioPlan plan;
        plan.samples = 64 + random.below(64);
        plan.batch_size = 1 + random.below(16);
        plan.seed = seed;
        plan.jitter_ns = static_cast<std::int64_t>(1 + random.below(50000));
        plan.interval_ns = static_cast<std::int64_t>(1 + random.below(2000));

        auto fixture = build_engine(plan, config, true, "property/bounds");
        REQUIRE_OK(fixture);
        CHECK(fixture.value().observatory->window_count() == 1);

        SummaryRequest request = fixture.value().summary_request();
        auto summary = fixture.value().observatory->summarize(request, fixture.value().context, true);
        REQUIRE_OK(summary);
        CHECK(summary.value().summary.retained <= config.window.capacity);
        CHECK(summary.value().summary.selected <= config.window.capacity);
        // Retained samples are exactly what the capacity bound allowed to stay.
        CHECK_EQ(summary.value().summary.retained + summary.value().summary.evicted_by_capacity,
                 plan.samples);
        CHECK(summary.value().summary.retained <= config.window.capacity);
        CHECK_EQ(summary.value().summary.evicted_by_capacity,
                 plan.samples - summary.value().summary.retained);

        // Every reported metric is identified by its own definition.
        for (const MetricValue& value : summary.value().summary.metrics) {
            const MetricDefinition* definition = metric_registry().find(value.id);
            REQUIRE(definition != nullptr);
            CHECK_EQ(definition->key, value.key);
            const std::uint64_t expected_terms =
                definition->input == MetricInput::Latency ? value.sample_count : value.sample_count - 1;
            if (value.sample_count >= definition->min_samples) {
                CHECK_EQ(value.term_count, expected_terms);
            }
        }

        // A positive level requires fresh evidence and no contradiction.
        if (makes_positive_assertion(summary.value().summary.classification.level)) {
            CHECK(summary.value().summary.evidence.fresh > 0);
            CHECK_EQ(summary.value().summary.evidence.conflicting, std::uint64_t{0});
            CHECK(summary.value().summary.fresh_samples >= config.policy.min_fresh_samples);
            CHECK(summary.value().summary.current_generation);
        } else if (asserts_instability(summary.value().summary.classification.level)) {
            CHECK(false);
        }
        CHECK(summary.value().summary.evidence.total <= summary.value().summary.selected);
    }
}

JITTER_TEST(property, classifications_never_contradict_their_evidence) {
    for (std::uint64_t seed = 100; seed < 140; ++seed) {
        SplitMix64 random(seed);
        EvidenceSummary evidence;
        evidence.total = random.below(200);
        evidence.fresh = random.below(evidence.total + 1);
        std::uint64_t remaining = evidence.total - evidence.fresh;
        evidence.stale = random.below(remaining + 1);
        remaining -= evidence.stale;
        evidence.expired = random.below(remaining + 1);
        remaining -= evidence.expired;
        evidence.unknown = random.below(remaining + 1);
        remaining -= evidence.unknown;
        evidence.conflicting = random.below(remaining + 1);
        remaining -= evidence.conflicting;
        evidence.unsupported = random.below(remaining + 1);
        remaining -= evidence.unsupported;
        evidence.incomplete = remaining;
        evidence.recompute_dominant();

        std::vector<std::int64_t> latencies;
        const std::uint64_t count = random.below(64);
        for (std::uint64_t i = 0; i < count; ++i) {
            latencies.push_back(static_cast<std::int64_t>(random.below(100000)));
        }

        InstabilityPolicy policy = default_instability_policy();
        ClassificationInput input;
        input.series = synthetic_series_id("property-series");
        input.path = synthetic_path_id("property-path");
        input.generation = synthetic_generation_id("property-generation");
        input.generation_ordinal = Ordinal(1);
        input.policy = policy;
        input.window = make_window_policy_id(WindowPolicy{});
        input.evidence = evidence;
        input.metrics = metric_registry().compute_all(default_metric_set(), latencies).values;

        const Classification classification = classify(input);
        if (makes_positive_assertion(classification.level)) {
            CHECK(evidence.fresh > 0);
            CHECK_EQ(evidence.conflicting, std::uint64_t{0});
            CHECK(evidence.fresh >= policy.min_fresh_samples);
        }
        if (evidence.total == 0) {
            CHECK_EQ(classification.level, InstabilityLevel::Missing);
        }
        if (evidence.fresh == 0 && evidence.total > 0 && evidence.conflicting == 0) {
            CHECK(!asserts_instability(classification.level));
            CHECK(!asserts_stability(classification.level));
        }
        // Determinism: a second identical call yields the same answer.
        const Classification repeat = classify(input);
        CHECK_EQ(repeat.level, classification.level);
        CHECK_EQ(repeat.reasons, classification.reasons);
    }
}

JITTER_TEST(property, store_round_trips_random_payload_sequences) {
    const std::string directory = scratch_directory("property-store");
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        SplitMix64 random(seed * 104729);
        const std::string path = directory + "/random-" + std::to_string(seed) + ".jostore";
        std::error_code error;
        std::filesystem::remove(path, error);

        StoreHeader header;
        header.store_epoch = seed;
        header.created_at_utc_ns = 1700000000000000000ll;
        header.boot_id = seed;
        header.store_id = Digest::of("property-" + std::to_string(seed));
        auto writer = StoreWriter::create(path, header, true);
        REQUIRE_OK(writer);

        const std::uint64_t records = 1 + random.below(40);
        std::vector<std::vector<std::uint8_t>> payloads;
        for (std::uint64_t i = 0; i < records; ++i) {
            const std::uint64_t length = random.below(200);
            std::vector<std::uint8_t> payload;
            for (std::uint64_t j = 0; j < length; ++j) {
                payload.push_back(static_cast<std::uint8_t>(random.below(256)));
            }
            payloads.push_back(payload);
            CHECK_OK(writer.value().append(StoreRecordType::Marker, payloads.back()));
        }
        CHECK_OK(writer.value().append_trailer());
        writer.value().close();

        auto loaded = load_store(path);
        REQUIRE_OK(loaded);
        CHECK(loaded.value().recovery.clean);
        CHECK_EQ(loaded.value().recovery.records_recovered, records + 1);
        for (std::uint64_t i = 0; i < records; ++i) {
            CHECK_EQ(loaded.value().records[i].payload, payloads[i]);
        }
    }
}

JITTER_TEST(property, metric_identities_are_stable_and_distinct_across_calls) {
    std::map<std::string, MetricId> seen;
    for (int pass = 0; pass < 3; ++pass) {
        for (const MetricDefinition& definition : metric_registry().definitions()) {
            const auto it = seen.find(std::string(definition.name));
            if (it == seen.end()) {
                seen.emplace(std::string(definition.name), definition.id);
                continue;
            }
            CHECK_EQ(it->second, definition.id);
        }
    }
    CHECK_EQ(seen.size(), metric_registry().size());
    CHECK_OK(verify_metric_table());
}

JITTER_TEST(property, repeated_ingest_of_the_same_plan_is_stable) {
    ScenarioPlan plan;
    plan.samples = 200;
    plan.batch_size = 25;
    plan.spike_every = 10;

    std::string reference;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto fixture = build_engine(plan, fixture_config(256, 256), true, "property/repeat");
        REQUIRE_OK(fixture);
        SummaryRequest request = fixture.value().summary_request();
        auto document =
            fixture.value().observatory->export_summary(request, fixture.value().context, false);
        REQUIRE_OK(document);
        if (reference.empty()) {
            reference = document.value();
        } else {
            CHECK_EQ(document.value(), reference);
        }
    }
    CHECK(!reference.empty());
}
