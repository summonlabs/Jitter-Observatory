// Jitter Observatory - deterministic synthetic scenarios and plan files.
// Copyright 2026 Summon Software Labs.
#include <jitter/scenario.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <jitter/bytes.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

// SplitMix64: a small, fully specified generator so that a plan is reproducible on any
// platform without depending on a library implementation.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [-1, 1].
    double next_signed_unit() {
        const std::uint64_t raw = next() >> 11;                  // 53 bits
        const double unit = static_cast<double>(raw) / 9007199254740992.0;  // 2^53
        return (unit * 2.0) - 1.0;
    }

private:
    std::uint64_t state_;
};

std::string field(const std::string& line, std::size_t index) {
    std::istringstream stream(line);
    std::string token;
    for (std::size_t i = 0; i <= index; ++i) {
        if (!(stream >> token)) {
            return {};
        }
    }
    return token;
}

Status write_key(std::ofstream& stream, std::string_view key, const std::string& value) {
    stream << key << ' ' << value << '\n';
    if (!stream) {
        return Status::failure(ErrorCode::IoFailure, "plan file write failed", std::string(key));
    }
    return Status::success();
}

}  // namespace

Status validate_scenario_plan(const ScenarioPlan& plan) {
    if (plan.samples == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "scenario plan requires at least one sample");
    }
    if (plan.batch_size == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "scenario plan requires a non zero batch size");
    }
    if (plan.source.is_nil() || plan.series.is_nil() || plan.path.is_nil() ||
        plan.generation.is_nil() || plan.observation_clock.is_nil() || plan.ordinal.is_unset() ||
        plan.incarnation.is_unset() || plan.sequence_start.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "scenario plan is missing one or more required identities");
    }
    if (plan.hop_ids.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "scenario plan declares no hops");
    }
    if (plan.hop_ids.size() != plan.hop_clocks.size()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "scenario plan hop identities and hop clocks do not line up");
    }
    return Status::success();
}

