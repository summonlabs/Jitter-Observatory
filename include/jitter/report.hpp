// Jitter Observatory - history reports and deterministic explanations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <jitter/baseline.hpp>
#include <jitter/classification.hpp>
#include <jitter/episode.hpp>
#include <jitter/error.hpp>
#include <jitter/evidence.hpp>
#include <jitter/path.hpp>
#include <jitter/summary.hpp>

namespace jitter {

// One route generation of one path, with the history that belongs to it. History is
// never aggregated across segments.
struct GenerationSegment {
    GenerationId generation;
    Ordinal ordinal;
    Revision revision;
    Digest topology;
    std::int64_t opened_at_utc_ns = 0;
    std::optional<std::int64_t> closed_at_utc_ns;
    std::uint32_t hop_count = 0;
    std::string cause;
    std::string close_reason;
    bool current = false;
    std::uint64_t episode_count = 0;
    std::uint64_t open_episode_count = 0;
    std::uint64_t retained_observations = 0;
    std::vector<std::string> reasons;
};

struct HistoryReport {
    SeriesId series;
    PathId path;
    std::vector<GenerationSegment> segments;
    std::uint64_t generations_retained = 0;
    std::uint64_t generations_dropped = 0;
    std::vector<Episode> episodes;
    std::uint64_t episodes_dropped = 0;
    std::vector<ConflictRecord> conflicts;
    std::uint64_t conflicts_dropped = 0;
    bool rows_truncated = false;
    std::vector<std::string> reasons;
    Digest digest;
};

struct HistoryRequest {
    SeriesId series;
    PathId path;
    std::uint64_t max_rows = 1000;
    bool include_conflicts = true;
};

struct HistoryInput {
    HistoryRequest request;
    std::vector<PathGeneration> generations;
    std::uint64_t generations_dropped = 0;
    std::vector<Episode> episodes;
    std::uint64_t episodes_dropped = 0;
    std::vector<ConflictRecord> conflicts;
    std::uint64_t conflicts_dropped = 0;
    GenerationId current_generation;
    std::vector<std::pair<GenerationId, std::uint64_t>> retained_observations;
};

Result<HistoryReport> build_history_report(const HistoryInput& input);

struct ExplanationLine {
    std::string code;
    std::string detail;
};

// A deterministic, machine readable account of why the runtime said what it said.
// Lines are sorted by (code, detail) and the whole document is content addressed.
struct Explanation {
    std::string subject;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    InstabilityLevel level = InstabilityLevel::Unknown;
    EvidenceState evidence = EvidenceState::Missing;
    InstabilityPolicyId policy;
    WindowPolicyId window;
    FreshnessPolicyId freshness;
    bool current_generation = true;
    std::vector<ExplanationLine> lines;
    std::vector<std::string> reasons;
    Digest digest;
};

Explanation explain_summary(const SeriesSummary& summary, const WindowPolicy& window_policy,
                            const InstabilityPolicy& instability_policy);

}  // namespace jitter
