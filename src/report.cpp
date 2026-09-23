// Jitter Observatory - history reports and deterministic explanations.
// Copyright 2026 Summon Software Labs.
#include <jitter/report.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/text.hpp>

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

void add_line(Explanation& explanation, std::string code, std::string detail) {
    explanation.lines.push_back(ExplanationLine{std::move(code), std::move(detail)});
}

}  // namespace

Result<HistoryReport> build_history_report(const HistoryInput& input) {
    if (input.request.series.is_nil()) {
        return Result<HistoryReport>::fail(ErrorCode::InvalidArgument,
                                           "history request requires a series identity");
    }
    HistoryReport report;
    report.series = input.request.series;
    report.path = input.request.path;
    report.generations_dropped = input.generations_dropped;
    report.episodes_dropped = input.episodes_dropped;
    report.conflicts_dropped = input.conflicts_dropped;

    std::uint64_t budget = input.request.max_rows == 0 ? 1 : input.request.max_rows;

    for (const PathGeneration& generation : input.generations) {
        GenerationSegment segment;
        segment.generation = generation.id;
        segment.ordinal = generation.ordinal;
        segment.revision = generation.revision;
        segment.topology = generation.topology;
        segment.opened_at_utc_ns = generation.opened_at_utc_ns;
        segment.closed_at_utc_ns = generation.closed_at_utc_ns;
        segment.hop_count = generation.hop_count;
        segment.cause = generation.cause;
        segment.close_reason = generation.close_reason;
        segment.current = generation.id == input.current_generation;
        for (const auto& entry : input.retained_observations) {
            if (entry.first == generation.id) {
                segment.retained_observations = entry.second;
            }
        }
        report.segments.push_back(segment);
        ++report.generations_retained;
    }

    for (const Episode& episode : input.episodes) {
        for (GenerationSegment& segment : report.segments) {
            if (segment.generation == episode.generation) {
                ++segment.episode_count;
                if (episode.is_open()) {
                    ++segment.open_episode_count;
                }
            }
        }
    }

    for (const Episode& episode : input.episodes) {
        if (budget == 0) {
            report.rows_truncated = true;
            break;
        }
        report.episodes.push_back(episode);
        --budget;
    }

    if (input.request.include_conflicts) {
        for (const ConflictRecord& conflict : input.conflicts) {
            if (budget == 0) {
                report.rows_truncated = true;
                break;
            }
            report.conflicts.push_back(conflict);
            --budget;
        }
    }

    if (report.generations_dropped > 0) {
        add_reason(report.reasons, "generation_history_truncated");
    }
    if (report.episodes_dropped > 0) {
        add_reason(report.reasons, "episode_history_truncated");
    }
    if (report.conflicts_dropped > 0) {
        add_reason(report.reasons, "conflict_history_truncated");
    }
    if (report.rows_truncated) {
        add_reason(report.reasons, "rows_truncated_by_request_limit");
    }
    if (report.segments.size() > 1) {
        add_reason(report.reasons, "history_is_segmented_by_route_generation");
    }

    DigestBuilder builder("jitter.history.v1");
    builder.id(report.series);
    builder.id(report.path);
    builder.u32(static_cast<std::uint32_t>(report.segments.size()));
    for (const GenerationSegment& segment : report.segments) {
        builder.id(segment.generation);
        builder.u64(segment.ordinal.value());
        builder.u64(segment.revision.value());
        builder.i64(segment.opened_at_utc_ns);
        builder.optional_i64(segment.closed_at_utc_ns);
        builder.boolean(segment.current);
        builder.u64(segment.episode_count);
        builder.u64(segment.retained_observations);
    }
    builder.u32(static_cast<std::uint32_t>(report.episodes.size()));
    for (const Episode& episode : report.episodes) {
        builder.id(episode.id);
        builder.u8(static_cast<std::uint8_t>(episode.peak_level));
        builder.i64(episode.opened_at_utc_ns);
        builder.optional_i64(episode.closed_at_utc_ns);
        builder.u8(static_cast<std::uint8_t>(episode.close_reason));
    }
    builder.u32(static_cast<std::uint32_t>(report.conflicts.size()));
    for (const ConflictRecord& conflict : report.conflicts) {
        builder.raw(std::span<const std::uint8_t>(conflict.identity.data(), Digest::kBytes));
    }
    report.digest = builder.digest();

    std::sort(report.reasons.begin(), report.reasons.end());
    report.reasons.erase(std::unique(report.reasons.begin(), report.reasons.end()), report.reasons.end());
    return report;
}

