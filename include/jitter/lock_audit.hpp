// Jitter Observatory - lock order and reentrancy auditing.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <mutex>

#include <jitter/version.hpp>

#ifndef JITTER_LOCK_AUDIT
#define JITTER_LOCK_AUDIT 0
#endif

namespace jitter {

// Runtime lock audit. It answers two questions with evidence rather than by
// inspection: was any lock ever acquired while another audited lock was already held
// on the same thread (a nesting that can deadlock), and was any audited lock ever
// acquired recursively by its own holder (undefined behaviour for a non recursive
// mutex)?
struct LockAuditSnapshot {
    std::uint64_t acquisitions = 0;
    std::uint64_t contentions = 0;
    std::uint64_t nested_acquisitions = 0;
    std::uint64_t reentrant_acquisitions = 0;
    std::uint64_t max_hold_depth = 0;
    std::uint64_t uncontended_acquisitions = 0;

    bool violation_free() const noexcept {
        return nested_acquisitions == 0 && reentrant_acquisitions == 0;
    }
};

#if JITTER_LOCK_AUDIT

class LockAudit {
public:
    static void on_acquire(const void* mutex_id) noexcept;
    static void on_release(const void* mutex_id) noexcept;
    static void on_contended() noexcept;
    static void on_uncontended() noexcept;
    static LockAuditSnapshot snapshot() noexcept;
    static void reset() noexcept;
    static bool enabled() noexcept { return true; }
};

// Drop-in replacement for std::mutex that records lock order behaviour. In release
// builds without the audit it forwards to std::mutex with no extra work.
class AuditedMutex {
public:
    AuditedMutex() = default;
    AuditedMutex(const AuditedMutex&) = delete;
    AuditedMutex& operator=(const AuditedMutex&) = delete;

    void lock() {
        if (mutex_.try_lock()) {
            LockAudit::on_uncontended();
        } else {
            LockAudit::on_contended();
            mutex_.lock();
        }
        LockAudit::on_acquire(this);
    }

    void unlock() {
        LockAudit::on_release(this);
        mutex_.unlock();
    }

    bool try_lock() {
        if (!mutex_.try_lock()) {
            LockAudit::on_contended();
            return false;
        }
        LockAudit::on_uncontended();
        LockAudit::on_acquire(this);
        return true;
    }

private:
    std::mutex mutex_;
};

#else

class LockAudit {
public:
    static LockAuditSnapshot snapshot() noexcept { return {}; }
    static void reset() noexcept {}
    static bool enabled() noexcept { return false; }
};

class AuditedMutex {
public:
    AuditedMutex() = default;
    AuditedMutex(const AuditedMutex&) = delete;
    AuditedMutex& operator=(const AuditedMutex&) = delete;

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    bool try_lock() { return mutex_.try_lock(); }

private:
    std::mutex mutex_;
};

#endif

using Mutex = AuditedMutex;
using LockGuard = std::lock_guard<Mutex>;
using UniqueLock = std::unique_lock<Mutex>;

// Documented lock order for the whole runtime. No two of these are ever held at the
// same time by one thread; the order below exists so that a future change has a rule
// to violate and an auditor to catch it.
enum class LockRank : std::uint8_t {
    None = 0,
    Queue = 1,   // runtime work queue
    Engine = 2,  // observation engine state
    Store = 3,   // persistence writer
};

}  // namespace jitter
