// Jitter Observatory - the observation engine.
// Copyright 2026 Summon Software Labs.
#include <jitter/engine.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/codec.hpp>
#include <jitter/checked.hpp>
#include <jitter/metrics.hpp>
#include <jitter/render.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::uint64_t kMaxTrackedPositions = 65536;

struct PositionKey {
    SeriesId series;
    GenerationId generation;
    SourceSequence sequence;

    friend bool operator<(const PositionKey& a, const PositionKey& b) noexcept {
        if (a.series != b.series) return a.series < b.series;
        if (a.generation != b.generation) return a.generation < b.generation;
        return a.sequence < b.sequence;
    }
};

struct PositionValue {
    SourceId source;
    SourceAuthority authority = SourceAuthority::Unknown;
    Digest content;
};

// A source may publish evidence that is weaker than what it declared, never stronger.
Status check_origin_consistency(EvidenceOrigin declared, EvidenceOrigin claimed) {
    if (claimed == EvidenceOrigin::Unknown) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "a batch must declare whether its evidence is real or synthetic");
    }
    if (claimed == EvidenceOrigin::Unsupported) {
        return Status::failure(ErrorCode::Unsupported,
                               "batch declares an evidence origin this runtime cannot support");
    }
    if (declared == EvidenceOrigin::Unknown) {
        return Status::failure(ErrorCode::PolicyViolation,
                               "the registered source does not declare an evidence origin");
    }
    if (declared == EvidenceOrigin::Unsupported) {
        return Status::failure(ErrorCode::Unsupported,
                               "the registered source declares an unsupported evidence origin");
    }
    if (declared == EvidenceOrigin::Synthetic && claimed == EvidenceOrigin::Real) {
        return Status::failure(ErrorCode::PolicyViolation,
                               "a synthetic source may not claim real evidence",
                               "declared=synthetic claimed=real");
    }
    return Status::success();
}

void add_note(std::vector<std::string>& notes, const std::string& note) {
    if (note.empty()) {
        return;
    }
    if (std::find(notes.begin(), notes.end(), note) == notes.end()) {
        notes.push_back(note);
    }
}

}  // namespace

std::vector<MetricKey> default_metric_set() {
    return {
        MetricKey::AbsoluteDeltaMean,   MetricKey::AbsoluteDeltaP95,
        MetricKey::AbsoluteDeltaMax,    MetricKey::AbsoluteDeltaMedian,
        MetricKey::AbsoluteDeltaEwma16, MetricKey::IpdvMean,
        MetricKey::IpdvStdDev,          MetricKey::SampleStdDev,
        MetricKey::SampleVariance,      MetricKey::CoefficientOfVariation,
        MetricKey::MeanAbsoluteDeviation, MetricKey::MedianAbsoluteDeviation,
        MetricKey::PeakToPeak,          MetricKey::InterquartileRange,
        MetricKey::LatencyMean,         MetricKey::LatencyP95,
    };
}

Observatory::Observatory(EngineConfig config)
    : config_(std::move(config)), clocks_(config_.comparability), paths_(clocks_) {
    if (config_.metrics.empty()) {
        config_.metrics = default_metric_set();
    }
}

Status Observatory::initialize() {
    JITTER_TRY(verify_metric_table());
    JITTER_TRY(validate_window_policy(config_.window));
    JITTER_TRY(validate_freshness_policy(config_.freshness));
    JITTER_TRY(validate_instability_policy(config_.policy));
    if (config_.metrics.size() > Limits::kMaxWatchedMetrics) {
        return Status::failure(ErrorCode::LimitExceeded,
                               "configured metric list exceeds the supported bound",
                               std::to_string(config_.metrics.size()));
    }
    for (const MetricKey key : config_.metrics) {
        if (key == MetricKey::Count) {
            return Status::failure(ErrorCode::InvalidArgument, "configured metric list names an invalid key");
        }
    }
    if (config_.max_windows == 0 || config_.max_baselines == 0) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "engine requires non zero window and baseline bounds");
    }
    ClockDomainDescriptor local = LocalClockDomain::descriptor();
    JITTER_TRY(clocks_.register_domain(local));
    initialized_ = true;
    return Status::success();
}

Status Observatory::register_clock_domain_locked(ClockDomainDescriptor& descriptor) {
    return clocks_.register_domain(descriptor);
}

Status Observatory::register_source_locked(SourceDescriptor& descriptor) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_source_id(descriptor);
    }
    if (!descriptor.clock_domain.is_nil() && !clocks_.has_domain(descriptor.clock_domain)) {
        return Status::failure(ErrorCode::NotFound,
                               "source references an unregistered clock domain",
                               descriptor.clock_domain.hex());
    }
    if (descriptor.origin == EvidenceOrigin::Synthetic && !config_.accept_synthetic) {
        return Status::failure(ErrorCode::PolicyViolation,
                               "this configuration does not accept synthetic sources");
    }
    return sources_.register_source(descriptor);
}

Status Observatory::register_series_locked(SeriesDescriptor& descriptor) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_series_id(descriptor);
    }
    if (paths_.find(descriptor.path) == nullptr) {
        return Status::failure(ErrorCode::NotFound, "series references an unregistered path",
                               descriptor.path.hex());
    }
    if (!clocks_.has_domain(descriptor.clock_domain)) {
        return Status::failure(ErrorCode::NotFound, "series references an unregistered clock domain",
                               descriptor.clock_domain.hex());
    }
    return series_.register_series(descriptor);
}

Status Observatory::register_path_locked(PathDescriptor& descriptor) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_path_id(descriptor);
    }
    for (const HopDescriptor& hop : descriptor.hops) {
        if (hop.timing_clock.has_value() && !clocks_.has_domain(hop.timing_clock.value())) {
            return Status::failure(ErrorCode::NotFound,
                                   "hop declares an unregistered timing clock domain",
                                   hop.name + ":" + hop.timing_clock.value().hex());
        }
    }
    return paths_.register_path(descriptor);
}

