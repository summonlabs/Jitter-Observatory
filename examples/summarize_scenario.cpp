// Jitter Observatory example: build a synthetic scenario, ingest it, and print the
// deterministic canonical summary document.
// Copyright 2026 Summon Software Labs.

#include <iostream>

#include <jitter/engine.hpp>
#include <jitter/scenario.hpp>

int main() {
    using namespace jitter;

    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 7;
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 128;
    window.capacity = 256;
    config.window = window;

    Observatory observatory(config);
    Status status = observatory.initialize();
    if (!status.ok()) {
        std::cerr << "initialise failed: " << status.error().to_text() << "\n";
        return 1;
    }

    ScenarioPlan plan;
    plan.samples = 512;
    plan.seed = 4242;
    plan.spike_every = 64;  // deterministic spikes in a SYNTHETIC series
    auto registered = register_scenario(observatory, plan, "example/forward");
    if (!registered.ok()) {
        std::cerr << "scenario failed: " << registered.error().to_text() << "\n";
        return 1;
    }
    const ScenarioPlan effective = registered.value();

    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns +
                         static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                         effective.receive_delay_ns;

    status = ingest_plan(observatory, effective, context);
    if (!status.ok()) {
        std::cerr << "ingest failed: " << status.error().to_text() << "\n";
        return 1;
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
    request.current_generation = true;

    auto document = observatory.export_summary(request, context, true);
    if (!document.ok()) {
        std::cerr << "summary failed: " << document.error().to_text() << "\n";
        return 1;
    }
    std::cout << document.value() << "\n";
    std::cout << "series=" << effective.series.hex() << " path=" << effective.path.hex()
              << " generation=" << effective.generation.hex() << "\n";
    return 0;
}
