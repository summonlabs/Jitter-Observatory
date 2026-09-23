// Jitter Observatory - canonical JSON rendering.
// Copyright 2026 Summon Software Labs.
#include <jitter/render.hpp>

#include <jitter/metrics.hpp>
#include <jitter/text.hpp>
#include <jitter/version.hpp>

namespace jitter {
namespace {

template <class Tag>
JsonValue id_json(const StrongId<Tag>& id) { return JsonValue::make_string(id.hex()); }

JsonValue digest_json(const Digest& digest) { return JsonValue::make_string(digest.hex()); }

JsonValue optional_i64_json(const std::optional<std::int64_t>& value) {
    if (!value.has_value()) {
        return JsonValue::make_null();
    }
    return JsonValue::make_i64(value.value());
}

JsonValue optional_double_json(const std::optional<double>& value) {
    if (!value.has_value()) {
        return JsonValue::make_null();
    }
    return JsonValue::make_double(value.value());
}

JsonValue strings_json(const std::vector<std::string>& values) {
    JsonValue array = JsonValue::make_array();
    for (const std::string& value : values) {
        array.push(JsonValue::make_string(value));
    }
    return array;
}

JsonValue metric_key_json(MetricKey key) {
    return JsonValue::make_string(metric_registry().definition(key).name);
}

}  // namespace

JsonValue render_version() {
    JsonValue value = JsonValue::make_object();
    value.set("product", JsonValue::make_string(kProductName));
    value.set("slug", JsonValue::make_string(kProductSlug));
    value.set("version", JsonValue::make_string(kVersionString));
    value.set("organization", JsonValue::make_string(kOrganization));
    return value;
}

JsonValue render_metric_value(const MetricValue& value) {
    JsonValue json = JsonValue::make_object();
    json.set("identity", id_json(value.id));
    json.set("key", metric_key_json(value.key));
    json.set("name", JsonValue::make_string(value.name));
    json.set("unit", JsonValue::make_string(unit_label(value.unit)));
    json.set("value", JsonValue::make_double(value.value));
    json.set("sample_count", JsonValue::make_u64(value.sample_count));
    json.set("term_count", JsonValue::make_u64(value.term_count));
    json.set("definition_version", JsonValue::make_u64(value.version));
    return json;
}

JsonValue render_metric_definition(const MetricDefinition& definition) {
    JsonValue json = JsonValue::make_object();
    json.set("identity", id_json(definition.id));
    json.set("key", metric_key_json(definition.key));
    json.set("name", JsonValue::make_string(definition.name));
    json.set("formula", JsonValue::make_string(definition.formula));
    json.set("family", JsonValue::make_string(to_string(definition.family)));
    json.set("unit", JsonValue::make_string(unit_label(definition.unit)));
    json.set("estimator", JsonValue::make_string(to_string(definition.estimator)));
    json.set("input", JsonValue::make_string(to_string(definition.input)));
    json.set("requires_ordered_samples", JsonValue::make_bool(definition.requires_ordered_samples));
    json.set("min_samples", JsonValue::make_u64(definition.min_samples));
    json.set("min_terms", JsonValue::make_u64(definition.min_terms));
    json.set("definition_version", JsonValue::make_u64(definition.version));
    return json;
}

JsonValue render_evidence_summary(const EvidenceSummary& summary) {
    JsonValue json = JsonValue::make_object();
    json.set("total", JsonValue::make_u64(summary.total));
    json.set("fresh", JsonValue::make_u64(summary.fresh));
    json.set("stale", JsonValue::make_u64(summary.stale));
    json.set("expired", JsonValue::make_u64(summary.expired));
    json.set("conflicting", JsonValue::make_u64(summary.conflicting));
    json.set("incomplete", JsonValue::make_u64(summary.incomplete));
    json.set("unsupported", JsonValue::make_u64(summary.unsupported));
    json.set("unknown", JsonValue::make_u64(summary.unknown));
    json.set("real_origin", JsonValue::make_u64(summary.real_origin));
    json.set("synthetic_origin", JsonValue::make_u64(summary.synthetic_origin));
    json.set("dominant", JsonValue::make_string(to_string(summary.dominant)));
    json.set("admits_positive_assertion", JsonValue::make_bool(summary.admits_positive_assertion()));
    json.set("reasons", strings_json(summary.reasons));
    return json;
}

JsonValue render_evidence_assessment(const SampleAssessment& assessment) {
    JsonValue json = JsonValue::make_object();
    json.set("sample", id_json(assessment.sample));
    json.set("state", JsonValue::make_string(to_string(assessment.state)));
    json.set("reason", JsonValue::make_string(assessment.reason));
    json.set("ingest_age_ns", optional_i64_json(assessment.ages.ingest_age_ns));
    json.set("observation_age_ns", optional_i64_json(assessment.ages.observation_age_ns));
    json.set("in_domain_span_ns", optional_i64_json(assessment.ages.in_domain_span_ns));
    json.set("comparability", JsonValue::make_string(to_string(assessment.ages.comparability)));
    json.set("comparability_reason", JsonValue::make_string(assessment.ages.reason));
    return json;
}

JsonValue render_classification(const Classification& classification) {
    JsonValue json = JsonValue::make_object();
    json.set("level", JsonValue::make_string(to_string(classification.level)));
    json.set("evidence", JsonValue::make_string(to_string(classification.evidence)));
    json.set("current_generation", JsonValue::make_bool(classification.current_generation));
    json.set("asserts_instability", JsonValue::make_bool(asserts_instability(classification.level)));
    json.set("asserts_stability", JsonValue::make_bool(asserts_stability(classification.level)));
    json.set("makes_positive_assertion",
             JsonValue::make_bool(makes_positive_assertion(classification.level)));
    json.set("fresh_samples", JsonValue::make_u64(classification.fresh_samples));
    json.set("terms", JsonValue::make_u64(classification.terms));
    json.set("episode_close_windows", JsonValue::make_u64(classification.episode_close_windows));
    json.set("window_policy", id_json(classification.window));
    json.set("instability_policy", id_json(classification.policy));

    JsonValue driver = JsonValue::make_object();
    driver.set("present", JsonValue::make_bool(classification.has_driver));
    if (classification.has_driver) {
        driver.set("key", metric_key_json(classification.driver_key));
        driver.set("identity", id_json(classification.driver_id));
        driver.set("value", JsonValue::make_double(classification.driver_value));
    }
    json.set("driver", std::move(driver));

    JsonValue rules = JsonValue::make_array();
    for (const RuleEvaluation& rule : classification.rules) {
        JsonValue item = JsonValue::make_object();
        item.set("metric", JsonValue::make_string(rule.metric_name));
        item.set("identity", id_json(rule.metric_id));
        item.set("direction", JsonValue::make_string(to_string(rule.direction)));
        item.set("elevated_at", JsonValue::make_double(rule.elevated_at));
        item.set("unstable_at", JsonValue::make_double(rule.unstable_at));
        item.set("value_present", JsonValue::make_bool(rule.value_present));
        if (rule.value_present) {
            item.set("value", JsonValue::make_double(rule.value));
        }
        item.set("breached_elevated", JsonValue::make_bool(rule.breached_elevated));
        item.set("breached_unstable", JsonValue::make_bool(rule.breached_unstable));
        item.set("term_count", JsonValue::make_u64(rule.term_count));
        item.set("reason", JsonValue::make_string(rule.reason));
        rules.push(std::move(item));
    }
    json.set("rules", std::move(rules));
    json.set("reasons", strings_json(classification.reasons));
    return json;
}

JsonValue render_summary(const SeriesSummary& summary) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(summary.id));
    json.set("series", id_json(summary.series));
    json.set("path", id_json(summary.path));
    json.set("generation", id_json(summary.generation));
    json.set("generation_ordinal", JsonValue::make_u64(summary.generation_ordinal.value()));
    json.set("window_policy", id_json(summary.window));
    json.set("instability_policy", id_json(summary.policy));
    json.set("freshness_policy", id_json(summary.freshness_policy));
    json.set("current_generation", JsonValue::make_bool(summary.current_generation));

    JsonValue counts = JsonValue::make_object();
    counts.set("retained", JsonValue::make_u64(summary.retained));
    counts.set("selected", JsonValue::make_u64(summary.selected));
    counts.set("excluded_by_policy", JsonValue::make_u64(summary.excluded_by_policy));
    counts.set("evicted_by_capacity", JsonValue::make_u64(summary.evicted_by_capacity));
    counts.set("out_of_order_accepted", JsonValue::make_u64(summary.out_of_order_accepted));
    counts.set("duplicate_ignored", JsonValue::make_u64(summary.duplicate_ignored));
    counts.set("fresh_samples", JsonValue::make_u64(summary.fresh_samples));
    json.set("counts", std::move(counts));

    JsonValue bounds = JsonValue::make_object();
    bounds.set("first_observation_ticks", optional_i64_json(summary.first_observation_ticks));
    bounds.set("last_observation_ticks", optional_i64_json(summary.last_observation_ticks));
    bounds.set("first_received_utc_ns", optional_i64_json(summary.first_received_utc_ns));
    bounds.set("last_received_utc_ns", optional_i64_json(summary.last_received_utc_ns));
    json.set("observation_bounds", std::move(bounds));

    json.set("selection_reason", JsonValue::make_string(summary.selection_reason));
    json.set("dominant_origin", JsonValue::make_string(to_string(summary.dominant_origin)));
    json.set("evidence", render_evidence_summary(summary.evidence));

    JsonValue metrics = JsonValue::make_array();
    for (const MetricValue& value : summary.metrics) {
        metrics.push(render_metric_value(value));
    }
    json.set("metrics", std::move(metrics));
    json.set("unavailable_metrics", strings_json(summary.unavailable_metrics));
    json.set("classification", render_classification(summary.classification));
    json.set("reasons", strings_json(summary.reasons));
    json.set("digest", digest_json(summary.digest));
    json.set("measurement_digest", digest_json(summary.measurement_digest));
    return json;
}