Status Observatory::declare_clock_equivalence_locked(ClockEquivalence equivalence) {
    JITTER_TRY(clocks_.declare_equivalence(equivalence));
    has_clock_equivalence_ = true;
    return Status::success();
}

Result<GenerationOutcome> Observatory::observe_topology_locked(PathId path, const PathTopology& topology,
                                                        std::int64_t now_utc_ns,
                                                        std::string_view cause) {
    auto outcome = paths_.observe_topology(path, topology, now_utc_ns, cause);
    if (!outcome.ok()) {
        return outcome.error();
    }
    if (outcome.value().created_new) {
        ++counters_.generations_opened;
        // History is segmented: every open episode of a series on this path that
        // belongs to an older generation is closed rather than extended.
        for (const SeriesDescriptor& descriptor : series_.series()) {
            if (descriptor.path != path) {
                continue;
            }
            const auto closed = episodes_.retire(descriptor.id, outcome.value().generation.id,
                                                 make_instability_policy_id(config_.policy),
                                                 make_window_policy_id(config_.window), now_utc_ns);
            counters_.episodes_closed += static_cast<std::uint64_t>(closed.size());
        }
    }
    return outcome;
}

std::optional<std::int64_t> Observatory::newest_received_utc_ns() const {
    LockGuard guard(mutex_);
    bool found = false;
    std::int64_t newest = 0;
    for (const auto& entry : windows_) {
        for (const LatencySample& sample : entry.second.snapshot()) {
            if (!found || sample.received_at.ticks > newest) {
                newest = sample.received_at.ticks;
                found = true;
            }
        }
    }
    if (!found) {
        return std::nullopt;
    }
    return newest;
}

std::optional<std::int64_t> Observatory::newest_observation_ticks(
    SeriesId series, GenerationId generation) const {
    LockGuard guard(mutex_);
    const WindowKey key{series, generation};
    const auto it = windows_.find(key);
    if (it == windows_.end()) {
        return std::nullopt;
    }
    return it->second.newest_observation_ticks();
}

Result<SampleWindow*> Observatory::window_for(SeriesId series, GenerationId generation, bool create) {
    const WindowKey key{series, generation};
    const auto it = windows_.find(key);
    if (it != windows_.end()) {
        return &it->second;
    }
    if (!create) {
        return Result<SampleWindow*>::fail(ErrorCode::NotFound,
                                           "no window exists for this series and generation",
                                           series.hex() + "/" + generation.hex());
    }
    if (windows_.size() >= config_.max_windows) {
        return Result<SampleWindow*>::fail(ErrorCode::CapacityExceeded,
                                           "engine holds as many windows as its bound allows",
                                           std::to_string(windows_.size()));
    }
    auto inserted = windows_.emplace(key, SampleWindow(config_.window));
    ++counters_.windows_created;
    return &inserted.first->second;
}

Result<const SampleWindow*> Observatory::find_window(SeriesId series, GenerationId generation) const {
    const WindowKey key{series, generation};
    const auto it = windows_.find(key);
    if (it == windows_.end()) {
        return Result<const SampleWindow*>::fail(ErrorCode::NotFound,
                                                 "no window exists for this series and generation",
                                                 series.hex() + "/" + generation.hex());
    }
    return &it->second;
}

Status Observatory::store_samples(SampleWindow& window, const LatencyBatch& batch,
                                  IngestOutcome& outcome) {
    for (const LatencySample& sample : batch.samples) {
        const Status status = window.add(sample);
        if (!status.ok()) {
            return status;
        }
    }
    outcome.window_retained = static_cast<std::uint64_t>(window.retained());
    return Status::success();
}