Result<ScenarioPlan> register_scenario(Observatory& observatory, ScenarioPlan plan,
                                       std::string_view path_name) {
    // Identities are content addressed, so they must be derived before the descriptor is
    // registered: registration normalises a copy and the caller keeps the derived value.
    ClockDomainDescriptor observation;
    observation.name = "synthetic.observation";
    observation.kind = ClockKind::Synthetic;
    observation.unit = TimeUnit::Nanoseconds;
    observation.epoch_note = "origin at the start of the synthetic run";
    observation.declared_utc_aligned = false;
    observation.id = make_clock_domain_id(observation);
    JITTER_TRY(observatory.register_clock_domain(observation));
    plan.observation_clock = observation.id;

    ClockDomainDescriptor hop_clock;
    hop_clock.name = "synthetic.hop";
    hop_clock.kind = ClockKind::Synthetic;
    hop_clock.unit = TimeUnit::Nanoseconds;
    hop_clock.epoch_note = "origin at the start of the synthetic run";
    hop_clock.id = make_clock_domain_id(hop_clock);
    JITTER_TRY(observatory.register_clock_domain(hop_clock));
    plan.hop_clock = hop_clock.id;

    ClockDomainDescriptor second_hop_clock;
    second_hop_clock.name = plan.incomparable_second_hop ? "synthetic.hop.b"
                                                         : "synthetic.hop.a2";
    second_hop_clock.kind = plan.incomparable_second_hop ? ClockKind::HardwareCounter
                                                         : ClockKind::Synthetic;
    second_hop_clock.unit = plan.incomparable_second_hop ? TimeUnit::CounterTicks
                                                         : TimeUnit::Nanoseconds;
    second_hop_clock.epoch_note = "free running counter with no declared relation to the "
                                  "observation clock";
    second_hop_clock.id = make_clock_domain_id(second_hop_clock);
    JITTER_TRY(observatory.register_clock_domain(second_hop_clock));
    plan.second_hop_clock = second_hop_clock.id;

    // The synthetic generator stamps observation times and receive times from one run
    // clock, so the observation domain really is the runtime's own timeline. That is a
    // fact about this generator, and it is declared explicitly rather than assumed.
    ClockEquivalence observation_equivalence;
    observation_equivalence.a = plan.observation_clock;
    observation_equivalence.b = LocalClockDomain::id();
    observation_equivalence.max_offset_ns = 0;
    observation_equivalence.declared_by_authority = SourceAuthority::Authoritative;
    observation_equivalence.justification =
        "the synthetic generator derives observation and receive times from one run clock, so the "
        "observation domain is the runtime timeline";
    JITTER_TRY(observatory.declare_clock_equivalence(observation_equivalence));

    // Hop clocks are derived from the same run clock, so they are comparable with the
    // series clock. A hop that the scenario declares incomparable deliberately has no
    // declaration, and per-hop attribution for it is therefore refused.
    ClockEquivalence hop_equivalence;
    hop_equivalence.a = plan.hop_clock;
    hop_equivalence.b = plan.observation_clock;
    hop_equivalence.max_offset_ns = 500;
    hop_equivalence.declared_by_authority = SourceAuthority::Authoritative;
    hop_equivalence.justification = "the synthetic hop clock is derived from the run clock";
    JITTER_TRY(observatory.declare_clock_equivalence(hop_equivalence));

    if (!plan.incomparable_second_hop) {
        ClockEquivalence second_equivalence;
        second_equivalence.a = plan.second_hop_clock;
        second_equivalence.b = plan.observation_clock;
        second_equivalence.max_offset_ns = 500;
        second_equivalence.declared_by_authority = SourceAuthority::Authoritative;
        second_equivalence.justification =
            "the second synthetic hop clock is derived from the run clock";
        JITTER_TRY(observatory.declare_clock_equivalence(second_equivalence));
    }

    SourceDescriptor source;
    source.name = "synthetic.generator";
    source.authority = plan.authority;
    source.origin = plan.origin;
    source.clock_domain = plan.observation_clock;
    source.description = "deterministic synthetic observation source used by tools and tests";
    source.protocol_revision = 1;
    source.id = make_source_id(source);
    JITTER_TRY(observatory.register_source(source));
    plan.source = source.id;

    PathDescriptor path;
    path.name = std::string(path_name);
    path.origin = EvidenceOrigin::Synthetic;
    path.description = "synthetic multi hop path";

    HopDescriptor first;
    first.index = 0;
    first.name = "ingress";
    first.timing_clock = plan.hop_clock;
    first.origin = EvidenceOrigin::Synthetic;
    first.device_note = "synthetic hop, no hardware claim";
    path.hops.push_back(first);

    HopDescriptor second;
    second.index = 1;
    second.name = "transit";
    second.timing_clock = plan.second_hop_clock;
    second.origin = EvidenceOrigin::Synthetic;
    second.device_note = "synthetic hop, no hardware claim";
    path.hops.push_back(second);

    HopDescriptor third;
    third.index = 2;
    third.name = "egress";
    third.timing_clock = std::nullopt;
    third.origin = EvidenceOrigin::Synthetic;
    third.device_note = "synthetic hop that emits no timing";
    path.hops.push_back(third);

    path.id = make_path_id(path);
    for (HopDescriptor& hop : path.hops) {
        hop.id = make_hop_id(hop, path.id);
    }
    JITTER_TRY(observatory.register_path(path));
    plan.path = path.id;
    plan.hop_ids.clear();
    plan.hop_clocks.clear();
    for (const HopDescriptor& hop : path.hops) {
        plan.hop_ids.push_back(hop.id);
        plan.hop_clocks.push_back(hop.timing_clock.value_or(ClockDomainId{}));
    }

    SeriesDescriptor series;
    series.name = "synthetic.one_way_delay";
    series.kind = MeasurementKind::OneWayDelay;
    series.unit = TimeUnit::Nanoseconds;
    series.path = path.id;
    series.clock_domain = observation.id;
    series.origin = plan.origin;
    series.description = "synthetic one way delay series";
    series.id = make_series_id(series);
    JITTER_TRY(observatory.register_series(series));
    plan.series = series.id;

    auto outcome = observatory.observe_topology(plan.path, make_topology(path), plan.start_utc_ns,
                                                "scenario_registered");
    if (!outcome.ok()) {
        return outcome.error();
    }
    plan.generation = outcome.value().generation.id;
    plan.ordinal = outcome.value().generation.ordinal;

    // Registration is validated before it is handed back: a plan that cannot produce
    // observations must never reach the ingest path silently.
    JITTER_TRY(validate_scenario_plan(plan));
    return plan;
}

