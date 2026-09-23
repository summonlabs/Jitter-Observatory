// Jitter Observatory - policy driven instability classification.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/evidence.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/metrics.hpp>
#include <jitter/sample.hpp>

namespace jitter {

// What the runtime is willing to say. The first seven are refusals to make a positive
// statement; only Stable, Elevated and Unstable are assertions, and only Elevated and
// Unstable concern instability.
enum class InstabilityLevel : std::uint8_t {
    Unknown = 0,
    Unsupported = 1,
    Missing = 2,
    Insufficient = 3,
    Stale = 4,
    Expired = 5,
    Conflicting = 6,
    Stable = 7,
    Elevated = 8,
    Unstable = 9,
};

std::string_view to_string(InstabilityLevel level) noexcept;
bool parse_instability_level(std::string_view text, InstabilityLevel& out) noexcept;
bool asserts_instability(InstabilityLevel level) noexcept;
bool asserts_stability(InstabilityLevel level) noexcept;
bool makes_positive_assertion(InstabilityLevel level) noexcept;

enum class ThresholdDirection : std::uint8_t {
    AtLeast = 0,  // breach when value >= threshold
    AtMost = 1,   // breach when value <= threshold
};

std::string_view to_string(ThresholdDirection direction) noexcept;

struct ThresholdRule {
    MetricKey metric = MetricKey::AbsoluteDeltaMean;
    ThresholdDirection direction = ThresholdDirection::AtLeast;
    double elevated_at = 0.0;
    double unstable_at = 0.0;
    // Required. A threshold without a stated justification is refused so that no
    // number in the runtime is a bare assertion.
    std::string justification;
};

struct InstabilityPolicy {
    std::string name = "default";
    std::vector<ThresholdRule> rules;
    // Minimum number of fresh observations before any positive level may be asserted.
    std::uint64_t min_fresh_samples = 8;
    // Minimum number of terms the driving metric must have combined.
    std::uint64_t min_terms = 4;
    // When true, every rule must breach before the level is raised.
    bool require_all_rules = false;
    // Number of tolerated contradicted observations. Zero means any contradiction
    // blocks a positive assertion.
    std::uint64_t max_conflicts_tolerated = 0;
    // Consecutive non-instability classifications needed before an episode is closed
    // as recovered.
    std::uint32_t episode_close_windows = 2;
    FreshnessPolicy freshness;
};

// A documented default policy. Every threshold carries the justification it was
// declared with; nothing in the runtime invents a threshold.
InstabilityPolicy default_instability_policy();

InstabilityPolicyId make_instability_policy_id(const InstabilityPolicy& policy);
Status validate_instability_policy(const InstabilityPolicy& policy);

struct RuleEvaluation {
    MetricKey metric = MetricKey::AbsoluteDeltaMean;
    MetricId metric_id;
    std::string_view metric_name;
    ThresholdDirection direction = ThresholdDirection::AtLeast;
    double elevated_at = 0.0;
    double unstable_at = 0.0;
    double value = 0.0;
    bool value_present = false;
    bool breached_elevated = false;
    bool breached_unstable = false;
    std::uint64_t term_count = 0;
    std::string reason;
};

struct ClassificationInput {
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    InstabilityPolicy policy;
    WindowPolicyId window;
    EvidenceSummary evidence;
    MetricSet metrics;
    std::vector<std::string> metric_diagnostics;
    // False when the generation under test is no longer the open generation of the path.
    bool current_generation = true;
};

struct Classification {
    InstabilityLevel level = InstabilityLevel::Unknown;
    EvidenceState evidence = EvidenceState::Missing;
    bool has_driver = false;
    MetricKey driver_key = MetricKey::Count;
    MetricId driver_id;
    double driver_value = 0.0;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    InstabilityPolicyId policy;
    WindowPolicyId window;
    bool current_generation = true;
    // Consecutive non-instability classifications required before an episode closes as
    // recovered. Carried on the result so that episode tracking stays deterministic.
    std::uint32_t episode_close_windows = 2;
    std::uint64_t fresh_samples = 0;
    std::uint64_t terms = 0;
    std::vector<RuleEvaluation> rules;
    // Stable machine readable codes, sorted and de-duplicated.
    std::vector<std::string> reasons;

    bool asserts_current_instability() const noexcept {
        return current_generation && asserts_instability(level);
    }
};

// Deterministic classification. The same inputs always yield the same level, the same
// driver and the same reason codes.
Classification classify(const ClassificationInput& input);

}  // namespace jitter