Result<IngestOutcome> Observatory::ingest_locked(const LatencyBatch& batch, const IngestContext& context) {
    if (!initialized_) {
        return Result<IngestOutcome>::fail(ErrorCode::NotReady,
                                           "engine must be initialized before ingest");
    }
    JITTER_TRY(validate_batch(batch));

    IngestOutcome outcome;
    outcome.batch = batch.id;
    const std::uint64_t sample_count = static_cast<std::uint64_t>(batch.samples.size());

    const SourceDescriptor* source = sources_.find(batch.header.source);
    if (source == nullptr) {
        return Result<IngestOutcome>::fail(ErrorCode::NotFound, "batch arrives from an unknown source",
                                           batch.header.source.hex());
    }
    if (batch.header.protocol_revision != source->protocol_revision) {
        return Result<IngestOutcome>::fail(
            ErrorCode::VersionUnsupported, "batch protocol revision does not match the registered source",
            std::to_string(batch.header.protocol_revision) + "/" +
                std::to_string(source->protocol_revision));
    }
    if (batch.header.authority != source->authority) {
        return Result<IngestOutcome>::fail(
            ErrorCode::PolicyViolation,
            "batch claims an authority that is not registered for its source",
            std::string(to_string(batch.header.authority)) + "/" +
                std::string(to_string(source->authority)));
    }
    JITTER_TRY(check_origin_consistency(source->origin, batch.header.origin));
    if (batch.header.origin == EvidenceOrigin::Synthetic && !config_.accept_synthetic) {
        return Result<IngestOutcome>::fail(ErrorCode::PolicyViolation,
                                           "this configuration does not accept synthetic evidence");
    }

    const SeriesDescriptor* series = series_.find(batch.header.series);
    if (series == nullptr) {
        return Result<IngestOutcome>::fail(ErrorCode::NotFound, "batch names an unknown series",
                                           batch.header.series.hex());
    }
    if (series->path != batch.header.path) {
        return Result<IngestOutcome>::fail(ErrorCode::Conflict,
                                           "batch series does not belong to the batch path",
                                           series->path.hex() + "/" + batch.header.path.hex());
    }
    if (series->clock_domain != batch.header.observation_clock) {
        return Result<IngestOutcome>::fail(
            ErrorCode::Conflict, "batch observation clock does not match the series clock domain",
            series->clock_domain.hex() + "/" + batch.header.observation_clock.hex());
    }

    if (paths_.find(batch.header.path) == nullptr) {
        return Result<IngestOutcome>::fail(ErrorCode::NotFound, "batch names an unknown path",
                                           batch.header.path.hex());
    }
    const PathGeneration* generation = paths_.find_generation(batch.header.generation);
    if (generation == nullptr) {
        return Result<IngestOutcome>::fail(ErrorCode::StaleGeneration,
                                           "batch names a generation this runtime never opened",
                                           batch.header.generation.hex());
    }
    if (generation->ordinal != batch.header.generation_ordinal) {
        return Result<IngestOutcome>::fail(ErrorCode::StaleGeneration,
                                           "batch generation ordinal does not match the generation",
                                           batch.header.generation_ordinal.to_string_value());
    }
    if (!generation->is_open()) {
        return Result<IngestOutcome>::fail(
            ErrorCode::StaleGeneration,
            "batch targets a closed generation and cannot be current evidence",
            batch.header.generation.hex());
    }

    for (const LatencySample& sample : batch.samples) {
        if (sample.provenance.origin != batch.header.origin ||
            sample.provenance.authority != batch.header.authority) {
            return Result<IngestOutcome>::fail(
                ErrorCode::InvalidArgument,
                "a sample's declared origin or authority differs from its batch header",
                sample.id.hex());
        }
    }

    Provenance provenance;
    provenance.source = batch.header.source;
    provenance.incarnation = batch.header.incarnation;
    provenance.epoch = batch.header.epoch;
    provenance.sequence = batch.header.sequence;
    provenance.authority = batch.header.authority;
    provenance.origin = batch.header.origin;
    provenance.ingest_path = batch.header.ingest_path;

    SourceGuardState& guard = guards_[batch.header.source];
    const SequenceDecision decision =
        evaluate_sequence(guard, provenance, batch.content, context.historical);
    outcome.verdict = decision.verdict;
    outcome.reason = decision.reason;

    switch (decision.verdict) {
        case SequenceVerdict::DuplicateIdempotent: {
            outcome.duplicates = sample_count;
            ++counters_.batches_duplicate;
            counters_.samples_duplicate += sample_count;
            return outcome;
        }
        case SequenceVerdict::SequenceConflict: {
            ConflictRecord record;
            record.series = batch.header.series;
            record.path = batch.header.path;
            record.generation = batch.header.generation;
            record.sequence = batch.header.sequence;
            record.source_a = batch.header.source;
            record.source_b = batch.header.source;
            record.content_a = guard.last_content;
            record.content_b = batch.content;
            record.authority_a = batch.header.authority;
            record.authority_b = batch.header.authority;
            record.detected_at_utc_ns = context.now_utc_ns;
            record = make_conflict_record(record);
            if (conflicts_.append(record)) {
                ++counters_.conflicts_recorded;
                outcome.conflict_recorded = true;
            }
            add_note(outcome.notes, "contradiction_recorded_without_resolution");
            outcome.rejected = sample_count;
            ++counters_.batches_rejected;
            return outcome;
        }
        case SequenceVerdict::StaleIncarnation:
        case SequenceVerdict::StaleEpoch:
        case SequenceVerdict::StaleSequence: {
            outcome.rejected = sample_count;
            outcome.notes.push_back("replay_fenced:" + decision.reason);
            ++counters_.batches_rejected;
            return outcome;
        }
        case SequenceVerdict::Historical:
        case SequenceVerdict::Accept:
            break;
    }

    // Cross-source position tracking: two sources claiming the same position in a
    // series with different content is a contradiction, and authority decides.
    static_cast<void>(kMaxTrackedPositions);

    auto window = window_for(batch.header.series, batch.header.generation, true);
    if (!window.ok()) {
        return window.error();
    }
    const bool created = window.value()->retained() == 0;
    outcome.window_created = created && decision.verdict == SequenceVerdict::Accept;

    const Status stored = store_samples(*window.value(), batch, outcome);
    if (!stored.ok()) {
        return stored.error();
    }

    if (decision.verdict == SequenceVerdict::Historical) {
        outcome.historical = sample_count;
        ++counters_.batches_historical;
        add_note(outcome.notes, "historical_ingest_does_not_advance_the_source_guard");
    } else {
        apply_sequence(guard, provenance, batch.content);
        outcome.accepted = sample_count;
        ++counters_.batches_accepted;
    }
    counters_.samples_stored += sample_count;
    return outcome;
}

SampleAssessment Observatory::assess(const LatencySample& sample, const IngestContext& context) const {
    SampleAssessment assessment = assess_sample(sample, clocks_, context.now_utc_ns, config_.freshness);
    if (!config_.admit_persisted_evidence && restored_.find(sample.id) != restored_.end()) {
        assessment.state = EvidenceState::Stale;
        assessment.reason = "restored_evidence_is_not_admitted_as_current";
    }
    return assessment;
}

Result<SummaryOutcome> Observatory::summarize_locked(const SummaryRequest& request,
                                              const IngestContext& context, bool track_episodes) {
    if (!initialized_) {
        return Result<SummaryOutcome>::fail(ErrorCode::NotReady,
                                            "engine must be initialized before summarize");
    }
    auto window = find_window(request.series, request.generation);
    if (!window.ok()) {
        return window.error();
    }
    const WindowSelection selection = window.value()->select();

    std::vector<SampleAssessment> assessments;
    assessments.reserve(selection.samples.size());
    for (const LatencySample* sample : selection.samples) {
        assessments.push_back(assess(*sample, context));
    }

    SummaryInput input;
    input.request = request;
    input.request.window = window.value()->policy();
    if (input.request.metrics.empty()) {
        input.request.metrics = config_.metrics;
    }
    // The caller cannot declare a closed generation to be the current one: the path
    // catalog knows which generation is open, so the engine supplies that answer.
    input.request.current_generation = paths_.is_current(request.generation);
    input.selection = selection;
    input.assessments = std::move(assessments);
    input.retained = static_cast<std::uint64_t>(window.value()->retained());
    input.evicted_by_capacity = window.value()->evicted_by_capacity();
    input.out_of_order_accepted = window.value()->out_of_order_accepted();
    input.duplicate_ignored = window.value()->duplicate_ignored();

    auto summary = jitter::summarize(input);
    if (!summary.ok()) {
        return summary.error();
    }
    ++counters_.summaries_computed;

    SummaryOutcome outcome;
    outcome.summary = std::move(summary.value());
    if (track_episodes) {
        outcome.episodes = episodes_.observe(outcome.summary.classification, context.now_utc_ns);
        if (outcome.episodes.opened.has_value()) {
            ++counters_.episodes_opened;
        }
        if (outcome.episodes.closed.has_value()) {
            ++counters_.episodes_closed;
        }
    }
    return outcome;
}

