// Jitter Observatory - canonical record encodings.
// Copyright 2026 Summon Software Labs.
#include <jitter/codec.hpp>

#include <jitter/limits.hpp>

namespace jitter::codec {
namespace {

void write_time_point(ByteWriter& writer, const TimePoint& value) {
    write_id(writer, value.domain);
    writer.i64(value.ticks);
}

Result<TimePoint> read_time_point(ByteReader& reader) {
    TimePoint value;
    JITTER_TRY_ASSIGN(value.domain, read_id<ClockDomainTag>(reader));
    JITTER_TRY_ASSIGN(value.ticks, reader.i64());
    return value;
}

}  // namespace

void write_clock_domain(ByteWriter& writer, const ClockDomainDescriptor& value) {
    writer.str(value.name);
    writer.u8(static_cast<std::uint8_t>(value.kind));
    writer.u8(static_cast<std::uint8_t>(value.unit));
    writer.str(value.epoch_note);
    writer.i64(value.declared_accuracy_ns);
    writer.boolean(value.declared_utc_aligned);
    write_id(writer, value.owner);
}

Result<ClockDomainDescriptor> read_clock_domain(ByteReader& reader) {
    ClockDomainDescriptor value;
    JITTER_TRY_ASSIGN(value.name, reader.str());
    JITTER_TRY_DECL(std::uint8_t, kind, reader.u8());
    if (kind > static_cast<std::uint8_t>(ClockKind::Synthetic)) {
        return Result<ClockDomainDescriptor>::fail(ErrorCode::ProtocolViolation,
                                                   "unknown clock kind in encoding",
                                                   std::to_string(kind));
    }
    value.kind = static_cast<ClockKind>(kind);
    JITTER_TRY_DECL(std::uint8_t, unit, reader.u8());
    if (unit > static_cast<std::uint8_t>(TimeUnit::CounterTicks)) {
        return Result<ClockDomainDescriptor>::fail(ErrorCode::ProtocolViolation,
                                                   "unknown time unit in encoding",
                                                   std::to_string(unit));
    }
    value.unit = static_cast<TimeUnit>(unit);
    JITTER_TRY_ASSIGN(value.epoch_note, reader.str());
    JITTER_TRY_ASSIGN(value.declared_accuracy_ns, reader.i64());
    JITTER_TRY_ASSIGN(value.declared_utc_aligned, reader.boolean());
    JITTER_TRY_ASSIGN(value.owner, read_id<SourceTag>(reader));
    value.id = make_clock_domain_id(value);
    return value;
}

void write_equivalence(ByteWriter& writer, const ClockEquivalence& value) {
    write_id(writer, value.a);
    write_id(writer, value.b);
    writer.i64(value.max_offset_ns);
    writer.u8(static_cast<std::uint8_t>(value.declared_by_authority));
    writer.str(value.justification);
    write_optional_i64(writer, value.valid_until_utc_ns);
}

Result<ClockEquivalence> read_equivalence(ByteReader& reader) {
    ClockEquivalence value;
    JITTER_TRY_ASSIGN(value.a, read_id<ClockDomainTag>(reader));
    JITTER_TRY_ASSIGN(value.b, read_id<ClockDomainTag>(reader));
    JITTER_TRY_ASSIGN(value.max_offset_ns, reader.i64());
    JITTER_TRY_DECL(std::uint8_t, authority, reader.u8());
    if (authority > static_cast<std::uint8_t>(SourceAuthority::Authoritative)) {
        return Result<ClockEquivalence>::fail(ErrorCode::ProtocolViolation,
                                              "unknown source authority in encoding");
    }
    value.declared_by_authority = static_cast<SourceAuthority>(authority);
    JITTER_TRY_ASSIGN(value.justification, reader.str());
    JITTER_TRY_ASSIGN(value.valid_until_utc_ns, read_optional_i64(reader));
    return value;
}

void write_source(ByteWriter& writer, const SourceDescriptor& value) {
    writer.str(value.name);
    writer.u8(static_cast<std::uint8_t>(value.authority));
    writer.u8(static_cast<std::uint8_t>(value.origin));
    write_id(writer, value.clock_domain);
    writer.str(value.description);
    writer.u32(value.protocol_revision);
}

Result<SourceDescriptor> read_source(ByteReader& reader) {
    SourceDescriptor value;
    JITTER_TRY_ASSIGN(value.name, reader.str());
    JITTER_TRY_DECL(std::uint8_t, authority, reader.u8());
    if (authority > static_cast<std::uint8_t>(SourceAuthority::Authoritative)) {
        return Result<SourceDescriptor>::fail(ErrorCode::ProtocolViolation,
                                              "unknown source authority in encoding");
    }
    value.authority = static_cast<SourceAuthority>(authority);
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<SourceDescriptor>::fail(ErrorCode::ProtocolViolation,
                                              "unknown evidence origin in encoding");
    }
    value.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.clock_domain, read_id<ClockDomainTag>(reader));
    JITTER_TRY_ASSIGN(value.description, reader.str());
    JITTER_TRY_ASSIGN(value.protocol_revision, reader.u32());
    value.id = make_source_id(value);
    return value;
}

