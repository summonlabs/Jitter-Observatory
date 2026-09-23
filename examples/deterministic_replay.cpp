// Jitter Observatory example: determinism and conservative replay handling.
// Copyright 2026 Summon Software Labs.
//
// Three runs over one synthetic workload:
//   in order    the batches exactly as the generator produced them
//   shuffled    the same observations, but reordered *inside* each batch
//   replayed    the batches themselves delivered out of order
//
// The first two must agree on the measurement content, because a window canonicalises
// its observations before anything is computed. The third must not: a source's sequence
// numbers are a monotonic claim about the order it emitted observations in, so a
// reordered delivery is fenced instead of being silently accepted.

#include <algorithm>
#include <iostream>
#include <random>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/scenario.hpp>

namespace {

using namespace jitter;

enum class Delivery { InOrder, ShuffledWithinBatch, ShuffledBatches };

struct RunResult {
    SeriesSummary summary;
    std::uint64_t batches_accepted = 0;
    std::uint64_t batches_fenced = 0;
    std::uint64_t samples_stored = 0;
};

Result<RunResult> run_once(const std::vector<LatencyBatch>& batches, Delivery delivery,
                           std::uint64_t seed, const ScenarioPlan& plan, const EngineConfig& config) {
    Observatory observatory(config);
    JITTER_TRY(observatory.initialize());
    auto registered = register_scenario(observatory, plan, "example/replay");
    if (!registered.ok()) {
        return registered.error();
    }
    const ScenarioPlan effective = registered.value();

    std::vector<LatencyBatch> workload = batches;
    std::mt19937 generator(static_cast<std::mt19937::result_type>(seed));
    if (delivery == Delivery::ShuffledWithinBatch) {
        for (LatencyBatch& batch : workload) {
            std::shuffle(batch.samples.begin(), batch.samples.end(), generator);
            auto rebuilt = make_batch(batch);
            if (!rebuilt.ok()) {
                return rebuilt.error();
            }
            batch = rebuilt.value();
        }
    } else if (delivery == Delivery::ShuffledBatches) {
        std::shuffle(workload.begin(), workload.end(), generator);
    }

    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns +
                         static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                         effective.receive_delay_ns;

    RunResult result;
    for (const LatencyBatch& batch : workload) {
        auto outcome = observatory.ingest(batch, context);
        if (!outcome.ok()) {
            return outcome.error();
        }
        if (outcome.value().admitted()) {
            ++result.batches_accepted;
        } else if (outcome.value().verdict == SequenceVerdict::StaleSequence ||
                   outcome.value().verdict == SequenceVerdict::DuplicateIdempotent ||
                   outcome.value().verdict == SequenceVerdict::SequenceConflict) {
            ++result.batches_fenced;
        }
    }
    result.samples_stored = observatory.counters().samples_stored;

    const EngineConfig effective_config = observatory.config();
    SummaryRequest request;
    request.series = effective.series;
    request.path = effective.path;
    request.generation = effective.generation;
    request.generation_ordinal = effective.ordinal;
    request.window = effective_config.window;
    request.policy = effective_config.policy;
    request.metrics = effective_config.metrics;
    request.now_utc_ns = context.now_utc_ns;
    request.current_generation = true;

    auto summary = observatory.summarize(request, context, false);
    if (!summary.ok()) {
        return summary.error();
    }
    result.summary = summary.value().summary;
    return result;
}

void report(const char* label, const RunResult& run) {
    std::cout << label << ": accepted=" << run.batches_accepted << " fenced=" << run.batches_fenced
              << " stored=" << run.samples_stored
              << " measurement=" << run.summary.measurement_digest.hex()
              << " level=" << to_string(run.summary.classification.level) << "\n";
}

}  // namespace

int main() {
    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 11;
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 400;
    window.capacity = 400;
    config.window = window;

    ScenarioPlan plan;
    plan.samples = 400;
    plan.seed = 99;
    plan.batch_size = 8;

    // The identities a scenario declares are content addressed, so they are registered
    // once and then reused verbatim by every run.
    {
        Observatory probe(config);
        if (!probe.initialize().ok()) {
            std::cerr << "initialise failed\n";
            return 1;
        }
        auto registered = register_scenario(probe, plan, "example/replay");
        if (!registered.ok()) {
            std::cerr << registered.error().to_text() << "\n";
            return 1;
        }
        plan = registered.value();
    }
    const std::vector<LatencyBatch> batches = make_batches(plan);

    auto in_order = run_once(batches, Delivery::InOrder, 0, plan, config);
    if (!in_order.ok()) {
        std::cerr << in_order.error().to_text() << "\n";
        return 1;
    }
    auto within_batch = run_once(batches, Delivery::ShuffledWithinBatch, 1234, plan, config);
    if (!within_batch.ok()) {
        std::cerr << within_batch.error().to_text() << "\n";
        return 1;
    }
    auto replayed = run_once(batches, Delivery::ShuffledBatches, 4321, plan, config);
    if (!replayed.ok()) {
        std::cerr << replayed.error().to_text() << "\n";
        return 1;
    }

    report("in_order        ", in_order.value());
    report("shuffled_in_batch", within_batch.value());
    report("shuffled_batches ", replayed.value());

    const bool stable = in_order.value().summary.measurement_digest ==
                        within_batch.value().summary.measurement_digest;
    const bool conservative = replayed.value().batches_fenced > 0 &&
                              replayed.value().samples_stored < in_order.value().samples_stored;
    std::cout << "measurement_stable_across_arrival_order=" << (stable ? "true" : "false") << "\n";
    std::cout << "reordered_batches_are_fenced=" << (conservative ? "true" : "false") << "\n";
    return (stable && conservative) ? 0 : 2;
}