Result<BaselineComparisonReport> Observatory::compare_locked(const CompareRequest& request,
                                                      const IngestContext& context) {
    auto outcome = summarize_locked(request.current, context, false);
    if (!outcome.ok()) {
        return outcome.error();
    }
    const Baseline* baseline = find_baseline_locked(request.baseline);
    if (baseline == nullptr) {
        return Result<BaselineComparisonReport>::fail(ErrorCode::NotFound, "unknown baseline identity",
                                                      request.baseline.hex());
    }
    CurrentSnapshot snapshot;
    snapshot.series = outcome.value().summary.series;
    snapshot.path = outcome.value().summary.path;
    snapshot.generation = outcome.value().summary.generation;
    snapshot.generation_ordinal = outcome.value().summary.generation_ordinal;
    snapshot.window = outcome.value().summary.window;
    snapshot.policy = outcome.value().summary.policy;
    snapshot.sample_count = outcome.value().summary.selected;
    snapshot.evidence = outcome.value().summary.evidence;
    for (const MetricValue& value : outcome.value().summary.metrics) {
        snapshot.metrics.insert(value);
    }
    return compare_to_baseline(*baseline, snapshot);
}

Result<HistoryReport> Observatory::history_locked(const HistoryRequest& request) {
    if (request.path.is_nil()) {
        return Result<HistoryReport>::fail(ErrorCode::InvalidArgument,
                                           "history request requires a path identity");
    }
    HistoryInput input;
    input.request = request;
    input.generations = paths_.generations(request.path);
    if (input.generations.size() > Limits::kMaxGenerationHistoryPerPath) {
        input.generations_dropped =
            static_cast<std::uint64_t>(input.generations.size()) - Limits::kMaxGenerationHistoryPerPath;
    }
    input.episodes = episodes_.episodes(request.series);
    input.episodes_dropped = episodes_.dropped();
    input.conflicts = conflicts_.for_series(request.series);
    input.conflicts_dropped = conflicts_.dropped();
    const PathGeneration* current = paths_.current(request.path);
    if (current != nullptr) {
        input.current_generation = current->id;
    }
    for (const PathGeneration& generation : input.generations) {
        const WindowKey key{request.series, generation.id};
        const auto it = windows_.find(key);
        input.retained_observations.emplace_back(
            generation.id, it == windows_.end() ? 0u : static_cast<std::uint64_t>(it->second.retained()));
    }
    return build_history_report(input);
}

Result<PathAttributionReport> Observatory::attribute_locked(const AttributionRequest& request,
                                                     const IngestContext& context) {
    const SeriesDescriptor* series = series_.find(request.series);
    if (series == nullptr) {
        return Result<PathAttributionReport>::fail(ErrorCode::NotFound, "unknown series identity",
                                                   request.series.hex());
    }
    const PathDescriptor* path = paths_.find(series->path);
    if (path == nullptr) {
        return Result<PathAttributionReport>::fail(ErrorCode::NotFound, "series path is not registered",
                                                   series->path.hex());
    }
    const PathGeneration* generation = paths_.current(series->path);
    if (generation == nullptr) {
        return Result<PathAttributionReport>::fail(ErrorCode::NotReady,
                                                   "path has no generation to attribute");
    }
    auto window = find_window(request.series, generation->id);
    if (!window.ok()) {
        return window.error();
    }
    const WindowSelection selection = window.value()->select();
    auto attribution = build_attribution_request(selection, *series, *path, clocks_, request.metric,
                                                 request.min_arrivals, generation->is_open(),
                                                 context.now_utc_ns);
    if (!attribution.ok()) {
        return attribution.error();
    }
    return attribute_hops(attribution.value());
}

Result<Explanation> Observatory::explain_locked(const SummaryRequest& request, const IngestContext& context) {
    auto outcome = summarize_locked(request, context, false);
    if (!outcome.ok()) {
        return outcome.error();
    }
    const SampleWindow* window = find_window(request.series, request.generation).value();
    return explain_summary(outcome.value().summary, window->policy(), request.policy);
}

Result<Baseline> Observatory::capture_baseline_locked(const std::string& name, const SummaryRequest& request,
                                               const IngestContext& context, std::string_view note) {
    auto outcome = summarize_locked(request, context, false);
    if (!outcome.ok()) {
        return outcome.error();
    }
    std::size_t total = 0;
    for (const auto& entry : baselines_) {
        total += entry.second.size();
    }
    if (total >= config_.max_baselines) {
        return Result<Baseline>::fail(ErrorCode::CapacityExceeded,
                                      "engine holds as many baselines as its bound allows",
                                      std::to_string(total));
    }
    Baseline baseline;
    baseline.name = name;
    baseline.series = outcome.value().summary.series;
    baseline.path = outcome.value().summary.path;
    baseline.generation = outcome.value().summary.generation;
    baseline.generation_ordinal = outcome.value().summary.generation_ordinal;
    baseline.window = outcome.value().summary.window;
    baseline.policy = outcome.value().summary.policy;
    baseline.sample_count = outcome.value().summary.selected;
    baseline.fresh_sample_count = outcome.value().summary.fresh_samples;
    baseline.first_observation_utc_ns = outcome.value().summary.first_received_utc_ns;
    baseline.last_observation_utc_ns = outcome.value().summary.last_received_utc_ns;
    baseline.evidence = outcome.value().summary.evidence.dominant;
    baseline.origin = outcome.value().summary.dominant_origin;
    baseline.created_at_utc_ns = context.now_utc_ns;
    baseline.metrics = outcome.value().summary.metrics;
    baseline.note = std::string(note);
    auto made = make_baseline(std::move(baseline));
    if (!made.ok()) {
        return made.error();
    }
    baselines_[made.value().series].push_back(made.value());
    return made.value();
}