void write_series(ByteWriter& writer, const SeriesDescriptor& value) {
    writer.str(value.name);
    writer.u8(static_cast<std::uint8_t>(value.kind));
    writer.u8(static_cast<std::uint8_t>(value.unit));
    write_id(writer, value.path);
    write_id(writer, value.clock_domain);
    writer.u8(static_cast<std::uint8_t>(value.origin));
    writer.str(value.description);
}

Result<SeriesDescriptor> read_series(ByteReader& reader) {
    SeriesDescriptor value;
    JITTER_TRY_ASSIGN(value.name, reader.str());
    JITTER_TRY_DECL(std::uint8_t, kind, reader.u8());
    if (kind > static_cast<std::uint8_t>(MeasurementKind::Custom)) {
        return Result<SeriesDescriptor>::fail(ErrorCode::ProtocolViolation,
                                              "unknown measurement kind in encoding");
    }
    value.kind = static_cast<MeasurementKind>(kind);
    JITTER_TRY_DECL(std::uint8_t, unit, reader.u8());
    if (unit > static_cast<std::uint8_t>(TimeUnit::CounterTicks)) {
        return Result<SeriesDescriptor>::fail(ErrorCode::ProtocolViolation,
                                              "unknown time unit in encoding");
    }
    value.unit = static_cast<TimeUnit>(unit);
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_ASSIGN(value.clock_domain, read_id<ClockDomainTag>(reader));
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<SeriesDescriptor>::fail(ErrorCode::ProtocolViolation,
                                              "unknown evidence origin in encoding");
    }
    value.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.description, reader.str());
    value.id = make_series_id(value);
    return value;
}

void write_hop(ByteWriter& writer, const HopDescriptor& value) {
    writer.u16(value.index);
    writer.str(value.name);
    write_optional_id(writer, value.timing_clock.has_value() ? value.timing_clock.value()
                                                             : ClockDomainId{});
    writer.u8(static_cast<std::uint8_t>(value.origin));
    writer.str(value.device_note);
}

Result<HopDescriptor> read_hop(ByteReader& reader) {
    HopDescriptor value;
    JITTER_TRY_ASSIGN(value.index, reader.u16());
    JITTER_TRY_ASSIGN(value.name, reader.str());
    JITTER_TRY_DECL(auto, clock, read_optional_id<ClockDomainTag>(reader));
    if (clock.has_value()) {
        value.timing_clock = clock.value();
    }
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<HopDescriptor>::fail(ErrorCode::ProtocolViolation,
                                           "unknown evidence origin in encoding");
    }
    value.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.device_note, reader.str());
    return value;
}

void write_path(ByteWriter& writer, const PathDescriptor& value) {
    writer.str(value.name);
    writer.u8(static_cast<std::uint8_t>(value.origin));
    writer.str(value.description);
    writer.u32(static_cast<std::uint32_t>(value.hops.size()));
    for (const HopDescriptor& hop : value.hops) {
        write_hop(writer, hop);
    }
}

Result<PathDescriptor> read_path(ByteReader& reader) {
    PathDescriptor value;
    JITTER_TRY_ASSIGN(value.name, reader.str());
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<PathDescriptor>::fail(ErrorCode::ProtocolViolation,
                                            "unknown evidence origin in encoding");
    }
    value.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.description, reader.str());
    JITTER_TRY_DECL(std::uint32_t, hop_count, reader.u32());
    if (hop_count > Limits::kMaxHopsPerPath) {
        return Result<PathDescriptor>::fail(ErrorCode::LimitExceeded,
                                            "encoded path declares too many hops",
                                            std::to_string(hop_count));
    }
    value.hops.reserve(hop_count);
    for (std::uint32_t i = 0; i < hop_count; ++i) {
        JITTER_TRY_DECL(HopDescriptor, hop, read_hop(reader));
        value.hops.push_back(std::move(hop));
    }
    value.id = make_path_id(value);
    for (HopDescriptor& hop : value.hops) {
        hop.id = make_hop_id(hop, value.id);
    }
    return value;
}

