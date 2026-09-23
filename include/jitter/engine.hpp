// Jitter Observatory - the observation engine.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/attribution.hpp>
#include <jitter/baseline.hpp>
#include <jitter/classification.hpp>
#include <jitter/episode.hpp>
#include <jitter/error.hpp>
#include <jitter/evidence.hpp>
#include <jitter/json.hpp>
#include <jitter/lock_audit.hpp>
#include <jitter/path.hpp>
#include <jitter/persistence.hpp>
#include <jitter/report.hpp>
#include <jitter/sample.hpp>
#include <jitter/series.hpp>
#include <jitter/source.hpp>
#include <jitter/summary.hpp>
#include <jitter/time.hpp>
#include <jitter/window.hpp>

namespace jitter {

struct EngineConfig {
    ComparabilityPolicy comparability;
    FreshnessPolicy freshness;
    WindowPolicy window;
    InstabilityPolicy policy;
    std::vector<MetricKey> metrics;
    // Identity of the current process incarnation. Persisted evidence recorded under a
    // different incarnation is reported and, by default, is not admitted as current.
    std::uint64_t boot_id = 0;
    // When false, evidence restored from a store is retained for history but is never
    // treated as current evidence. This is the conservative default.
    bool admit_persisted_evidence = false;
    // Synthetic sources are accepted but always labelled; real sources may not be
    // downgraded to synthetic and synthetic sources may not claim to be real.
    bool accept_synthetic = true;
    std::uint64_t max_windows = 4096;
    std::uint64_t max_baselines = 4096;
};

std::vector<MetricKey> default_metric_set();

struct IngestContext {
    std::int64_t now_utc_ns = 0;
    // Explicitly marks archive loading. Historical ingest never advances a source's
    // monotonic guard and never becomes current evidence.
    bool historical = false;
};

struct IngestOutcome {
    BatchId batch;
    SequenceVerdict verdict = SequenceVerdict::Accept;
    std::uint64_t accepted = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t historical = 0;
    std::uint64_t rejected = 0;
    std::uint64_t window_retained = 0;
    bool window_created = false;
    bool conflict_recorded = false;
    std::string reason;
    std::vector<std::string> notes;

    bool admitted() const noexcept { return verdict == SequenceVerdict::Accept && accepted > 0; }
};

struct SummaryOutcome {
    SeriesSummary summary;
    EpisodeUpdate episodes;
};

struct CompareRequest {
    SummaryRequest current;
    BaselineId baseline;
};

struct RestoreOptions {
    // Must be true to load a store whose tail is damaged; the tail is then cut back to
    // the last intact record and the action is reported.
    bool repair_truncated_tail = false;
    bool admit_persisted_evidence = false;
};

struct RestoreOutcome {
    std::uint64_t clock_domains = 0;
    std::uint64_t equivalences = 0;
    std::uint64_t sources = 0;
    std::uint64_t series = 0;
    std::uint64_t paths = 0;
    std::uint64_t generations = 0;
    std::uint64_t samples = 0;
    std::uint64_t baselines = 0;
    std::uint64_t episodes = 0;
    std::uint64_t open_episodes_closed = 0;
    std::uint64_t conflicts = 0;
    std::uint64_t source_guards = 0;
    std::uint64_t samples_admitted_as_current = 0;
    std::uint64_t samples_withheld_from_current = 0;
    bool foreign_incarnation = false;
    bool foreign_store_epoch = false;
    StoreRecovery recovery;
    std::vector<std::string> notes;
};

struct EngineCounters {
    std::uint64_t batches_accepted = 0;
    std::uint64_t batches_rejected = 0;
    std::uint64_t batches_duplicate = 0;
    std::uint64_t batches_historical = 0;
    std::uint64_t samples_stored = 0;
    std::uint64_t samples_duplicate = 0;
    std::uint64_t conflicts_recorded = 0;
    std::uint64_t windows_created = 0;
    std::uint64_t generations_opened = 0;
    std::uint64_t summaries_computed = 0;
    std::uint64_t episodes_opened = 0;
    std::uint64_t episodes_closed = 0;
    std::uint64_t restores = 0;
    std::uint64_t saves = 0;
};

// Owns the observation state of one runtime instance. All public methods are
// deterministic functions of the declared state plus their explicit arguments; none of
// them reads the ambient clock.
class Observatory {
public:
    explicit Observatory(EngineConfig config = {});

