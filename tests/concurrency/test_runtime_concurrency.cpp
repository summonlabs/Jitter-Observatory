// Jitter Observatory - worker runtime, backpressure, cancellation and lock auditing.
// Copyright 2026 Summon Software Labs.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <jitter/lock_audit.hpp>
#include <jitter/runtime.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

struct Harness {
    EngineConfig config;
    ScenarioPlan plan;
    std::unique_ptr<Observatory> observatory;
    IngestContext context;
    std::vector<LatencyBatch> batches;
};

Result<Harness> make_harness(std::uint64_t samples = 256, std::uint64_t batch_size = 8) {
    Harness harness;
    harness.config = fixture_config(256, 512);
    harness.observatory = std::make_unique<Observatory>(harness.config);
    JITTER_TRY(harness.observatory->initialize());
    ScenarioPlan plan;
    plan.samples = samples;
    plan.batch_size = batch_size;
    auto registered = register_scenario(*harness.observatory, plan, "concurrency/forward");
    if (!registered.ok()) {
        return registered.error();
    }
    harness.plan = registered.value();
    harness.context.now_utc_ns = harness.plan.start_utc_ns +
                                 static_cast<std::int64_t>(harness.plan.samples) *
                                     harness.plan.interval_ns +
                                 harness.plan.receive_delay_ns;
    harness.batches = make_batches(harness.plan);
    return harness;
}

}  // namespace