std::vector<LatencyBatch> make_batches(const ScenarioPlan& plan) {
    std::vector<LatencyBatch> batches;
    if (!validate_scenario_plan(plan).ok()) {
        return batches;
    }
    const std::uint64_t batch_size = plan.batch_size == 0 ? 1 : plan.batch_size;
    SplitMix64 random(plan.seed);
    std::uint64_t produced = 0;
    SourceSequence sequence = plan.sequence_start;
    bool first_batch = true;

    while (produced < plan.samples) {
        LatencyBatch batch;
        batch.header.source = plan.source;
        batch.header.incarnation = plan.incarnation;
        batch.header.epoch = plan.epoch;
        batch.header.sequence = sequence;
        batch.header.authority = plan.authority;
        batch.header.origin = plan.origin;
        batch.header.ingest_path = plan.ingest_path;
        batch.header.series = plan.series;
        batch.header.path = plan.path;
        batch.header.generation = plan.generation;
        batch.header.generation_ordinal = plan.ordinal;
        batch.header.observation_clock = plan.observation_clock;
        batch.header.protocol_revision = 1;

        const std::uint64_t remaining = plan.samples - produced;
        const std::uint64_t count = std::min(batch_size, remaining);
        for (std::uint64_t i = 0; i < count; ++i, ++produced) {
            LatencySample sample;
            sample.series = plan.series;
            sample.path = plan.path;
            sample.generation = plan.generation;
            sample.generation_ordinal = plan.ordinal;
            sample.provenance.source = plan.source;
            sample.provenance.incarnation = plan.incarnation;
            sample.provenance.epoch = plan.epoch;
            sample.provenance.sequence = sequence;
            sample.provenance.authority = plan.authority;
            sample.provenance.origin = plan.origin;
            sample.provenance.ingest_path = plan.ingest_path;

            const auto index = static_cast<std::int64_t>(produced);
            std::int64_t latency = plan.base_latency_ns +
                                   static_cast<std::int64_t>(random.next_signed_unit() *
                                                             static_cast<double>(plan.jitter_ns));
            if (plan.spike_every > 0 && index > 0 && (index % plan.spike_every) == 0) {
                latency += plan.spike_ns;
            }
            sample.latency_ns = latency;

            const std::int64_t observed_ticks = plan.start_utc_ns + (index * plan.interval_ns);
            sample.observed_at.domain = plan.observation_clock;
            sample.observed_at.ticks = observed_ticks;
            sample.received_at.domain = LocalClockDomain::id();
            sample.received_at.ticks = observed_ticks + plan.receive_delay_ns;

            if (plan.hop_timings) {
                std::size_t timing_index = 0;
                for (std::size_t hop = 0; hop < plan.hop_ids.size(); ++hop) {
                    if (plan.hop_clocks[hop].is_nil()) {
                        continue;
                    }
                    HopTiming timing;
                    timing.hop = plan.hop_ids[hop];
                    timing.arrived.domain = plan.hop_clocks[hop];
                    const std::int64_t share =
                        plan.hop_spread_ns * static_cast<std::int64_t>(timing_index + 1);
                    const std::int64_t drift = static_cast<std::int64_t>(
                        random.next_signed_unit() * static_cast<double>(plan.hop_spread_ns));
                    timing.arrived.ticks = observed_ticks + share + drift;
                    sample.hop_timings.push_back(timing);
                    ++timing_index;
                }
            }

            auto made = make_sample(sample);
            if (!made.ok()) {
                continue;
            }
            batch.samples.push_back(made.value());
        }

        auto made_batch = make_batch(batch);
        if (!made_batch.ok()) {
            continue;
        }
        batches.push_back(made_batch.value());
        sequence = sequence.next();
        first_batch = false;
    }
    static_cast<void>(first_batch);
    return batches;
}

Status ingest_plan(Observatory& observatory, const ScenarioPlan& plan, const IngestContext& context) {
    JITTER_TRY(validate_scenario_plan(plan));
    const std::vector<LatencyBatch> batches = make_batches(plan);
    if (batches.empty()) {
        return Status::failure(ErrorCode::Conflict,
                               "scenario produced no batches, so nothing was ingested");
    }
    IngestContext current = context;
    std::uint64_t produced = 0;
    for (const LatencyBatch& batch : batches) {
        produced += batch.samples.size();
    }
    if (produced != plan.samples) {
        return Status::failure(ErrorCode::Conflict,
                               "scenario produced fewer observations than it declared",
                               std::to_string(produced) + "/" + std::to_string(plan.samples));
    }
    for (const LatencyBatch& batch : batches) {
        auto outcome = observatory.ingest(batch, current);
        if (!outcome.ok()) {
            return outcome.error();
        }
        if (!outcome.value().admitted() && outcome.value().historical == 0) {
            return Status::failure(ErrorCode::Conflict,
                                   "scenario batch was not admitted: " + outcome.value().reason);
        }
    }
    return Status::success();
}

