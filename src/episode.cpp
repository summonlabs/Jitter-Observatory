// Jitter Observatory - historical instability episodes.
// Copyright 2026 Summon Software Labs.
#include <jitter/episode.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>

namespace jitter {
namespace {

void add_reason(std::vector<std::string>& reasons, const std::string& reason) {
    if (reason.empty()) {
        return;
    }
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(reason);
    }
}

}  // namespace

std::string_view to_string(EpisodeCloseReason reason) noexcept {
    switch (reason) {
        case EpisodeCloseReason::None: return "none";
        case EpisodeCloseReason::Recovered: return "recovered";
        case EpisodeCloseReason::GenerationChanged: return "generation_changed";
        case EpisodeCloseReason::EvidenceLost: return "evidence_lost";
        case EpisodeCloseReason::PolicyChanged: return "policy_changed";
        case EpisodeCloseReason::WindowChanged: return "window_changed";
        case EpisodeCloseReason::Superseded: return "superseded";
    }
    return "none";
}

EpisodeId make_episode_id(const Episode& episode) {
    DigestBuilder builder(kDomainEpisode);
    builder.id(episode.series);
    builder.id(episode.path);
    builder.id(episode.generation);
    builder.u64(episode.generation_ordinal.value());
    builder.i64(episode.opened_at_utc_ns);
    builder.id(episode.policy);
    builder.id(episode.window);
    builder.id(episode.driver_id);
    builder.u8(static_cast<std::uint8_t>(episode.driver_key));
    return builder.as_id<EpisodeTag>();
}

void EpisodeLog::close_episode(Episode& episode, std::int64_t now_utc_ns, EpisodeCloseReason reason,
                               std::string_view detail) {
    episode.closed_at_utc_ns = now_utc_ns < episode.opened_at_utc_ns ? episode.opened_at_utc_ns
                                                                    : now_utc_ns;
    episode.close_reason = reason;
    episode.close_detail = std::string(detail);
    add_reason(episode.reasons, std::string("closed:") + std::string(to_string(reason)));
}

void EpisodeLog::insert_closed(const Episode& episode) {
    std::vector<Episode>& bucket = episodes_[episode.series];
    bucket.push_back(episode);
    std::sort(bucket.begin(), bucket.end(), [](const Episode& a, const Episode& b) {
        if (a.opened_at_utc_ns != b.opened_at_utc_ns) {
            return a.opened_at_utc_ns < b.opened_at_utc_ns;
        }
        return a.id < b.id;
    });
    while (bucket.size() > Limits::kMaxEpisodesPerSeries) {
        // Drop the oldest closed episode and count it; open episodes are never dropped.
        bucket.erase(bucket.begin());
        ++dropped_;
    }
}

EpisodeUpdate EpisodeLog::observe(const Classification& classification, std::int64_t now_utc_ns) {
    EpisodeUpdate update;
    const EpisodeKey key{classification.series, classification.generation, classification.policy,
                         classification.window};

    if (!classification.current_generation) {
        // An observation of a closed generation can never open or extend a current
        // episode. Any open episode for that key is closed as superseded.
        const auto it = open_.find(key);
        if (it != open_.end()) {
            Episode closed = it->second;
            close_episode(closed, now_utc_ns, EpisodeCloseReason::Superseded,
                          "classification_targets_a_closed_generation");
            insert_closed(closed);
            open_.erase(it);
            update.closed = closed;
        }
        return update;
    }

    auto it = open_.find(key);

    if (asserts_instability(classification.level)) {
        if (it == open_.end()) {
            Episode episode;
            episode.series = classification.series;
            episode.path = classification.path;
            episode.generation = classification.generation;
            episode.generation_ordinal = classification.generation_ordinal;
            episode.policy = classification.policy;
            episode.window = classification.window;
            episode.peak_level = classification.level;
            episode.driver_key = classification.driver_key;
            episode.driver_id = classification.driver_id;
            episode.opening_value = classification.driver_value;
            episode.peak_value = classification.driver_value;
            episode.opened_at_utc_ns = now_utc_ns;
            episode.observations = 1;
            episode.breach_observations = 1;
            episode.close_windows_required = 0;
            episode.reasons = classification.reasons;
            add_reason(episode.reasons, "opened:instability_asserted");
            episode.id = make_episode_id(episode);
            open_.emplace(key, episode);
            update.opened = episode;
            return update;
        }

        Episode& episode = it->second;
        ++episode.observations;
        ++episode.breach_observations;
        episode.recovery_observations = 0;
        if (classification.driver_value > episode.peak_value) {
            episode.peak_value = classification.driver_value;
            episode.driver_key = classification.driver_key;
            episode.driver_id = classification.driver_id;
        }
        if (classification.level == InstabilityLevel::Unstable) {
            episode.peak_level = InstabilityLevel::Unstable;
        }
        for (const std::string& reason : classification.reasons) {
            add_reason(episode.reasons, reason);
        }
        update.extended = true;
        return update;
    }

    if (it == open_.end()) {
        return update;
    }

    const std::uint32_t required = classification.episode_close_windows;
    Episode& episode = it->second;
    episode.close_windows_required = required;
    ++episode.observations;

    const bool evidence_lost = classification.level == InstabilityLevel::Missing ||
                               classification.level == InstabilityLevel::Stale ||
                               classification.level == InstabilityLevel::Expired ||
                               classification.level == InstabilityLevel::Conflicting ||
                               classification.level == InstabilityLevel::Unknown ||
                               classification.level == InstabilityLevel::Unsupported;
    if (evidence_lost) {
        // The runtime cannot confirm the episode is still happening. That is recorded
        // as evidence loss, not as recovery.
        Episode closed = episode;
        close_episode(closed, now_utc_ns, EpisodeCloseReason::EvidenceLost,
                      std::string("level:") + std::string(to_string(classification.level)));
        insert_closed(closed);
        open_.erase(it);
        update.closed = closed;
        return update;
    }

    ++episode.recovery_observations;
    for (const std::string& reason : classification.reasons) {
        add_reason(episode.reasons, reason);
    }
    if (episode.recovery_observations >= required) {
        Episode closed = episode;
        close_episode(closed, now_utc_ns, EpisodeCloseReason::Recovered, "level_returned_below_thresholds");
        insert_closed(closed);
        open_.erase(it);
        update.closed = closed;
        return update;
    }
    update.extended = true;
    return update;
}