void write_generation(ByteWriter& writer, const PathGeneration& value) {
    write_id(writer, value.path);
    writer.u64(value.ordinal.value());
    write_digest(writer, value.topology);
    writer.u64(value.revision.value());
    writer.i64(value.opened_at_utc_ns);
    write_optional_i64(writer, value.closed_at_utc_ns);
    writer.u32(value.hop_count);
    writer.str(value.cause);
    writer.str(value.close_reason);
}

Result<PathGeneration> read_generation(ByteReader& reader) {
    PathGeneration value;
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, ordinal, reader.u64());
    if (ordinal == 0) {
        return Result<PathGeneration>::fail(ErrorCode::ProtocolViolation,
                                            "generation ordinal must be non zero");
    }
    value.ordinal = Ordinal(ordinal);
    JITTER_TRY_ASSIGN(value.topology, read_digest(reader));
    JITTER_TRY_DECL(std::uint64_t, revision, reader.u64());
    value.revision = Revision(revision);
    JITTER_TRY_ASSIGN(value.opened_at_utc_ns, reader.i64());
    JITTER_TRY_ASSIGN(value.closed_at_utc_ns, read_optional_i64(reader));
    JITTER_TRY_ASSIGN(value.hop_count, reader.u32());
    JITTER_TRY_ASSIGN(value.cause, reader.str());
    JITTER_TRY_ASSIGN(value.close_reason, reader.str());
    value.id = make_generation_id(value.path, value.ordinal, value.topology);
    return value;
}

void write_sample(ByteWriter& writer, const LatencySample& value) {
    write_id(writer, value.series);
    write_id(writer, value.path);
    write_id(writer, value.generation);
    writer.u64(value.generation_ordinal.value());
    write_id(writer, value.provenance.source);
    writer.u64(value.provenance.incarnation.value());
    writer.u64(value.provenance.epoch.value());
    writer.u64(value.provenance.sequence.value());
    writer.u8(static_cast<std::uint8_t>(value.provenance.authority));
    writer.u8(static_cast<std::uint8_t>(value.provenance.origin));
    writer.str(value.provenance.ingest_path);
    writer.i64(value.latency_ns);
    write_time_point(writer, value.observed_at);
    write_time_point(writer, value.received_at);
    writer.u32(static_cast<std::uint32_t>(value.hop_timings.size()));
    for (const HopTiming& timing : value.hop_timings) {
        write_id(writer, timing.hop);
        write_time_point(writer, timing.arrived);
    }
    writer.u32(static_cast<std::uint32_t>(value.metadata.size()));
    for (const auto& entry : value.metadata) {
        writer.str(entry.first);
        writer.str(entry.second);
    }
}

