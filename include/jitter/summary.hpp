// Jitter Observatory - deterministic summaries.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <jitter/classification.hpp>
#include <jitter/error.hpp>
#include <jitter/evidence.hpp>
#include <jitter/id.hpp>
#include <jitter/metrics.hpp>
#include <jitter/window.hpp>

namespace jitter {

struct SummaryRequest {
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    WindowPolicy window;
    InstabilityPolicy policy;
    std::vector<MetricKey> metrics;
    std::int64_t now_utc_ns = 0;
    bool current_generation = true;
};

struct SummaryInput {
    SummaryRequest request;
    WindowSelection selection;
    // Exactly one assessment per selected observation, in the same order.
    std::vector<SampleAssessment> assessments;
    std::uint64_t retained = 0;
    std::uint64_t evicted_by_capacity = 0;
    std::uint64_t out_of_order_accepted = 0;
    std::uint64_t duplicate_ignored = 0;
};

struct SeriesSummary {
    SummaryId id;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    WindowPolicyId window;
    InstabilityPolicyId policy;
    FreshnessPolicyId freshness_policy;
    bool current_generation = true;

    std::uint64_t retained = 0;
    std::uint64_t selected = 0;
    std::uint64_t excluded_by_policy = 0;
    std::uint64_t evicted_by_capacity = 0;
    std::uint64_t out_of_order_accepted = 0;
    std::uint64_t duplicate_ignored = 0;
    std::uint64_t fresh_samples = 0;

    std::optional<std::int64_t> first_observation_ticks;
    std::optional<std::int64_t> last_observation_ticks;
    std::optional<std::int64_t> first_received_utc_ns;
    std::optional<std::int64_t> last_received_utc_ns;
    std::string selection_reason;

    EvidenceOrigin dominant_origin = EvidenceOrigin::Unknown;
    EvidenceSummary evidence;
    std::vector<MetricValue> metrics;
    std::vector<std::string> unavailable_metrics;
    Classification classification;
    std::vector<std::string> reasons;
    // Identity of the whole summary, including the operational counters that record how
    // the evidence arrived.
    Digest digest;
    // Identity of the measurement content alone: selection, evidence, metrics and
    // classification. Two runs over the same observations agree on this even when their
    // arrival orders differed.
    Digest measurement_digest;

    const MetricValue* find(MetricId metric_id) const noexcept;
    const MetricValue* find(MetricKey metric_key) const noexcept;
};

// Every number in the summary is computed from the fresh subset of the selection, in
// canonical order. Two calls with the same inputs produce byte identical results and
// the same identity.
Result<SeriesSummary> summarize(const SummaryInput& input);

}  // namespace jitter
