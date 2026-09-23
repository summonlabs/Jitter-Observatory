// Jitter Observatory - policy driven instability classification.
// Copyright 2026 Summon Software Labs.
#include <jitter/classification.hpp>

#include <algorithm>
#include <cmath>

#include <jitter/bytes.hpp>

namespace jitter {
namespace {

constexpr std::uint64_t kMaxPolicyNameBytes = 64;
constexpr std::uint64_t kMaxJustificationBytes = 256;

void add_reason(std::vector<std::string>& reasons, const std::string& reason) {
    if (reason.empty()) {
        return;
    }
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(reason);
    }
}

void finalize_reasons(std::vector<std::string>& reasons) {
    std::sort(reasons.begin(), reasons.end());
    reasons.erase(std::unique(reasons.begin(), reasons.end()), reasons.end());
}

bool breaches(double value, double threshold, ThresholdDirection direction) noexcept {
    if (direction == ThresholdDirection::AtLeast) {
        return value >= threshold;
    }
    return value <= threshold;
}

}  // namespace

std::string_view to_string(InstabilityLevel level) noexcept {
    switch (level) {
        case InstabilityLevel::Unknown: return "unknown";
        case InstabilityLevel::Unsupported: return "unsupported";
        case InstabilityLevel::Missing: return "missing";
        case InstabilityLevel::Insufficient: return "insufficient";
        case InstabilityLevel::Stale: return "stale";
        case InstabilityLevel::Expired: return "expired";
        case InstabilityLevel::Conflicting: return "conflicting";
        case InstabilityLevel::Stable: return "stable";
        case InstabilityLevel::Elevated: return "elevated";
        case InstabilityLevel::Unstable: return "unstable";
    }
    return "unknown";
}

bool parse_instability_level(std::string_view value, InstabilityLevel& out) noexcept {
    if (value == "unknown") { out = InstabilityLevel::Unknown; return true; }
    if (value == "unsupported") { out = InstabilityLevel::Unsupported; return true; }
    if (value == "missing") { out = InstabilityLevel::Missing; return true; }
    if (value == "insufficient") { out = InstabilityLevel::Insufficient; return true; }
    if (value == "stale") { out = InstabilityLevel::Stale; return true; }
    if (value == "expired") { out = InstabilityLevel::Expired; return true; }
    if (value == "conflicting") { out = InstabilityLevel::Conflicting; return true; }
    if (value == "stable") { out = InstabilityLevel::Stable; return true; }
    if (value == "elevated") { out = InstabilityLevel::Elevated; return true; }
    if (value == "unstable") { out = InstabilityLevel::Unstable; return true; }
    return false;
}

bool asserts_instability(InstabilityLevel level) noexcept {
    return level == InstabilityLevel::Elevated || level == InstabilityLevel::Unstable;
}

bool asserts_stability(InstabilityLevel level) noexcept { return level == InstabilityLevel::Stable; }

bool makes_positive_assertion(InstabilityLevel level) noexcept {
    return asserts_instability(level) || asserts_stability(level);
}

std::string_view to_string(ThresholdDirection direction) noexcept {
    switch (direction) {
        case ThresholdDirection::AtLeast: return "at_least";
        case ThresholdDirection::AtMost: return "at_most";
    }
    return "at_least";
}

InstabilityPolicy default_instability_policy() {
    InstabilityPolicy policy;
    policy.name = "default";
    policy.min_fresh_samples = 8;
    policy.min_terms = 4;
    policy.require_all_rules = false;
    policy.max_conflicts_tolerated = 0;
    policy.episode_close_windows = 2;
    policy.freshness = FreshnessPolicy{};

    ThresholdRule p95;
    p95.metric = MetricKey::AbsoluteDeltaP95;
    p95.direction = ThresholdDirection::AtLeast;
    p95.elevated_at = 250000.0;   // 0.25 ms
    p95.unstable_at = 1000000.0;  // 1 ms
    p95.justification =
        "the 95th percentile of the successive absolute latency delta is above a quarter of a "
        "millisecond for the elevated band and above one millisecond for the unstable band";
    policy.rules.push_back(p95);

    ThresholdRule max_delta;
    max_delta.metric = MetricKey::AbsoluteDeltaMax;
    max_delta.direction = ThresholdDirection::AtLeast;
    max_delta.elevated_at = 1000000.0;   // 1 ms
    max_delta.unstable_at = 10000000.0;  // 10 ms
    max_delta.justification =
        "a single successive absolute latency delta above one millisecond is elevated and above "
        "ten milliseconds is unstable";
    policy.rules.push_back(max_delta);

    ThresholdRule cv;
    cv.metric = MetricKey::CoefficientOfVariation;
    cv.direction = ThresholdDirection::AtLeast;
    cv.elevated_at = 0.25;
    cv.unstable_at = 1.0;
    cv.justification =
        "the sample standard deviation exceeding a quarter of the mean latency is elevated and "
        "exceeding the mean itself is unstable";
    policy.rules.push_back(cv);
    return policy;
}

