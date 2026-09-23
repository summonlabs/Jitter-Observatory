// Jitter Observatory - canonical record encodings shared by store and wire.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <jitter/attribution.hpp>
#include <jitter/baseline.hpp>
#include <jitter/bytes.hpp>
#include <jitter/classification.hpp>
#include <jitter/episode.hpp>
#include <jitter/evidence.hpp>
#include <jitter/path.hpp>
#include <jitter/sample.hpp>
#include <jitter/series.hpp>
#include <jitter/source.hpp>
#include <jitter/time.hpp>

namespace jitter::codec {

template <class Tag>
void write_id(ByteWriter& writer, const StrongId<Tag>& value) {
    writer.raw(value.bytes());
}

template <class Tag>
Result<StrongId<Tag>> read_id(ByteReader& reader) {
    JITTER_TRY_DECL(auto, bytes, reader.raw(StrongId<Tag>::kBytes));
    return StrongId<Tag>::from_bytes(bytes.data());
}

inline void write_digest(ByteWriter& writer, const Digest& value) {
    writer.raw(std::span<const std::uint8_t>(value.data(), Digest::kBytes));
}

inline Result<Digest> read_digest(ByteReader& reader) {
    JITTER_TRY_DECL(auto, bytes, reader.raw(Digest::kBytes));
    std::array<std::uint8_t, Digest::kBytes> copy{};
    for (std::size_t i = 0; i < Digest::kBytes; ++i) {
        copy[i] = bytes[i];
    }
    return Digest(copy);
}

inline void write_optional_u64(ByteWriter& writer, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
        writer.boolean(true);
        writer.u64(value.value());
    } else {
        writer.boolean(false);
    }
}

inline Result<std::optional<std::uint64_t>> read_optional_u64(ByteReader& reader) {
    JITTER_TRY_DECL(bool, present, reader.boolean());
    if (!present) {
        return std::optional<std::uint64_t>{};
    }
    JITTER_TRY_DECL(std::uint64_t, value, reader.u64());
    return std::optional<std::uint64_t>(value);
}

inline void write_optional_i64(ByteWriter& writer, const std::optional<std::int64_t>& value) {
    if (value.has_value()) {
        writer.boolean(true);
        writer.i64(value.value());
    } else {
        writer.boolean(false);
    }
}

inline Result<std::optional<std::int64_t>> read_optional_i64(ByteReader& reader) {
    JITTER_TRY_DECL(bool, present, reader.boolean());
    if (!present) {
        return std::optional<std::int64_t>{};
    }
    JITTER_TRY_DECL(std::int64_t, value, reader.i64());
    return std::optional<std::int64_t>(value);
}

template <class Tag>
void write_optional_id(ByteWriter& writer, const StrongId<Tag>& value) {
    if (value.is_nil()) {
        writer.boolean(false);
        return;
    }
    writer.boolean(true);
    write_id(writer, value);
}

template <class Tag>
Result<std::optional<StrongId<Tag>>> read_optional_id(ByteReader& reader) {
    JITTER_TRY_DECL(bool, present, reader.boolean());
    if (!present) {
        return std::optional<StrongId<Tag>>{};
    }
    JITTER_TRY_DECL(StrongId<Tag>, value, read_id<Tag>(reader));
    return std::optional<StrongId<Tag>>(value);
}

void write_clock_domain(ByteWriter& writer, const ClockDomainDescriptor& value);
Result<ClockDomainDescriptor> read_clock_domain(ByteReader& reader);

void write_equivalence(ByteWriter& writer, const ClockEquivalence& value);
Result<ClockEquivalence> read_equivalence(ByteReader& reader);

void write_source(ByteWriter& writer, const SourceDescriptor& value);
Result<SourceDescriptor> read_source(ByteReader& reader);

void write_series(ByteWriter& writer, const SeriesDescriptor& value);
Result<SeriesDescriptor> read_series(ByteReader& reader);

void write_hop(ByteWriter& writer, const HopDescriptor& value);
Result<HopDescriptor> read_hop(ByteReader& reader);

void write_path(ByteWriter& writer, const PathDescriptor& value);
Result<PathDescriptor> read_path(ByteReader& reader);

void write_generation(ByteWriter& writer, const PathGeneration& value);
Result<PathGeneration> read_generation(ByteReader& reader);

void write_sample(ByteWriter& writer, const LatencySample& value);
Result<LatencySample> read_sample(ByteReader& reader);

void write_batch_header(ByteWriter& writer, const BatchHeader& value);
Result<BatchHeader> read_batch_header(ByteReader& reader);

void write_batch(ByteWriter& writer, const LatencyBatch& value);
Result<LatencyBatch> read_batch(ByteReader& reader);

void write_metric_value(ByteWriter& writer, const MetricValue& value);
Result<MetricValue> read_metric_value(ByteReader& reader);

void write_baseline(ByteWriter& writer, const Baseline& value);
Result<Baseline> read_baseline(ByteReader& reader);

void write_episode(ByteWriter& writer, const Episode& value);
Result<Episode> read_episode(ByteReader& reader);

void write_source_guard(ByteWriter& writer, SourceId source, const SourceGuardState& value);
Result<std::pair<SourceId, SourceGuardState>> read_source_guard(ByteReader& reader);

void write_conflict(ByteWriter& writer, const ConflictRecord& value);
Result<ConflictRecord> read_conflict(ByteReader& reader);

}  // namespace jitter::codec
