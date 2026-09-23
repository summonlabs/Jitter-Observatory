// Jitter Observatory - deterministic synthetic scenarios and plan files.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/error.hpp>
#include <jitter/sample.hpp>
#include <jitter/transport.hpp>

namespace jitter {

// A fully specified, deterministic workload. Everything about it is declared up front
// so that two runs on two machines produce byte identical observations, which is what
// makes the determinism proofs meaningful.
struct ScenarioPlan {
    std::uint64_t seed = 12345;
    std::uint64_t samples = 256;
    std::uint64_t batch_size = 16;
    std::int64_t base_latency_ns = 120000;
    std::int64_t jitter_ns = 8000;
    std::int64_t spike_every = 0;  // 0 disables spikes
    std::int64_t spike_ns = 2000000;
    std::int64_t start_utc_ns = 1700000000000000000ll;
    std::int64_t interval_ns = 1000000;
    std::int64_t receive_delay_ns = 50000;
    std::int64_t hop_spread_ns = 3000;
    bool hop_timings = true;

    SourceId source;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal ordinal;
    ClockDomainId observation_clock;
    ClockDomainId hop_clock;
    ClockDomainId second_hop_clock;
    SourceIncarnation incarnation{1};
    SourceEpoch epoch{1};
    SourceSequence sequence_start{1};
    SourceAuthority authority = SourceAuthority::Simulated;
    EvidenceOrigin origin = EvidenceOrigin::Synthetic;
    std::string ingest_path = "in_process";
    std::vector<HopId> hop_ids;
    std::vector<ClockDomainId> hop_clocks;
    // Set when the scenario must attribute hops whose clocks cannot be compared.
    bool incomparable_second_hop = false;
};

// Registers the clock domains, source, series and path described by the plan, opens
// the first route generation and fills the plan's identity fields in.
Result<ScenarioPlan> register_scenario(Observatory& observatory, ScenarioPlan plan,
                                       std::string_view path_name = "synthetic/forward");

// True only when every identity the plan needs is present. make_batches refuses an
// incomplete plan rather than producing a partial run.
Status validate_scenario_plan(const ScenarioPlan& plan);

// Generates the batches the plan describes. Observations are produced with an explicit
// pseudo random stream, so the same plan always yields the same samples.
std::vector<LatencyBatch> make_batches(const ScenarioPlan& plan);

// Convenience: generate and ingest every batch in process order.
Status ingest_plan(Observatory& observatory, const ScenarioPlan& plan, const IngestContext& context);

// Plan files use one "key value" pair per line so that a separate process can be
// driven with exactly the same workload. Unknown keys are rejected, not ignored.
Status write_plan_file(const std::string& path, const ScenarioPlan& plan);
Result<ScenarioPlan> read_plan_file(const std::string& path);

}  // namespace jitter
