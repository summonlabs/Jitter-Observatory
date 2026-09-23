// Independent downstream consumer of the installed Jitter Observatory package.
// Copyright 2026 Summon Software Labs.
//
// This program only uses the public API through find_package; if the installed package
// is incomplete it will not build or run.

#include <iostream>
#include <string>

#include <jitter/engine.hpp>
#include <jitter/metrics.hpp>
#include <jitter/scenario.hpp>
#include <jitter/version.hpp>

int main() {
    using namespace jitter;

    std::cout << "consuming " << kProductName << " " << kVersionString << "\n";

    if (!verify_metric_table().ok()) {
        std::cerr << "metric table verification failed\n";
        return 1;
    }

    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 3;
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 64;
    window.capacity = 64;
    config.window = window;

    Observatory observatory(config);
    if (!observatory.initialize().ok()) {
        std::cerr << "engine initialisation failed\n";
        return 1;
    }

    ScenarioPlan plan;
    plan.samples = 128;
    plan.seed = 2026;
    auto registered = register_scenario(observatory, plan, "downstream/forward");
    if (!registered.ok()) {
        std::cerr << registered.error().to_text() << "\n";
        return 1;
    }
    const ScenarioPlan effective = registered.value();

    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns +
                         static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                         effective.receive_delay_ns;
    if (!ingest_plan(observatory, effective, context).ok()) {
        std::cerr << "ingest failed\n";
        return 1;
    }

    // The engine publishes the configuration it is actually running with, including the
    // metric set it filled in from its defaults.
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

    auto summary = observatory.summarize(request, context, false);
    if (!summary.ok()) {
        std::cerr << summary.error().to_text() << "\n";
        return 1;
    }

    const MetricValue* delta = summary.value().summary.find(MetricKey::AbsoluteDeltaMean);
    if (delta == nullptr) {
        std::cerr << "expected the absolute delta metric to be present\n";
        return 1;
    }
    std::cout << "samples=" << summary.value().summary.selected
              << " level=" << to_string(summary.value().summary.classification.level)
              << " absolute_delta_mean_ns=" << delta->value << "\n";
    return 0;
}
