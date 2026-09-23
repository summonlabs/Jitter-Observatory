// Jitter Observatory - freshness, provenance conflict and evidence accounting.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/provenance.hpp>
#include <jitter/sample.hpp>
#include <jitter/time.hpp>

namespace jitter {

// The admissibility of a piece of evidence at a stated instant. The states are
// mutually exclusive and are never merged: "we have no data" and "we have data that
// looks calm" are different answers.
enum class EvidenceState : std::uint8_t {
    Fresh = 0,         // within every declared age budget and unabridged
    Stale = 1,         // present but older than the freshness budget
    Expired = 2,       // older than the expiry budget; retained for history only
    Conflicting = 3,   // contradicted by another admissible observation
    Incomplete = 4,    // present but not covering the requested scope
    Unsupported = 5,   // the question cannot be answered with the available inputs
    Unknown = 6,       // present but its age cannot be established
    Missing = 7,       // no evidence at all
};

std::string_view to_string(EvidenceState state) noexcept;
bool parse_evidence_state(std::string_view text, EvidenceState& out) noexcept;
// True only for Fresh: the states that may support a positive assertion.
bool is_positive_evidence(EvidenceState state) noexcept;

struct FreshnessPolicy {
    std::string name = "default";
    // Budget for now_utc - received_at_utc on the runtime's own clock.
    std::int64_t max_ingest_age_ns = 5000000000ll;
    std::int64_t expiry_ingest_age_ns = 60000000000ll;
    // Budget for now_utc - observed_at, which requires a comparable clock domain.
    std::int64_t max_observation_age_ns = 2000000000ll;
    std::int64_t expiry_observation_age_ns = 30000000000ll;
    // A reading whose observation time is further in the future than this is not
    // trusted: it is reported as Unknown rather than Fresh.
    std::int64_t future_tolerance_ns = 1000000000ll;
    // When set, evidence whose observation age cannot be established is Unknown rather
    // than Fresh. Conservative by default.
    bool require_observation_age = true;
};

FreshnessPolicyId make_freshness_policy_id(const FreshnessPolicy& policy);
Status validate_freshness_policy(const FreshnessPolicy& policy);

struct SampleAssessment {
    MeasurementId sample;
    EvidenceState state = EvidenceState::Unknown;
    std::string reason;
    AgeAssessment ages;
};

// Evaluates one observation against a policy at an explicitly supplied instant. The
// function never reads the ambient clock, so the same inputs always produce the same
// state.
SampleAssessment assess_sample(const LatencySample& sample, const ClockModel& clocks,
                               std::int64_t now_utc_ns, const FreshnessPolicy& policy);

struct EvidenceSummary {
    std::uint64_t total = 0;
    std::uint64_t fresh = 0;
    std::uint64_t stale = 0;
    std::uint64_t expired = 0;
    std::uint64_t conflicting = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t unsupported = 0;
    std::uint64_t unknown = 0;
    std::uint64_t real_origin = 0;
    std::uint64_t synthetic_origin = 0;
    // Deterministic dominant state: conflicting outranks everything, then fresh, then
    // stale, then expired, then unsupported, then unknown, then incomplete, then missing.
    EvidenceState dominant = EvidenceState::Missing;
    std::vector<std::string> reasons;

    // A positive assertion about the current state requires at least one fresh
    // observation and no contradicted observation.
    bool admits_positive_assertion() const noexcept {
        return fresh > 0 && conflicting == 0;
    }
    void recompute_dominant();
};

EvidenceSummary summarize_evidence(const std::vector<SampleAssessment>& assessments);

enum class ConflictOutcome : std::uint8_t {
    ResolvedByAuthority = 0,
    UnresolvedEqualAuthority = 1,
    UnresolvedSameSource = 2,
    RefusedUnknownAuthority = 3,
};

std::string_view to_string(ConflictOutcome outcome) noexcept;

// A contradiction between two observations that claim the same position in a source's
// sequence but carry different content.
struct ConflictRecord {
    SeriesId series;
    PathId path;
    GenerationId generation;
    SourceSequence sequence;
    SourceId source_a;
    SourceId source_b;
    Digest content_a;
    Digest content_b;
    SourceAuthority authority_a = SourceAuthority::Unknown;
    SourceAuthority authority_b = SourceAuthority::Unknown;
    std::int64_t detected_at_utc_ns = 0;
    ConflictOutcome outcome = ConflictOutcome::UnresolvedEqualAuthority;
    std::optional<SourceId> winner;
    std::string reason;
    Digest identity;

    bool resolved() const noexcept { return outcome == ConflictOutcome::ResolvedByAuthority; }
};

ConflictRecord make_conflict_record(ConflictRecord record);

// Resolves a contradiction using authority only. Equal authority is never broken by
// arrival order, value magnitude or recency.
ConflictOutcome resolve_conflict(SourceAuthority a, SourceAuthority b, SourceId source_a,
                                 SourceId source_b, std::optional<SourceId>& winner);

class ConflictLog {
public:
    // Returns true when the contradiction was newly recorded, false when it was an
    // already known record.
    bool append(ConflictRecord record);
    std::size_t size() const noexcept { return records_.size(); }
    std::uint64_t dropped() const noexcept { return dropped_; }
    const std::vector<ConflictRecord>& records() const noexcept { return records_; }
    std::vector<ConflictRecord> for_series(SeriesId series) const;
    bool has_conflict(SeriesId series, GenerationId generation) const noexcept;
    void clear() noexcept;

private:
    std::vector<ConflictRecord> records_;
    std::uint64_t dropped_ = 0;
};

}  // namespace jitter