JsonValue render_episode(const Episode& episode) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(episode.id));
    json.set("series", id_json(episode.series));
    json.set("path", id_json(episode.path));
    json.set("generation", id_json(episode.generation));
    json.set("generation_ordinal", JsonValue::make_u64(episode.generation_ordinal.value()));
    json.set("instability_policy", id_json(episode.policy));
    json.set("window_policy", id_json(episode.window));
    json.set("peak_level", JsonValue::make_string(to_string(episode.peak_level)));
    json.set("driver_metric", metric_key_json(episode.driver_key));
    json.set("driver_identity", id_json(episode.driver_id));
    json.set("opening_value", JsonValue::make_double(episode.opening_value));
    json.set("peak_value", JsonValue::make_double(episode.peak_value));
    json.set("opened_at_utc_ns", JsonValue::make_i64(episode.opened_at_utc_ns));
    json.set("closed_at_utc_ns", optional_i64_json(episode.closed_at_utc_ns));
    json.set("open", JsonValue::make_bool(episode.is_open()));
    json.set("observations", JsonValue::make_u64(episode.observations));
    json.set("breach_observations", JsonValue::make_u64(episode.breach_observations));
    json.set("recovery_observations", JsonValue::make_u64(episode.recovery_observations));
    json.set("close_windows_required", JsonValue::make_u64(episode.close_windows_required));
    json.set("close_reason", JsonValue::make_string(to_string(episode.close_reason)));
    json.set("close_detail", JsonValue::make_string(episode.close_detail));
    json.set("duration_ns", optional_i64_json(episode.duration_ns()));
    json.set("reasons", strings_json(episode.reasons));
    return json;
}

