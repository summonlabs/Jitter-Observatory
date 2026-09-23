// Jitter Observatory - measured series declarations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/path.hpp>
#include <jitter/provenance.hpp>
#include <jitter/time.hpp>

namespace jitter {

// What a series actually measures. The kind participates in the series identity so
// that two series measuring different things can never be conflated.
enum class MeasurementKind : std::uint8_t {
    Unknown = 0,
    OneWayDelay = 1,
    RoundTripTime = 2,
    InterArrivalGap = 3,
    QueueDwell = 4,
    ServiceTime = 5,
    CompletionLatency = 6,
    ReplicationLag = 7,
    Custom = 8,
};

std::string_view to_string(MeasurementKind kind) noexcept;
bool parse_measurement_kind(std::string_view text, MeasurementKind& out) noexcept;

struct SeriesDescriptor {
    SeriesId id;
    std::string name;
    MeasurementKind kind = MeasurementKind::Unknown;
    // Unit of the latency readings this series publishes.
    TimeUnit unit = TimeUnit::Nanoseconds;
    PathId path;
    // Clock domain in which the latency was measured. Per-hop attribution is only
    // possible between hops whose timestamps can be compared with this domain.
    ClockDomainId clock_domain;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    std::string description;
};

SeriesId make_series_id(const SeriesDescriptor& descriptor);
Status validate_series(const SeriesDescriptor& descriptor);

class SeriesCatalog {
public:
    // Registers a series and fills in its identity when the caller left it nil.
    Status register_series(SeriesDescriptor& descriptor);
    const SeriesDescriptor* find(SeriesId id) const noexcept;
    bool has(SeriesId id) const noexcept { return find(id) != nullptr; }
    std::size_t size() const noexcept { return series_.size(); }
    std::vector<SeriesDescriptor> series() const;

private:
    std::vector<SeriesDescriptor> series_;
};

}  // namespace jitter