JITTER_TEST(concurrency, submit_from_many_threads_completes_every_job) {
    LockAudit::reset();
    auto harness = make_harness();
    REQUIRE_OK(harness);

    RuntimeConfig config;
    config.worker_threads = 4;
    config.queue_capacity = 32;
    ObservationRuntime runtime(*harness.value().observatory, config);
    REQUIRE_OK(runtime.start());

    const std::size_t thread_count = 4;
    std::atomic<std::uint64_t> rejected{0};
    std::vector<std::thread> submitters;
    submitters.reserve(thread_count);
    for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        submitters.emplace_back([&, thread_index]() {
            for (std::size_t i = thread_index; i < harness.value().batches.size(); i += thread_count) {
                const LatencyBatch batch = harness.value().batches[i];
                const IngestContext context = harness.value().context;
                Status submitted = runtime.submit(
                    [batch, context](Observatory& engine, const CancellationToken& token) -> Status {
                        if (token.cancelled()) {
                            return Status::failure(ErrorCode::Cancelled, "cancelled");
                        }
                        auto outcome = engine.ingest(batch, context);
                        return outcome.ok() ? Status::success() : outcome.error();
                    },
                    "ingest");
                if (!submitted.ok()) {
                    if (submitted.code() == ErrorCode::Backpressure) {
                        rejected.fetch_add(1);
                        continue;
                    }
                    rejected.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : submitters) {
        thread.join();
    }

    // Deterministic completion: the runtime reports idle when every accepted job has
    // finished, which is the only synchronisation this test uses.
    REQUIRE_OK(runtime.drain());
    const RuntimeStats stats = runtime.stats();
    CHECK_EQ(stats.completed + rejected.load(), stats.submitted + rejected.load());
    CHECK(stats.completed > 0);
    CHECK_EQ(stats.failed, std::uint64_t{0});
    CHECK_EQ(stats.in_flight, std::uint64_t{0});
    CHECK_EQ(stats.queued_now, std::uint64_t{0});
    CHECK_EQ(stats.workers_started, std::uint64_t{4});
    CHECK(stats.peak_queue_depth <= config.queue_capacity);
    CHECK_EQ(stats.cancelled, std::uint64_t{0});

    if (LockAudit::enabled()) {
        CHECK_EQ(stats.lock_audit.nested_acquisitions, std::uint64_t{0});
        CHECK_EQ(stats.lock_audit.reentrant_acquisitions, std::uint64_t{0});
        CHECK(stats.lock_audit.acquisitions > 0);
        CHECK(stats.lock_audit.violation_free());
    }

    CHECK_OK(runtime.shutdown(true));
    CHECK(!runtime.running());
}

JITTER_TEST(concurrency, backpressure_is_reported_and_counted) {
    auto harness = make_harness();
    REQUIRE_OK(harness);

    RuntimeConfig config;
    config.worker_threads = 1;
    config.queue_capacity = 2;
    config.backpressure = BackpressurePolicy::Reject;
    ObservationRuntime runtime(*harness.value().observatory, config);
    CHECK_OK(validate_runtime_config(config));
    REQUIRE_OK(runtime.start());

    std::uint64_t rejected = 0;
    for (int i = 0; i < 200; ++i) {
        Status status = runtime.submit(
            [](Observatory&, const CancellationToken&) -> Status { return Status::success(); },
            "noop");
        if (!status.ok()) {
            CHECK_EQ(status.code(), ErrorCode::Backpressure);
            ++rejected;
        }
    }
    REQUIRE_OK(runtime.drain());
    const RuntimeStats stats = runtime.stats();
    CHECK_EQ(stats.rejected_backpressure, rejected);
    CHECK(rejected > 0);
    CHECK_EQ(stats.failed, std::uint64_t{0});
    CHECK_OK(runtime.shutdown(true));
}

JITTER_TEST(concurrency, drop_oldest_backpressure_discards_and_counts) {
    auto harness = make_harness();
    REQUIRE_OK(harness);

    RuntimeConfig config;
    config.worker_threads = 1;
    config.queue_capacity = 1;
    config.backpressure = BackpressurePolicy::DropOldest;
    ObservationRuntime runtime(*harness.value().observatory, config);
    REQUIRE_OK(runtime.start());

    for (int i = 0; i < 500; ++i) {
        CHECK_OK(runtime.submit(
            [](Observatory&, const CancellationToken&) -> Status { return Status::success(); }, "noop"));
    }
    REQUIRE_OK(runtime.drain());
    const RuntimeStats stats = runtime.stats();
    CHECK(stats.dropped_oldest > 0);
    CHECK_EQ(stats.failed, std::uint64_t{0});
    CHECK_EQ(stats.in_flight, std::uint64_t{0});
    CHECK_OK(runtime.shutdown(true));
}

JITTER_TEST(concurrency, cancellation_is_real_and_shutdown_refuses_new_work) {
    auto harness = make_harness();
    REQUIRE_OK(harness);

    RuntimeConfig config;
    config.worker_threads = 2;
    config.queue_capacity = 16;
    ObservationRuntime runtime(*harness.value().observatory, config);
    REQUIRE_OK(runtime.start());

    runtime.cancel();
    std::uint64_t submitted = 0;
    for (int i = 0; i < 8; ++i) {
        const Status status = runtime.submit(
            [](Observatory&, const CancellationToken& token) -> Status {
                if (token.cancelled()) {
                    return Status::failure(ErrorCode::Cancelled, "cancelled");
                }
                return Status::success();
            },
            "cancellable");
        if (status.ok()) {
            ++submitted;
        }
    }
    REQUIRE_OK(runtime.drain());
    const RuntimeStats cancelled = runtime.stats();
    CHECK_EQ(cancelled.cancelled, submitted);
    CHECK_EQ(cancelled.failed, std::uint64_t{0});

    CHECK_OK(runtime.shutdown(true));
    const Status after = runtime.submit(
        [](Observatory&, const CancellationToken&) -> Status { return Status::success(); }, "late");
    CHECK_ERR(after, ErrorCode::ShuttingDown);
    CHECK_EQ(runtime.stats().rejected_after_shutdown, std::uint64_t{1});
}

JITTER_TEST(concurrency, shutdown_discards_queued_work_and_counts_it) {
    auto harness = make_harness();
    REQUIRE_OK(harness);

    RuntimeConfig config;
    config.worker_threads = 1;
    config.queue_capacity = 64;
    ObservationRuntime runtime(*harness.value().observatory, config);
    REQUIRE_OK(runtime.start());

    for (int i = 0; i < 64; ++i) {
        static_cast<void>(runtime.submit(
            [](Observatory&, const CancellationToken&) -> Status { return Status::success(); }, "noop"));
    }
    const RuntimeStats before = runtime.stats();
    CHECK(before.submitted > 0);
    CHECK_OK(runtime.shutdown(true));
    const RuntimeStats after = runtime.stats();
    CHECK_EQ(after.cancelled + after.completed, before.submitted);
}

JITTER_TEST(concurrency, runtime_configuration_is_validated) {
    RuntimeConfig config;
    config.worker_threads = 0;
    CHECK_ERR(validate_runtime_config(config), ErrorCode::InvalidArgument);
    config.worker_threads = Limits::kMaxWorkerThreads + 1;
    CHECK_ERR(validate_runtime_config(config), ErrorCode::LimitExceeded);
    config.worker_threads = 2;
    config.queue_capacity = 0;
    CHECK_ERR(validate_runtime_config(config), ErrorCode::InvalidArgument);
    config.queue_capacity = Limits::kMaxQueueDepth + 1;
    CHECK_ERR(validate_runtime_config(config), ErrorCode::LimitExceeded);
}

JITTER_TEST(concurrency, concurrent_readers_and_writers_share_the_engine_safely) {
    LockAudit::reset();
    auto harness = make_harness(512, 16);
    REQUIRE_OK(harness);
    Observatory& observatory = *harness.value().observatory;
    const SummaryRequest request = [&]() {
        SummaryRequest built;
        built.series = harness.value().plan.series;
        built.path = harness.value().plan.path;
        built.generation = harness.value().plan.generation;
        built.generation_ordinal = harness.value().plan.ordinal;
        built.window = harness.value().config.window;
        built.policy = harness.value().config.policy;
        built.metrics = harness.value().config.metrics;
        built.now_utc_ns = harness.value().context.now_utc_ns;
        return built;
    }();

    // Every participant runs the same deterministic workload; the assertion is that no
    // thread observes a broken invariant and that the auditor sees no nested locking.
    // One writer submits batches in order through the runtime, which serialises engine
    // access; several readers query the engine at the same time. Every reader must see a
    // self consistent summary, and the auditor must see no nested locking.
    RuntimeConfig runtime_config;
    runtime_config.worker_threads = 3;
    runtime_config.queue_capacity = 64;
    ObservationRuntime runtime(observatory, runtime_config);
    REQUIRE_OK(runtime.start());

    std::atomic<std::uint64_t> failures{0};
    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (const LatencyBatch& batch : harness.value().batches) {
            Status submitted = runtime.submit(
                [batch, context = harness.value().context](Observatory& engine,
                                                           const CancellationToken& token) -> Status {
                    if (token.cancelled()) {
                        return Status::failure(ErrorCode::Cancelled, "cancelled");
                    }
                    auto outcome = engine.ingest(batch, context);
                    return outcome.ok() ? Status::success() : outcome.error();
                },
                "ingest");
            while (!submitted.ok() && submitted.code() == ErrorCode::Backpressure) {
                if (!runtime.drain().ok()) {
                    failures.fetch_add(1);
                    break;
                }
                submitted = runtime.submit(
                    [batch, context = harness.value().context](Observatory& engine,
                                                               const CancellationToken& token) -> Status {
                        if (token.cancelled()) {
                            return Status::failure(ErrorCode::Cancelled, "cancelled");
                        }
                        auto outcome = engine.ingest(batch, context);
                        return outcome.ok() ? Status::success() : outcome.error();
                    },
                    "ingest-retry");
            }
            if (!submitted.ok()) {
                failures.fetch_add(1);
            }
        }
        writer_done.store(true);
    });

    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&]() {
            while (!writer_done.load()) {
                auto summary = observatory.summarize(request, harness.value().context, false);
                if (!summary.ok()) {
                    // Before the first batch lands there is no window to summarise, which is
                    // an explicit miss rather than an inconsistency.
                    if (summary.code() != ErrorCode::NotFound) {
                        failures.fetch_add(1);
                    }
                    continue;
                }
                const EvidenceSummary& evidence = summary.value().summary.evidence;
                if (evidence.total > summary.value().summary.retained ||
                    evidence.fresh > evidence.total) {
                    failures.fetch_add(1);
                }
                if (makes_positive_assertion(summary.value().summary.classification.level) &&
                    (evidence.fresh == 0 || evidence.conflicting > 0)) {
                    failures.fetch_add(1);
                }
                auto document = observatory.export_summary(request, harness.value().context, false);
                if (!document.ok() && document.code() != ErrorCode::NotFound) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }
    REQUIRE_OK(runtime.drain());
    CHECK_OK(runtime.shutdown(false));
    CHECK_EQ(failures.load(), std::uint64_t{0});
    // Every observation is either stored or explicitly fenced by the sequence guard. A
    // multi worker runtime may deliver a source's batches out of order, and the engine
    // refuses the reordered ones instead of accepting them out of sequence.
    const EngineCounters counters = observatory.counters();
    const std::uint64_t accounted = counters.samples_stored + counters.batches_rejected * 16 +
                                    counters.samples_duplicate;
    CHECK(accounted >= harness.value().plan.samples);
    CHECK(counters.samples_stored > 0);

    const LockAuditSnapshot audit = LockAudit::snapshot();
    if (LockAudit::enabled()) {
        CHECK_EQ(audit.nested_acquisitions, std::uint64_t{0});
        CHECK_EQ(audit.reentrant_acquisitions, std::uint64_t{0});
        CHECK(audit.violation_free());
    }
}