JsonValue render_conflict(const ConflictRecord& conflict) {
    JsonValue json = JsonValue::make_object();
    json.set("identity", digest_json(conflict.identity));
    json.set("series", id_json(conflict.series));
    json.set("path", id_json(conflict.path));
    json.set("generation", id_json(conflict.generation));
    json.set("sequence", JsonValue::make_u64(conflict.sequence.value()));
    json.set("source_a", id_json(conflict.source_a));
    json.set("source_b", id_json(conflict.source_b));
    json.set("content_a", digest_json(conflict.content_a));
    json.set("content_b", digest_json(conflict.content_b));
    json.set("authority_a", JsonValue::make_string(to_string(conflict.authority_a)));
    json.set("authority_b", JsonValue::make_string(to_string(conflict.authority_b)));
    json.set("detected_at_utc_ns", JsonValue::make_i64(conflict.detected_at_utc_ns));
    json.set("outcome", JsonValue::make_string(to_string(conflict.outcome)));
    json.set("resolved", JsonValue::make_bool(conflict.resolved()));
    if (conflict.winner.has_value()) {
        json.set("winner", id_json(conflict.winner.value()));
    } else {
        json.set("winner", JsonValue::make_null());
    }
    json.set("reason", JsonValue::make_string(conflict.reason));
    return json;
}