    // Verifies the metric table and the configuration and registers the local clock.
    Status initialize();
    bool initialized() const noexcept { return initialized_; }

    // Registration derives and writes back the content addressed identity of a
    // descriptor whose identity is still nil, so a caller always ends up holding the
    // identity the runtime actually registered.
    Status register_clock_domain(ClockDomainDescriptor& descriptor);
    Status register_source(SourceDescriptor& descriptor);
    Status register_series(SeriesDescriptor& descriptor);
    Status register_path(PathDescriptor& descriptor);
    Status declare_clock_equivalence(ClockEquivalence equivalence);

    Result<GenerationOutcome> observe_topology(PathId path, const PathTopology& topology,
                                               std::int64_t now_utc_ns, std::string_view cause);

    // Advances the metadata revision of a path's current generation. A revision change
    // never opens a generation and never segments history.
    Status bump_path_revision(PathId path, std::string_view cause);

    Result<IngestOutcome> ingest(const LatencyBatch& batch, const IngestContext& context);

    Result<SummaryOutcome> summarize(const SummaryRequest& request, const IngestContext& context,
                                     bool track_episodes);

    Result<BaselineComparisonReport> compare(const CompareRequest& request,
                                             const IngestContext& context);

    Result<HistoryReport> history(const HistoryRequest& request);

    Result<PathAttributionReport> attribute(const AttributionRequest& request,
                                            const IngestContext& context);

    Result<Explanation> explain(const SummaryRequest& request, const IngestContext& context);

    Result<Baseline> capture_baseline(const std::string& name, const SummaryRequest& request,
                                      const IngestContext& context, std::string_view note);

    // Persistence. Saving writes the full declared state; restoring rebuilds it and
    // reports every deviation rather than repairing quietly.
    Status save(const std::string& path, bool truncate_existing, const IngestContext& context);
    Result<RestoreOutcome> restore(const std::string& path, const RestoreOptions& options);

    // Configuration and counters are returned by value: they are plain data with no
    // interior references.
    EngineConfig config() const {
        LockGuard guard(mutex_);
        return config_;
    }
    EngineCounters counters() const {
        LockGuard guard(mutex_);
        return counters_;
    }

    // Catalog accessors expose engine owned state in place. They return references rather
    // than copies so that a pointer obtained from a lookup always points at the engine's
    // own storage; returning a copy would hand out pointers into a temporary. The engine
    // serialises its own mutations internally, but a caller that inspects catalogs while
    // another thread ingests must serialise that access itself, exactly as it would for
    // any other read of shared state.
    const ClockModel& clocks() const noexcept { return clocks_; }
    const SourceRegistry& sources() const noexcept { return sources_; }
    const SeriesCatalog& series_catalog() const noexcept { return series_; }
    const PathCatalog& paths() const noexcept { return paths_; }
    const EpisodeLog& episode_log() const noexcept { return episodes_; }
    const ConflictLog& conflict_log() const noexcept { return conflicts_; }

    std::vector<Baseline> baselines(SeriesId series) const;
    std::optional<Baseline> find_baseline(BaselineId id) const;
    std::size_t window_count() const {
        LockGuard guard(mutex_);
        return windows_.size();
    }
    std::uint64_t restored_evidence_count() const {
        LockGuard guard(mutex_);
        return static_cast<std::uint64_t>(restored_.size());
    }

    // Renders a document using the canonical JSON writer. The same state and the same
    // request always produce identical bytes.
    // Structured documents, so that a caller can nest them in a larger document instead
    // of embedding a JSON string inside JSON.
    Result<JsonValue> summary_document(const SummaryRequest& request, const IngestContext& context);
    Result<JsonValue> history_document(const HistoryRequest& request);
    Result<JsonValue> comparison_document(const CompareRequest& request, const IngestContext& context);
    Result<JsonValue> attribution_document(const AttributionRequest& request,
                                           const IngestContext& context);
    Result<JsonValue> catalog_document();

    Result<std::string> export_summary(const SummaryRequest& request, const IngestContext& context,
                                       bool pretty);
    Result<std::string> export_history(const HistoryRequest& request, bool pretty);
    Result<std::string> export_comparison(const CompareRequest& request, const IngestContext& context,
                                          bool pretty);
    Result<std::string> export_attribution(const AttributionRequest& request,
                                           const IngestContext& context, bool pretty);
    Result<std::string> export_catalog(bool pretty);