std::vector<Baseline> Observatory::baselines(SeriesId series) const {
    LockGuard guard(mutex_);
    const auto it = baselines_.find(series);
    if (it == baselines_.end()) {
        return {};
    }
    return it->second;
}

const Baseline* Observatory::find_baseline_locked(BaselineId id) const noexcept {
    for (const auto& entry : baselines_) {
        for (const Baseline& baseline : entry.second) {
            if (baseline.id == id) {
                return &baseline;
            }
        }
    }
    return nullptr;
}

std::optional<Baseline> Observatory::find_baseline(BaselineId id) const {
    LockGuard guard(mutex_);
    const Baseline* baseline = find_baseline_locked(id);
    if (baseline == nullptr) {
        return std::nullopt;
    }
    return *baseline;
}

Status Observatory::save_locked(const std::string& path, bool truncate_existing,
                         const IngestContext& context) {
    if (!initialized_) {
        return Status::failure(ErrorCode::NotReady, "engine must be initialized before save");
    }
    StoreHeader header;
    header.format_version = kStoreFormatVersion;
    header.store_epoch = 1;
    header.created_at_utc_ns = context.now_utc_ns;
    header.boot_id = config_.boot_id;
    DigestBuilder id_builder("jitter.store.v1");
    id_builder.str(path);
    id_builder.i64(context.now_utc_ns);
    id_builder.u64(config_.boot_id);
    header.store_id = id_builder.digest();

    auto writer = StoreWriter::create(path, header, truncate_existing);
    if (!writer.ok()) {
        return writer.error();
    }

    for (const ClockDomainDescriptor& domain : clocks_.domains()) {
        ByteWriter payload;
        codec::write_clock_domain(payload, domain);
        JITTER_TRY(writer.value().append(StoreRecordType::ClockDomain, payload.data()));
    }
    for (const ClockEquivalence& equivalence : clocks_.equivalences()) {
        ByteWriter payload;
        codec::write_equivalence(payload, equivalence);
        JITTER_TRY(writer.value().append(StoreRecordType::Equivalence, payload.data()));
    }
    for (const SourceDescriptor& source : sources_.sources()) {
        ByteWriter payload;
        codec::write_source(payload, source);
        JITTER_TRY(writer.value().append(StoreRecordType::Source, payload.data()));
    }
    for (const SeriesDescriptor& descriptor : series_.series()) {
        ByteWriter payload;
        codec::write_series(payload, descriptor);
        JITTER_TRY(writer.value().append(StoreRecordType::Series, payload.data()));
    }
    for (const PathDescriptor& descriptor : paths_.paths()) {
        ByteWriter payload;
        codec::write_path(payload, descriptor);
        JITTER_TRY(writer.value().append(StoreRecordType::Path, payload.data()));
    }
    for (const PathGeneration& generation : paths_.all_generations()) {
        ByteWriter payload;
        codec::write_generation(payload, generation);
        JITTER_TRY(writer.value().append(StoreRecordType::Generation, payload.data()));
    }
    for (const auto& entry : windows_) {
        for (const LatencySample& sample : entry.second.snapshot()) {
            ByteWriter payload;
            codec::write_sample(payload, sample);
            JITTER_TRY(writer.value().append(StoreRecordType::Sample, payload.data()));
        }
    }
    for (const auto& entry : baselines_) {
        for (const Baseline& baseline : entry.second) {
            ByteWriter payload;
            codec::write_baseline(payload, baseline);
            JITTER_TRY(writer.value().append(StoreRecordType::Baseline, payload.data()));
        }
    }
    for (const Episode& episode : episodes_.all_episodes()) {
        ByteWriter payload;
        codec::write_episode(payload, episode);
        JITTER_TRY(writer.value().append(StoreRecordType::Episode, payload.data()));
    }
    for (const ConflictRecord& conflict : conflicts_.records()) {
        ByteWriter payload;
        codec::write_conflict(payload, conflict);
        JITTER_TRY(writer.value().append(StoreRecordType::Conflict, payload.data()));
    }
    for (const auto& entry : guards_) {
        ByteWriter payload;
        codec::write_source_guard(payload, entry.first, entry.second);
        JITTER_TRY(writer.value().append(StoreRecordType::SourceGuard, payload.data()));
    }

    JITTER_TRY(writer.value().append_trailer());
    JITTER_TRY(writer.value().flush());
    writer.value().close();
    ++counters_.saves;
    return Status::success();
}

