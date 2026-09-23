// Jitter Observatory - per-hop timing attribution.
// Copyright 2026 Summon Software Labs.
#include <jitter/attribution.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::string_view kLimitationNote =
    "hop local timing observations only; not a causal decomposition of end to end latency and "
    "never a fault classification";

void add_reason(std::vector<std::string>& reasons, const std::string& reason) {
    if (reason.empty()) {
        return;
    }
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(reason);
    }
}

Result<std::vector<std::int64_t>> to_nanos(const std::vector<TimePoint>& readings,
                                           const ClockDomainDescriptor& descriptor) {
    const auto scale = nanos_per_unit(descriptor.unit);
    if (!scale.has_value()) {
        return Result<std::vector<std::int64_t>>::fail(
            ErrorCode::Unsupported,
            "hop clock unit has no declared rate and cannot be converted to time",
            std::string(to_string(descriptor.unit)));
    }
    std::vector<std::int64_t> out;
    out.reserve(readings.size());
    for (const TimePoint& reading : readings) {
        std::int64_t scaled = 0;
        if (!checked_mul_i64(reading.ticks, scale.value(), scaled)) {
            return Result<std::vector<std::int64_t>>::fail(
                ErrorCode::Overflow, "hop timing conversion overflowed",
                text::i64_to_string(reading.ticks));
        }
        out.push_back(scaled);
    }
    return out;
}

}  // namespace

std::string_view to_string(AttributionState state) noexcept {
    switch (state) {
        case AttributionState::Attributed: return "attributed";
        case AttributionState::Unsupported: return "unsupported";
        case AttributionState::Insufficient: return "insufficient";
        case AttributionState::Absent: return "absent";
    }
    return "absent";
}

