// Jitter Observatory - persistence benchmarks.
// Copyright 2026 Summon Software Labs.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include <jitter/engine.hpp>
#include <jitter/persistence.hpp>
#include <jitter/scenario.hpp>

#include "bench_harness.hpp"

namespace {

using namespace jitter;

std::string work_directory() {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "jitter-observatory-bench";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return base.string();
}

}  // namespace

int main() {
    const std::string directory = work_directory();

    ScenarioPlan plan;
    plan.samples = 4096;
    plan.batch_size = 64;

    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 9;
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 4096;
    window.capacity = 4096;
    config.window = window;

    Observatory observatory(config);
    if (!observatory.initialize().ok()) {
        std::cerr << "initialise failed\n";
        return 1;
    }
    auto registered = register_scenario(observatory, plan, "bench/store");
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

    bench::run("store.save/4096_samples", 20, effective.samples, [&](std::uint64_t iteration) {
        const std::string path = directory + "/bench-" + std::to_string(iteration % 2) + ".jostore";
        const Status saved = observatory.save(path, true, context);
        return saved.ok() ? static_cast<std::uint64_t>(std::filesystem::file_size(path)) : 0ull;
    });

    const std::string path = directory + "/bench-read.jostore";
    static_cast<void>(observatory.save(path, true, context));

    bench::run("store.load/4096_samples", 50, effective.samples, [&](std::uint64_t) {
        auto loaded = load_store(path);
        if (!loaded.ok()) {
            return 0ull;
        }
        return loaded.value().recovery.records_recovered;
    });

    bench::run("store.load_and_restore", 20, effective.samples, [&](std::uint64_t) {
        Observatory local(config);
        static_cast<void>(local.initialize());
        RestoreOptions options;
        auto restored = local.restore(path, options);
        return restored.ok() ? restored.value().samples : 0ull;
    });

    bench::report();

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return 0;
}
