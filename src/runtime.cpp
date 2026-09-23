// Jitter Observatory - bounded worker runtime with real cancellation.
// Copyright 2026 Summon Software Labs.
#include <jitter/runtime.hpp>

#include <algorithm>

namespace jitter {

std::string_view to_string(BackpressurePolicy policy) noexcept {
    switch (policy) {
        case BackpressurePolicy::Reject: return "reject";
        case BackpressurePolicy::DropOldest: return "drop_oldest";
    }
    return "reject";
}

Status validate_runtime_config(const RuntimeConfig& config) {
    if (config.worker_threads == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "runtime requires at least one worker thread");
    }
    if (config.worker_threads > Limits::kMaxWorkerThreads) {
        return Status::failure(ErrorCode::LimitExceeded, "runtime worker count exceeds the supported bound",
                               std::to_string(config.worker_threads));
    }
    if (config.queue_capacity == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "runtime queue capacity must be non zero");
    }
    if (config.queue_capacity > Limits::kMaxQueueDepth) {
        return Status::failure(ErrorCode::LimitExceeded, "runtime queue capacity exceeds the supported bound",
                               std::to_string(config.queue_capacity));
    }
    return Status::success();
}

ObservationRuntime::ObservationRuntime(Observatory& observatory, RuntimeConfig config)
    : observatory_(observatory), config_(config) {}

ObservationRuntime::~ObservationRuntime() {
    if (running_) {
        static_cast<void>(shutdown(true));
    }
}

Status ObservationRuntime::start() {
    JITTER_TRY(validate_runtime_config(config_));
    LockGuard guard(mutex_);
    if (running_) {
        return Status::failure(ErrorCode::Conflict, "runtime is already running");
    }
    stopping_ = false;
    running_ = true;
    workers_.reserve(static_cast<std::size_t>(config_.worker_threads));
    for (std::uint64_t i = 0; i < config_.worker_threads; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
        ++stats_.workers_started;
    }
    return Status::success();
}

bool ObservationRuntime::running() const noexcept {
    LockGuard guard(mutex_);
    return running_;
}

Status ObservationRuntime::submit(Job job, std::string label) {
    if (!job) {
        return Status::failure(ErrorCode::InvalidArgument, "runtime refuses an empty job");
    }
    {
        LockGuard guard(mutex_);
        if (!running_ || stopping_) {
            ++stats_.rejected_after_shutdown;
            return Status::failure(ErrorCode::ShuttingDown, "runtime is not accepting work", label);
        }
        if (queue_.size() >= static_cast<std::size_t>(config_.queue_capacity)) {
            if (config_.backpressure == BackpressurePolicy::Reject) {
                ++stats_.rejected_backpressure;
                return Status::failure(ErrorCode::Backpressure, "runtime queue is full", label);
            }
            queue_.pop_front();
            ++stats_.dropped_oldest;
        }
        queue_.push_back(WorkItem{std::move(job), std::move(label)});
        ++stats_.submitted;
        stats_.peak_queue_depth = std::max<std::uint64_t>(stats_.peak_queue_depth, queue_.size());
    }
    work_available_.notify_one();
    return Status::success();
}

bool ObservationRuntime::pop(WorkItem& item) {
    LockGuard guard(mutex_);
    if (queue_.empty()) {
        return false;
    }
    item = std::move(queue_.front());
    queue_.pop_front();
    ++in_flight_;
    return true;
}

void ObservationRuntime::worker_loop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock<Mutex> lock(mutex_);
            work_available_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) {
                    idle_.notify_all();
                    return;
                }
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
            ++in_flight_;
        }

        Status result = Status::success();
        if (token_.cancelled()) {
            ++stats_.cancelled;
        } else {
            // The engine lock is acquired here, after the queue lock has been released,
            // so a worker never holds two runtime locks at the same time.
            result = item.job(observatory_, token_);
            if (!result.ok()) {
                LockGuard guard(mutex_);
                ++stats_.failed;
            }
        }

        {
            LockGuard guard(mutex_);
            --in_flight_;
            ++stats_.completed;
            if (queue_.empty() && in_flight_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

Status ObservationRuntime::drain() {
    std::unique_lock<Mutex> lock(mutex_);
    if (!running_) {
        return Status::failure(ErrorCode::NotReady, "runtime is not running");
    }
    idle_.wait(lock, [this]() { return queue_.empty() && in_flight_ == 0; });
    return Status::success();
}

void ObservationRuntime::cancel() { token_.cancel(); }

Status ObservationRuntime::shutdown(bool discard_pending) {
    {
        LockGuard guard(mutex_);
        if (!running_) {
            return Status::success();
        }
        stopping_ = true;
        if (discard_pending) {
            stats_.cancelled += queue_.size();
            queue_.clear();
        }
    }
    work_available_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    {
        LockGuard guard(mutex_);
        running_ = false;
    }
    return Status::success();
}

RuntimeStats ObservationRuntime::stats() const {
    LockGuard guard(mutex_);
    RuntimeStats snapshot = stats_;
    snapshot.queued_now = queue_.size();
    snapshot.in_flight = in_flight_;
    snapshot.lock_audit = LockAudit::snapshot();
    return snapshot;
}

}  // namespace jitter