Result<RestoreOutcome> Observatory::restore_locked(const std::string& path, const RestoreOptions& options) {
    auto loaded = load_store(path);
    if (!loaded.ok()) {
        return loaded.error();
    }
    RestoreOutcome outcome;
    outcome.recovery = loaded.value().recovery;

    if (loaded.value().recovery.mid_file_corruption) {
        return Result<RestoreOutcome>::fail(
            ErrorCode::Corrupt, "store has corruption before the end of the file",
            loaded.value().recovery.describe());
    }
    if (!loaded.value().recovery.clean && !options.repair_truncated_tail) {
        return Result<RestoreOutcome>::fail(
            ErrorCode::IntegrityFailure,
            "store is not clean; recovering it requires explicit authorisation",
            loaded.value().recovery.describe());
    }
    if (!loaded.value().recovery.clean) {
        add_note(outcome.notes, "recovered_from_damaged_tail:" + loaded.value().recovery.describe());
    }
    if (loaded.value().recovery.trailer_present &&
        loaded.value().trailer_record_count != loaded.value().recovery.records_recovered - 1) {
        return Result<RestoreOutcome>::fail(ErrorCode::IntegrityFailure,
                                            "store trailer record count does not match the file");
    }

    outcome.foreign_incarnation = loaded.value().header.boot_id != config_.boot_id;
    if (outcome.foreign_incarnation) {
        add_note(outcome.notes, "store_was_written_by_a_different_process_incarnation");
    }

    std::vector<LatencySample> samples;
    std::vector<std::pair<SourceId, SourceGuardState>> persisted_guards;
    for (const StoredRecord& record : loaded.value().records) {
        ByteReader reader(record.payload);
        switch (record.type) {
            case StoreRecordType::ClockDomain: {
                auto domain = codec::read_clock_domain(reader);
                if (!domain.ok()) {
                    return domain.error();
                }
                JITTER_TRY(clocks_.register_domain(domain.value()));
                ++outcome.clock_domains;
                break;
            }
            case StoreRecordType::Source: {
                auto source = codec::read_source(reader);
                if (!source.ok()) {
                    return source.error();
                }
                JITTER_TRY(sources_.register_source(source.value()));
                ++outcome.sources;
                break;
            }
            case StoreRecordType::Series: {
                auto descriptor = codec::read_series(reader);
                if (!descriptor.ok()) {
                    return descriptor.error();
                }
                JITTER_TRY(series_.register_series(descriptor.value()));
                ++outcome.series;
                break;
            }
            case StoreRecordType::Path: {
                auto descriptor = codec::read_path(reader);
                if (!descriptor.ok()) {
                    return descriptor.error();
                }
                if (paths_.find(descriptor.value().id) == nullptr) {
                    // Recovery imports the generations separately, in ordinal order.
                    JITTER_TRY(paths_.register_path_only(descriptor.value()));
                }
                ++outcome.paths;
                break;
            }
            case StoreRecordType::Generation: {
                auto generation = codec::read_generation(reader);
                if (!generation.ok()) {
                    return generation.error();
                }
                JITTER_TRY(paths_.import_generation(generation.value()));
                ++outcome.generations;
                break;
            }
            case StoreRecordType::Sample: {
                auto sample = codec::read_sample(reader);
                if (!sample.ok()) {
                    return sample.error();
                }
                samples.push_back(sample.value());
                break;
            }
            case StoreRecordType::Baseline: {
                auto baseline = codec::read_baseline(reader);
                if (!baseline.ok()) {
                    return baseline.error();
                }
                if (baselines_[baseline.value().series].size() < config_.max_baselines) {
                    baselines_[baseline.value().series].push_back(baseline.value());
                    ++outcome.baselines;
                } else {
                    add_note(outcome.notes, "baseline_dropped_at_restore_bound");
                }
                break;
            }
            case StoreRecordType::Episode: {
                auto episode = codec::read_episode(reader);
                if (!episode.ok()) {
                    return episode.error();
                }
                if (episode.value().is_open()) {
                    ++outcome.open_episodes_closed;
                }
                JITTER_TRY(episodes_.import_episode(episode.value(), loaded.value().header.created_at_utc_ns));
                ++outcome.episodes;
                break;
            }
            case StoreRecordType::Conflict: {
                auto conflict = codec::read_conflict(reader);
                if (!conflict.ok()) {
                    return conflict.error();
                }
                if (conflicts_.append(conflict.value())) {
                    ++outcome.conflicts;
                }
                break;
            }
            case StoreRecordType::SourceGuard: {
                auto guard = codec::read_source_guard(reader);
                if (!guard.ok()) {
                    return guard.error();
                }
                persisted_guards.push_back(guard.value());
                ++outcome.source_guards;
                break;
            }
            case StoreRecordType::Equivalence: {
                auto equivalence = codec::read_equivalence(reader);
                if (!equivalence.ok()) {
                    return equivalence.error();
                }
                JITTER_TRY(clocks_.declare_equivalence(equivalence.value()));
                ++outcome.equivalences;
                break;
            }
            case StoreRecordType::Marker:
            case StoreRecordType::Trailer:
                break;
        }
    }

    // Samples are grouped by generation so that a window never mixes generations.
    std::map<WindowKey, std::vector<LatencySample>> grouped;
    for (LatencySample& sample : samples) {
        grouped[WindowKey{sample.series, sample.generation}].push_back(std::move(sample));
    }
    for (auto& entry : grouped) {
        auto window = window_for(entry.first.series, entry.first.generation, true);
        if (!window.ok()) {
            add_note(outcome.notes, "window_bound_reached_during_restore");
            break;
        }
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const LatencySample& a, const LatencySample& b) {
                      if (a.observed_at.ticks != b.observed_at.ticks) {
                          return a.observed_at.ticks < b.observed_at.ticks;
                      }
                      return a.id < b.id;
                  });
        for (const LatencySample& sample : entry.second) {
            JITTER_TRY(window.value()->add(sample));
            ++outcome.samples;
            // Admitting persisted evidence is a decision the caller and the configuration
            // both have to make. A different process incarnation is reported either way.
            const bool withhold =
                !(options.admit_persisted_evidence && config_.admit_persisted_evidence);
            if (withhold) {
                restored_.insert(sample.id);
                ++outcome.samples_withheld_from_current;
            } else {
                ++outcome.samples_admitted_as_current;
            }
            // The source guard is rebuilt conservatively from what was persisted, so a
            // replay of already stored evidence is still fenced after a restart.
            SourceGuardState& guard = guards_[sample.provenance.source];
            const Digest& content = sample.content;
            if (!guard.initialized) {
                guard.initialized = true;
                guard.incarnation = sample.provenance.incarnation;
                guard.epoch = sample.provenance.epoch;
                guard.last_sequence = sample.provenance.sequence;
                guard.last_content = content;
            } else {
                apply_sequence(guard, sample.provenance, content);
            }
        }
    }

    // The persisted monotonic guard is applied last: it describes whole batches, which
    // individual samples cannot reconstruct, and it is what makes a replay after a
    // restart recognisable as a replay rather than as a contradiction.
    for (const auto& entry : persisted_guards) {
        if (!entry.second.initialized) {
            continue;
        }
        SourceGuardState& target = guards_[entry.first];
        if (!target.initialized || entry.second.last_sequence >= target.last_sequence) {
            target = entry.second;
        }
    }

    if (outcome.samples_withheld_from_current > 0) {
        add_note(outcome.notes,
                 "persisted_evidence_is_retained_for_history_but_is_not_current_evidence");
    }
    ++counters_.restores;
    return outcome;
}

