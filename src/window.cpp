// Jitter Observatory - bounded observation windows.
// Copyright 2026 Summon Software Labs.
#include <jitter/window.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>

namespace jitter {
namespace {

bool sample_less(const LatencySample& a, const LatencySample& b) noexcept {
    if (a.observed_at.ticks != b.observed_at.ticks) {
        return a.observed_at.ticks < b.observed_at.ticks;
    }
    if (a.provenance.sequence != b.provenance.sequence) {
        return a.provenance.sequence < b.provenance.sequence;
    }
    return a.id < b.id;
}

}  // namespace

std::string_view to_string(WindowKind kind) noexcept {
    switch (kind) {
        case WindowKind::Count: return "count";
        case WindowKind::Time: return "time";
        case WindowKind::Tumbling: return "tumbling";
    }
    return "count";
}

bool parse_window_kind(std::string_view value, WindowKind& out) noexcept {
    if (value == "count") { out = WindowKind::Count; return true; }
    if (value == "time") { out = WindowKind::Time; return true; }
    if (value == "tumbling") { out = WindowKind::Tumbling; return true; }
    return false;
}

WindowPolicyId make_window_policy_id(const WindowPolicy& policy) {
    DigestBuilder builder(kDomainWindowPolicy);
    builder.u8(static_cast<std::uint8_t>(policy.kind));
    builder.u64(policy.count);
    builder.i64(policy.duration_ns);
    builder.u64(policy.capacity);
    builder.i64(policy.anchor_ns);
    return builder.as_id<WindowPolicyTag>();
}

Status validate_window_policy(const WindowPolicy& policy) {
    if (policy.capacity == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "window capacity must be non zero");
    }
    if (policy.capacity > Limits::kMaxWindowSamples) {
        return Status::failure(ErrorCode::LimitExceeded, "window capacity exceeds the supported bound",
                               std::to_string(policy.capacity));
    }
    switch (policy.kind) {
        case WindowKind::Count:
            if (policy.count == 0) {
                return Status::failure(ErrorCode::InvalidArgument, "count window requires a non zero count");
            }
            if (policy.count > policy.capacity) {
                return Status::failure(ErrorCode::InvalidArgument,
                                       "count window size exceeds its capacity bound",
                                       std::to_string(policy.count) + "/" + std::to_string(policy.capacity));
            }
            break;
        case WindowKind::Time:
        case WindowKind::Tumbling:
            if (policy.duration_ns <= 0) {
                return Status::failure(ErrorCode::InvalidArgument,
                                       "time based window requires a positive duration");
            }
            if (static_cast<std::uint64_t>(policy.duration_ns) > Limits::kMaxWindowDurationNs) {
                return Status::failure(ErrorCode::LimitExceeded,
                                       "window duration exceeds the supported bound");
            }
            break;
    }
    return Status::success();
}

std::vector<std::int64_t> WindowSelection::latencies_ns() const {
    std::vector<std::int64_t> out;
    out.reserve(samples.size());
    for (const LatencySample* sample : samples) {
        out.push_back(sample->latency_ns);
    }
    return out;
}

SampleWindow::SampleWindow(WindowPolicy policy) : policy_(policy) {
    policy_id_ = make_window_policy_id(policy_);
}

void SampleWindow::clear() noexcept {
    samples_.clear();
    series_.reset();
    generation_.reset();
    generation_ordinal_.reset();
    has_newest_ = false;
    newest_ticks_ = 0;
}

Status SampleWindow::add(LatencySample sample) {
    if (!series_.has_value()) {
        series_ = sample.series;
        generation_ = sample.generation;
        generation_ordinal_ = sample.generation_ordinal;
    } else if (series_.value() != sample.series || generation_.value() != sample.generation) {
        ++rejected_mismatch_;
        return Status::failure(
            ErrorCode::Conflict,
            "window refuses observations from another series or generation",
            "series=" + series_.value().hex() + " generation=" + generation_.value().hex());
    }

    for (const LatencySample& existing : samples_) {
        if (existing.id == sample.id) {
            ++duplicate_ignored_;
            return Status::success();
        }
    }

    if (has_newest_ && sample.observed_at.ticks < newest_ticks_) {
        ++out_of_order_accepted_;
    } else {
        newest_ticks_ = sample.observed_at.ticks;
        has_newest_ = true;
    }

    samples_.push_back(std::move(sample));
    while (samples_.size() > static_cast<std::size_t>(policy_.capacity)) {
        samples_.pop_front();
        ++evicted_by_capacity_;
    }
    return Status::success();
}