Explanation explain_summary(const SeriesSummary& summary, const WindowPolicy& window_policy,
                            const InstabilityPolicy& instability_policy) {
    Explanation explanation;
    explanation.subject = "series_summary";
    explanation.series = summary.series;
    explanation.path = summary.path;
    explanation.generation = summary.generation;
    explanation.generation_ordinal = summary.generation_ordinal;
    explanation.level = summary.classification.level;
    explanation.evidence = summary.evidence.dominant;
    explanation.policy = summary.policy;
    explanation.window = summary.window;
    explanation.freshness = summary.freshness_policy;
    explanation.current_generation = summary.current_generation;

    add_line(explanation, "boundary.authority",
             "this runtime observes latency variance and path timing instability; it does not route "
             "traffic, enforce objectives, attribute causality or declare faults");
    add_line(explanation, "boundary.per_hop",
             "per-hop values are hop local timing observations and are produced only where clock "
             "comparability permits");
    add_line(explanation, "classification.level", std::string(to_string(summary.classification.level)));
    add_line(explanation, "classification.positive_assertion",
             makes_positive_assertion(summary.classification.level) ? "true" : "false");
    add_line(explanation, "evidence.dominant", std::string(to_string(summary.evidence.dominant)));
    add_line(explanation, "evidence.fresh", text::u64_to_string(summary.evidence.fresh));
    add_line(explanation, "evidence.stale", text::u64_to_string(summary.evidence.stale));
    add_line(explanation, "evidence.expired", text::u64_to_string(summary.evidence.expired));
    add_line(explanation, "evidence.conflicting", text::u64_to_string(summary.evidence.conflicting));
    add_line(explanation, "evidence.unknown", text::u64_to_string(summary.evidence.unknown));
    add_line(explanation, "evidence.unsupported", text::u64_to_string(summary.evidence.unsupported));
    add_line(explanation, "evidence.total", text::u64_to_string(summary.evidence.total));
    add_line(explanation, "evidence.real_origin", text::u64_to_string(summary.evidence.real_origin));
    add_line(explanation, "evidence.synthetic_origin",
             text::u64_to_string(summary.evidence.synthetic_origin));
    add_line(explanation, "evidence.admits_positive_assertion",
             summary.evidence.admits_positive_assertion() ? "true" : "false");
    add_line(explanation, "generation.current", summary.current_generation ? "true" : "false");
    add_line(explanation, "generation.ordinal", summary.generation_ordinal.to_string_value());
    add_line(explanation, "origin.dominant", std::string(to_string(summary.dominant_origin)));
    add_line(explanation, "policy.freshness.max_ingest_age_ns",
             text::i64_to_string(instability_policy.freshness.max_ingest_age_ns));
    add_line(explanation, "policy.freshness.max_observation_age_ns",
             text::i64_to_string(instability_policy.freshness.max_observation_age_ns));
    add_line(explanation, "policy.freshness.require_observation_age",
             instability_policy.freshness.require_observation_age ? "true" : "false");
    add_line(explanation, "policy.instability.name", instability_policy.name);
    add_line(explanation, "policy.instability.min_fresh_samples",
             text::u64_to_string(instability_policy.min_fresh_samples));
    add_line(explanation, "policy.instability.min_terms",
             text::u64_to_string(instability_policy.min_terms));
    add_line(explanation, "policy.instability.require_all_rules",
             instability_policy.require_all_rules ? "true" : "false");
    add_line(explanation, "policy.identity", summary.policy.hex());
    add_line(explanation, "window.capacity", text::u64_to_string(window_policy.capacity));
    add_line(explanation, "window.count", text::u64_to_string(window_policy.count));
    add_line(explanation, "window.duration_ns", text::i64_to_string(window_policy.duration_ns));
    add_line(explanation, "window.kind", std::string(to_string(window_policy.kind)));
    add_line(explanation, "window.selection_reason", summary.selection_reason);
    add_line(explanation, "window.selected", text::u64_to_string(summary.selected));
    add_line(explanation, "window.retained", text::u64_to_string(summary.retained));
    add_line(explanation, "window.evicted_by_capacity",
             text::u64_to_string(summary.evicted_by_capacity));
    add_line(explanation, "window.out_of_order_accepted",
             text::u64_to_string(summary.out_of_order_accepted));
    add_line(explanation, "window.excluded_by_policy",
             text::u64_to_string(summary.excluded_by_policy));
    add_line(explanation, "window.identity", summary.window.hex());
    add_line(explanation, "summary.identity", summary.id.hex());

    if (summary.classification.has_driver) {
        add_line(explanation, "driver.metric",
                 std::string(metric_registry().definition(summary.classification.driver_key).name));
        add_line(explanation, "driver.value", text::double_to_string(summary.classification.driver_value));
        add_line(explanation, "driver.terms", text::u64_to_string(summary.classification.terms));
    } else {
        add_line(explanation, "driver.metric", "none");
    }

    for (const MetricValue& value : summary.metrics) {
        const std::string prefix = std::string("metric.") + std::string(value.name);
        add_line(explanation, prefix + ".value", text::double_to_string(value.value));
        add_line(explanation, prefix + ".unit", std::string(unit_label(value.unit)));
        add_line(explanation, prefix + ".terms", text::u64_to_string(value.term_count));
        add_line(explanation, prefix + ".identity", value.id.hex());
    }
    for (const std::string& unavailable : summary.unavailable_metrics) {
        add_line(explanation, "metric.unavailable", unavailable);
    }

    for (const RuleEvaluation& rule : summary.classification.rules) {
        const std::string prefix = std::string("rule.") +
                                   std::string(metric_registry().definition(rule.metric).name);
        add_line(explanation, prefix + ".reason", rule.reason);
        add_line(explanation, prefix + ".elevated_at", text::double_to_string(rule.elevated_at));
        add_line(explanation, prefix + ".unstable_at", text::double_to_string(rule.unstable_at));
        add_line(explanation, prefix + ".value_present", rule.value_present ? "true" : "false");
        if (rule.value_present) {
            add_line(explanation, prefix + ".value", text::double_to_string(rule.value));
        }
        std::string justification;
        for (const ThresholdRule& declared : instability_policy.rules) {
            if (declared.metric == rule.metric) {
                justification = declared.justification;
                break;
            }
        }
        if (!justification.empty()) {
            add_line(explanation, prefix + ".justification", justification);
        }
    }

    for (const std::string& reason : summary.reasons) {
        add_reason(explanation.reasons, reason);
    }
    std::sort(explanation.reasons.begin(), explanation.reasons.end());

    std::sort(explanation.lines.begin(), explanation.lines.end(),
              [](const ExplanationLine& a, const ExplanationLine& b) {
                  if (a.code != b.code) {
                      return a.code < b.code;
                  }
                  return a.detail < b.detail;
              });
    explanation.lines.erase(
        std::unique(explanation.lines.begin(), explanation.lines.end(),
                    [](const ExplanationLine& a, const ExplanationLine& b) {
                        return a.code == b.code && a.detail == b.detail;
                    }),
        explanation.lines.end());

    DigestBuilder builder("jitter.explanation.v1");
    builder.str(explanation.subject);
    builder.id(explanation.series);
    builder.id(explanation.generation);
    builder.u8(static_cast<std::uint8_t>(explanation.level));
    builder.u8(static_cast<std::uint8_t>(explanation.evidence));
    builder.id(explanation.policy);
    builder.id(explanation.window);
    for (const ExplanationLine& line : explanation.lines) {
        builder.str(line.code);
        builder.str(line.detail);
    }
    explanation.digest = builder.digest();
    return explanation;
}

}  // namespace jitter
