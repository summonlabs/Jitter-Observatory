// Jitter Observatory - vendor-neutral latency variance and path timing instability observation.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>

namespace jitter {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProductName = "Jitter Observatory";
inline constexpr std::string_view kProductSlug = "jitter-observatory";
inline constexpr std::string_view kOrganization = "Summon Software Labs";

// Persisted store container format. Bumping this invalidates older/newer files on
// purpose: a store is never silently migrated, it is rejected with an explicit error.
inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::string_view kStoreMagic = "JITTEROB";

// Wire protocol spoken by the ingest listener and the ingest probe.
inline constexpr std::uint32_t kWireProtocolVersion = 1;
inline constexpr std::string_view kWireMagic = "JOW1";

// Canonical export schema. Governs JSON/CSV field names and ordering.
inline constexpr std::uint32_t kExportSchemaVersion = 1;

// Domain separation tags used when deriving content addressed identities.
inline constexpr std::string_view kDomainMeasurement = "jitter.measurement.v1";
inline constexpr std::string_view kDomainSeries = "jitter.series.v1";
inline constexpr std::string_view kDomainPath = "jitter.path.v1";
inline constexpr std::string_view kDomainHop = "jitter.hop.v1";
inline constexpr std::string_view kDomainSource = "jitter.source.v1";
inline constexpr std::string_view kDomainGeneration = "jitter.generation.v1";
inline constexpr std::string_view kDomainClockDomain = "jitter.clock_domain.v1";
inline constexpr std::string_view kDomainWindowPolicy = "jitter.window_policy.v1";
inline constexpr std::string_view kDomainBaseline = "jitter.baseline.v1";
inline constexpr std::string_view kDomainEpisode = "jitter.episode.v1";
inline constexpr std::string_view kDomainMetric = "jitter.metric.v1";
inline constexpr std::string_view kDomainPolicy = "jitter.instability_policy.v1";
inline constexpr std::string_view kDomainRecord = "jitter.record.v1";
inline constexpr std::string_view kDomainTopology = "jitter.topology.v1";
inline constexpr std::string_view kDomainBatch = "jitter.batch.v1";
inline constexpr std::string_view kDomainSummary = "jitter.summary.v1";
inline constexpr std::string_view kDomainEvidence = "jitter.evidence.v1";
inline constexpr std::string_view kDomainAttribution = "jitter.attribution.v1";

}  // namespace jitter