Result<LatencySample> read_sample(ByteReader& reader) {
    LatencySample value;
    JITTER_TRY_ASSIGN(value.series, read_id<SeriesTag>(reader));
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_ASSIGN(value.generation, read_id<GenerationTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, ordinal, reader.u64());
    value.generation_ordinal = Ordinal(ordinal);
    JITTER_TRY_ASSIGN(value.provenance.source, read_id<SourceTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, incarnation, reader.u64());
    value.provenance.incarnation = SourceIncarnation(incarnation);
    JITTER_TRY_DECL(std::uint64_t, epoch, reader.u64());
    value.provenance.epoch = SourceEpoch(epoch);
    JITTER_TRY_DECL(std::uint64_t, sequence, reader.u64());
    value.provenance.sequence = SourceSequence(sequence);
    JITTER_TRY_DECL(std::uint8_t, authority, reader.u8());
    if (authority > static_cast<std::uint8_t>(SourceAuthority::Authoritative)) {
        return Result<LatencySample>::fail(ErrorCode::ProtocolViolation,
                                           "unknown source authority in sample encoding");
    }
    value.provenance.authority = static_cast<SourceAuthority>(authority);
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<LatencySample>::fail(ErrorCode::ProtocolViolation,
                                           "unknown evidence origin in sample encoding");
    }
    value.provenance.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.provenance.ingest_path, reader.str());
    JITTER_TRY_ASSIGN(value.latency_ns, reader.i64());
    JITTER_TRY_ASSIGN(value.observed_at, read_time_point(reader));
    JITTER_TRY_ASSIGN(value.received_at, read_time_point(reader));
    JITTER_TRY_DECL(std::uint32_t, hop_count, reader.u32());
    if (hop_count > Limits::kMaxHopsPerPath) {
        return Result<LatencySample>::fail(ErrorCode::LimitExceeded,
                                           "encoded sample carries too many hop timings",
                                           std::to_string(hop_count));
    }
    value.hop_timings.reserve(hop_count);
    for (std::uint32_t i = 0; i < hop_count; ++i) {
        HopTiming timing;
        JITTER_TRY_ASSIGN(timing.hop, read_id<HopTag>(reader));
        JITTER_TRY_ASSIGN(timing.arrived, read_time_point(reader));
        value.hop_timings.push_back(timing);
    }
    JITTER_TRY_DECL(std::uint32_t, metadata_count, reader.u32());
    if (metadata_count > Limits::kMaxMetadataEntries) {
        return Result<LatencySample>::fail(ErrorCode::LimitExceeded,
                                           "encoded sample carries too many metadata entries",
                                           std::to_string(metadata_count));
    }
    for (std::uint32_t i = 0; i < metadata_count; ++i) {
        JITTER_TRY_DECL(std::string, key, reader.str());
        JITTER_TRY_DECL(std::string, item, reader.str());
        value.metadata.emplace(std::move(key), std::move(item));
    }
    return make_sample(value);
}

void write_batch_header(ByteWriter& writer, const BatchHeader& value) {
    write_id(writer, value.source);
    writer.u64(value.incarnation.value());
    writer.u64(value.epoch.value());
    writer.u64(value.sequence.value());
    writer.u8(static_cast<std::uint8_t>(value.authority));
    writer.u8(static_cast<std::uint8_t>(value.origin));
    writer.str(value.ingest_path);
    write_id(writer, value.series);
    write_id(writer, value.path);
    write_id(writer, value.generation);
    writer.u64(value.generation_ordinal.value());
    write_id(writer, value.observation_clock);
    writer.u32(value.protocol_revision);
}

Result<BatchHeader> read_batch_header(ByteReader& reader) {
    BatchHeader value;
    JITTER_TRY_ASSIGN(value.source, read_id<SourceTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, incarnation, reader.u64());
    value.incarnation = SourceIncarnation(incarnation);
    JITTER_TRY_DECL(std::uint64_t, epoch, reader.u64());
    value.epoch = SourceEpoch(epoch);
    JITTER_TRY_DECL(std::uint64_t, sequence, reader.u64());
    value.sequence = SourceSequence(sequence);
    JITTER_TRY_DECL(std::uint8_t, authority, reader.u8());
    if (authority > static_cast<std::uint8_t>(SourceAuthority::Authoritative)) {
        return Result<BatchHeader>::fail(ErrorCode::ProtocolViolation,
                                         "unknown source authority in batch header");
    }
    value.authority = static_cast<SourceAuthority>(authority);
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<BatchHeader>::fail(ErrorCode::ProtocolViolation,
                                         "unknown evidence origin in batch header");
    }
    value.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.ingest_path, reader.str());
    JITTER_TRY_ASSIGN(value.series, read_id<SeriesTag>(reader));
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_ASSIGN(value.generation, read_id<GenerationTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, ordinal, reader.u64());
    value.generation_ordinal = Ordinal(ordinal);
    JITTER_TRY_ASSIGN(value.observation_clock, read_id<ClockDomainTag>(reader));
    JITTER_TRY_ASSIGN(value.protocol_revision, reader.u32());
    return value;
}

void write_batch(ByteWriter& writer, const LatencyBatch& value) {
    write_batch_header(writer, value.header);
    writer.u32(static_cast<std::uint32_t>(value.samples.size()));
    for (const LatencySample& sample : value.samples) {
        write_sample(writer, sample);
    }
}

Result<LatencyBatch> read_batch(ByteReader& reader) {
    LatencyBatch value;
    JITTER_TRY_ASSIGN(value.header, read_batch_header(reader));
    JITTER_TRY_DECL(std::uint32_t, count, reader.u32());
    if (count == 0 || count > Limits::kMaxSamplesPerBatch) {
        return Result<LatencyBatch>::fail(ErrorCode::LimitExceeded,
                                          "encoded batch declares an unsupported sample count",
                                          std::to_string(count));
    }
    value.samples.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        JITTER_TRY_DECL(LatencySample, sample, read_sample(reader));
        value.samples.push_back(std::move(sample));
    }
    return make_batch(value);
}

