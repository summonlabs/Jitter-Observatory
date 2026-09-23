// Jitter Observatory - sources and replay fencing.
// Copyright 2026 Summon Software Labs.
#include <jitter/source.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::uint64_t kMaxSourceNameBytes = 128;
constexpr std::uint64_t kMaxSourceDescriptionBytes = 512;

}  // namespace

SourceId make_source_id(const SourceDescriptor& descriptor) {
    DigestBuilder builder(kDomainSource);
    builder.str(descriptor.name);
    builder.u8(static_cast<std::uint8_t>(descriptor.authority));
    builder.u8(static_cast<std::uint8_t>(descriptor.origin));
    builder.id(descriptor.clock_domain);
    builder.u32(descriptor.protocol_revision);
    builder.str(descriptor.description);
    return builder.as_id<SourceTag>();
}

Status validate_source(const SourceDescriptor& descriptor) {
    if (descriptor.name.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "source name must not be empty");
    }
    if (descriptor.name.size() > kMaxSourceNameBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "source name is too long",
                               std::to_string(descriptor.name.size()));
    }
    if (descriptor.description.size() > kMaxSourceDescriptionBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "source description is too long");
    }
    if (descriptor.protocol_revision == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "source protocol revision must be non zero");
    }
    const SourceId derived = make_source_id(descriptor);
    if (!descriptor.id.is_nil() && descriptor.id != derived) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "source id does not match its content addressed identity",
                               "declared=" + descriptor.id.hex() + " derived=" + derived.hex());
    }
    return Status::success();
}

Status SourceRegistry::register_source(SourceDescriptor& descriptor) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_source_id(descriptor);
    }
    JITTER_TRY(validate_source(descriptor));
    for (const auto& existing : sources_) {
        if (existing.id == descriptor.id) {
            return Status::success();
        }
    }
    if (sources_.size() >= Limits::kMaxSources) {
        return Status::failure(ErrorCode::CapacityExceeded, "source registry is full",
                               std::to_string(sources_.size()));
    }
    sources_.push_back(descriptor);
    return Status::success();
}

const SourceDescriptor* SourceRegistry::find(SourceId id) const noexcept {
    for (const auto& source : sources_) {
        if (source.id == id) {
            return &source;
        }
    }
    return nullptr;
}

SourceAuthority SourceRegistry::authority_of(SourceId id) const noexcept {
    const SourceDescriptor* descriptor = find(id);
    return descriptor != nullptr ? descriptor->authority : SourceAuthority::Unknown;
}

EvidenceOrigin SourceRegistry::origin_of(SourceId id) const noexcept {
    const SourceDescriptor* descriptor = find(id);
    return descriptor != nullptr ? descriptor->origin : EvidenceOrigin::Unknown;
}

std::vector<SourceDescriptor> SourceRegistry::sources() const {
    std::vector<SourceDescriptor> copy = sources_;
    std::sort(copy.begin(), copy.end(),
              [](const SourceDescriptor& a, const SourceDescriptor& b) { return a.id < b.id; });
    return copy;
}

std::string_view to_string(SequenceVerdict verdict) noexcept {
    switch (verdict) {
        case SequenceVerdict::Accept: return "accept";
        case SequenceVerdict::DuplicateIdempotent: return "duplicate_idempotent";
        case SequenceVerdict::StaleIncarnation: return "stale_incarnation";
        case SequenceVerdict::StaleEpoch: return "stale_epoch";
        case SequenceVerdict::StaleSequence: return "stale_sequence";
        case SequenceVerdict::SequenceConflict: return "sequence_conflict";
        case SequenceVerdict::Historical: return "historical";
    }
    return "accept";
}

SequenceDecision evaluate_sequence(const SourceGuardState& state, const Provenance& provenance,
                                   const Digest& content_digest, bool historical_mode) {
    SequenceDecision decision;

    if (historical_mode) {
        decision.verdict = SequenceVerdict::Historical;
        decision.reason = "historical_ingest_declared";
        decision.advances_guard = false;
        decision.admissible = false;
        return decision;
    }

    if (!state.initialized) {
        decision.verdict = SequenceVerdict::Accept;
        decision.reason = "first_observation";
        decision.advances_guard = true;
        decision.admissible = true;
        return decision;
    }

    if (provenance.incarnation < state.incarnation) {
        decision.verdict = SequenceVerdict::StaleIncarnation;
        decision.reason = "incarnation_regression";
        decision.advances_guard = false;
        decision.admissible = false;
        return decision;
    }

    if (provenance.incarnation > state.incarnation) {
        decision.verdict = SequenceVerdict::Accept;
        decision.reason = "incarnation_advanced";
        decision.advances_guard = true;
        decision.admissible = true;
        return decision;
    }

    if (provenance.epoch < state.epoch) {
        decision.verdict = SequenceVerdict::StaleEpoch;
        decision.reason = "epoch_regression";
        decision.advances_guard = false;
        decision.admissible = false;
        return decision;
    }

    if (provenance.epoch > state.epoch) {
        decision.verdict = SequenceVerdict::Accept;
        decision.reason = "epoch_advanced";
        decision.advances_guard = true;
        decision.admissible = true;
        return decision;
    }

    if (provenance.sequence > state.last_sequence) {
        decision.verdict = SequenceVerdict::Accept;
        decision.reason = "sequence_advanced";
        decision.advances_guard = true;
        decision.admissible = true;
        return decision;
    }

    if (provenance.sequence == state.last_sequence) {
        if (content_digest == state.last_content) {
            decision.verdict = SequenceVerdict::DuplicateIdempotent;
            decision.reason = "identical_replay";
            decision.advances_guard = false;
            decision.admissible = false;
            return decision;
        }
        decision.verdict = SequenceVerdict::SequenceConflict;
        decision.reason = "same_sequence_different_content";
        decision.advances_guard = false;
        decision.admissible = false;
        return decision;
    }

    decision.verdict = SequenceVerdict::StaleSequence;
    decision.reason = "sequence_regression";
    decision.advances_guard = false;
    decision.admissible = false;
    return decision;
}

void apply_sequence(SourceGuardState& state, const Provenance& provenance,
                    const Digest& content_digest) {
    if (!state.initialized) {
        state.initialized = true;
        state.incarnation = provenance.incarnation;
        state.epoch = provenance.epoch;
        state.last_sequence = provenance.sequence;
        state.last_content = content_digest;
        return;
    }
    if (provenance.incarnation > state.incarnation) {
        state.incarnation = provenance.incarnation;
        state.epoch = provenance.epoch;
        state.last_sequence = provenance.sequence;
        state.last_content = content_digest;
        return;
    }
    if (provenance.epoch > state.epoch) {
        state.epoch = provenance.epoch;
        state.last_sequence = provenance.sequence;
        state.last_content = content_digest;
        return;
    }
    if (provenance.sequence >= state.last_sequence) {
        state.last_sequence = provenance.sequence;
        state.last_content = content_digest;
    }
}

}  // namespace jitter
