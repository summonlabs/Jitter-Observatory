// Jitter Observatory - measured series declarations.
// Copyright 2026 Summon Software Labs.
#include <jitter/series.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>

namespace jitter {
namespace {

constexpr std::uint64_t kMaxSeriesNameBytes = 128;
constexpr std::uint64_t kMaxSeriesDescriptionBytes = 512;

}  // namespace

std::string_view to_string(MeasurementKind kind) noexcept {
    switch (kind) {
        case MeasurementKind::Unknown: return "unknown";
        case MeasurementKind::OneWayDelay: return "one_way_delay";
        case MeasurementKind::RoundTripTime: return "round_trip_time";
        case MeasurementKind::InterArrivalGap: return "inter_arrival_gap";
        case MeasurementKind::QueueDwell: return "queue_dwell";
        case MeasurementKind::ServiceTime: return "service_time";
        case MeasurementKind::CompletionLatency: return "completion_latency";
        case MeasurementKind::ReplicationLag: return "replication_lag";
        case MeasurementKind::Custom: return "custom";
    }
    return "unknown";
}

bool parse_measurement_kind(std::string_view value, MeasurementKind& out) noexcept {
    if (value == "unknown") { out = MeasurementKind::Unknown; return true; }
    if (value == "one_way_delay") { out = MeasurementKind::OneWayDelay; return true; }
    if (value == "round_trip_time") { out = MeasurementKind::RoundTripTime; return true; }
    if (value == "inter_arrival_gap") { out = MeasurementKind::InterArrivalGap; return true; }
    if (value == "queue_dwell") { out = MeasurementKind::QueueDwell; return true; }
    if (value == "service_time") { out = MeasurementKind::ServiceTime; return true; }
    if (value == "completion_latency") { out = MeasurementKind::CompletionLatency; return true; }
    if (value == "replication_lag") { out = MeasurementKind::ReplicationLag; return true; }
    if (value == "custom") { out = MeasurementKind::Custom; return true; }
    return false;
}

SeriesId make_series_id(const SeriesDescriptor& descriptor) {
    DigestBuilder builder(kDomainSeries);
    builder.str(descriptor.name);
    builder.u8(static_cast<std::uint8_t>(descriptor.kind));
    builder.u8(static_cast<std::uint8_t>(descriptor.unit));
    builder.id(descriptor.path);
    builder.id(descriptor.clock_domain);
    builder.u8(static_cast<std::uint8_t>(descriptor.origin));
    builder.str(descriptor.description);
    return builder.as_id<SeriesTag>();
}

Status validate_series(const SeriesDescriptor& descriptor) {
    if (descriptor.name.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "series name must not be empty");
    }
    if (descriptor.name.size() > kMaxSeriesNameBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "series name is too long");
    }
    if (descriptor.description.size() > kMaxSeriesDescriptionBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "series description is too long");
    }
    if (descriptor.unit == TimeUnit::Unknown) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "series requires a declared unit for its latency readings");
    }
    const SeriesId derived = make_series_id(descriptor);
    if (!descriptor.id.is_nil() && descriptor.id != derived) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "series id does not match its content addressed identity",
                               "declared=" + descriptor.id.hex() + " derived=" + derived.hex());
    }
    return Status::success();
}

Status SeriesCatalog::register_series(SeriesDescriptor& descriptor) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_series_id(descriptor);
    }
    JITTER_TRY(validate_series(descriptor));
    for (const auto& existing : series_) {
        if (existing.id == descriptor.id) {
            return Status::success();
        }
    }
    if (series_.size() >= Limits::kMaxSeries) {
        return Status::failure(ErrorCode::CapacityExceeded, "series catalog is full",
                               std::to_string(series_.size()));
    }
    series_.push_back(descriptor);
    return Status::success();
}

const SeriesDescriptor* SeriesCatalog::find(SeriesId id) const noexcept {
    for (const auto& entry : series_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<SeriesDescriptor> SeriesCatalog::series() const {
    std::vector<SeriesDescriptor> copy = series_;
    std::sort(copy.begin(), copy.end(),
              [](const SeriesDescriptor& a, const SeriesDescriptor& b) { return a.id < b.id; });
    return copy;
}

}  // namespace jitter
