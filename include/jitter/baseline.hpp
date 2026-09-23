// Jitter Observatory - frozen baselines and deterministic comparison.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/evidence.hpp>
#include <jitter/id.hpp>
#include <jitter/metrics.hpp>
#include <jitter/provenance.hpp>
#include <jitter/sample.hpp>

namespace jitter {

// A frozen measurement of one (series, generation) under one window policy. A baseline
// records the exact metric identities it measured, so it can never be compared against
// a different definition by accident.
struct Baseline {
    BaselineId id;
    std::string name;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    WindowPolicyId window;
    InstabilityPolicyId policy;
    std::uint64_t sample_count = 0;
    std::uint64_t fresh_sample_count = 0;
    std::optional<std::int64_t> first_observation_utc_ns;
    std::optional<std::int64_t> last_observation_utc_ns;
    EvidenceState evidence = EvidenceState::Missing;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    std::int64_t created_at_utc_ns = 0;
    std::vector<MetricValue> metrics;
    Digest digest;
    std::string note;

    const MetricValue* find(MetricId metric_id) const noexcept;
    const MetricValue* find(MetricKey metric_key) const noexcept;
    std::optional<std::int64_t> span_ns() const noexcept;
};

Result<Baseline> make_baseline(Baseline baseline);

// The current state a baseline is compared against.
struct CurrentSnapshot {
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    WindowPolicyId window;
    InstabilityPolicyId policy;
    std::uint64_t sample_count = 0;
    EvidenceSummary evidence;
    MetricSet metrics;
};

enum class ComparisonVerdict : std::uint8_t {
    Comparable = 0,
    Incomparable = 1,
    Missing = 2,
    Insufficient = 3,
    Stale = 4,
    Expired = 5,
    Conflicting = 6,
    Unsupported = 7,
};

std::string_view to_string(ComparisonVerdict verdict) noexcept;

struct MetricComparison {
    MetricId metric;
    MetricKey key = MetricKey::Count;
    std::string_view name;
    MetricUnit unit = MetricUnit::Nanoseconds;
    std::optional<double> baseline_value;
    std::optional<double> current_value;
    std::optional<double> delta;
    std::optional<double> ratio;
    std::optional<double> relative_change;
    std::uint64_t baseline_terms = 0;
    std::uint64_t current_terms = 0;
    ComparisonVerdict verdict = ComparisonVerdict::Comparable;
    std::string reason;
};

struct BaselineComparisonReport {
    BaselineId baseline;
    std::string baseline_name;
    SeriesId series;
    PathId path;
    GenerationId baseline_generation;
    GenerationId current_generation;
    Ordinal baseline_generation_ordinal;
    Ordinal current_generation_ordinal;
    bool generation_changed = false;
    bool window_changed = false;
    bool policy_changed = false;
    EvidenceState current_evidence = EvidenceState::Missing;
    ComparisonVerdict overall = ComparisonVerdict::Incomparable;
    std::vector<MetricComparison> comparisons;
    std::vector<std::string> reasons;
    Digest digest;
};

// Comparison refuses to produce a delta across a route generation change, across a
// window policy change, or from evidence that cannot support a positive statement.
BaselineComparisonReport compare_to_baseline(const Baseline& baseline, const CurrentSnapshot& current);

}  // namespace jitter