InstabilityPolicyId make_instability_policy_id(const InstabilityPolicy& policy) {
    DigestBuilder builder(kDomainPolicy);
    builder.str(policy.name);
    builder.u32(static_cast<std::uint32_t>(policy.rules.size()));
    for (const ThresholdRule& rule : policy.rules) {
        builder.u8(static_cast<std::uint8_t>(rule.metric));
        builder.u8(static_cast<std::uint8_t>(rule.direction));
        builder.f64(rule.elevated_at);
        builder.f64(rule.unstable_at);
        builder.str(rule.justification);
    }
    builder.u64(policy.min_fresh_samples);
    builder.u64(policy.min_terms);
    builder.boolean(policy.require_all_rules);
    builder.u64(policy.max_conflicts_tolerated);
    builder.u32(policy.episode_close_windows);
    builder.id(make_freshness_policy_id(policy.freshness));
    return builder.as_id<InstabilityPolicyTag>();
}

Status validate_instability_policy(const InstabilityPolicy& policy) {
    if (policy.name.empty() || policy.name.size() > kMaxPolicyNameBytes) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "instability policy needs a short, non empty name");
    }
    if (policy.rules.size() > Limits::kMaxWatchedMetrics) {
        return Status::failure(ErrorCode::LimitExceeded, "instability policy watches too many metrics",
                               std::to_string(policy.rules.size()));
    }
    if (policy.min_terms == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "instability policy requires a non zero term minimum");
    }
    if (policy.episode_close_windows == 0) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "episode close window count must be at least one");
    }
    for (std::size_t i = 0; i < policy.rules.size(); ++i) {
        const ThresholdRule& rule = policy.rules[i];
        if (rule.metric == MetricKey::Count) {
            return Status::failure(ErrorCode::InvalidArgument, "threshold rule names an invalid metric",
                                   std::to_string(i));
        }
        if (rule.justification.empty()) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "threshold rule requires a justification", std::to_string(i));
        }
        if (rule.justification.size() > kMaxJustificationBytes) {
            return Status::failure(ErrorCode::LimitExceeded, "threshold justification is too long");
        }
        if (!std::isfinite(rule.elevated_at) || !std::isfinite(rule.unstable_at)) {
            return Status::failure(ErrorCode::InvalidArgument, "threshold values must be finite",
                                   std::to_string(i));
        }
        if (rule.direction == ThresholdDirection::AtLeast && rule.unstable_at < rule.elevated_at) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "for an at-least rule the unstable threshold must not be below the "
                                   "elevated threshold",
                                   std::to_string(i));
        }
        if (rule.direction == ThresholdDirection::AtMost && rule.unstable_at > rule.elevated_at) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "for an at-most rule the unstable threshold must not be above the "
                                   "elevated threshold",
                                   std::to_string(i));
        }
        for (std::size_t j = i + 1; j < policy.rules.size(); ++j) {
            if (policy.rules[j].metric == rule.metric && policy.rules[j].direction == rule.direction) {
                return Status::failure(ErrorCode::Duplicate,
                                       "the same metric is watched twice by one policy",
                                       std::to_string(i) + "/" + std::to_string(j));
            }
        }
    }
    JITTER_TRY(validate_freshness_policy(policy.freshness));
    return Status::success();
}

