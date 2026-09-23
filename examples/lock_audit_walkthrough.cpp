// Jitter Observatory example: run a multi threaded ingest workload and report the
// lock audit, proving no audited lock was ever nested or acquired recursively.
// Copyright 2026 Summon Software Labs.

#include <iostream>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/lock_audit.hpp>
#include <jitter/runtime.hpp>
#include <jitter/scenario.hpp>

int main() {
    using namespace jitter;

    EngineConfig config;
    config.policy = default_instability_policy();
    Observatory observatory(config);
    if (!observatory.initialize().ok()) {
        std::cerr << "initialise failed\n";
        return 1;
    }
    ScenarioPlan plan;
    plan.samples = 512;
    auto registered = register_scenario(observatory, plan, "example/concurrency");
    if (!registered.ok()) {
        std::cerr << registered.error().to_text() << "\n";
        return 1;
    }
    const ScenarioPlan effective = registered.value();
    const std::vector<LatencyBatch> batches = make_batches(effective);

    RuntimeConfig runtime_config;
    runtime_config.worker_threads = 4;
    runtime_config.queue_capacity = 64;
    ObservationRuntime runtime(observatory, runtime_config);
    if (!runtime.start().ok()) {
        std::cerr << "runtime start failed\n";
        return 1;
    }

    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns +
                         static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                         effective.receive_delay_ns;

    for (const LatencyBatch& batch : batches) {
        const Status submitted = runtime.submit(
            [&batch, context](Observatory& engine, const CancellationToken& token) -> Status {
                if (token.cancelled()) {
                    return Status::failure(ErrorCode::Cancelled, "cancelled before ingest");
                }
                auto outcome = engine.ingest(batch, context);
                if (!outcome.ok()) {
                    return outcome.error();
                }
                return Status::success();
            },
            "ingest");
        if (!submitted.ok() && submitted.code() == ErrorCode::Backpressure) {
            static_cast<void>(runtime.drain());
            const Status retried = runtime.submit(
                [&batch, context](Observatory& engine, const CancellationToken& token) -> Status {
                    if (token.cancelled()) {
                        return Status::failure(ErrorCode::Cancelled, "cancelled before ingest");
                    }
                    auto outcome = engine.ingest(batch, context);
                    return outcome.ok() ? Status::success() : outcome.error();
                },
                "ingest-retry");
            if (!retried.ok()) {
                std::cerr << "resubmit failed: " << retried.error().to_text() << "\n";
                return 1;
            }
        }
    }

    if (!runtime.drain().ok()) {
        std::cerr << "drain failed\n";
        return 1;
    }
    const RuntimeStats stats = runtime.stats();
    static_cast<void>(runtime.shutdown(false));

    std::cout << "submitted=" << stats.submitted << " completed=" << stats.completed
              << " failed=" << stats.failed << "\n";
    std::cout << "lock_audit.enabled=" << (LockAudit::enabled() ? "true" : "false") << "\n";
    std::cout << "lock_audit.acquisitions=" << stats.lock_audit.acquisitions << "\n";
    std::cout << "lock_audit.contended=" << stats.lock_audit.contentions << "\n";
    std::cout << "lock_audit.nested=" << stats.lock_audit.nested_acquisitions << "\n";
    std::cout << "lock_audit.reentrant=" << stats.lock_audit.reentrant_acquisitions << "\n";
    std::cout << "lock_audit.max_hold_depth=" << stats.lock_audit.max_hold_depth << "\n";
    std::cout << "violation_free=" << (stats.lock_audit.violation_free() ? "true" : "false") << "\n";
    return stats.lock_audit.violation_free() ? 0 : 3;
}
