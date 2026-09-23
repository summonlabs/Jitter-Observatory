// Jitter Observatory - observation construction and validation.
// Copyright 2026 Summon Software Labs.
#include <jitter/sample.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>
#include <jitter/text.hpp>

namespace jitter {

Digest sample_content_digest(const LatencySample& sample) {
    DigestBuilder builder(kDomainMeasurement);
    builder.id(sample.series);
    builder.id(sample.path);
    builder.id(sample.generation);
    builder.u64(sample.generation_ordinal.value());
    builder.id(sample.provenance.source);
    builder.u64(sample.provenance.incarnation.value());
    builder.u64(sample.provenance.epoch.value());
    builder.u64(sample.provenance.sequence.value());
    builder.u8(static_cast<std::uint8_t>(sample.provenance.authority));
    builder.u8(static_cast<std::uint8_t>(sample.provenance.origin));
    builder.str(sample.provenance.ingest_path);
    builder.i64(sample.latency_ns);
    builder.id(sample.observed_at.domain);
    builder.i64(sample.observed_at.ticks);
    builder.id(sample.received_at.domain);
    builder.i64(sample.received_at.ticks);
    builder.u32(static_cast<std::uint32_t>(sample.hop_timings.size()));
    for (const HopTiming& timing : sample.hop_timings) {
        builder.id(timing.hop);
        builder.id(timing.arrived.domain);
        builder.i64(timing.arrived.ticks);
    }
    builder.u32(static_cast<std::uint32_t>(sample.metadata.size()));
    for (const auto& entry : sample.metadata) {
        builder.str(entry.first);
        builder.str(entry.second);
    }
    return builder.digest();
}

Status validate_sample(const LatencySample& sample) {
    if (sample.series.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "sample requires a series identity");
    }
    if (sample.path.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "sample requires a path identity");
    }
    if (sample.generation.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "sample requires a generation identity");
    }
    if (sample.generation_ordinal.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "sample requires the generation ordinal it belongs to");
    }
    if (sample.provenance.source.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "sample requires a source identity");
    }
    if (sample.provenance.incarnation.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument, "sample requires a source incarnation");
    }
    if (sample.provenance.sequence.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument, "sample requires a source sequence");
    }
    if (sample.provenance.origin == EvidenceOrigin::Unknown) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "sample requires an explicit evidence origin (real, synthetic or unsupported)");
    }
    if (sample.observed_at.domain.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "sample requires the clock domain of its observation time");
    }
    if (sample.received_at.domain.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "sample requires the clock domain of its receive time");
    }
    if (sample.received_at.domain != LocalClockDomain::id()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "receive time must be expressed on the runtime local clock domain",
                               sample.received_at.domain.hex());
    }
    if (sample.latency_ns < Limits::kMinLatencyNs || sample.latency_ns > Limits::kMaxLatencyNs) {
        return Status::failure(ErrorCode::OutOfRange, "sample latency is outside the supported range",
                               text::i64_to_string(sample.latency_ns));
    }
    if (sample.hop_timings.size() > Limits::kMaxHopsPerPath) {
        return Status::failure(ErrorCode::LimitExceeded, "sample carries more hop timings than supported",
                               std::to_string(sample.hop_timings.size()));
    }
    for (const HopTiming& timing : sample.hop_timings) {
        if (timing.hop.is_nil()) {
            return Status::failure(ErrorCode::InvalidArgument, "hop timing requires a hop identity");
        }
        if (timing.arrived.domain.is_nil()) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "hop timing requires the clock domain of its arrival time");
        }
    }
    if (sample.metadata.size() > Limits::kMaxMetadataEntries) {
        return Status::failure(ErrorCode::LimitExceeded, "sample carries too many metadata entries",
                               std::to_string(sample.metadata.size()));
    }
    std::uint64_t total_metadata = 0;
    for (const auto& entry : sample.metadata) {
        if (entry.first.empty()) {
            return Status::failure(ErrorCode::InvalidArgument, "metadata key must not be empty");
        }
        if (entry.first.size() > Limits::kMaxMetadataKeyBytes) {
            return Status::failure(ErrorCode::LimitExceeded, "metadata key is too long", entry.first);
        }
        if (entry.second.size() > Limits::kMaxMetadataValueBytes) {
            return Status::failure(ErrorCode::LimitExceeded, "metadata value is too long", entry.first);
        }
        auto add_key = add_u64(total_metadata, static_cast<std::uint64_t>(entry.first.size()), "metadata");
        if (!add_key.ok()) {
            return add_key.error();
        }
        total_metadata = add_key.value();
        auto add_value = add_u64(total_metadata, static_cast<std::uint64_t>(entry.second.size()), "metadata");
        if (!add_value.ok()) {
            return add_value.error();
        }
        total_metadata = add_value.value();
    }
    if (total_metadata > Limits::kMaxTotalMetadataBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "sample metadata exceeds the total byte budget");
    }
    return Status::success();
}