Result<JsonValue> Observatory::summary_document_locked(const SummaryRequest& request,
                                                       const IngestContext& context) {
    auto outcome = summarize_locked(request, context, false);
    if (!outcome.ok()) {
        return outcome.error();
    }
    return make_document("series_summary", render_summary(outcome.value().summary));
}

Result<std::string> Observatory::export_summary_locked(const SummaryRequest& request,
                                                       const IngestContext& context, bool pretty) {
    auto document = summary_document_locked(request, context);
    if (!document.ok()) {
        return document.error();
    }
    return pretty ? document.value().dump_pretty() : document.value().dump();
}

Result<JsonValue> Observatory::history_document_locked(const HistoryRequest& request) {
    auto report = history_locked(request);
    if (!report.ok()) {
        return report.error();
    }
    return make_document("history", render_history(report.value()));
}

Result<std::string> Observatory::export_history_locked(const HistoryRequest& request, bool pretty) {
    auto document = history_document_locked(request);
    if (!document.ok()) {
        return document.error();
    }
    return pretty ? document.value().dump_pretty() : document.value().dump();
}

Result<JsonValue> Observatory::comparison_document_locked(const CompareRequest& request,
                                                          const IngestContext& context) {
    auto report = compare_locked(request, context);
    if (!report.ok()) {
        return report.error();
    }
    return make_document("baseline_comparison", render_comparison(report.value()));
}

Result<std::string> Observatory::export_comparison_locked(const CompareRequest& request,
                                                          const IngestContext& context, bool pretty) {
    auto document = comparison_document_locked(request, context);
    if (!document.ok()) {
        return document.error();
    }
    return pretty ? document.value().dump_pretty() : document.value().dump();
}

Result<JsonValue> Observatory::attribution_document_locked(const AttributionRequest& request,
                                                           const IngestContext& context) {
    auto report = attribute_locked(request, context);
    if (!report.ok()) {
        return report.error();
    }
    return make_document("hop_attribution", render_attribution(report.value()));
}

Result<std::string> Observatory::export_attribution_locked(const AttributionRequest& request,
                                                           const IngestContext& context, bool pretty) {
    auto document = attribution_document_locked(request, context);
    if (!document.ok()) {
        return document.error();
    }
    return pretty ? document.value().dump_pretty() : document.value().dump();
}

Result<JsonValue> Observatory::catalog_document_locked() {
    JsonValue catalog = JsonValue::make_object();

    JsonValue clocks = JsonValue::make_array();
    for (const ClockDomainDescriptor& domain : clocks_.domains()) {
        clocks.push(render_clock_domain(domain));
    }
    catalog.set("clock_domains", std::move(clocks));

    JsonValue sources = JsonValue::make_array();
    for (const SourceDescriptor& source : sources_.sources()) {
        sources.push(render_source(source));
    }
    catalog.set("sources", std::move(sources));

    JsonValue series = JsonValue::make_array();
    for (const SeriesDescriptor& descriptor : series_.series()) {
        series.push(render_series(descriptor));
    }
    catalog.set("series", std::move(series));

    JsonValue paths = JsonValue::make_array();
    for (const PathDescriptor& descriptor : paths_.paths()) {
        paths.push(render_path(descriptor));
    }
    catalog.set("paths", std::move(paths));

    JsonValue generations = JsonValue::make_array();
    for (const PathGeneration& generation : paths_.all_generations()) {
        generations.push(render_generation(generation));
    }
    catalog.set("generations", std::move(generations));

    JsonValue metrics = JsonValue::make_array();
    for (const MetricDefinition& definition : metric_registry().definitions()) {
        metrics.push(render_metric_definition(definition));
    }
    catalog.set("metric_definitions", std::move(metrics));

    JsonValue counters = JsonValue::make_object();
    counters.set("batches_accepted", JsonValue::make_u64(counters_.batches_accepted));
    counters.set("batches_rejected", JsonValue::make_u64(counters_.batches_rejected));
    counters.set("batches_duplicate", JsonValue::make_u64(counters_.batches_duplicate));
    counters.set("batches_historical", JsonValue::make_u64(counters_.batches_historical));
    counters.set("samples_stored", JsonValue::make_u64(counters_.samples_stored));
    counters.set("samples_duplicate", JsonValue::make_u64(counters_.samples_duplicate));
    counters.set("conflicts_recorded", JsonValue::make_u64(counters_.conflicts_recorded));
    counters.set("windows_created", JsonValue::make_u64(counters_.windows_created));
    counters.set("generations_opened", JsonValue::make_u64(counters_.generations_opened));
    counters.set("summaries_computed", JsonValue::make_u64(counters_.summaries_computed));
    counters.set("episodes_opened", JsonValue::make_u64(counters_.episodes_opened));
    counters.set("episodes_closed", JsonValue::make_u64(counters_.episodes_closed));
    counters.set("restores", JsonValue::make_u64(counters_.restores));
    counters.set("saves", JsonValue::make_u64(counters_.saves));
    counters.set("restored_evidence_held", JsonValue::make_u64(restored_.size()));
    catalog.set("counters", std::move(counters));

    JsonValue policy = JsonValue::make_object();
    policy.set("instability_policy", JsonValue::make_string(make_instability_policy_id(config_.policy).hex()));
    policy.set("window_policy", JsonValue::make_string(make_window_policy_id(config_.window).hex()));
    policy.set("freshness_policy", JsonValue::make_string(make_freshness_policy_id(config_.freshness).hex()));
    policy.set("admit_persisted_evidence", JsonValue::make_bool(config_.admit_persisted_evidence));
    policy.set("accept_synthetic", JsonValue::make_bool(config_.accept_synthetic));
    policy.set("boot_id", JsonValue::make_u64(config_.boot_id));
    catalog.set("configuration", std::move(policy));

    return make_document("catalog", std::move(catalog));
}

