// Jitter Observatory - canonical JSON rendering of runtime results.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <jitter/attribution.hpp>
#include <jitter/baseline.hpp>
#include <jitter/classification.hpp>
#include <jitter/engine.hpp>
#include <jitter/episode.hpp>
#include <jitter/json.hpp>
#include <jitter/report.hpp>
#include <jitter/source.hpp>
#include <jitter/summary.hpp>
#include <jitter/time.hpp>

namespace jitter {

JsonValue render_version();
JsonValue render_metric_value(const MetricValue& value);
JsonValue render_evidence_summary(const EvidenceSummary& summary);
JsonValue render_classification(const Classification& classification);
JsonValue render_summary(const SeriesSummary& summary);
JsonValue render_episode(const Episode& episode);
JsonValue render_conflict(const ConflictRecord& conflict);
JsonValue render_baseline(const Baseline& baseline);
JsonValue render_comparison(const BaselineComparisonReport& report);
JsonValue render_history(const HistoryReport& report);
JsonValue render_attribution(const PathAttributionReport& report);
JsonValue render_explanation(const Explanation& explanation);
JsonValue render_clock_domain(const ClockDomainDescriptor& domain);
JsonValue render_source(const SourceDescriptor& source);
JsonValue render_series(const SeriesDescriptor& series);
JsonValue render_path(const PathDescriptor& path);
JsonValue render_generation(const PathGeneration& generation);
JsonValue render_metric_definition(const MetricDefinition& definition);
JsonValue render_sample(const LatencySample& sample);
JsonValue render_ingest_outcome(const IngestOutcome& outcome);
JsonValue render_evidence_assessment(const SampleAssessment& assessment);

// Stable machine readable envelope shared by every exported document.
JsonValue make_document(std::string_view kind, JsonValue payload);

}  // namespace jitter