void write_metric_value(ByteWriter& writer, const MetricValue& value) {
    write_id(writer, value.id);
    writer.u32(value.version);
    writer.f64_bytes(value.value);
    writer.u64(value.sample_count);
    writer.u64(value.term_count);
}

Result<MetricValue> read_metric_value(ByteReader& reader) {
    MetricValue value;
    JITTER_TRY_ASSIGN(value.id, read_id<MetricTag>(reader));
    const MetricDefinition* definition = metric_registry().find(value.id);
    if (definition == nullptr) {
        return Result<MetricValue>::fail(ErrorCode::NotFound,
                                         "encoded metric identity is not known to this build",
                                         value.id.hex());
    }
    value.key = definition->key;
    value.name = definition->name;
    value.unit = definition->unit;
    JITTER_TRY_ASSIGN(value.version, reader.u32());
    if (value.version != definition->version) {
        return Result<MetricValue>::fail(ErrorCode::VersionUnsupported,
                                         "encoded metric definition version differs from this build",
                                         std::string(definition->name));
    }
    JITTER_TRY_ASSIGN(value.value, reader.f64_bytes());
    JITTER_TRY_ASSIGN(value.sample_count, reader.u64());
    JITTER_TRY_ASSIGN(value.term_count, reader.u64());
    return value;
}

void write_baseline(ByteWriter& writer, const Baseline& value) {
    writer.str(value.name);
    write_id(writer, value.series);
    write_id(writer, value.path);
    write_id(writer, value.generation);
    writer.u64(value.generation_ordinal.value());
    write_id(writer, value.window);
    write_id(writer, value.policy);
    writer.u64(value.sample_count);
    writer.u64(value.fresh_sample_count);
    write_optional_i64(writer, value.first_observation_utc_ns);
    write_optional_i64(writer, value.last_observation_utc_ns);
    writer.u8(static_cast<std::uint8_t>(value.evidence));
    writer.u8(static_cast<std::uint8_t>(value.origin));
    writer.i64(value.created_at_utc_ns);
    writer.str(value.note);
    writer.u32(static_cast<std::uint32_t>(value.metrics.size()));
    for (const MetricValue& metric : value.metrics) {
        write_metric_value(writer, metric);
    }
}

Result<Baseline> read_baseline(ByteReader& reader) {
    Baseline value;
    JITTER_TRY_ASSIGN(value.name, reader.str());
    JITTER_TRY_ASSIGN(value.series, read_id<SeriesTag>(reader));
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_ASSIGN(value.generation, read_id<GenerationTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, ordinal, reader.u64());
    value.generation_ordinal = Ordinal(ordinal);
    JITTER_TRY_ASSIGN(value.window, read_id<WindowPolicyTag>(reader));
    JITTER_TRY_ASSIGN(value.policy, read_id<InstabilityPolicyTag>(reader));
    JITTER_TRY_ASSIGN(value.sample_count, reader.u64());
    JITTER_TRY_ASSIGN(value.fresh_sample_count, reader.u64());
    JITTER_TRY_ASSIGN(value.first_observation_utc_ns, read_optional_i64(reader));
    JITTER_TRY_ASSIGN(value.last_observation_utc_ns, read_optional_i64(reader));
    JITTER_TRY_DECL(std::uint8_t, evidence, reader.u8());
    if (evidence > static_cast<std::uint8_t>(EvidenceState::Missing)) {
        return Result<Baseline>::fail(ErrorCode::ProtocolViolation,
                                      "unknown evidence state in baseline encoding");
    }
    value.evidence = static_cast<EvidenceState>(evidence);
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<Baseline>::fail(ErrorCode::ProtocolViolation,
                                      "unknown evidence origin in baseline encoding");
    }
    value.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_ASSIGN(value.created_at_utc_ns, reader.i64());
    JITTER_TRY_ASSIGN(value.note, reader.str());
    JITTER_TRY_DECL(std::uint32_t, metric_count, reader.u32());
    if (metric_count > Limits::kMaxWatchedMetrics * 4u) {
        return Result<Baseline>::fail(ErrorCode::LimitExceeded,
                                      "encoded baseline holds too many metric values",
                                      std::to_string(metric_count));
    }
    value.metrics.reserve(metric_count);
    for (std::uint32_t i = 0; i < metric_count; ++i) {
        JITTER_TRY_DECL(MetricValue, metric, read_metric_value(reader));
        value.metrics.push_back(std::move(metric));
    }
    return make_baseline(value);
}

