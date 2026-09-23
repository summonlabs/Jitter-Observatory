// Jitter Observatory - historical instability episodes.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/classification.hpp>
#include <jitter/error.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>

namespace jitter {

enum class EpisodeCloseReason : std::uint8_t {
    None = 0,
    Recovered = 1,          // the level returned below the elevation threshold
    GenerationChanged = 2,  // the route changed; history is segmented, not extended
    EvidenceLost = 3,       // the evidence needed to confirm the episode disappeared
    PolicyChanged = 4,
    WindowChanged = 5,
    Superseded = 6,
};

std::string_view to_string(EpisodeCloseReason reason) noexcept;

struct Episode {
    EpisodeId id;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    InstabilityPolicyId policy;
    WindowPolicyId window;
    InstabilityLevel peak_level = InstabilityLevel::Unknown;
    MetricKey driver_key = MetricKey::Count;
    MetricId driver_id;
    double opening_value = 0.0;
    double peak_value = 0.0;
    std::int64_t opened_at_utc_ns = 0;
    std::optional<std::int64_t> closed_at_utc_ns;
    std::uint64_t observations = 0;
    std::uint64_t breach_observations = 0;
    std::uint64_t recovery_observations = 0;
    std::uint32_t close_windows_required = 2;
    EpisodeCloseReason close_reason = EpisodeCloseReason::None;
    std::string close_detail;
    std::vector<std::string> reasons;

    bool is_open() const noexcept { return !closed_at_utc_ns.has_value(); }
    std::optional<std::int64_t> duration_ns() const noexcept {
        if (!closed_at_utc_ns.has_value()) {
            return std::nullopt;
        }
        return closed_at_utc_ns.value() - opened_at_utc_ns;
    }
};

EpisodeId make_episode_id(const Episode& episode);

struct EpisodeKey {
    SeriesId series;
    GenerationId generation;
    InstabilityPolicyId policy;
    WindowPolicyId window;

    friend bool operator<(const EpisodeKey& a, const EpisodeKey& b) noexcept {
        if (a.series != b.series) return a.series < b.series;
        if (a.generation != b.generation) return a.generation < b.generation;
        if (a.policy != b.policy) return a.policy < b.policy;
        return a.window < b.window;
    }
};

struct EpisodeUpdate {
    std::optional<Episode> opened;
    std::optional<Episode> closed;
    bool extended = false;
};

// Bounded episode store. It never rewrites an episode retroactively: a closed episode
// is final, and a generation change always closes rather than extends.
class EpisodeLog {
public:
    EpisodeUpdate observe(const Classification& classification, std::int64_t now_utc_ns);
    // Closes every open episode of the series whose generation, policy or window is no
    // longer the one in force. A route generation change always closes rather than
    // extends, which is what segments history per generation.
    std::vector<Episode> retire(SeriesId series, GenerationId current_generation,
                                InstabilityPolicyId current_policy, WindowPolicyId current_window,
                                std::int64_t now_utc_ns);
    // Imports a persisted episode. An episode that was open when it was persisted can
    // no longer be confirmed as ongoing, so it is closed as evidence lost and that
    // decision is recorded on the episode itself.
    Status import_episode(Episode episode, std::int64_t now_utc_ns);

    std::vector<Episode> episodes(SeriesId series) const;
    std::vector<Episode> open_episodes() const;
    std::vector<Episode> all_episodes() const;
    std::size_t size() const noexcept { return episodes_.size(); }
    std::uint64_t dropped() const noexcept { return dropped_; }
    void clear() noexcept;

private:
    void close_episode(Episode& episode, std::int64_t now_utc_ns, EpisodeCloseReason reason,
                       std::string_view detail);
    void insert_closed(const Episode& episode);

    std::map<EpisodeKey, Episode> open_;
    std::map<SeriesId, std::vector<Episode>> episodes_;
    std::uint64_t dropped_ = 0;
};

}  // namespace jitter
