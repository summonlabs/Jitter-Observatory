// Jitter Observatory - frozen baselines and deterministic comparison.
// Copyright 2026 Summon Software Labs.
#include <jitter/baseline.hpp>

#include <algorithm>
#include <cmath>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>

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

ComparisonVerdict verdict_for(EvidenceState state) {
    switch (state) {
        case EvidenceState::Fresh: return ComparisonVerdict::Comparable;
        case EvidenceState::Stale: return ComparisonVerdict::Stale;
        case EvidenceState::Expired: return ComparisonVerdict::Expired;
        case EvidenceState::Conflicting: return ComparisonVerdict::Conflicting;
        case EvidenceState::Incomplete: return ComparisonVerdict::Insufficient;
        case EvidenceState::Unsupported: return ComparisonVerdict::Unsupported;
        case EvidenceState::Unknown: return ComparisonVerdict::Insufficient;
        case EvidenceState::Missing: return ComparisonVerdict::Missing;
    }
    return ComparisonVerdict::Incomparable;
}

}  // namespace

const MetricValue* Baseline::find(MetricId metric_id) const noexcept {
    for (const MetricValue& value : metrics) {
        if (value.id == metric_id) {
            return &value;
        }
    }
    return nullptr;
}

const MetricValue* Baseline::find(MetricKey metric_key) const noexcept {
    for (const MetricValue& value : metrics) {
        if (value.key == metric_key) {
            return &value;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> Baseline::span_ns() const noexcept {
    if (!first_observation_utc_ns.has_value() || !last_observation_utc_ns.has_value()) {
        return std::nullopt;
    }
    return last_observation_utc_ns.value() - first_observation_utc_ns.value();
}

Result<Baseline> make_baseline(Baseline baseline) {
    if (baseline.series.is_nil()) {
        return Result<Baseline>::fail(ErrorCode::InvalidArgument, "baseline requires a series identity");
    }
    if (baseline.generation.is_nil()) {
        return Result<Baseline>::fail(ErrorCode::InvalidArgument,
                                      "baseline requires a generation identity");
    }
    if (baseline.name.empty()) {
        return Result<Baseline>::fail(ErrorCode::InvalidArgument, "baseline requires a name");
    }
    std::sort(baseline.metrics.begin(), baseline.metrics.end(),
              [](const MetricValue& a, const MetricValue& b) {
                  if (a.name != b.name) {
                      return a.name < b.name;
                  }
                  return a.id < b.id;
              });
    for (std::size_t i = 1; i < baseline.metrics.size(); ++i) {
        if (baseline.metrics[i].id == baseline.metrics[i - 1].id) {
            return Result<Baseline>::fail(ErrorCode::Duplicate,
                                          "baseline holds two values for the same metric identity",
                                          std::string(baseline.metrics[i].name));
        }
    }

    DigestBuilder builder(kDomainBaseline);
    builder.str(baseline.name);
    builder.id(baseline.series);
    builder.id(baseline.path);
    builder.id(baseline.generation);
    builder.u64(baseline.generation_ordinal.value());
    builder.id(baseline.window);
    builder.id(baseline.policy);
    builder.u64(baseline.sample_count);
    builder.u64(baseline.fresh_sample_count);
    builder.optional_i64(baseline.first_observation_utc_ns);
    builder.optional_i64(baseline.last_observation_utc_ns);
    builder.u8(static_cast<std::uint8_t>(baseline.evidence));
    builder.u8(static_cast<std::uint8_t>(baseline.origin));
    builder.str(baseline.note);
    builder.u32(static_cast<std::uint32_t>(baseline.metrics.size()));
    for (const MetricValue& value : baseline.metrics) {
        builder.id(value.id);
        builder.u32(value.version);
        builder.f64(value.value);
        builder.u64(value.sample_count);
        builder.u64(value.term_count);
    }
    baseline.digest = builder.digest();
    baseline.id = BaselineId::from_digest(baseline.digest);
    return baseline;
}

std::string_view to_string(ComparisonVerdict verdict) noexcept {
    switch (verdict) {
        case ComparisonVerdict::Comparable: return "comparable";
        case ComparisonVerdict::Incomparable: return "incomparable";
        case ComparisonVerdict::Missing: return "missing";
        case ComparisonVerdict::Insufficient: return "insufficient";
        case ComparisonVerdict::Stale: return "stale";
        case ComparisonVerdict::Expired: return "expired";
        case ComparisonVerdict::Conflicting: return "conflicting";
        case ComparisonVerdict::Unsupported: return "unsupported";
    }
    return "incomparable";
}

BaselineComparisonReport compare_to_baseline(const Baseline& baseline,
                                             const CurrentSnapshot& current) {
    BaselineComparisonReport report;
    report.baseline = baseline.id;
    report.baseline_name = baseline.name;
    report.series = baseline.series;
    report.path = baseline.path;
    report.baseline_generation = baseline.generation;
    report.current_generation = current.generation;
    report.baseline_generation_ordinal = baseline.generation_ordinal;
    report.current_generation_ordinal = current.generation_ordinal;
    report.current_evidence = current.evidence.dominant;
    report.generation_changed = baseline.generation != current.generation;
    report.window_changed = baseline.window != current.window;
    report.policy_changed = baseline.policy != current.policy;

    const bool scope_ok = !report.generation_changed && !report.window_changed;
    if (report.generation_changed) {
        add_reason(report.reasons, "generation_changed");
    }
    if (report.window_changed) {
        add_reason(report.reasons, "window_policy_changed");
    }
    if (report.policy_changed) {
        add_reason(report.reasons, "instability_policy_changed");
    }

    const bool evidence_ok = current.evidence.admits_positive_assertion();
    if (!evidence_ok) {
        add_reason(report.reasons,
                   std::string("evidence:") + std::string(to_string(current.evidence.dominant)));
    }

    report.overall = ComparisonVerdict::Comparable;
    if (!scope_ok) {
        report.overall = ComparisonVerdict::Incomparable;
    } else if (!evidence_ok) {
        report.overall = verdict_for(current.evidence.dominant);
    }

    for (const MetricValue& baseline_value : baseline.metrics) {
        MetricComparison comparison;
        comparison.metric = baseline_value.id;
        comparison.key = baseline_value.key;
        comparison.name = baseline_value.name;
        comparison.unit = baseline_value.unit;
        comparison.baseline_value = baseline_value.value;
        comparison.baseline_terms = baseline_value.term_count;

        const MetricValue* current_value = current.metrics.find(baseline_value.id);
        if (current_value != nullptr) {
            comparison.current_value = current_value->value;
            comparison.current_terms = current_value->term_count;
        }

        if (report.generation_changed) {
            comparison.verdict = ComparisonVerdict::Incomparable;
            comparison.reason = "generation_changed";
            report.comparisons.push_back(comparison);
            continue;
        }
        if (report.window_changed) {
            comparison.verdict = ComparisonVerdict::Incomparable;
            comparison.reason = "window_policy_changed";
            report.comparisons.push_back(comparison);
            continue;
        }
        if (current_value == nullptr) {
            comparison.verdict = ComparisonVerdict::Missing;
            comparison.reason = "metric_absent_from_current_window";
            report.comparisons.push_back(comparison);
            continue;
        }
        if (!evidence_ok) {
            comparison.verdict = verdict_for(current.evidence.dominant);
            comparison.reason = std::string("evidence:") + std::string(to_string(current.evidence.dominant));
            report.comparisons.push_back(comparison);
            continue;
        }

        // Identical identity implies identical definition version and unit, which is
        // exactly why the delta below is meaningful.
        comparison.verdict = ComparisonVerdict::Comparable;
        comparison.reason = "comparable";
        comparison.delta = current_value->value - baseline_value.value;
        if (baseline_value.value != 0.0) {
            comparison.ratio = current_value->value / baseline_value.value;
            comparison.relative_change =
                (current_value->value - baseline_value.value) / std::fabs(baseline_value.value);
        } else {
            comparison.reason = "comparable_zero_baseline_ratio_undefined";
        }
        report.comparisons.push_back(comparison);
    }

    std::sort(report.comparisons.begin(), report.comparisons.end(),
              [](const MetricComparison& a, const MetricComparison& b) {
                  if (a.name != b.name) {
                      return a.name < b.name;
                  }
                  return a.metric < b.metric;
              });

    DigestBuilder builder("jitter.comparison.v1");
    builder.id(report.baseline);
    builder.id(report.series);
    builder.id(report.path);
    builder.id(report.baseline_generation);
    builder.id(report.current_generation);
    builder.u8(static_cast<std::uint8_t>(report.overall));
    builder.u32(static_cast<std::uint32_t>(report.comparisons.size()));
    for (const MetricComparison& comparison : report.comparisons) {
        builder.id(comparison.metric);
        builder.u8(static_cast<std::uint8_t>(comparison.verdict));
        if (comparison.delta.has_value()) {
            builder.boolean(true);
            builder.f64(comparison.delta.value());
        } else {
            builder.boolean(false);
        }
        if (comparison.ratio.has_value()) {
            builder.boolean(true);
            builder.f64(comparison.ratio.value());
        } else {
            builder.boolean(false);
        }
    }
    report.digest = builder.digest();

    std::sort(report.reasons.begin(), report.reasons.end());
    report.reasons.erase(std::unique(report.reasons.begin(), report.reasons.end()), report.reasons.end());
    return report;
}

}  // namespace jitter