Result<std::string> Observatory::export_catalog_locked(bool pretty) {
    auto document = catalog_document_locked();
    if (!document.ok()) {
        return document.error();
    }
    return pretty ? document.value().dump_pretty() : document.value().dump();
}

// ---------------------------------------------------------------------------
// Public, synchronized surface. Every entry point takes the single engine lock, and
// no entry point calls another entry point, so the lock is never acquired twice by one
// thread. The lock auditor verifies that claim at run time.
// ---------------------------------------------------------------------------
Status Observatory::register_clock_domain(ClockDomainDescriptor& descriptor) {
    LockGuard guard(mutex_);
    return register_clock_domain_locked(descriptor);
}

Status Observatory::register_source(SourceDescriptor& descriptor) {
    LockGuard guard(mutex_);
    return register_source_locked(descriptor);
}

Status Observatory::register_series(SeriesDescriptor& descriptor) {
    LockGuard guard(mutex_);
    return register_series_locked(descriptor);
}

Status Observatory::register_path(PathDescriptor& descriptor) {
    LockGuard guard(mutex_);
    return register_path_locked(descriptor);
}

Status Observatory::declare_clock_equivalence(ClockEquivalence equivalence) {
    LockGuard guard(mutex_);
    return declare_clock_equivalence_locked(std::move(equivalence));
}

Result<GenerationOutcome> Observatory::observe_topology(PathId path, const PathTopology& topology,
                                                        std::int64_t now_utc_ns,
                                                        std::string_view cause) {
    LockGuard guard(mutex_);
    return observe_topology_locked(path, topology, now_utc_ns, cause);
}

Status Observatory::bump_path_revision(PathId path, std::string_view cause) {
    LockGuard guard(mutex_);
    return bump_path_revision_locked(path, cause);
}

Status Observatory::bump_path_revision_locked(PathId path, std::string_view cause) {
    return paths_.bump_revision(path, cause);
}

Result<IngestOutcome> Observatory::ingest(const LatencyBatch& batch, const IngestContext& context) {
    LockGuard guard(mutex_);
    return ingest_locked(batch, context);
}

Result<SummaryOutcome> Observatory::summarize(const SummaryRequest& request,
                                              const IngestContext& context, bool track_episodes) {
    LockGuard guard(mutex_);
    return summarize_locked(request, context, track_episodes);
}

Result<BaselineComparisonReport> Observatory::compare(const CompareRequest& request,
                                                      const IngestContext& context) {
    LockGuard guard(mutex_);
    return compare_locked(request, context);
}

Result<HistoryReport> Observatory::history(const HistoryRequest& request) {
    LockGuard guard(mutex_);
    return history_locked(request);
}

Result<PathAttributionReport> Observatory::attribute(const AttributionRequest& request,
                                                     const IngestContext& context) {
    LockGuard guard(mutex_);
    return attribute_locked(request, context);
}

Result<Explanation> Observatory::explain(const SummaryRequest& request,
                                         const IngestContext& context) {
    LockGuard guard(mutex_);
    return explain_locked(request, context);
}

Result<Baseline> Observatory::capture_baseline(const std::string& name, const SummaryRequest& request,
                                               const IngestContext& context, std::string_view note) {
    LockGuard guard(mutex_);
    return capture_baseline_locked(name, request, context, note);
}

Status Observatory::save(const std::string& path, bool truncate_existing,
                         const IngestContext& context) {
    LockGuard guard(mutex_);
    return save_locked(path, truncate_existing, context);
}

Result<RestoreOutcome> Observatory::restore(const std::string& path, const RestoreOptions& options) {
    LockGuard guard(mutex_);
    return restore_locked(path, options);
}

Result<JsonValue> Observatory::summary_document(const SummaryRequest& request,
                                                const IngestContext& context) {
    LockGuard guard(mutex_);
    return summary_document_locked(request, context);
}

Result<JsonValue> Observatory::history_document(const HistoryRequest& request) {
    LockGuard guard(mutex_);
    return history_document_locked(request);
}

Result<JsonValue> Observatory::comparison_document(const CompareRequest& request,
                                                   const IngestContext& context) {
    LockGuard guard(mutex_);
    return comparison_document_locked(request, context);
}

Result<JsonValue> Observatory::attribution_document(const AttributionRequest& request,
                                                    const IngestContext& context) {
    LockGuard guard(mutex_);
    return attribution_document_locked(request, context);
}

Result<JsonValue> Observatory::catalog_document() {
    LockGuard guard(mutex_);
    return catalog_document_locked();
}

Result<std::string> Observatory::export_summary(const SummaryRequest& request,
                                                const IngestContext& context, bool pretty) {
    LockGuard guard(mutex_);
    return export_summary_locked(request, context, pretty);
}

Result<std::string> Observatory::export_history(const HistoryRequest& request, bool pretty) {
    LockGuard guard(mutex_);
    return export_history_locked(request, pretty);
}

Result<std::string> Observatory::export_comparison(const CompareRequest& request,
                                                   const IngestContext& context, bool pretty) {
    LockGuard guard(mutex_);
    return export_comparison_locked(request, context, pretty);
}

Result<std::string> Observatory::export_attribution(const AttributionRequest& request,
                                                    const IngestContext& context, bool pretty) {
    LockGuard guard(mutex_);
    return export_attribution_locked(request, context, pretty);
}

Result<std::string> Observatory::export_catalog(bool pretty) {
    LockGuard guard(mutex_);
    return export_catalog_locked(pretty);
}

}  // namespace jitter
