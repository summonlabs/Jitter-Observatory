// Jitter Observatory - metric, window, ingest and summary benchmarks.
// Copyright 2026 Summon Software Labs.

#include <iostream>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/metrics.hpp>
#include <jitter/scenario.hpp>
#include <jitter/window.hpp>

#include "bench_harness.hpp"

namespace {

using namespace jitter;

std::vector<std::int64_t> make_series(std::uint64_t count, std::int64_t base, std::int64_t spread) {
    std::vector<std::int64_t> values;
    values.reserve(static_cast<std::size_t>(count));
    std::uint64_t state = 0x12345678ull;
    for (std::uint64_t i = 0; i < count; ++i) {
        state = (state * 6364136223846793005ull) + 1442695040888963407ull;
        const auto noise = static_cast<std::int64_t>((state >> 33) % static_cast<std::uint64_t>(spread));
        values.push_back(base + noise);
    }
    return values;
}

}  // namespace

int main() {
    // Metric computation over a fixed, canonically ordered series.
    const std::vector<std::int64_t> series = make_series(4096, 120000, 5000);
    const MetricRegistry& registry = metric_registry();

    bench::run("metric.absolute_delta.mean/4096", 2000, series.size(), [&](std::uint64_t) {
        auto value = registry.compute(MetricKey::AbsoluteDeltaMean, series);
        return value.ok() ? static_cast<std::uint64_t>(value.value().value) : 0ull;
    });
    bench::run("metric.sample_variance/4096", 2000, series.size(), [&](std::uint64_t) {
        auto value = registry.compute(MetricKey::SampleVariance, series);
        return value.ok() ? static_cast<std::uint64_t>(value.value().value) : 0ull;
    });
    bench::run("metric.all_configured/4096", 500, series.size(), [&](std::uint64_t) {
        const auto computed = registry.compute_all(default_metric_set(), series);
        return static_cast<std::uint64_t>(computed.values.size() * 1000 +
                                          computed.unavailable.size());
    });

    // Window insertion and selection.
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 4096;
    window.capacity = 4096;

    ScenarioPlan plan;
    plan.samples = 2048;
    plan.batch_size = 32;
    // The whole plan has to stay inside the freshness budget, otherwise every reading
    // would be stale and the summary would (correctly) contain no numbers at all.
    plan.interval_ns = 100000;

    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 5;
    config.window = window;
    config.metrics = default_metric_set();

    Observatory observatory(config);
    if (!observatory.initialize().ok()) {
        std::cerr << "initialise failed\n";
        return 1;
    }
    auto registered = register_scenario(observatory, plan, "bench/forward");
    if (!registered.ok()) {
        std::cerr << registered.error().to_text() << "\n";
        return 1;
    }
    const ScenarioPlan effective = registered.value();
    const std::vector<LatencyBatch> batches = make_batches(effective);

    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns +
                         static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                         effective.receive_delay_ns;

    // Two separate measurements, because they answer different questions:
    //   * admission: validate, fence and store one batch into an engine that is already
    //     running, with a fresh sequence position each iteration
    //   * lifecycle: register a scenario, ingest a whole plan, from an empty engine
    // Generation and admission are measured separately: the first is synthetic source
    // cost, the second is the runtime's own validation, fencing and window insertion.
    std::uint64_t sequence = effective.sequence_start.value() + 1000000;
    const LatencyBatch template_batch = batches.front();

    bench::run("engine.generate_batch/32_samples", 2000, template_batch.samples.size(),
               [&](std::uint64_t) {
                   ScenarioPlan advancing = effective;
                   advancing.samples = effective.batch_size;  // exactly one batch
                   advancing.sequence_start = SourceSequence(sequence++);
                   const std::vector<LatencyBatch> produced = make_batches(advancing);
                   return produced.empty() ? 0ull : produced.front().samples.size();
               });

    // The batches for the admission measurement are produced up front so that the timed
    // loop contains admission and nothing else.
    std::vector<LatencyBatch> admission_batches;
    admission_batches.reserve(2000);
    for (std::uint64_t i = 0; i < 2000; ++i) {
        ScenarioPlan advancing = effective;
        advancing.samples = effective.batch_size;
        advancing.sequence_start = SourceSequence(sequence++);
        const std::vector<LatencyBatch> produced = make_batches(advancing);
        if (!produced.empty()) {
            admission_batches.push_back(produced.front());
        }
    }
    std::size_t admission_index = 0;
    bench::run("engine.admit_batch/32_samples", admission_batches.size(),
               template_batch.samples.size(), [&](std::uint64_t) {
                   if (admission_index >= admission_batches.size()) {
                       return 0ull;
                   }
                   auto outcome = observatory.ingest(admission_batches[admission_index++], context);
                   if (!outcome.ok()) {
                       return 0ull;
                   }
                   return outcome.value().accepted;
               });

    bench::run("engine.ingest_plan/2048_samples_with_setup", 200, effective.samples,
               [&](std::uint64_t) {
                   Observatory local(config);
                   static_cast<void>(local.initialize());
                   auto local_plan = register_scenario(local, effective, "bench/forward");
                   if (!local_plan.ok()) {
                       return 0ull;
                   }
                   auto outcome = ingest_plan(local, local_plan.value(), context);
                   return outcome.ok() ? local.counters().samples_stored : 0ull;
               });

    for (const LatencyBatch& batch : batches) {
        auto outcome = observatory.ingest(batch, context);
        if (!outcome.ok()) {
            std::cerr << outcome.error().to_text() << "\n";
            return 1;
        }
    }

    SummaryRequest request;
    request.series = effective.series;
    request.path = effective.path;
    request.generation = effective.generation;
    request.generation_ordinal = effective.ordinal;
    const EngineConfig effective_config = observatory.config();
    request.window = effective_config.window;
    request.policy = effective_config.policy;
    request.metrics = effective_config.metrics;
    request.now_utc_ns = context.now_utc_ns;

    bench::run("engine.summarize/2048_samples", 500, 2048, [&](std::uint64_t) {
        auto outcome = observatory.summarize(request, context, false);
        if (!outcome.ok()) {
            return 0ull;
        }
        return static_cast<std::uint64_t>(outcome.value().summary.metrics.size());
    });

    bench::run("engine.export_summary_json", 500, 1, [&](std::uint64_t) {
        auto document = observatory.export_summary(request, context, false);
        return document.ok() ? static_cast<std::uint64_t>(document.value().size()) : 0ull;
    });

    bench::report();
    return 0;
}