std::vector<Episode> EpisodeLog::retire(SeriesId series, GenerationId current_generation,
                                        InstabilityPolicyId current_policy,
                                        WindowPolicyId current_window, std::int64_t now_utc_ns) {
    std::vector<Episode> closed_now;
    for (auto it = open_.begin(); it != open_.end();) {
        if (it->first.series != series) {
            ++it;
            continue;
        }
        if (it->first.generation != current_generation) {
            Episode closed = it->second;
            close_episode(closed, now_utc_ns, EpisodeCloseReason::GenerationChanged,
                          "route_generation_changed");
            insert_closed(closed);
            closed_now.push_back(closed);
            it = open_.erase(it);
            continue;
        }
        if (it->first.policy != current_policy) {
            Episode closed = it->second;
            close_episode(closed, now_utc_ns, EpisodeCloseReason::PolicyChanged,
                          "instability_policy_changed");
            insert_closed(closed);
            closed_now.push_back(closed);
            it = open_.erase(it);
            continue;
        }
        if (it->first.window != current_window) {
            Episode closed = it->second;
            close_episode(closed, now_utc_ns, EpisodeCloseReason::WindowChanged,
                          "window_policy_changed");
            insert_closed(closed);
            closed_now.push_back(closed);
            it = open_.erase(it);
            continue;
        }
        ++it;
    }
    return closed_now;
}

Status EpisodeLog::import_episode(Episode episode, std::int64_t now_utc_ns) {
    if (episode.series.is_nil() || episode.generation.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "imported episode requires series and generation identities");
    }
    if (episode.is_open()) {
        close_episode(episode, now_utc_ns, EpisodeCloseReason::EvidenceLost,
                      "restored_open_episode_cannot_be_confirmed_after_restart");
    }
    insert_closed(episode);
    return Status::success();
}

std::vector<Episode> EpisodeLog::episodes(SeriesId series) const {
    const auto it = episodes_.find(series);
    if (it == episodes_.end()) {
        return {};
    }
    return it->second;
}

std::vector<Episode> EpisodeLog::open_episodes() const {
    std::vector<Episode> out;
    out.reserve(open_.size());
    for (const auto& entry : open_) {
        out.push_back(entry.second);
    }
    return out;
}

std::vector<Episode> EpisodeLog::all_episodes() const {
    std::vector<Episode> out;
    for (const auto& entry : episodes_) {
        out.insert(out.end(), entry.second.begin(), entry.second.end());
    }
    for (const auto& entry : open_) {
        out.push_back(entry.second);
    }
    std::sort(out.begin(), out.end(), [](const Episode& a, const Episode& b) {
        if (a.opened_at_utc_ns != b.opened_at_utc_ns) {
            return a.opened_at_utc_ns < b.opened_at_utc_ns;
        }
        return a.id < b.id;
    });
    return out;
}

void EpisodeLog::clear() noexcept {
    open_.clear();
    episodes_.clear();
    dropped_ = 0;
}

}  // namespace jitter