Status write_plan_file(const std::string& path, const ScenarioPlan& plan) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return Status::failure(ErrorCode::IoFailure, "plan file could not be created", path);
    }
    JITTER_TRY(write_key(stream, "seed", text::u64_to_string(plan.seed)));
    JITTER_TRY(write_key(stream, "samples", text::u64_to_string(plan.samples)));
    JITTER_TRY(write_key(stream, "batch_size", text::u64_to_string(plan.batch_size)));
    JITTER_TRY(write_key(stream, "base_latency_ns", text::i64_to_string(plan.base_latency_ns)));
    JITTER_TRY(write_key(stream, "jitter_ns", text::i64_to_string(plan.jitter_ns)));
    JITTER_TRY(write_key(stream, "spike_every", text::i64_to_string(plan.spike_every)));
    JITTER_TRY(write_key(stream, "spike_ns", text::i64_to_string(plan.spike_ns)));
    JITTER_TRY(write_key(stream, "start_utc_ns", text::i64_to_string(plan.start_utc_ns)));
    JITTER_TRY(write_key(stream, "interval_ns", text::i64_to_string(plan.interval_ns)));
    JITTER_TRY(write_key(stream, "receive_delay_ns", text::i64_to_string(plan.receive_delay_ns)));
    JITTER_TRY(write_key(stream, "hop_spread_ns", text::i64_to_string(plan.hop_spread_ns)));
    JITTER_TRY(write_key(stream, "hop_timings", plan.hop_timings ? "1" : "0"));
    JITTER_TRY(write_key(stream, "incomparable_second_hop", plan.incomparable_second_hop ? "1" : "0"));
    JITTER_TRY(write_key(stream, "source", plan.source.hex()));
    JITTER_TRY(write_key(stream, "series", plan.series.hex()));
    JITTER_TRY(write_key(stream, "path", plan.path.hex()));
    JITTER_TRY(write_key(stream, "generation", plan.generation.hex()));
    JITTER_TRY(write_key(stream, "ordinal", text::u64_to_string(plan.ordinal.value())));
    JITTER_TRY(write_key(stream, "observation_clock", plan.observation_clock.hex()));
    JITTER_TRY(write_key(stream, "hop_clock", plan.hop_clock.hex()));
    JITTER_TRY(write_key(stream, "second_hop_clock", plan.second_hop_clock.hex()));
    JITTER_TRY(write_key(stream, "incarnation", text::u64_to_string(plan.incarnation.value())));
    JITTER_TRY(write_key(stream, "epoch", text::u64_to_string(plan.epoch.value())));
    JITTER_TRY(write_key(stream, "sequence_start", text::u64_to_string(plan.sequence_start.value())));
    JITTER_TRY(write_key(stream, "authority", std::string(to_string(plan.authority))));
    JITTER_TRY(write_key(stream, "origin", std::string(to_string(plan.origin))));
    JITTER_TRY(write_key(stream, "ingest_path", plan.ingest_path));
    JITTER_TRY(write_key(stream, "hop_count", text::u64_to_string(plan.hop_ids.size())));
    for (std::size_t i = 0; i < plan.hop_ids.size(); ++i) {
        const std::string key = "hop" + text::u64_to_string(i);
        std::string value = plan.hop_ids[i].hex();
        value.push_back(' ');
        value.append(plan.hop_clocks[i].is_nil() ? std::string("-") : plan.hop_clocks[i].hex());
        JITTER_TRY(write_key(stream, key, value));
    }
    stream.flush();
    if (!stream) {
        return Status::failure(ErrorCode::IoFailure, "plan file could not be flushed", path);
    }
    return Status::success();
}