    // Newest receive time the engine currently holds, or nullopt when it holds no
    // evidence. Tooling uses it to pick an explicit "now" instead of the ambient clock.
    std::optional<std::int64_t> newest_received_utc_ns() const;
    // Newest observation time held for one generation, in that generation's observation
    // clock ticks.
    std::optional<std::int64_t> newest_observation_ticks(SeriesId series,
                                                         GenerationId generation) const;

private:
    // Unlocked implementations. They must never call a public entry point, which is
    // what keeps the engine lock non recursive.
    Status register_clock_domain_locked(ClockDomainDescriptor& descriptor);
    Status register_source_locked(SourceDescriptor& descriptor);
    Status register_series_locked(SeriesDescriptor& descriptor);
    Status register_path_locked(PathDescriptor& descriptor);
    Status declare_clock_equivalence_locked(ClockEquivalence equivalence);
    Result<GenerationOutcome> observe_topology_locked(PathId path, const PathTopology& topology,
                                                      std::int64_t now_utc_ns,
                                                      std::string_view cause);
    Status bump_path_revision_locked(PathId path, std::string_view cause);
    Result<IngestOutcome> ingest_locked(const LatencyBatch& batch, const IngestContext& context);
    Result<SummaryOutcome> summarize_locked(const SummaryRequest& request,
                                            const IngestContext& context, bool track_episodes);
    Result<BaselineComparisonReport> compare_locked(const CompareRequest& request,
                                                    const IngestContext& context);
    Result<HistoryReport> history_locked(const HistoryRequest& request);
    Result<PathAttributionReport> attribute_locked(const AttributionRequest& request,
                                                   const IngestContext& context);
    Result<Explanation> explain_locked(const SummaryRequest& request, const IngestContext& context);
    Result<Baseline> capture_baseline_locked(const std::string& name, const SummaryRequest& request,
                                             const IngestContext& context, std::string_view note);
    Status save_locked(const std::string& path, bool truncate_existing, const IngestContext& context);
    Result<RestoreOutcome> restore_locked(const std::string& path, const RestoreOptions& options);
    Result<JsonValue> summary_document_locked(const SummaryRequest& request,
                                              const IngestContext& context);
    Result<JsonValue> history_document_locked(const HistoryRequest& request);
    Result<JsonValue> comparison_document_locked(const CompareRequest& request,
                                                 const IngestContext& context);
    Result<JsonValue> attribution_document_locked(const AttributionRequest& request,
                                                  const IngestContext& context);
    Result<JsonValue> catalog_document_locked();
    Result<std::string> export_summary_locked(const SummaryRequest& request,
                                              const IngestContext& context, bool pretty);
    Result<std::string> export_history_locked(const HistoryRequest& request, bool pretty);
    Result<std::string> export_comparison_locked(const CompareRequest& request,
                                                 const IngestContext& context, bool pretty);
    Result<std::string> export_attribution_locked(const AttributionRequest& request,
                                                  const IngestContext& context, bool pretty);
    Result<std::string> export_catalog_locked(bool pretty);
    struct WindowKey {
        SeriesId series;
        GenerationId generation;
        friend bool operator<(const WindowKey& a, const WindowKey& b) noexcept {
            if (a.series != b.series) {
                return a.series < b.series;
            }
            return a.generation < b.generation;
        }
    };

    const Baseline* find_baseline_locked(BaselineId id) const noexcept;
    Result<SampleWindow*> window_for(SeriesId series, GenerationId generation, bool create);
    Result<const SampleWindow*> find_window(SeriesId series, GenerationId generation) const;
    SummaryRequest make_request(SeriesId series, GenerationId generation,
                                const IngestContext& context) const;
    SampleAssessment assess(const LatencySample& sample, const IngestContext& context) const;
    Status store_samples(SampleWindow& window, const LatencyBatch& batch, IngestOutcome& outcome);

    mutable Mutex mutex_;
    EngineConfig config_;
    bool initialized_ = false;
    ClockModel clocks_;
    SourceRegistry sources_;
    SeriesCatalog series_;
    PathCatalog paths_;
    std::map<WindowKey, SampleWindow> windows_;
    std::map<SourceId, SourceGuardState> guards_;
    std::map<SeriesId, std::vector<Baseline>> baselines_;
    std::set<MeasurementId> restored_;
    ConflictLog conflicts_;
    EpisodeLog episodes_;
    EngineCounters counters_;
    bool has_clock_equivalence_ = false;
};

}  // namespace jitter
