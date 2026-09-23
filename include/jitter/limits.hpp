// Jitter Observatory - bounded resource policy.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>

namespace jitter {

// Every externally influenced size in the runtime is checked against these bounds.
// The values are deliberately finite and small enough that a single hostile peer
// cannot exhaust memory, worker time, or disk.
struct Limits {
    // Ingest payload and metadata
    static constexpr std::uint64_t kMaxBatchBytes = 4ull * 1024ull * 1024ull;   // 4 MiB
    static constexpr std::uint64_t kMaxSamplesPerBatch = 4096ull;
    static constexpr std::uint64_t kMaxSamplesPerSecondPerSource = 1000000ull;
    static constexpr std::uint64_t kMaxMetadataEntries = 32ull;
    static constexpr std::uint64_t kMaxMetadataKeyBytes = 96ull;
    static constexpr std::uint64_t kMaxMetadataValueBytes = 512ull;
    static constexpr std::uint64_t kMaxTotalMetadataBytes = 8ull * 1024ull;

    // Topology and identity registries
    static constexpr std::uint64_t kMaxSources = 256ull;
    static constexpr std::uint64_t kMaxClockDomains = 128ull;
    static constexpr std::uint64_t kMaxSeries = 4096ull;
    static constexpr std::uint64_t kMaxPaths = 1024ull;
    static constexpr std::uint64_t kMaxHopsPerPath = 64ull;
    static constexpr std::uint64_t kMaxGenerationsPerPath = 4096ull;
    static constexpr std::uint64_t kMaxGenerationHistoryPerPath = 256ull;

    // Windows and metrics
    static constexpr std::uint64_t kMaxWindowSamples = 65536ull;
    static constexpr std::uint64_t kMaxWindowDurationNs = 3600ull * 1000000000ull;  // 1 hour
    static constexpr std::uint64_t kMaxWatchedMetrics = 32ull;
    static constexpr std::uint64_t kMaxMetricParameters = 8ull;

    // Evidence and history
    static constexpr std::uint64_t kMaxBaselinesPerSeries = 64ull;
    static constexpr std::uint64_t kMaxEpisodesPerSeries = 4096ull;
    static constexpr std::uint64_t kMaxConflictsRetained = 4096ull;
    static constexpr std::uint64_t kMaxExplanationLines = 1024ull;
    static constexpr std::uint64_t kMaxResultRows = 100000ull;
    static constexpr std::uint64_t kMaxHistoryRecords = 65536ull;

    // Persistence
    static constexpr std::uint64_t kMaxStoreRecords = 4194304ull;                 // 4 M records
    static constexpr std::uint64_t kMaxStoreBytes = 16ull * 1024ull * 1024ull * 1024ull;  // 16 GiB
    static constexpr std::uint64_t kMaxRecordPayloadBytes = 1024ull * 1024ull;    // 1 MiB
    static constexpr std::uint64_t kMaxLoadRecordsPerCall = 4194304ull;

    // Runtime
    static constexpr std::uint64_t kMaxQueueDepth = 65536ull;
    static constexpr std::uint64_t kMaxWorkerThreads = 32ull;

    // Transport
    static constexpr std::uint64_t kMaxWireFrameBytes = 4ull * 1024ull * 1024ull;  // 4 MiB
    static constexpr std::uint64_t kMaxWirePreambleBytes = 64ull;
    static constexpr std::uint64_t kMaxConnections = 64ull;

    // Values
    static constexpr std::int64_t kMaxLatencyNs = 3600ll * 1000000000ll;          // 1 hour
    static constexpr std::int64_t kMinLatencyNs = -3600ll * 1000000000ll;
};

}  // namespace jitter
