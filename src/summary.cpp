// Jitter Observatory - deterministic summaries.
// Copyright 2026 Summon Software Labs.
#include <jitter/summary.hpp>

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

const MetricValue* SeriesSummary::find(MetricId metric_id) const noexcept {
    for (const MetricValue& value : metrics) {
        if (value.id == metric_id) {
            return &value;
        }
    }
    return nullptr;
}

const MetricValue* SeriesSummary::find(MetricKey metric_key) const noexcept {
    for (const MetricValue& value : metrics) {
        if (value.key == metric_key) {
            return &value;
        }
    }
    return nullptr;
}

Result<SeriesSummary> summarize(const SummaryInput& input) {
    if (input.assessments.size() != input.selection.samples.size()) {
        return Result<SeriesSummary>::fail(
            ErrorCode::InvalidArgument,
            "summary requires exactly one evidence assessment per selected observation",
            "assessments=" + std::to_string(input.assessments.size()) +
                " samples=" + std::to_string(input.selection.samples.size()));
    }

    SeriesSummary summary;
    summary.series = input.request.series;
    summary.path = input.request.path;
    summary.generation = input.request.generation;
    summary.generation_ordinal = input.request.generation_ordinal;
    summary.window = make_window_policy_id(input.request.window);
    summary.policy = make_instability_policy_id(input.request.policy);
    summary.freshness_policy = make_freshness_policy_id(input.request.policy.freshness);
    summary.current_generation = input.request.current_generation;
    summary.retained = input.retained == 0 ? input.selection.retained : input.retained;
    summary.excluded_by_policy = input.selection.excluded_by_policy;
    summary.evicted_by_capacity = input.evicted_by_capacity;
    summary.out_of_order_accepted = input.out_of_order_accepted;
    summary.duplicate_ignored = input.duplicate_ignored;
    summary.selection_reason = input.selection.selection_reason;

    summary.evidence = summarize_evidence(input.assessments);
    summary.fresh_samples = summary.evidence.fresh;

    for (std::size_t i = 0; i < input.selection.samples.size(); ++i) {
        const LatencySample& sample = *input.selection.samples[i];
        if (sample.provenance.origin == EvidenceOrigin::Real) {
            ++summary.evidence.real_origin;
        } else if (sample.provenance.origin == EvidenceOrigin::Synthetic) {
            ++summary.evidence.synthetic_origin;
        }
        if (!summary.first_observation_ticks.has_value() ||
            sample.observed_at.ticks < summary.first_observation_ticks.value()) {
            summary.first_observation_ticks = sample.observed_at.ticks;
        }
        if (!summary.last_observation_ticks.has_value() ||
            sample.observed_at.ticks > summary.last_observation_ticks.value()) {
            summary.last_observation_ticks = sample.observed_at.ticks;
        }
        if (!summary.first_received_utc_ns.has_value() ||
            sample.received_at.ticks < summary.first_received_utc_ns.value()) {
            summary.first_received_utc_ns = sample.received_at.ticks;
        }
        if (!summary.last_received_utc_ns.has_value() ||
            sample.received_at.ticks > summary.last_received_utc_ns.value()) {
            summary.last_received_utc_ns = sample.received_at.ticks;
        }
    }

    if (summary.evidence.real_origin > 0 && summary.evidence.synthetic_origin > 0) {
        summary.dominant_origin = EvidenceOrigin::Unknown;
        add_reason(summary.reasons, "mixed_evidence_origin");
    } else if (summary.evidence.synthetic_origin > 0) {
        summary.dominant_origin = EvidenceOrigin::Synthetic;
    } else if (summary.evidence.real_origin > 0) {
        summary.dominant_origin = EvidenceOrigin::Real;
    } else {
        summary.dominant_origin = EvidenceOrigin::Unknown;
    }

    // Only fresh observations may influence a number.
    std::vector<std::int64_t> fresh_latencies;
    fresh_latencies.reserve(input.selection.samples.size());
    for (std::size_t i = 0; i < input.selection.samples.size(); ++i) {
        if (input.assessments[i].state == EvidenceState::Fresh) {
            fresh_latencies.push_back(input.selection.samples[i]->latency_ns);
        }
    }
    summary.selected = static_cast<std::uint64_t>(input.selection.samples.size());

    bool previous_ticks_valid = false;
    std::int64_t previous_ticks = 0;
    bool monotonic = true;
    for (const LatencySample* sample : input.selection.samples) {
        if (previous_ticks_valid && sample->observed_at.ticks < previous_ticks) {
            monotonic = false;
        }
        previous_ticks = sample->observed_at.ticks;
        previous_ticks_valid = true;
    }
    if (!monotonic) {
        add_reason(summary.reasons, "selection_order_was_repaired");
    }

    const MetricRegistry& registry = metric_registry();
    const auto computed = registry.compute_all(input.request.metrics, fresh_latencies);
    summary.metrics = computed.values.ordered();
    summary.unavailable_metrics = computed.unavailable;
    for (const std::string& unavailable : summary.unavailable_metrics) {
        add_reason(summary.reasons, "metric_unavailable:" + unavailable);
    }

    ClassificationInput classification_input;
    classification_input.series = summary.series;
    classification_input.path = summary.path;
    classification_input.generation = summary.generation;
    classification_input.generation_ordinal = summary.generation_ordinal;
    classification_input.policy = input.request.policy;
    classification_input.window = summary.window;
    classification_input.evidence = summary.evidence;
    classification_input.metrics = computed.values;
    classification_input.metric_diagnostics = computed.unavailable;
    classification_input.current_generation = summary.current_generation;
    summary.classification = classify(classification_input);

    for (const std::string& reason : summary.evidence.reasons) {
        add_reason(summary.reasons, "evidence:" + reason);
    }
    for (const std::string& reason : summary.classification.reasons) {
        add_reason(summary.reasons, "classification:" + reason);
    }
    add_reason(summary.reasons, std::string("level:") + std::string(to_string(summary.classification.level)));
    std::sort(summary.reasons.begin(), summary.reasons.end());
    summary.reasons.erase(std::unique(summary.reasons.begin(), summary.reasons.end()), summary.reasons.end());

    DigestBuilder builder(kDomainSummary);
    builder.id(summary.series);
    builder.id(summary.path);
    builder.id(summary.generation);
    builder.u64(summary.generation_ordinal.value());
    builder.id(summary.window);
    builder.id(summary.policy);
    builder.id(summary.freshness_policy);
    builder.boolean(summary.current_generation);
    builder.u64(summary.retained);
    builder.u64(summary.selected);
    builder.u64(summary.excluded_by_policy);
    builder.u64(summary.evicted_by_capacity);
    builder.u64(summary.out_of_order_accepted);
    builder.u64(summary.duplicate_ignored);
    builder.optional_i64(summary.first_observation_ticks);
    builder.optional_i64(summary.last_observation_ticks);
    builder.optional_i64(summary.first_received_utc_ns);
    builder.optional_i64(summary.last_received_utc_ns);
    builder.str(summary.selection_reason);
    builder.u8(static_cast<std::uint8_t>(summary.dominant_origin));
    builder.u64(summary.evidence.total);
    builder.u64(summary.evidence.fresh);
    builder.u64(summary.evidence.stale);
    builder.u64(summary.evidence.expired);
    builder.u64(summary.evidence.conflicting);
    builder.u64(summary.evidence.unsupported);
    builder.u64(summary.evidence.unknown);
    builder.u64(summary.evidence.real_origin);
    builder.u64(summary.evidence.synthetic_origin);
    builder.u32(static_cast<std::uint32_t>(summary.metrics.size()));
    for (const MetricValue& value : summary.metrics) {
        builder.id(value.id);
        builder.u32(value.version);
        builder.f64(value.value);
        builder.u64(value.sample_count);
        builder.u64(value.term_count);
    }
    for (const std::string& reason : summary.reasons) {
        builder.str(reason);
    }
    summary.digest = builder.digest();
    summary.id = SummaryId::from_digest(summary.digest);

    DigestBuilder measurement("jitter.summary.measurement.v1");
    measurement.id(summary.series);
    measurement.id(summary.path);
    measurement.id(summary.generation);
    measurement.u64(summary.generation_ordinal.value());
    measurement.id(summary.window);
    measurement.id(summary.policy);
    measurement.id(summary.freshness_policy);
    measurement.boolean(summary.current_generation);
    measurement.u64(summary.selected);
    measurement.u64(summary.excluded_by_policy);
    measurement.optional_i64(summary.first_observation_ticks);
    measurement.optional_i64(summary.last_observation_ticks);
    measurement.u64(summary.evidence.total);
    measurement.u64(summary.evidence.fresh);
    measurement.u64(summary.evidence.stale);
    measurement.u64(summary.evidence.expired);
    measurement.u64(summary.evidence.conflicting);
    measurement.u64(summary.evidence.unsupported);
    measurement.u64(summary.evidence.unknown);
    measurement.u64(summary.evidence.real_origin);
    measurement.u64(summary.evidence.synthetic_origin);
    measurement.u8(static_cast<std::uint8_t>(summary.evidence.dominant));
    measurement.u8(static_cast<std::uint8_t>(summary.classification.level));
    measurement.u32(static_cast<std::uint32_t>(summary.metrics.size()));
    for (const MetricValue& value : summary.metrics) {
        measurement.id(value.id);
        measurement.u32(value.version);
        measurement.f64(value.value);
        measurement.u64(value.sample_count);
        measurement.u64(value.term_count);
    }
    summary.measurement_digest = measurement.digest();
    return summary;
}

}  // namespace jitter
