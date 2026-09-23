// Jitter Observatory - lock order and reentrancy auditing.
// Copyright 2026 Summon Software Labs.
#include <jitter/lock_audit.hpp>

#if JITTER_LOCK_AUDIT

#include <atomic>
#include <vector>

namespace jitter {
namespace {

constexpr std::size_t kMaxTrackedDepth = 32;

struct AuditState {
    std::atomic<std::uint64_t> acquisitions{0};
    std::atomic<std::uint64_t> contentions{0};
    std::atomic<std::uint64_t> nested{0};
    std::atomic<std::uint64_t> reentrant{0};
    std::atomic<std::uint64_t> max_depth{0};
    std::atomic<std::uint64_t> uncontended{0};
};

AuditState& state() {
    static AuditState instance;
    return instance;
}

struct ThreadStack {
    const void* held[kMaxTrackedDepth]{};
    std::size_t depth = 0;
};

ThreadStack& thread_stack() {
    static thread_local ThreadStack stack;
    return stack;
}

}  // namespace

void LockAudit::on_acquire(const void* mutex_id) noexcept {
    ThreadStack& stack = thread_stack();
    for (std::size_t i = 0; i < stack.depth; ++i) {
        if (stack.held[i] == mutex_id) {
            state().reentrant.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (stack.depth > 0) {
        state().nested.fetch_add(1, std::memory_order_relaxed);
    }
    if (stack.depth < kMaxTrackedDepth) {
        stack.held[stack.depth] = mutex_id;
        ++stack.depth;
    }
    const std::uint64_t depth = stack.depth;
    std::uint64_t observed = state().max_depth.load(std::memory_order_relaxed);
    while (depth > observed &&
           !state().max_depth.compare_exchange_weak(observed, depth, std::memory_order_relaxed)) {
    }
    state().acquisitions.fetch_add(1, std::memory_order_relaxed);
}

void LockAudit::on_release(const void* mutex_id) noexcept {
    ThreadStack& stack = thread_stack();
    for (std::size_t i = stack.depth; i > 0; --i) {
        if (stack.held[i - 1] == mutex_id) {
            for (std::size_t j = i - 1; j + 1 < stack.depth; ++j) {
                stack.held[j] = stack.held[j + 1];
            }
            --stack.depth;
            stack.held[stack.depth] = nullptr;
            return;
        }
    }
}

void LockAudit::on_contended() noexcept { state().contentions.fetch_add(1, std::memory_order_relaxed); }

void LockAudit::on_uncontended() noexcept {
    state().uncontended.fetch_add(1, std::memory_order_relaxed);
}

LockAuditSnapshot LockAudit::snapshot() noexcept {
    LockAuditSnapshot snapshot;
    snapshot.acquisitions = state().acquisitions.load(std::memory_order_relaxed);
    snapshot.contentions = state().contentions.load(std::memory_order_relaxed);
    snapshot.nested_acquisitions = state().nested.load(std::memory_order_relaxed);
    snapshot.reentrant_acquisitions = state().reentrant.load(std::memory_order_relaxed);
    snapshot.max_hold_depth = state().max_depth.load(std::memory_order_relaxed);
    snapshot.uncontended_acquisitions = state().uncontended.load(std::memory_order_relaxed);
    return snapshot;
}

void LockAudit::reset() noexcept {
    state().acquisitions.store(0, std::memory_order_relaxed);
    state().contentions.store(0, std::memory_order_relaxed);
    state().nested.store(0, std::memory_order_relaxed);
    state().reentrant.store(0, std::memory_order_relaxed);
    state().max_depth.store(0, std::memory_order_relaxed);
    state().uncontended.store(0, std::memory_order_relaxed);
}

}  // namespace jitter

#endif