JsonValue render_baseline(const Baseline& baseline) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(baseline.id));
    json.set("name", JsonValue::make_string(baseline.name));
    json.set("series", id_json(baseline.series));
    json.set("path", id_json(baseline.path));
    json.set("generation", id_json(baseline.generation));
    json.set("generation_ordinal", JsonValue::make_u64(baseline.generation_ordinal.value()));
    json.set("window_policy", id_json(baseline.window));
    json.set("instability_policy", id_json(baseline.policy));
    json.set("sample_count", JsonValue::make_u64(baseline.sample_count));
    json.set("fresh_sample_count", JsonValue::make_u64(baseline.fresh_sample_count));
    json.set("first_observation_utc_ns", optional_i64_json(baseline.first_observation_utc_ns));
    json.set("last_observation_utc_ns", optional_i64_json(baseline.last_observation_utc_ns));
    json.set("span_ns", optional_i64_json(baseline.span_ns()));
    json.set("evidence", JsonValue::make_string(to_string(baseline.evidence)));
    json.set("origin", JsonValue::make_string(to_string(baseline.origin)));
    json.set("created_at_utc_ns", JsonValue::make_i64(baseline.created_at_utc_ns));
    json.set("note", JsonValue::make_string(baseline.note));
    JsonValue metrics = JsonValue::make_array();
    for (const MetricValue& value : baseline.metrics) {
        metrics.push(render_metric_value(value));
    }
    json.set("metrics", std::move(metrics));
    json.set("digest", digest_json(baseline.digest));
    return json;
}

JsonValue render_comparison(const BaselineComparisonReport& report) {
    JsonValue json = JsonValue::make_object();
    json.set("baseline", id_json(report.baseline));
    json.set("baseline_name", JsonValue::make_string(report.baseline_name));
    json.set("series", id_json(report.series));
    json.set("path", id_json(report.path));
    json.set("baseline_generation", id_json(report.baseline_generation));
    json.set("current_generation", id_json(report.current_generation));
    json.set("baseline_generation_ordinal",
             JsonValue::make_u64(report.baseline_generation_ordinal.value()));
    json.set("current_generation_ordinal",
             JsonValue::make_u64(report.current_generation_ordinal.value()));
    json.set("generation_changed", JsonValue::make_bool(report.generation_changed));
    json.set("window_changed", JsonValue::make_bool(report.window_changed));
    json.set("policy_changed", JsonValue::make_bool(report.policy_changed));
    json.set("current_evidence", JsonValue::make_string(to_string(report.current_evidence)));
    json.set("overall", JsonValue::make_string(to_string(report.overall)));
    JsonValue comparisons = JsonValue::make_array();
    for (const MetricComparison& comparison : report.comparisons) {
        JsonValue item = JsonValue::make_object();
        item.set("metric", JsonValue::make_string(comparison.name));
        item.set("identity", id_json(comparison.metric));
        item.set("unit", JsonValue::make_string(unit_label(comparison.unit)));
        item.set("baseline_value", optional_double_json(comparison.baseline_value));
        item.set("current_value", optional_double_json(comparison.current_value));
        item.set("delta", optional_double_json(comparison.delta));
        item.set("ratio", optional_double_json(comparison.ratio));
        item.set("relative_change", optional_double_json(comparison.relative_change));
        item.set("baseline_terms", JsonValue::make_u64(comparison.baseline_terms));
        item.set("current_terms", JsonValue::make_u64(comparison.current_terms));
        item.set("verdict", JsonValue::make_string(to_string(comparison.verdict)));
        item.set("reason", JsonValue::make_string(comparison.reason));
        comparisons.push(std::move(item));
    }
    json.set("comparisons", std::move(comparisons));
    json.set("reasons", strings_json(report.reasons));
    json.set("digest", digest_json(report.digest));
    return json;
}