Result<ScenarioPlan> read_plan_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return Result<ScenarioPlan>::fail(ErrorCode::IoFailure, "plan file could not be opened", path);
    }
    ScenarioPlan plan;
    plan.hop_ids.clear();
    plan.hop_clocks.clear();
    std::string line;
    std::uint64_t declared_hops = 0;
    while (std::getline(stream, line)) {
        const std::string_view view = text::trim(line);
        if (view.empty() || view[0] == '#') {
            continue;
        }
        const std::string key = field(std::string(view), 0);
        const std::string value = field(std::string(view), 1);
        if (key == "seed") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid seed"); plan.seed = v.value(); }
        else if (key == "samples") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid samples"); plan.samples = v.value(); }
        else if (key == "batch_size") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid batch_size"); plan.batch_size = v.value(); }
        else if (key == "base_latency_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid base_latency_ns"); plan.base_latency_ns = v.value(); }
        else if (key == "jitter_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid jitter_ns"); plan.jitter_ns = v.value(); }
        else if (key == "spike_every") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid spike_every"); plan.spike_every = v.value(); }
        else if (key == "spike_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid spike_ns"); plan.spike_ns = v.value(); }
        else if (key == "start_utc_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid start_utc_ns"); plan.start_utc_ns = v.value(); }
        else if (key == "interval_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid interval_ns"); plan.interval_ns = v.value(); }
        else if (key == "receive_delay_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid receive_delay_ns"); plan.receive_delay_ns = v.value(); }
        else if (key == "hop_spread_ns") { auto v = text::parse_i64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid hop_spread_ns"); plan.hop_spread_ns = v.value(); }
        else if (key == "hop_timings") { plan.hop_timings = value == "1"; }
        else if (key == "incomparable_second_hop") { plan.incomparable_second_hop = value == "1"; }
        else if (key == "source") { auto v = SourceId::parse(value); if (!v.ok()) return v.error(); plan.source = v.value(); }
        else if (key == "series") { auto v = SeriesId::parse(value); if (!v.ok()) return v.error(); plan.series = v.value(); }
        else if (key == "path") { auto v = PathId::parse(value); if (!v.ok()) return v.error(); plan.path = v.value(); }
        else if (key == "generation") { auto v = GenerationId::parse(value); if (!v.ok()) return v.error(); plan.generation = v.value(); }
        else if (key == "ordinal") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid ordinal"); plan.ordinal = Ordinal(v.value()); }
        else if (key == "observation_clock") { auto v = ClockDomainId::parse(value); if (!v.ok()) return v.error(); plan.observation_clock = v.value(); }
        else if (key == "hop_clock") { auto v = ClockDomainId::parse(value); if (!v.ok()) return v.error(); plan.hop_clock = v.value(); }
        else if (key == "second_hop_clock") { auto v = ClockDomainId::parse(value); if (!v.ok()) return v.error(); plan.second_hop_clock = v.value(); }
        else if (key == "incarnation") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid incarnation"); plan.incarnation = SourceIncarnation(v.value()); }
        else if (key == "epoch") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid epoch"); plan.epoch = SourceEpoch(v.value()); }
        else if (key == "sequence_start") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid sequence_start"); plan.sequence_start = SourceSequence(v.value()); }
        else if (key == "authority") { SourceAuthority authority{}; if (!parse_source_authority(value, authority)) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid authority"); plan.authority = authority; }
        else if (key == "origin") { EvidenceOrigin origin{}; if (!parse_evidence_origin(value, origin)) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid origin"); plan.origin = origin; }
        else if (key == "ingest_path") { plan.ingest_path = value; }
        else if (key == "hop_count") { auto v = text::parse_u64(value); if (!v) return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument, "invalid hop_count"); declared_hops = v.value(); }
        else if (text::starts_with(key, "hop")) {
            const std::string id_text = field(std::string(view), 1);
            const std::string clock_text = field(std::string(view), 2);
            auto hop = HopId::parse(id_text);
            if (!hop.ok()) {
                return hop.error();
            }
            ClockDomainId clock;
            if (clock_text != "-") {
                auto parsed_clock = ClockDomainId::parse(clock_text);
                if (!parsed_clock.ok()) {
                    return parsed_clock.error();
                }
                clock = parsed_clock.value();
            }
            plan.hop_ids.push_back(hop.value());
            plan.hop_clocks.push_back(clock);
        } else {
            return Result<ScenarioPlan>::fail(ErrorCode::InvalidArgument,
                                              "plan file contains an unknown key", key);
        }
    }
    if (plan.hop_ids.size() != declared_hops) {
        return Result<ScenarioPlan>::fail(ErrorCode::Corrupt,
                                          "plan file hop count does not match the hop entries",
                                          text::u64_to_string(plan.hop_ids.size()) + "/" +
                                              text::u64_to_string(declared_hops));
    }
    return plan;
}

}  // namespace jitter
