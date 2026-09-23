// Jitter Observatory - bounded worker runtime with real cancellation.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/error.hpp>
#include <jitter/limits.hpp>
#include <jitter/lock_audit.hpp>

namespace jitter {

// Cooperative cancellation. A cancelled token never becomes uncancelled.
class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const noexcept { flag_->store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

enum class BackpressurePolicy : std::uint8_t {
    // Refuse new work when the queue is full. Nothing is lost silently.
    Reject = 0,
    // Discard the oldest queued work when the queue is full, and count it.
    DropOldest = 1,
};

std::string_view to_string(BackpressurePolicy policy) noexcept;

struct RuntimeConfig {
    std::uint64_t worker_threads = 4;
    std::uint64_t queue_capacity = 1024;
    BackpressurePolicy backpressure = BackpressurePolicy::Reject;
};

Status validate_runtime_config(const RuntimeConfig& config);

struct RuntimeStats {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t rejected_backpressure = 0;
    std::uint64_t dropped_oldest = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t rejected_after_shutdown = 0;
    std::uint64_t peak_queue_depth = 0;
    std::uint64_t queued_now = 0;
    std::uint64_t in_flight = 0;
    std::uint64_t workers_started = 0;
    LockAuditSnapshot lock_audit;
};

// Bounded, deterministic worker runtime. Jobs run under a single engine lock that is
// acquired only after the queue lock has been released, so no thread ever holds two
// runtime locks and the auditor can prove it.
class ObservationRuntime {
public:
    using Job = std::function<Status(Observatory&, const CancellationToken&)>;

    explicit ObservationRuntime(Observatory& observatory, RuntimeConfig config = {});
    ~ObservationRuntime();

    ObservationRuntime(const ObservationRuntime&) = delete;
    ObservationRuntime& operator=(const ObservationRuntime&) = delete;

    Status start();
    bool running() const noexcept;

    // Queues work. Returns Backpressure when the queue is full under the Reject policy,
    // and ShuttingDown once the runtime has been stopped.
    Status submit(Job job, std::string label);

    // Blocks until the queue is empty and every in-flight job has finished. This is the
    // deterministic completion primitive: it never depends on a timer.
    Status drain();

    // Requests cooperative cancellation. Queued work that has not started is discarded
    // and counted; running work observes the token.
    void cancel();

    // Stops accepting work, drains or cancels, and joins every worker.
    Status shutdown(bool discard_pending);

    RuntimeStats stats() const;
    const RuntimeConfig& config() const noexcept { return config_; }

private:
    struct WorkItem {
        Job job;
        std::string label;
    };

    void worker_loop();
    bool pop(WorkItem& item);

    Observatory& observatory_;
    RuntimeConfig config_;
    std::vector<std::thread> workers_;
    mutable Mutex mutex_;
    // condition_variable_any is required because the audited mutex is not std::mutex.
    std::condition_variable_any work_available_;
    std::condition_variable_any idle_;
    std::deque<WorkItem> queue_;
    CancellationToken token_;
    bool running_ = false;
    bool stopping_ = false;
    std::uint64_t in_flight_ = 0;
    RuntimeStats stats_;
};

}  // namespace jitter
