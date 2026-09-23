// Jitter Observatory - per-hop timing attribution, only where timing permits.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/id.hpp>
#include <jitter/metrics.hpp>
#include <jitter/path.hpp>
#include <jitter/series.hpp>
#include <jitter/time.hpp>
#include <jitter/window.hpp>

namespace jitter {

enum class AttributionState : std::uint8_t {
    Attributed = 0,    // a hop local timing value was produced
    Unsupported = 1,   // the hop cannot be attributed at all
    Insufficient = 2,  // the hop has timing but not enough of it
    Absent = 3,        // the hop emitted no timing in the observed window
};

std::string_view to_string(AttributionState state) noexcept;

struct HopAttribution {
    HopId hop;
    std::uint16_t index = 0;
    std::string name;
    ClockDomainId clock;
    bool clock_comparable = false;
    std::int64_t clock_max_offset_ns = 0;
    AttributionState state = AttributionState::Absent;
    std::uint64_t arrivals = 0;
    std::uint64_t terms = 0;
    bool value_present = false;
    double value = 0.0;
    MetricId metric;
    std::string reason;
};

struct AttributionRequest {
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    // Clock domain of the end to end latency readings of the series. Per-hop values are
    // only formed between the hop clock and this clock.
    ClockDomainId series_clock;
    MetricKey metric = MetricKey::AbsoluteDeltaMean;
    std::uint64_t min_arrivals = 4;
    bool current_generation = true;
    const ClockModel* clocks = nullptr;
    const PathDescriptor* path_descriptor = nullptr;
    std::int64_t now_utc_ns = 0;
    // Arrival readings per hop, in the canonical sample order of the window.
    std::map<HopId, std::vector<TimePoint>> arrivals_by_hop;
};

struct PathAttributionReport {
    SeriesId series;
    PathId path;
    GenerationId generation;
    Ordinal generation_ordinal;
    MetricKey metric = MetricKey::AbsoluteDeltaMean;
    MetricId metric_id;
    bool current_generation = true;
    // True only when every hop on the path produced an attributed value.
    bool complete = false;
    std::uint64_t total_hops = 0;
    std::uint64_t attributed_hops = 0;
    std::vector<HopAttribution> hops;
    std::vector<std::string> reasons;
    Digest digest;
    // Stated boundary of the result. Per-hop values are hop local timing observations.
    // They are not a causal decomposition of end to end latency and never a fault.
    std::string limitation_note;
};

PathAttributionReport attribute_hops(const AttributionRequest& request);

// Builds an attribution request from a window selection. Samples that carry no hop
// timings contribute nothing; the hops they would have covered are reported as absent.
Result<AttributionRequest> build_attribution_request(const WindowSelection& selection,
                                                     const SeriesDescriptor& series,
                                                     const PathDescriptor& path,
                                                     const ClockModel& clocks, MetricKey metric,
                                                     std::uint64_t min_arrivals,
                                                     bool current_generation, std::int64_t now_utc_ns);

}  // namespace jitter