WindowSelection SampleWindow::select() const {
    WindowSelection selection;
    selection.retained = static_cast<std::uint64_t>(samples_.size());
    if (samples_.empty()) {
        selection.selection_reason = "window_empty";
        return selection;
    }

    std::vector<const LatencySample*> ordered;
    ordered.reserve(samples_.size());
    for (const LatencySample& sample : samples_) {
        ordered.push_back(&sample);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const LatencySample* a, const LatencySample* b) { return sample_less(*a, *b); });

    switch (policy_.kind) {
        case WindowKind::Count: {
            const std::size_t wanted = static_cast<std::size_t>(policy_.count);
            if (ordered.size() > wanted) {
                selection.excluded_by_policy = static_cast<std::uint64_t>(ordered.size() - wanted);
                ordered.erase(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(ordered.size() - wanted));
            }
            selection.selection_reason = "most_recent_count";
            break;
        }
        case WindowKind::Time: {
            const std::int64_t newest = ordered.back()->observed_at.ticks;
            std::vector<const LatencySample*> kept;
            for (const LatencySample* sample : ordered) {
                // The comparison is written so that the subtraction is only performed
                // when it cannot overflow, whatever the source stamped into the reading.
                const std::int64_t ticks = sample->observed_at.ticks;
                if (newest >= ticks && (newest - ticks) <= policy_.duration_ns) {
                    kept.push_back(sample);
                }
            }
            selection.excluded_by_policy = static_cast<std::uint64_t>(ordered.size() - kept.size());
            ordered = std::move(kept);
            selection.selection_reason = "within_duration_of_newest_observation";
            break;
        }
        case WindowKind::Tumbling: {
            const std::int64_t newest = ordered.back()->observed_at.ticks;
            std::vector<const LatencySample*> kept;
            // A reading more than one bucket before the newest is outside the window by
            // construction, so the anchor arithmetic only runs on values that are close
            // enough to each other for the subtraction to be safe.
            const bool bucket_known = newest >= policy_.anchor_ns &&
                                      (newest - policy_.anchor_ns) <=
                                          static_cast<std::int64_t>(Limits::kMaxWindowDurationNs);
            const std::int64_t offset = bucket_known ? newest - policy_.anchor_ns : 0;
            const std::int64_t bucket = offset - (offset % policy_.duration_ns);
            for (const LatencySample* sample : ordered) {
                if (!bucket_known) {
                    // Fall back to a plain "within one duration of the newest" selection
                    // rather than performing arithmetic that could overflow.
                    const std::int64_t ticks = sample->observed_at.ticks;
                    if (newest >= ticks && (newest - ticks) <= policy_.duration_ns) {
                        kept.push_back(sample);
                    }
                    continue;
                }
                const std::int64_t ticks = sample->observed_at.ticks;
                const bool in_range = ticks >= policy_.anchor_ns &&
                                      (ticks - policy_.anchor_ns) <=
                                          static_cast<std::int64_t>(Limits::kMaxWindowDurationNs);
                if (!in_range) {
                    continue;
                }
                const std::int64_t sample_offset = ticks - policy_.anchor_ns;
                const std::int64_t sample_bucket = sample_offset - (sample_offset % policy_.duration_ns);
                if (sample_bucket == bucket) {
                    kept.push_back(sample);
                }
            }
            selection.excluded_by_policy = static_cast<std::uint64_t>(ordered.size() - kept.size());
            ordered = std::move(kept);
            selection.selection_reason = "same_tumbling_bucket_as_newest";
            break;
        }
    }

    selection.samples = ordered;
    return selection;
}

std::vector<LatencySample> SampleWindow::snapshot() const {
    std::vector<LatencySample> copy(samples_.begin(), samples_.end());
    std::sort(copy.begin(), copy.end(), sample_less);
    return copy;
}

std::optional<std::int64_t> SampleWindow::oldest_observation_ticks() const noexcept {
    if (samples_.empty()) {
        return std::nullopt;
    }
    std::int64_t oldest = samples_.front().observed_at.ticks;
    for (const LatencySample& sample : samples_) {
        oldest = std::min(oldest, sample.observed_at.ticks);
    }
    return oldest;
}

std::optional<std::int64_t> SampleWindow::newest_observation_ticks() const noexcept {
    if (!has_newest_) {
        return std::nullopt;
    }
    return newest_ticks_;
}

}  // namespace jitter