Classification classify(const ClassificationInput& input) {
    Classification result;
    result.series = input.series;
    result.path = input.path;
    result.generation = input.generation;
    result.generation_ordinal = input.generation_ordinal;
    result.policy = make_instability_policy_id(input.policy);
    result.window = input.window;
    result.evidence = input.evidence.dominant;
    result.current_generation = input.current_generation;
    result.episode_close_windows = input.policy.episode_close_windows;
    result.fresh_samples = input.evidence.fresh;

    for (const std::string& diagnostic : input.metric_diagnostics) {
        add_reason(result.reasons, "metric_unavailable:" + diagnostic);
    }

    if (!input.current_generation) {
        result.level = InstabilityLevel::Unsupported;
        add_reason(result.reasons, "generation_is_not_current");
        finalize_reasons(result.reasons);
        return result;
    }

    if (input.policy.rules.empty()) {
        result.level = InstabilityLevel::Unsupported;
        add_reason(result.reasons, "no_threshold_rules_declared");
        finalize_reasons(result.reasons);
        return result;
    }

    if (input.evidence.total == 0) {
        result.level = InstabilityLevel::Missing;
        add_reason(result.reasons, "no_observations_in_window");
        finalize_reasons(result.reasons);
        return result;
    }

    if (input.evidence.conflicting > input.policy.max_conflicts_tolerated) {
        result.level = InstabilityLevel::Conflicting;
        add_reason(result.reasons, "evidence_is_contradicted");
        finalize_reasons(result.reasons);
        return result;
    }

    if (input.evidence.fresh == 0) {
        if (input.evidence.stale == 0 && input.evidence.unknown == 0 &&
            input.evidence.unsupported == 0 && input.evidence.incomplete == 0 &&
            input.evidence.expired > 0) {
            result.level = InstabilityLevel::Expired;
            add_reason(result.reasons, "all_evidence_expired");
            finalize_reasons(result.reasons);
            return result;
        }
        result.level = InstabilityLevel::Stale;
        add_reason(result.reasons, "no_fresh_evidence");
        finalize_reasons(result.reasons);
        return result;
    }

    if (input.evidence.fresh < input.policy.min_fresh_samples) {
        result.level = InstabilityLevel::Insufficient;
        add_reason(result.reasons, "fewer_fresh_observations_than_policy_requires");
        finalize_reasons(result.reasons);
        return result;
    }

    bool any_unstable = false;
    bool any_elevated = false;
    bool all_unstable = true;
    bool all_elevated = true;
    bool any_evaluable = false;
    std::size_t evaluable_rules = 0;

    for (const ThresholdRule& rule : input.policy.rules) {
        const MetricDefinition& definition = metric_registry().definition(rule.metric);
        RuleEvaluation evaluation;
        evaluation.metric = rule.metric;
        evaluation.metric_id = definition.id;
        evaluation.metric_name = definition.name;
        evaluation.direction = rule.direction;
        evaluation.elevated_at = rule.elevated_at;
        evaluation.unstable_at = rule.unstable_at;

        const MetricValue* value = input.metrics.find(definition.id);
        if (value == nullptr) {
            evaluation.reason = "metric_not_computed";
            result.rules.push_back(evaluation);
            all_unstable = false;
            all_elevated = false;
            continue;
        }
        evaluation.value = value->value;
        evaluation.value_present = true;
        evaluation.term_count = value->term_count;
        if (value->term_count < input.policy.min_terms) {
            evaluation.value_present = false;
            evaluation.reason = "fewer_terms_than_policy_requires";
            result.rules.push_back(evaluation);
            all_unstable = false;
            all_elevated = false;
            continue;
        }
        any_evaluable = true;
        ++evaluable_rules;
        evaluation.breached_elevated = breaches(value->value, rule.elevated_at, rule.direction);
        evaluation.breached_unstable = breaches(value->value, rule.unstable_at, rule.direction);
        evaluation.reason = evaluation.breached_unstable
                                ? "breaches_unstable_threshold"
                                : (evaluation.breached_elevated ? "breaches_elevated_threshold"
                                                                : "within_thresholds");
        any_unstable = any_unstable || evaluation.breached_unstable;
        any_elevated = any_elevated || evaluation.breached_elevated;
        all_unstable = all_unstable && evaluation.breached_unstable;
        all_elevated = all_elevated && evaluation.breached_elevated;
        result.rules.push_back(evaluation);
    }

    if (!any_evaluable) {
        result.level = InstabilityLevel::Insufficient;
        add_reason(result.reasons, "no_rule_could_be_evaluated");
        finalize_reasons(result.reasons);
        for (const RuleEvaluation& evaluation : result.rules) {
            add_reason(result.reasons, "rule:" + std::string(metric_registry().definition(evaluation.metric).name) +
                                           ":" + evaluation.reason);
        }
        finalize_reasons(result.reasons);
        return result;
    }

    const bool unstable = input.policy.require_all_rules ? all_unstable : any_unstable;
    const bool elevated = input.policy.require_all_rules ? all_elevated : any_elevated;

    if (unstable) {
        result.level = InstabilityLevel::Unstable;
    } else if (elevated) {
        result.level = InstabilityLevel::Elevated;
    } else {
        result.level = InstabilityLevel::Stable;
    }

    // Driver: the first breaching rule in policy order, which is stable across runs
    // because policy rule order is part of the policy identity.
    for (const RuleEvaluation& evaluation : result.rules) {
        if (!evaluation.value_present) {
            continue;
        }
        const bool want_unstable = result.level == InstabilityLevel::Unstable;
        if (want_unstable && evaluation.breached_unstable) {
            result.has_driver = true;
            result.driver_key = evaluation.metric;
            result.driver_id = evaluation.metric_id;
            result.driver_value = evaluation.value;
            result.terms = evaluation.term_count;
            break;
        }
        if (!want_unstable && evaluation.breached_elevated) {
            result.has_driver = true;
            result.driver_key = evaluation.metric;
            result.driver_id = evaluation.metric_id;
            result.driver_value = evaluation.value;
            result.terms = evaluation.term_count;
            break;
        }
    }

    for (const RuleEvaluation& evaluation : result.rules) {
        const std::string metric_name(metric_registry().definition(evaluation.metric).name);
        add_reason(result.reasons, "rule:" + metric_name + ":" + evaluation.reason);
        if (evaluation.breached_elevated) {
            add_reason(result.reasons, "threshold_breached_elevated:" + metric_name);
        }
        if (evaluation.breached_unstable) {
            add_reason(result.reasons, "threshold_breached_unstable:" + metric_name);
        }
    }
    add_reason(result.reasons, std::string("level:") + std::string(to_string(result.level)));
    add_reason(result.reasons, "evaluable_rules:" + std::to_string(evaluable_rules));
    finalize_reasons(result.reasons);
    return result;
}

}  // namespace jitter
