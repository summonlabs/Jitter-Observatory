// Jitter Observatory - bounded observation windows.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/sample.hpp>

namespace jitter {

enum class WindowKind : std::uint8_t {
    // The most recent N observations.
    Count = 0,
    // Observations within a duration of the newest observation in the window.
    Time = 1,
    // Observations in the same fixed bucket, anchored at anchor_ns, as the newest.
    Tumbling = 2,
};

std::string_view to_string(WindowKind kind) noexcept;
bool parse_window_kind(std::string_view text, WindowKind& out) noexcept;

// A window policy fully determines which retained observations belong to the window.
// It contains no wall-clock dependence: selection is always relative to the newest
// observation actually present.
struct WindowPolicy {
    WindowKind kind = WindowKind::Count;
    std::uint64_t count = 256;
    std::int64_t duration_ns = 1000000000ll;
    // Hard storage bound. The window never retains more than this many observations.
    std::uint64_t capacity = 512;
    std::int64_t anchor_ns = 0;
};

WindowPolicyId make_window_policy_id(const WindowPolicy& policy);
Status validate_window_policy(const WindowPolicy& policy);

// Canonically ordered selection of retained observations.
struct WindowSelection {
    std::vector<const LatencySample*> samples;
    std::uint64_t retained = 0;
    std::uint64_t excluded_by_policy = 0;
    std::string selection_reason;

    std::vector<std::int64_t> latencies_ns() const;
};

// Bounded ring of observations for exactly one (series, generation) pair. Mixing
// series or generations in one window is refused, which is what keeps route
// generation changes from blending into a single history.
class SampleWindow {
public:
    explicit SampleWindow(WindowPolicy policy = {});

    const WindowPolicy& policy() const noexcept { return policy_; }
    WindowPolicyId policy_id() const noexcept { return policy_id_; }

    // Adds an observation. Observational out-of-order arrival is accepted and counted;
    // it never reorders or corrupts the window contents.
    Status add(LatencySample sample);

    std::size_t retained() const noexcept { return samples_.size(); }
    bool empty() const noexcept { return samples_.empty(); }
    void clear() noexcept;

    std::uint64_t evicted_by_capacity() const noexcept { return evicted_by_capacity_; }
    std::uint64_t out_of_order_accepted() const noexcept { return out_of_order_accepted_; }
    std::uint64_t duplicate_ignored() const noexcept { return duplicate_ignored_; }
    std::uint64_t rejected_mismatch() const noexcept { return rejected_mismatch_; }

    std::optional<SeriesId> series() const noexcept { return series_; }
    std::optional<GenerationId> generation() const noexcept { return generation_; }
    std::optional<Ordinal> generation_ordinal() const noexcept { return generation_ordinal_; }

    // Canonically ordered by (observation time, source sequence, identity).
    WindowSelection select() const;
    std::vector<LatencySample> snapshot() const;

    // Oldest and newest observation times present, in observation clock ticks.
    std::optional<std::int64_t> oldest_observation_ticks() const noexcept;
    std::optional<std::int64_t> newest_observation_ticks() const noexcept;

private:
    WindowPolicy policy_;
    WindowPolicyId policy_id_;
    // A deque keeps eviction of the oldest observation constant time and keeps pointers
    // to retained observations stable, which the selection relies on.
    std::deque<LatencySample> samples_;
    std::optional<SeriesId> series_;
    std::optional<GenerationId> generation_;
    std::optional<Ordinal> generation_ordinal_;
    std::uint64_t evicted_by_capacity_ = 0;
    std::uint64_t out_of_order_accepted_ = 0;
    std::uint64_t duplicate_ignored_ = 0;
    std::uint64_t rejected_mismatch_ = 0;
    std::int64_t newest_ticks_ = 0;
    bool has_newest_ = false;
};

}  // namespace jitter
