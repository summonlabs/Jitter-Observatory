// Jitter Observatory - observations: the atomic evidence of the runtime.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/path.hpp>
#include <jitter/provenance.hpp>
#include <jitter/time.hpp>

namespace jitter {

// A timing reading taken at one hop. Only present when the hop actually emits
// timestamps; a hop without timing can never be attributed.
struct HopTiming {
    HopId hop;
    TimePoint arrived;
};

// One observed latency value with everything needed to judge it.
struct LatencySample {
    // Content addressed identity: equal ids mean byte-identical observations.
    MeasurementId id;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    Provenance provenance;
    // Latency in nanoseconds. Integer nanoseconds so that delta metrics are exact and
    // translation invariant.
    std::int64_t latency_ns = 0;
    // When the source says the measurement happened, on the source clock domain.
    TimePoint observed_at;
    // When this runtime received it, on the local UTC domain.
    TimePoint received_at;
    // Per-hop timestamps, when and only when the source provides them.
    std::vector<HopTiming> hop_timings;
    std::map<std::string, std::string> metadata;
    // Digest over every field above. Two samples share an id exactly when they agree
    // on all of them.
    Digest content;
};

Digest sample_content_digest(const LatencySample& sample);

// Validates bounds, identity and clock consistency, then derives id and content.
// The caller must not be able to construct a sample that fails these checks and still
// be stored, so the engine only accepts samples produced by this function.
Result<LatencySample> make_sample(LatencySample sample);

// Bounds and self-consistency only; catalog dependent checks live in the engine.
Status validate_sample(const LatencySample& sample);

struct BatchHeader {
    SourceId source;
    SourceIncarnation incarnation;
    SourceEpoch epoch;
    // Monotonic per (source, incarnation, epoch) transport sequence.
    SourceSequence sequence;
    SourceAuthority authority = SourceAuthority::Unknown;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    std::string ingest_path;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    // Clock domain of the samples' observation timestamps.
    ClockDomainId observation_clock;
    std::uint32_t protocol_revision = 1;
};

// The unit of ingest: one source, one series, one generation, one contiguous run of
// observations. Samples from different series or generations are never mixed in a
// batch, so identity is unambiguous at the transport boundary.
struct LatencyBatch {
    BatchId id;
    BatchHeader header;
    std::vector<LatencySample> samples;
    Digest content;
};

Digest batch_content_digest(const LatencyBatch& batch);
Result<LatencyBatch> make_batch(LatencyBatch batch);
Status validate_batch(const LatencyBatch& batch);

}  // namespace jitter