Result<LatencySample> make_sample(LatencySample sample) {
    JITTER_TRY(validate_sample(sample));
    sample.content = sample_content_digest(sample);
    if (!sample.id.is_nil() && sample.id != MeasurementId::from_digest(sample.content)) {
        return Result<LatencySample>::fail(
            ErrorCode::InvalidArgument,
            "sample id does not match the content addressed identity of its content",
            "declared=" + sample.id.hex() + " derived=" + MeasurementId::from_digest(sample.content).hex());
    }
    sample.id = MeasurementId::from_digest(sample.content);
    return sample;
}

Digest batch_content_digest(const LatencyBatch& batch) {
    DigestBuilder builder(kDomainBatch);
    builder.id(batch.header.source);
    builder.u64(batch.header.incarnation.value());
    builder.u64(batch.header.epoch.value());
    builder.u64(batch.header.sequence.value());
    builder.u8(static_cast<std::uint8_t>(batch.header.authority));
    builder.u8(static_cast<std::uint8_t>(batch.header.origin));
    builder.str(batch.header.ingest_path);
    builder.id(batch.header.series);
    builder.id(batch.header.path);
    builder.id(batch.header.generation);
    builder.u64(batch.header.generation_ordinal.value());
    builder.id(batch.header.observation_clock);
    builder.u32(batch.header.protocol_revision);
    builder.u32(static_cast<std::uint32_t>(batch.samples.size()));
    for (const LatencySample& sample : batch.samples) {
        builder.raw(std::span<const std::uint8_t>(sample.content.data(), Digest::kBytes));
    }
    return builder.digest();
}

Status validate_batch(const LatencyBatch& batch) {
    if (batch.header.source.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch requires a source identity");
    }
    if (batch.header.incarnation.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch requires a source incarnation");
    }
    if (batch.header.sequence.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch requires a source sequence");
    }
    if (batch.header.series.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch requires a series identity");
    }
    if (batch.header.path.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch requires a path identity");
    }
    if (batch.header.generation.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch requires a generation identity");
    }
    if (batch.header.observation_clock.is_nil()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "batch requires the clock domain of its observations");
    }
    if (batch.header.origin == EvidenceOrigin::Unknown) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "batch requires an explicit evidence origin");
    }
    if (batch.samples.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "batch must carry at least one sample");
    }
    if (batch.samples.size() > Limits::kMaxSamplesPerBatch) {
        return Status::failure(ErrorCode::LimitExceeded, "batch carries more samples than supported",
                               std::to_string(batch.samples.size()));
    }
    for (const LatencySample& sample : batch.samples) {
        JITTER_TRY(validate_sample(sample));
        if (sample.series != batch.header.series) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "batch mixes samples from different series");
        }
        if (sample.path != batch.header.path) {
            return Status::failure(ErrorCode::InvalidArgument, "batch mixes samples from different paths");
        }
        if (sample.generation != batch.header.generation) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "batch mixes samples from different generations");
        }
        if (sample.provenance.source != batch.header.source) {
            return Status::failure(ErrorCode::InvalidArgument, "batch mixes samples from different sources");
        }
        if (sample.observed_at.domain != batch.header.observation_clock) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "batch mixes observation clock domains");
        }
    }
    return Status::success();
}

Result<LatencyBatch> make_batch(LatencyBatch batch) {
    JITTER_TRY(validate_batch(batch));
    batch.content = batch_content_digest(batch);
    batch.id = BatchId::from_digest(batch.content);
    return batch;
}

}  // namespace jitter
