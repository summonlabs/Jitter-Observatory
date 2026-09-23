// Jitter Observatory - observation sources and replay fencing.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/provenance.hpp>

namespace jitter {

struct SourceDescriptor {
    SourceId id;
    std::string name;
    SourceAuthority authority = SourceAuthority::Unknown;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    // Clock domain in which this source's observation timestamps are expressed.
    ClockDomainId clock_domain;
    std::string description;
    std::uint32_t protocol_revision = 1;
};

SourceId make_source_id(const SourceDescriptor& descriptor);
Status validate_source(const SourceDescriptor& descriptor);

class SourceRegistry {
public:
    // Registers a source and fills in its identity when the caller left it nil.
    Status register_source(SourceDescriptor& descriptor);
    const SourceDescriptor* find(SourceId id) const noexcept;
    bool has(SourceId id) const noexcept { return find(id) != nullptr; }
    SourceAuthority authority_of(SourceId id) const noexcept;
    EvidenceOrigin origin_of(SourceId id) const noexcept;
    std::size_t size() const noexcept { return sources_.size(); }
    std::vector<SourceDescriptor> sources() const;
    void clear() { sources_.clear(); }

private:
    std::vector<SourceDescriptor> sources_;
};

// Per-source monotonic guard. Every accepted observation advances it; every replay,
// reorder or contradiction is classified against it.
struct SourceGuardState {
    SourceIncarnation incarnation;
    SourceEpoch epoch;
    SourceSequence last_sequence;
    Digest last_content;
    bool initialized = false;
};

enum class SequenceVerdict : std::uint8_t {
    Accept = 0,             // new, admissible evidence
    DuplicateIdempotent = 1,// same sequence, byte identical content: not an error, not new evidence
    StaleIncarnation = 2,   // from an older incarnation of the source
    StaleEpoch = 3,         // from an older configuration epoch
    StaleSequence = 4,      // reordered or replayed sequence number
    SequenceConflict = 5,   // same sequence, different content: a genuine contradiction
    Historical = 6,         // explicitly declared archive ingest, not current evidence
};

std::string_view to_string(SequenceVerdict verdict) noexcept;

struct SequenceDecision {
    SequenceVerdict verdict = SequenceVerdict::Accept;
    // Stable machine readable reason.
    std::string reason;
    bool advances_guard = false;
    bool admissible = false;  // true only for Accept
};

// Pure function: the same guard state, provenance and content always yield the same
// decision, with no dependence on arrival timing.
SequenceDecision evaluate_sequence(const SourceGuardState& state, const Provenance& provenance,
                                   const Digest& content_digest, bool historical_mode);

// Applies an accepted (or duplicate) decision to the guard.
void apply_sequence(SourceGuardState& state, const Provenance& provenance, const Digest& content_digest);

}  // namespace jitter