JsonValue render_history(const HistoryReport& report) {
    JsonValue json = JsonValue::make_object();
    json.set("series", id_json(report.series));
    json.set("path", id_json(report.path));
    json.set("generations_retained", JsonValue::make_u64(report.generations_retained));
    json.set("generations_dropped", JsonValue::make_u64(report.generations_dropped));
    json.set("episodes_dropped", JsonValue::make_u64(report.episodes_dropped));
    json.set("conflicts_dropped", JsonValue::make_u64(report.conflicts_dropped));
    json.set("rows_truncated", JsonValue::make_bool(report.rows_truncated));

    JsonValue segments = JsonValue::make_array();
    for (const GenerationSegment& segment : report.segments) {
        JsonValue item = JsonValue::make_object();
        item.set("generation", id_json(segment.generation));
        item.set("ordinal", JsonValue::make_u64(segment.ordinal.value()));
        item.set("revision", JsonValue::make_u64(segment.revision.value()));
        item.set("topology", digest_json(segment.topology));
        item.set("opened_at_utc_ns", JsonValue::make_i64(segment.opened_at_utc_ns));
        item.set("closed_at_utc_ns", optional_i64_json(segment.closed_at_utc_ns));
        item.set("hop_count", JsonValue::make_u64(segment.hop_count));
        item.set("cause", JsonValue::make_string(segment.cause));
        item.set("close_reason", JsonValue::make_string(segment.close_reason));
        item.set("current", JsonValue::make_bool(segment.current));
        item.set("episode_count", JsonValue::make_u64(segment.episode_count));
        item.set("open_episode_count", JsonValue::make_u64(segment.open_episode_count));
        item.set("retained_observations", JsonValue::make_u64(segment.retained_observations));
        segments.push(std::move(item));
    }
    json.set("segments", std::move(segments));

    JsonValue episodes = JsonValue::make_array();
    for (const Episode& episode : report.episodes) {
        episodes.push(render_episode(episode));
    }
    json.set("episodes", std::move(episodes));

    JsonValue conflicts = JsonValue::make_array();
    for (const ConflictRecord& conflict : report.conflicts) {
        conflicts.push(render_conflict(conflict));
    }
    json.set("conflicts", std::move(conflicts));
    json.set("reasons", strings_json(report.reasons));
    json.set("digest", digest_json(report.digest));
    return json;
}

JsonValue render_attribution(const PathAttributionReport& report) {
    JsonValue json = JsonValue::make_object();
    json.set("series", id_json(report.series));
    json.set("path", id_json(report.path));
    json.set("generation", id_json(report.generation));
    json.set("generation_ordinal", JsonValue::make_u64(report.generation_ordinal.value()));
    json.set("metric", metric_key_json(report.metric));
    json.set("metric_identity", id_json(report.metric_id));
    json.set("current_generation", JsonValue::make_bool(report.current_generation));
    json.set("complete", JsonValue::make_bool(report.complete));
    json.set("total_hops", JsonValue::make_u64(report.total_hops));
    json.set("attributed_hops", JsonValue::make_u64(report.attributed_hops));
    json.set("limitation_note", JsonValue::make_string(report.limitation_note));
    JsonValue hops = JsonValue::make_array();
    for (const HopAttribution& hop : report.hops) {
        JsonValue item = JsonValue::make_object();
        item.set("hop", id_json(hop.hop));
        item.set("index", JsonValue::make_u64(hop.index));
        item.set("name", JsonValue::make_string(hop.name));
        item.set("clock_domain", id_json(hop.clock));
        item.set("clock_comparable", JsonValue::make_bool(hop.clock_comparable));
        item.set("clock_max_offset_ns", JsonValue::make_i64(hop.clock_max_offset_ns));
        item.set("state", JsonValue::make_string(to_string(hop.state)));
        item.set("arrivals", JsonValue::make_u64(hop.arrivals));
        item.set("terms", JsonValue::make_u64(hop.terms));
        item.set("value_present", JsonValue::make_bool(hop.value_present));
        if (hop.value_present) {
            item.set("value", JsonValue::make_double(hop.value));
        } else {
            item.set("value", JsonValue::make_null());
        }
        item.set("reason", JsonValue::make_string(hop.reason));
        hops.push(std::move(item));
    }
    json.set("hops", std::move(hops));
    json.set("reasons", strings_json(report.reasons));
    json.set("digest", digest_json(report.digest));
    return json;
}