void write_episode(ByteWriter& writer, const Episode& value) {
    write_id(writer, value.series);
    write_id(writer, value.path);
    write_id(writer, value.generation);
    writer.u64(value.generation_ordinal.value());
    write_id(writer, value.policy);
    write_id(writer, value.window);
    writer.u8(static_cast<std::uint8_t>(value.peak_level));
    writer.u8(static_cast<std::uint8_t>(value.driver_key));
    write_id(writer, value.driver_id);
    writer.f64_bytes(value.opening_value);
    writer.f64_bytes(value.peak_value);
    writer.i64(value.opened_at_utc_ns);
    write_optional_i64(writer, value.closed_at_utc_ns);
    writer.u64(value.observations);
    writer.u64(value.breach_observations);
    writer.u64(value.recovery_observations);
    writer.u32(value.close_windows_required);
    writer.u8(static_cast<std::uint8_t>(value.close_reason));
    writer.str(value.close_detail);
    writer.u32(static_cast<std::uint32_t>(value.reasons.size()));
    for (const std::string& reason : value.reasons) {
        writer.str(reason);
    }
}

Result<Episode> read_episode(ByteReader& reader) {
    Episode value;
    JITTER_TRY_ASSIGN(value.series, read_id<SeriesTag>(reader));
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_ASSIGN(value.generation, read_id<GenerationTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, ordinal, reader.u64());
    value.generation_ordinal = Ordinal(ordinal);
    JITTER_TRY_ASSIGN(value.policy, read_id<InstabilityPolicyTag>(reader));
    JITTER_TRY_ASSIGN(value.window, read_id<WindowPolicyTag>(reader));
    JITTER_TRY_DECL(std::uint8_t, level, reader.u8());
    if (level > static_cast<std::uint8_t>(InstabilityLevel::Unstable)) {
        return Result<Episode>::fail(ErrorCode::ProtocolViolation,
                                     "unknown instability level in episode encoding");
    }
    value.peak_level = static_cast<InstabilityLevel>(level);
    JITTER_TRY_DECL(std::uint8_t, driver, reader.u8());
    if (driver > static_cast<std::uint8_t>(MetricKey::LatencyP99)) {
        return Result<Episode>::fail(ErrorCode::ProtocolViolation,
                                     "unknown metric key in episode encoding");
    }
    value.driver_key = static_cast<MetricKey>(driver);
    JITTER_TRY_ASSIGN(value.driver_id, read_id<MetricTag>(reader));
    JITTER_TRY_ASSIGN(value.opening_value, reader.f64_bytes());
    JITTER_TRY_ASSIGN(value.peak_value, reader.f64_bytes());
    JITTER_TRY_ASSIGN(value.opened_at_utc_ns, reader.i64());
    JITTER_TRY_ASSIGN(value.closed_at_utc_ns, read_optional_i64(reader));
    JITTER_TRY_ASSIGN(value.observations, reader.u64());
    JITTER_TRY_ASSIGN(value.breach_observations, reader.u64());
    JITTER_TRY_ASSIGN(value.recovery_observations, reader.u64());
    JITTER_TRY_ASSIGN(value.close_windows_required, reader.u32());
    JITTER_TRY_DECL(std::uint8_t, close_reason, reader.u8());
    if (close_reason > static_cast<std::uint8_t>(EpisodeCloseReason::Superseded)) {
        return Result<Episode>::fail(ErrorCode::ProtocolViolation,
                                     "unknown episode close reason in encoding");
    }
    value.close_reason = static_cast<EpisodeCloseReason>(close_reason);
    JITTER_TRY_ASSIGN(value.close_detail, reader.str());
    JITTER_TRY_DECL(std::uint32_t, reason_count, reader.u32());
    if (reason_count > Limits::kMaxExplanationLines) {
        return Result<Episode>::fail(ErrorCode::LimitExceeded,
                                     "encoded episode carries too many reasons");
    }
    value.reasons.reserve(reason_count);
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        JITTER_TRY_DECL(std::string, reason, reader.str());
        value.reasons.push_back(std::move(reason));
    }
    value.id = make_episode_id(value);
    return value;
}