PathAttributionReport attribute_hops(const AttributionRequest& request) {
    PathAttributionReport report;
    report.series = request.series;
    report.path = request.path;
    report.generation = request.generation;
    report.generation_ordinal = request.generation_ordinal;
    report.metric = request.metric;
    report.metric_id = metric_registry().definition(request.metric).id;
    report.current_generation = request.current_generation;
    report.limitation_note = std::string(kLimitationNote);

    if (request.clocks == nullptr || request.path_descriptor == nullptr) {
        add_reason(report.reasons, "attribution_requires_clock_model_and_path");
        report.digest = Digest::of("attribution:missing_inputs");
        return report;
    }
    if (!request.current_generation) {
        add_reason(report.reasons, "generation_is_not_current");
    }

    const ClockDomainDescriptor* series_clock = request.clocks->find(request.series_clock);
    if (series_clock == nullptr) {
        add_reason(report.reasons, "series_clock_domain_unknown");
    }

    report.total_hops = static_cast<std::uint64_t>(request.path_descriptor->hops.size());

    for (const HopDescriptor& hop : request.path_descriptor->hops) {
        HopAttribution attribution;
        attribution.hop = hop.id;
        attribution.index = hop.index;
        attribution.name = hop.name;
        attribution.metric = report.metric_id;

        const auto found = request.arrivals_by_hop.find(hop.id);
        if (found == request.arrivals_by_hop.end() || found->second.empty()) {
            attribution.state = AttributionState::Absent;
            attribution.reason = "hop_timing_absent";
            add_reason(report.reasons, "hop_timing_absent:" + hop.name);
            report.hops.push_back(attribution);
            continue;
        }

        const std::vector<TimePoint>& arrivals = found->second;
        attribution.arrivals = static_cast<std::uint64_t>(arrivals.size());
        attribution.clock = arrivals.front().domain;

        if (arrivals.front().domain.is_nil()) {
            attribution.state = AttributionState::Unsupported;
            attribution.reason = "hop_timing_has_no_clock_domain";
            add_reason(report.reasons, "hop_timing_has_no_clock_domain:" + hop.name);
            report.hops.push_back(attribution);
            continue;
        }

        for (const TimePoint& reading : arrivals) {
            if (reading.domain != arrivals.front().domain) {
                attribution.state = AttributionState::Unsupported;
                attribution.reason = "hop_timing_mixes_clock_domains";
                add_reason(report.reasons, "hop_timing_mixes_clock_domains:" + hop.name);
                break;
            }
        }
        if (attribution.state == AttributionState::Unsupported) {
            report.hops.push_back(attribution);
            continue;
        }

        const ComparabilityAssessment comparability =
            request.clocks->assess(arrivals.front().domain, request.series_clock, request.now_utc_ns);
        attribution.clock_comparable = comparability.comparable();
        attribution.clock_max_offset_ns = comparability.max_offset_ns;

        if (!comparability.comparable()) {
            // No comparison, no number. The hop is reported as unsupported with the
            // exact reason, and no value is fabricated.
            attribution.state = AttributionState::Unsupported;
            attribution.reason = "hop_clock_incomparable:" + comparability.reason;
            add_reason(report.reasons,
                       "hop_clock_incomparable:" + hop.name + ":" + comparability.reason);
            report.hops.push_back(attribution);
            continue;
        }

        if (attribution.arrivals < request.min_arrivals) {
            attribution.state = AttributionState::Insufficient;
            attribution.reason = "fewer_arrivals_than_required";
            add_reason(report.reasons, "fewer_arrivals_than_required:" + hop.name);
            report.hops.push_back(attribution);
            continue;
        }

        const ClockDomainDescriptor* hop_clock = request.clocks->find(arrivals.front().domain);
        if (hop_clock == nullptr) {
            attribution.state = AttributionState::Unsupported;
            attribution.reason = "hop_clock_domain_unknown";
            add_reason(report.reasons, "hop_clock_domain_unknown:" + hop.name);
            report.hops.push_back(attribution);
            continue;
        }

        auto nanos = to_nanos(arrivals, *hop_clock);
        if (!nanos.ok()) {
            attribution.state = AttributionState::Unsupported;
            attribution.reason = std::string("hop_timing_not_convertible:") +
                                 std::string(to_string(nanos.code()));
            add_reason(report.reasons, "hop_timing_not_convertible:" + hop.name);
            report.hops.push_back(attribution);
            continue;
        }

        auto value = metric_registry().compute(request.metric, nanos.value());
        if (!value.ok()) {
            attribution.state = value.code() == ErrorCode::InsufficientEvidence
                                    ? AttributionState::Insufficient
                                    : AttributionState::Unsupported;
            attribution.reason = std::string("metric_unavailable:") + std::string(to_string(value.code()));
            add_reason(report.reasons, "hop_metric_unavailable:" + hop.name + ":" +
                                           std::string(to_string(value.code())));
            report.hops.push_back(attribution);
            continue;
        }

        attribution.state = AttributionState::Attributed;
        attribution.value_present = true;
        attribution.value = value.value().value;
        attribution.terms = value.value().term_count;
        attribution.reason = "attributed";
        ++report.attributed_hops;
        report.hops.push_back(attribution);
    }

    report.complete = !request.path_descriptor->hops.empty() &&
                      report.attributed_hops == report.total_hops;
    if (request.path_descriptor->hops.empty()) {
        add_reason(report.reasons, "path_declares_no_hops");
    }
    if (!report.complete && !request.path_descriptor->hops.empty()) {
        add_reason(report.reasons, "attribution_incomplete");
    }
    if (!report.current_generation) {
        report.complete = false;
    }

    std::sort(report.reasons.begin(), report.reasons.end());
    report.reasons.erase(std::unique(report.reasons.begin(), report.reasons.end()), report.reasons.end());

    DigestBuilder builder(kDomainAttribution);
    builder.id(report.series);
    builder.id(report.path);
    builder.id(report.generation);
    builder.u64(report.generation_ordinal.value());
    builder.id(report.metric_id);
    builder.boolean(report.complete);
    builder.boolean(report.current_generation);
    builder.u32(static_cast<std::uint32_t>(report.hops.size()));
    for (const HopAttribution& hop : report.hops) {
        builder.id(hop.hop);
        builder.u16(hop.index);
        builder.u8(static_cast<std::uint8_t>(hop.state));
        builder.boolean(hop.value_present);
        builder.f64(hop.value);
        builder.u64(hop.terms);
        builder.str(hop.reason);
    }
    report.digest = builder.digest();
    return report;
}

Result<AttributionRequest> build_attribution_request(const WindowSelection& selection,
                                                     const SeriesDescriptor& series,
                                                     const PathDescriptor& path,
                                                     const ClockModel& clocks, MetricKey metric,
                                                     std::uint64_t min_arrivals,
                                                     bool current_generation, std::int64_t now_utc_ns) {
    AttributionRequest request;
    request.series = series.id;
    request.path = path.id;
    request.generation = selection.samples.empty() ? GenerationId{} : selection.samples.front()->generation;
    request.generation_ordinal =
        selection.samples.empty() ? Ordinal{} : selection.samples.front()->generation_ordinal;
    request.series_clock = series.clock_domain;
    request.metric = metric;
    request.min_arrivals = min_arrivals;
    request.current_generation = current_generation;
    request.clocks = &clocks;
    request.path_descriptor = &path;
    request.now_utc_ns = now_utc_ns;

    if (selection.samples.empty()) {
        return Result<AttributionRequest>::fail(ErrorCode::NoEvidence,
                                                "cannot attribute hops without observations");
    }

    for (const LatencySample* sample : selection.samples) {
        if (sample->series != series.id) {
            return Result<AttributionRequest>::fail(ErrorCode::Conflict,
                                                    "attribution input mixes series",
                                                    sample->series.hex());
        }
        if (sample->path != path.id) {
            return Result<AttributionRequest>::fail(ErrorCode::Conflict, "attribution input mixes paths",
                                                    sample->path.hex());
        }
        for (const HopTiming& timing : sample->hop_timings) {
            request.arrivals_by_hop[timing.hop].push_back(timing.arrived);
        }
    }
    return request;
}

}  // namespace jitter