JsonValue render_explanation(const Explanation& explanation) {
    JsonValue json = JsonValue::make_object();
    json.set("subject", JsonValue::make_string(explanation.subject));
    json.set("series", id_json(explanation.series));
    json.set("path", id_json(explanation.path));
    json.set("generation", id_json(explanation.generation));
    json.set("generation_ordinal", JsonValue::make_u64(explanation.generation_ordinal.value()));
    json.set("level", JsonValue::make_string(to_string(explanation.level)));
    json.set("evidence", JsonValue::make_string(to_string(explanation.evidence)));
    json.set("current_generation", JsonValue::make_bool(explanation.current_generation));
    json.set("instability_policy", id_json(explanation.policy));
    json.set("window_policy", id_json(explanation.window));
    json.set("freshness_policy", id_json(explanation.freshness));
    JsonValue lines = JsonValue::make_array();
    for (const ExplanationLine& line : explanation.lines) {
        JsonValue item = JsonValue::make_object();
        item.set("code", JsonValue::make_string(line.code));
        item.set("detail", JsonValue::make_string(line.detail));
        lines.push(std::move(item));
    }
    json.set("lines", std::move(lines));
    json.set("reasons", strings_json(explanation.reasons));
    json.set("digest", digest_json(explanation.digest));
    return json;
}

JsonValue render_clock_domain(const ClockDomainDescriptor& domain) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(domain.id));
    json.set("name", JsonValue::make_string(domain.name));
    json.set("kind", JsonValue::make_string(to_string(domain.kind)));
    json.set("unit", JsonValue::make_string(to_string(domain.unit)));
    json.set("unit_convertible", JsonValue::make_bool(nanos_per_unit(domain.unit).has_value()));
    json.set("epoch_note", JsonValue::make_string(domain.epoch_note));
    json.set("declared_accuracy_ns", JsonValue::make_i64(domain.declared_accuracy_ns));
    json.set("declared_utc_aligned", JsonValue::make_bool(domain.declared_utc_aligned));
    json.set("owner", id_json(domain.owner));
    return json;
}

JsonValue render_source(const SourceDescriptor& source) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(source.id));
    json.set("name", JsonValue::make_string(source.name));
    json.set("authority", JsonValue::make_string(to_string(source.authority)));
    json.set("origin", JsonValue::make_string(to_string(source.origin)));
    json.set("clock_domain", id_json(source.clock_domain));
    json.set("protocol_revision", JsonValue::make_u64(source.protocol_revision));
    json.set("description", JsonValue::make_string(source.description));
    return json;
}

JsonValue render_series(const SeriesDescriptor& series) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(series.id));
    json.set("name", JsonValue::make_string(series.name));
    json.set("kind", JsonValue::make_string(to_string(series.kind)));
    json.set("unit", JsonValue::make_string(to_string(series.unit)));
    json.set("path", id_json(series.path));
    json.set("clock_domain", id_json(series.clock_domain));
    json.set("origin", JsonValue::make_string(to_string(series.origin)));
    json.set("description", JsonValue::make_string(series.description));
    return json;
}

JsonValue render_path(const PathDescriptor& path) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(path.id));
    json.set("name", JsonValue::make_string(path.name));
    json.set("origin", JsonValue::make_string(to_string(path.origin)));
    json.set("description", JsonValue::make_string(path.description));
    JsonValue hops = JsonValue::make_array();
    for (const HopDescriptor& hop : path.hops) {
        JsonValue item = JsonValue::make_object();
        item.set("hop", id_json(hop.id));
        item.set("index", JsonValue::make_u64(hop.index));
        item.set("name", JsonValue::make_string(hop.name));
        if (hop.timing_clock.has_value()) {
            item.set("timing_clock", id_json(hop.timing_clock.value()));
        } else {
            item.set("timing_clock", JsonValue::make_null());
        }
        item.set("origin", JsonValue::make_string(to_string(hop.origin)));
        item.set("device_note", JsonValue::make_string(hop.device_note));
        hops.push(std::move(item));
    }
    json.set("hops", std::move(hops));
    return json;
}