void write_source_guard(ByteWriter& writer, SourceId source, const SourceGuardState& value) {
    write_id(writer, source);
    writer.u64(value.incarnation.value());
    writer.u64(value.epoch.value());
    writer.u64(value.last_sequence.value());
    write_digest(writer, value.last_content);
    writer.boolean(value.initialized);
}

Result<std::pair<SourceId, SourceGuardState>> read_source_guard(ByteReader& reader) {
    SourceId source;
    SourceGuardState guard;
    JITTER_TRY_ASSIGN(source, read_id<SourceTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, incarnation, reader.u64());
    guard.incarnation = SourceIncarnation(incarnation);
    JITTER_TRY_DECL(std::uint64_t, epoch, reader.u64());
    guard.epoch = SourceEpoch(epoch);
    JITTER_TRY_DECL(std::uint64_t, sequence, reader.u64());
    guard.last_sequence = SourceSequence(sequence);
    JITTER_TRY_ASSIGN(guard.last_content, read_digest(reader));
    JITTER_TRY_ASSIGN(guard.initialized, reader.boolean());
    return std::make_pair(source, guard);
}

void write_conflict(ByteWriter& writer, const ConflictRecord& value) {
    write_id(writer, value.series);
    write_id(writer, value.path);
    write_id(writer, value.generation);
    writer.u64(value.sequence.value());
    write_id(writer, value.source_a);
    write_id(writer, value.source_b);
    write_digest(writer, value.content_a);
    write_digest(writer, value.content_b);
    writer.u8(static_cast<std::uint8_t>(value.authority_a));
    writer.u8(static_cast<std::uint8_t>(value.authority_b));
    writer.i64(value.detected_at_utc_ns);
    writer.u8(static_cast<std::uint8_t>(value.outcome));
    const bool has_winner = value.winner.has_value();
    writer.boolean(has_winner);
    if (has_winner) {
        write_id(writer, value.winner.value());
    }
    writer.str(value.reason);
}

Result<ConflictRecord> read_conflict(ByteReader& reader) {
    ConflictRecord value;
    JITTER_TRY_ASSIGN(value.series, read_id<SeriesTag>(reader));
    JITTER_TRY_ASSIGN(value.path, read_id<PathTag>(reader));
    JITTER_TRY_ASSIGN(value.generation, read_id<GenerationTag>(reader));
    JITTER_TRY_DECL(std::uint64_t, sequence, reader.u64());
    value.sequence = SourceSequence(sequence);
    JITTER_TRY_ASSIGN(value.source_a, read_id<SourceTag>(reader));
    JITTER_TRY_ASSIGN(value.source_b, read_id<SourceTag>(reader));
    JITTER_TRY_ASSIGN(value.content_a, read_digest(reader));
    JITTER_TRY_ASSIGN(value.content_b, read_digest(reader));
    JITTER_TRY_DECL(std::uint8_t, authority_a, reader.u8());
    JITTER_TRY_DECL(std::uint8_t, authority_b, reader.u8());
    if (authority_a > static_cast<std::uint8_t>(SourceAuthority::Authoritative) ||
        authority_b > static_cast<std::uint8_t>(SourceAuthority::Authoritative)) {
        return Result<ConflictRecord>::fail(ErrorCode::ProtocolViolation,
                                            "unknown source authority in conflict encoding");
    }
    value.authority_a = static_cast<SourceAuthority>(authority_a);
    value.authority_b = static_cast<SourceAuthority>(authority_b);
    JITTER_TRY_ASSIGN(value.detected_at_utc_ns, reader.i64());
    JITTER_TRY_DECL(std::uint8_t, outcome, reader.u8());
    if (outcome > static_cast<std::uint8_t>(ConflictOutcome::RefusedUnknownAuthority)) {
        return Result<ConflictRecord>::fail(ErrorCode::ProtocolViolation,
                                            "unknown conflict outcome in encoding");
    }
    value.outcome = static_cast<ConflictOutcome>(outcome);
    JITTER_TRY_DECL(bool, has_winner, reader.boolean());
    if (has_winner) {
        JITTER_TRY_DECL(SourceId, winner, read_id<SourceTag>(reader));
        value.winner = winner;
    }
    JITTER_TRY_ASSIGN(value.reason, reader.str());
    return make_conflict_record(value);
}

}  // namespace jitter::codec
