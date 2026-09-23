// Jitter Observatory - shared test fixtures.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <jitter/bytes.hpp>
#include <jitter/engine.hpp>
#include <jitter/scenario.hpp>

#include "support/harness.hpp"

namespace jitter::test {

inline SourceId synthetic_source_id(std::string_view name) {
    DigestBuilder builder(kDomainSource);
    builder.str(name);
    return builder.as_id<SourceTag>();
}

inline ClockDomainId synthetic_clock_id(std::string_view name) {
    ClockDomainDescriptor descriptor;
    descriptor.name = std::string(name);
    descriptor.kind = ClockKind::Synthetic;
    descriptor.unit = TimeUnit::Nanoseconds;
    descriptor.epoch_note = "test clock";
    return make_clock_domain_id(descriptor);
}

inline SeriesId synthetic_series_id(std::string_view name) {
    DigestBuilder builder(kDomainSeries);
    builder.str(name);
    return builder.as_id<SeriesTag>();
}

inline PathId synthetic_path_id(std::string_view name) {
    DigestBuilder builder(kDomainPath);
    builder.str(name);
    return builder.as_id<PathTag>();
}

inline GenerationId synthetic_generation_id(std::string_view name) {
    DigestBuilder builder(kDomainGeneration);
    builder.str(name);
    return builder.as_id<GenerationTag>();
}

struct SampleSpec {
    std::int64_t latency_ns = 100000;
    std::int64_t observed_ticks = 1000;
    std::int64_t received_ticks = 2000;
    ClockDomainId observation_clock;
    SourceId source;
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal ordinal{1};
    SourceIncarnation incarnation{1};
    SourceEpoch epoch{1};
    SourceSequence sequence{1};
    SourceAuthority authority = SourceAuthority::Corroborating;
    EvidenceOrigin origin = EvidenceOrigin::Synthetic;
    std::string ingest_path = "test";
    std::vector<HopTiming> hop_timings;
};

inline Result<LatencySample> build_sample(SampleSpec spec) {
    LatencySample sample;
    sample.series = spec.series;
    sample.path = spec.path;
    sample.generation = spec.generation;
    sample.generation_ordinal = spec.ordinal;
    sample.provenance.source = spec.source;
    sample.provenance.incarnation = spec.incarnation;
    sample.provenance.epoch = spec.epoch;
    sample.provenance.sequence = spec.sequence;
    sample.provenance.authority = spec.authority;
    sample.provenance.origin = spec.origin;
    sample.provenance.ingest_path = spec.ingest_path;
    sample.latency_ns = spec.latency_ns;
    sample.observed_at.domain = spec.observation_clock;
    sample.observed_at.ticks = spec.observed_ticks;
    sample.received_at.domain = LocalClockDomain::id();
    sample.received_at.ticks = spec.received_ticks;
    sample.hop_timings = spec.hop_timings;
    return make_sample(sample);
}

// An engine that has already registered and ingested a synthetic scenario.
struct EngineFixture {
    EngineConfig config;
    ScenarioPlan plan;
    IngestContext context;
    IngestContext observed_context;
    std::vector<LatencyBatch> batches;
    std::unique_ptr<Observatory> observatory;

    SummaryRequest summary_request() const {
        SummaryRequest request;
        request.series = plan.series;
        request.path = plan.path;
        request.generation = plan.generation;
        request.generation_ordinal = plan.ordinal;
        request.window = config.window;
        request.policy = config.policy;
        request.metrics = config.metrics;
        request.now_utc_ns = context.now_utc_ns;
        request.current_generation = true;
        return request;
    }
};

inline EngineConfig fixture_config(std::uint64_t samples_in_window = 256,
                                   std::uint64_t capacity = 512) {
    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 1;
    config.metrics = default_metric_set();
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = samples_in_window;
    window.capacity = capacity;
    config.window = window;
    return config;
}

inline Result<EngineFixture> build_engine(ScenarioPlan plan, EngineConfig config, bool ingest = true,
                                           std::string_view path_name = "test/forward") {
    EngineFixture fixture;
    fixture.config = config;
    fixture.observatory = std::make_unique<Observatory>(config);
    JITTER_TRY(fixture.observatory->initialize());
    auto registered = register_scenario(*fixture.observatory, plan, path_name);
    if (!registered.ok()) {
        return registered.error();
    }
    fixture.plan = registered.value();
    fixture.context.now_utc_ns = fixture.plan.start_utc_ns +
                                 static_cast<std::int64_t>(fixture.plan.samples) *
                                     fixture.plan.interval_ns +
                                 fixture.plan.receive_delay_ns;
    fixture.observed_context = fixture.context;
    fixture.batches = make_batches(fixture.plan);
    if (ingest) {
        for (const LatencyBatch& batch : fixture.batches) {
            auto outcome = fixture.observatory->ingest(batch, fixture.context);
            if (!outcome.ok()) {
                return outcome.error();
            }
        }
    }
    return fixture;
}

}  // namespace jitter::test