JsonValue render_generation(const PathGeneration& generation) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(generation.id));
    json.set("path", id_json(generation.path));
    json.set("ordinal", JsonValue::make_u64(generation.ordinal.value()));
    json.set("revision", JsonValue::make_u64(generation.revision.value()));
    json.set("topology", digest_json(generation.topology));
    json.set("opened_at_utc_ns", JsonValue::make_i64(generation.opened_at_utc_ns));
    json.set("closed_at_utc_ns", optional_i64_json(generation.closed_at_utc_ns));
    json.set("hop_count", JsonValue::make_u64(generation.hop_count));
    json.set("cause", JsonValue::make_string(generation.cause));
    json.set("close_reason", JsonValue::make_string(generation.close_reason));
    json.set("open", JsonValue::make_bool(generation.is_open()));
    return json;
}

JsonValue render_sample(const LatencySample& sample) {
    JsonValue json = JsonValue::make_object();
    json.set("id", id_json(sample.id));
    json.set("series", id_json(sample.series));
    json.set("path", id_json(sample.path));
    json.set("generation", id_json(sample.generation));
    json.set("generation_ordinal", JsonValue::make_u64(sample.generation_ordinal.value()));
    json.set("latency_ns", JsonValue::make_i64(sample.latency_ns));
    json.set("observed_at_ticks", JsonValue::make_i64(sample.observed_at.ticks));
    json.set("observed_clock", id_json(sample.observed_at.domain));
    json.set("received_at_utc_ns", JsonValue::make_i64(sample.received_at.ticks));
    json.set("received_clock", id_json(sample.received_at.domain));
    json.set("source", id_json(sample.provenance.source));
    json.set("incarnation", JsonValue::make_u64(sample.provenance.incarnation.value()));
    json.set("epoch", JsonValue::make_u64(sample.provenance.epoch.value()));
    json.set("sequence", JsonValue::make_u64(sample.provenance.sequence.value()));
    json.set("authority", JsonValue::make_string(to_string(sample.provenance.authority)));
    json.set("origin", JsonValue::make_string(to_string(sample.provenance.origin)));
    json.set("ingest_path", JsonValue::make_string(sample.provenance.ingest_path));
    JsonValue hops = JsonValue::make_array();
    for (const HopTiming& timing : sample.hop_timings) {
        JsonValue item = JsonValue::make_object();
        item.set("hop", id_json(timing.hop));
        item.set("arrived_ticks", JsonValue::make_i64(timing.arrived.ticks));
        item.set("clock", id_json(timing.arrived.domain));
        hops.push(std::move(item));
    }
    json.set("hop_timings", std::move(hops));
    JsonValue metadata = JsonValue::make_object();
    for (const auto& entry : sample.metadata) {
        metadata.set(entry.first, JsonValue::make_string(entry.second));
    }
    json.set("metadata", std::move(metadata));
    json.set("content", digest_json(sample.content));
    return json;
}

JsonValue render_ingest_outcome(const IngestOutcome& outcome) {
    JsonValue json = JsonValue::make_object();
    json.set("batch", id_json(outcome.batch));
    json.set("verdict", JsonValue::make_string(to_string(outcome.verdict)));
    json.set("admitted", JsonValue::make_bool(outcome.admitted()));
    json.set("accepted", JsonValue::make_u64(outcome.accepted));
    json.set("duplicates", JsonValue::make_u64(outcome.duplicates));
    json.set("historical", JsonValue::make_u64(outcome.historical));
    json.set("rejected", JsonValue::make_u64(outcome.rejected));
    json.set("window_retained", JsonValue::make_u64(outcome.window_retained));
    json.set("window_created", JsonValue::make_bool(outcome.window_created));
    json.set("conflict_recorded", JsonValue::make_bool(outcome.conflict_recorded));
    json.set("reason", JsonValue::make_string(outcome.reason));
    json.set("notes", strings_json(outcome.notes));
    return json;
}

JsonValue make_document(std::string_view kind, JsonValue payload) {
    JsonValue document = JsonValue::make_object();
    document.set("schema", JsonValue::make_u64(kExportSchemaVersion));
    document.set("kind", JsonValue::make_string(kind));
    document.set("producer", render_version());
    document.set("payload", std::move(payload));
    return document;
}

}  // namespace jitter
