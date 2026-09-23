// Jitter Observatory - adversarial inputs, malformed encodings and replay attempts.
// Copyright 2026 Summon Software Labs.
//
// Every test in this file tries to make the runtime accept something it should not, or
// to make it fail in a way that is not an explicit, typed error.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <jitter/codec.hpp>
#include <jitter/engine.hpp>
#include <jitter/persistence.hpp>
#include <jitter/wire.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

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
    std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

private:
    std::uint64_t state_;
};

std::string scratch_path(const std::string& name) {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "jitter-observatory-tests" / "adversarial";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return (base / name).string();
}

}  // namespace

JITTER_TEST(adversarial, random_bytes_never_produce_an_untyped_failure_in_frame_decoding) {
    SplitMix64 random(0xC0FFEE);
    std::size_t rejected = 0;
    std::size_t accepted = 0;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        const std::size_t length = random.below(120);
        std::vector<std::uint8_t> buffer(length);
        for (std::size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<std::uint8_t>(random.below(256));
        }
        auto decoded = decode_frame(buffer);
        if (decoded.ok()) {
            ++accepted;
            CHECK(decoded.value().consumed <= buffer.size());
            CHECK(is_known_message_type(static_cast<std::uint16_t>(decoded.value().frame.type)));
        } else {
            ++rejected;
            CHECK(decoded.code() != ErrorCode::Ok);
        }
    }
    // Random data essentially never forms a valid frame, and every rejection was typed.
    CHECK(rejected > 3900);
    CHECK_EQ(rejected + accepted, std::size_t{4000});
}

JITTER_TEST(adversarial, random_frames_with_a_valid_header_are_still_validated) {
    SplitMix64 random(0xBADF00D);
    for (int iteration = 0; iteration < 500; ++iteration) {
        const std::size_t length = random.below(64);
        std::vector<std::uint8_t> payload(length);
        for (std::size_t i = 0; i < length; ++i) {
            payload[i] = static_cast<std::uint8_t>(random.below(256));
        }
        std::vector<std::uint8_t> frame = encode_frame(WireMessageType::Batch, payload);
        // Corrupt one byte of the payload: the checksum must catch it.
        if (!payload.empty()) {
            const std::size_t index = kWireHeaderBytes + random.below(length);
            frame[index] = static_cast<std::uint8_t>(frame[index] ^ 0x5a);
        }
        auto decoded = decode_frame(frame);
        if (!payload.empty()) {
            CHECK_ERR(decoded, ErrorCode::IntegrityFailure);
        }
    }
}

JITTER_TEST(adversarial, random_bytes_in_a_store_file_are_never_a_crash) {
    SplitMix64 random(0x5EED);
    const std::string directory = scratch_directory("adversarial-store");
    for (int iteration = 0; iteration < 120; ++iteration) {
        const std::string path = directory + "/fuzz-" + std::to_string(iteration) + ".jostore";
        const std::size_t length = random.below(400);
        std::vector<std::uint8_t> bytes(length);
        for (std::size_t i = 0; i < length; ++i) {
            bytes[i] = static_cast<std::uint8_t>(random.below(256));
        }
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        auto loaded = load_store(path);
        if (loaded.ok()) {
            CHECK(loaded.value().recovery.records_recovered <= Limits::kMaxStoreRecords);
        } else {
            CHECK(loaded.code() != ErrorCode::Ok);
        }
    }
}

JITTER_TEST(adversarial, a_valid_store_with_mutated_bytes_is_detected) {
    const std::string path = scratch_path("mutated.jostore");
    ScenarioPlan plan;
    plan.samples = 24;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), true, "adversarial/mutate");
    REQUIRE_OK(fixture);
    REQUIRE_OK(fixture.value().observatory->save(path, true, fixture.value().context));

    std::vector<std::uint8_t> original;
    {
        std::ifstream stream(path, std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    REQUIRE(original.size() > 100);

    std::size_t detected = 0;
    for (std::size_t offset = 64; offset < original.size(); offset += 13) {
        std::vector<std::uint8_t> mutated = original;
        mutated[offset] = static_cast<std::uint8_t>(mutated[offset] ^ 0x01);
        const std::string mutated_path = scratch_path("mutated-case.jostore");
        {
            std::ofstream stream(mutated_path, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(mutated.data()),
                         static_cast<std::streamsize>(mutated.size()));
        }
        auto loaded = load_store(mutated_path);
        if (!loaded.ok() || !loaded.value().recovery.clean) {
            ++detected;
        }
    }
    CHECK(detected > 0);
}

JITTER_TEST(adversarial, samples_cannot_drift_from_their_content_identity) {
    ScenarioPlan plan;
    plan.samples = 4;
    plan.batch_size = 4;
    auto fixture = build_engine(plan, fixture_config(), false, "adversarial/identity");
    REQUIRE_OK(fixture);

    LatencySample sample = fixture.value().batches.front().samples.front();
    const MeasurementId original = sample.id;
    sample.latency_ns += 1;
    // The identity no longer describes the content, so rebuilding is refused.
    CHECK_ERR(make_sample(sample), ErrorCode::InvalidArgument);
    sample.id = MeasurementId{};
    auto rebuilt = make_sample(sample);
    REQUIRE_OK(rebuilt);
    CHECK(rebuilt.value().id != original);
}

JITTER_TEST(adversarial, observation_bounds_are_enforced) {
    SampleSpec spec;
    spec.observation_clock = synthetic_clock_id("adversarial-clock");
    spec.source = synthetic_source_id("adversarial-source");
    spec.series = synthetic_series_id("adversarial-series");
    spec.path = synthetic_path_id("adversarial-path");
    spec.generation = synthetic_generation_id("adversarial-generation");

    SampleSpec too_large = spec;
    too_large.latency_ns = Limits::kMaxLatencyNs + 1;
    CHECK_ERR(build_sample(too_large), ErrorCode::OutOfRange);

    SampleSpec too_small = spec;
    too_small.latency_ns = Limits::kMinLatencyNs - 1;
    CHECK_ERR(build_sample(too_small), ErrorCode::OutOfRange);

    SampleSpec wrong_receive_clock = spec;
    wrong_receive_clock.received_ticks = 10;
    auto built = build_sample(wrong_receive_clock);
    REQUIRE_OK(built);

    LatencySample no_origin = built.value();
    no_origin.provenance.origin = EvidenceOrigin::Unknown;
    no_origin.id = MeasurementId{};
    CHECK_ERR(make_sample(no_origin), ErrorCode::InvalidArgument);

    LatencySample no_sequence = built.value();
    no_sequence.provenance.sequence = SourceSequence{};
    no_sequence.id = MeasurementId{};
    CHECK_ERR(make_sample(no_sequence), ErrorCode::InvalidArgument);

    LatencySample too_many_entries = built.value();
    for (std::uint64_t i = 0; i <= Limits::kMaxMetadataEntries; ++i) {
        too_many_entries.metadata["key" + std::to_string(i)] = "value";
    }
    too_many_entries.id = MeasurementId{};
    CHECK_ERR(make_sample(too_many_entries), ErrorCode::LimitExceeded);

    LatencySample huge_value = built.value();
    huge_value.metadata["key"] = std::string(Limits::kMaxMetadataValueBytes + 1, 'x');
    huge_value.id = MeasurementId{};
    CHECK_ERR(make_sample(huge_value), ErrorCode::LimitExceeded);

    LatencySample huge_key = built.value();
    huge_key.metadata[std::string(Limits::kMaxMetadataKeyBytes + 1, 'k')] = "v";
    huge_key.id = MeasurementId{};
    CHECK_ERR(make_sample(huge_key), ErrorCode::LimitExceeded);

    std::map<std::string, std::string> many;
    for (std::uint64_t i = 0; i < Limits::kMaxMetadataEntries; ++i) {
        many["k" + std::to_string(i)] = std::string(Limits::kMaxMetadataValueBytes, 'v');
    }
    LatencySample huge_total = built.value();
    huge_total.metadata = many;
    huge_total.id = MeasurementId{};
    CHECK_ERR(make_sample(huge_total), ErrorCode::LimitExceeded);
}

JITTER_TEST(adversarial, batches_cannot_mix_identity_or_clock) {
    ScenarioPlan plan;
    plan.samples = 8;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false, "adversarial/mix");
    REQUIRE_OK(fixture);
    const LatencyBatch& original = fixture.value().batches.front();

    LatencyBatch mixed_series = original;
    mixed_series.samples[1].series = synthetic_series_id("other-series");
    mixed_series.samples[1].id = MeasurementId{};
    auto rebuilt = make_sample(mixed_series.samples[1]);
    REQUIRE_OK(rebuilt);
    mixed_series.samples[1] = rebuilt.value();
    CHECK_ERR(make_batch(mixed_series), ErrorCode::InvalidArgument);

    LatencyBatch mixed_clock = original;
    mixed_clock.samples[2].observed_at.domain = synthetic_clock_id("other-clock");
    mixed_clock.samples[2].id = MeasurementId{};
    auto rebuilt_clock = make_sample(mixed_clock.samples[2]);
    REQUIRE_OK(rebuilt_clock);
    mixed_clock.samples[2] = rebuilt_clock.value();
    CHECK_ERR(make_batch(mixed_clock), ErrorCode::InvalidArgument);

    LatencyBatch empty = original;
    empty.samples.clear();
    CHECK_ERR(make_batch(empty), ErrorCode::InvalidArgument);

    LatencyBatch no_generation = original;
    no_generation.header.generation = GenerationId{};
    CHECK_ERR(make_batch(no_generation), ErrorCode::InvalidArgument);
}

JITTER_TEST(adversarial, the_engine_refuses_spoofed_authority_origin_and_time) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false, "adversarial/spoof");
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    // Evidence from the future cannot establish the present.
    // A clock reading five seconds ahead of the runtime's own clock is outside the
    // declared future tolerance, so the evidence is not admissible as current.
    IngestContext before = fixture.value().context;
    before.now_utc_ns = fixture.value().plan.start_utc_ns - 5000000000ll;
    REQUIRE_OK(observatory.ingest(fixture.value().batches.front(), before));
    auto outcome = observatory.summarize(fixture.value().summary_request(), before, false);
    REQUIRE_OK(outcome);
    CHECK_EQ(outcome.value().summary.evidence.fresh, std::uint64_t{0});
    CHECK(!asserts_instability(outcome.value().summary.classification.level));
    CHECK(!asserts_stability(outcome.value().summary.classification.level));

    // A batch that claims a different authority or origin is refused outright.
    LatencyBatch spoofed_authority = fixture.value().batches[1];
    spoofed_authority.header.authority = SourceAuthority::Authoritative;
    CHECK_ERR(observatory.ingest(spoofed_authority, fixture.value().context),
              ErrorCode::PolicyViolation);

    LatencyBatch spoofed_origin = fixture.value().batches[1];
    spoofed_origin.header.origin = EvidenceOrigin::Real;
    CHECK_ERR(observatory.ingest(spoofed_origin, fixture.value().context), ErrorCode::PolicyViolation);

    LatencyBatch mixed_provenance = fixture.value().batches[1];
    mixed_provenance.samples[0].provenance.authority = SourceAuthority::Authoritative;
    mixed_provenance.samples[0].id = MeasurementId{};
    auto rebuilt = make_sample(mixed_provenance.samples[0]);
    REQUIRE_OK(rebuilt);
    mixed_provenance.samples[0] = rebuilt.value();
    auto rebuilt_batch = make_batch(mixed_provenance);
    REQUIRE_OK(rebuilt_batch);
    CHECK_ERR(observatory.ingest(rebuilt_batch.value(), fixture.value().context),
              ErrorCode::InvalidArgument);
}

JITTER_TEST(adversarial, replays_conflicts_and_epoch_rewinds_are_all_fenced) {
    ScenarioPlan plan;
    plan.samples = 24;
    plan.batch_size = 8;
    auto fixture = build_engine(plan, fixture_config(), false, "adversarial/replay");
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;

    REQUIRE_OK(observatory.ingest(fixture.value().batches[0], fixture.value().context));
    REQUIRE_OK(observatory.ingest(fixture.value().batches[1], fixture.value().context));

    // Byte identical replay of the newest accepted position.
    auto duplicate = observatory.ingest(fixture.value().batches[1], fixture.value().context);
    REQUIRE_OK(duplicate);
    CHECK_EQ(duplicate.value().verdict, SequenceVerdict::DuplicateIdempotent);

    // A replay of an older position is a rewind, which is fenced as stale.
    auto rewind = observatory.ingest(fixture.value().batches[0], fixture.value().context);
    REQUIRE_OK(rewind);
    CHECK_EQ(rewind.value().verdict, SequenceVerdict::StaleSequence);

    // Same sequence, different latency: a contradiction, recorded and refused.
    LatencyBatch contradictory = fixture.value().batches[1];
    contradictory.samples[0].latency_ns += 1;
    contradictory.samples[0].id = MeasurementId{};
    auto rebuilt_sample = make_sample(contradictory.samples[0]);
    REQUIRE_OK(rebuilt_sample);
    contradictory.samples[0] = rebuilt_sample.value();
    auto rebuilt_batch = make_batch(contradictory);
    REQUIRE_OK(rebuilt_batch);
    auto conflict = observatory.ingest(rebuilt_batch.value(), fixture.value().context);
    REQUIRE_OK(conflict);
    CHECK_EQ(conflict.value().verdict, SequenceVerdict::SequenceConflict);
    CHECK_EQ(conflict.value().accepted, std::uint64_t{0});
    CHECK_EQ(observatory.conflict_log().size(), std::size_t{1});

    // An epoch rewind inside the current incarnation.
    ScenarioPlan rewound = fixture.value().plan;
    rewound.epoch = SourceEpoch(0);
    rewound.sequence_start = SourceSequence(900);
    const std::vector<LatencyBatch> rewound_batches = make_batches(rewound);
    auto rejected = observatory.ingest(rewound_batches.front(), fixture.value().context);
    REQUIRE_OK(rejected);
    CHECK_EQ(rejected.value().verdict, SequenceVerdict::StaleEpoch);

    // A replayed batch from an older incarnation.
    ScenarioPlan older = fixture.value().plan;
    older.incarnation = SourceIncarnation(1);
    older.epoch = SourceEpoch(0);
    const std::vector<LatencyBatch> older_batches = make_batches(older);
    auto stale = observatory.ingest(older_batches.front(), fixture.value().context);
    REQUIRE_OK(stale);
    CHECK_EQ(stale.value().verdict, SequenceVerdict::StaleEpoch);
    CHECK_EQ(observatory.counters().samples_stored, std::uint64_t{16});
}

JITTER_TEST(adversarial, oversized_decoded_records_are_refused_before_allocation) {
    // A batch that claims more samples than the limit is refused by the decoder.
    ByteWriter writer;
    writer.u32(0xFFFFFFFFu);
    ByteReader reader(writer.data());
    CHECK_ERR(codec::read_batch(reader), ErrorCode::ProtocolViolation);

    ByteWriter header_writer;
    codec::write_batch_header(header_writer, BatchHeader{});
    std::vector<std::uint8_t> bytes = header_writer.data();
    bytes.push_back(0xff);
    bytes.push_back(0xff);
    bytes.push_back(0xff);
    bytes.push_back(0xff);
    ByteReader header_reader(bytes);
    CHECK_ERR(codec::read_batch(header_reader), ErrorCode::LimitExceeded);
}


JITTER_TEST(adversarial, extreme_timestamps_never_overflow_the_window_or_the_age_model) {
    WindowPolicy policy;
    policy.kind = WindowKind::Time;
    policy.duration_ns = 1000;
    policy.capacity = 8;

    const SeriesId series = synthetic_series_id("extreme-series");
    const PathId path = synthetic_path_id("extreme-path");
    const GenerationId generation = synthetic_generation_id("extreme-generation");

    // Readings at the very ends of the signed range are accepted, and selection must not
    // perform arithmetic that wraps around.
    SampleWindow window(policy);
    const std::int64_t ticks[] = {std::numeric_limits<std::int64_t>::min(),
                                  std::numeric_limits<std::int64_t>::min() + 10,
                                  std::numeric_limits<std::int64_t>::max(),
                                  std::numeric_limits<std::int64_t>::max() - 10};
    for (std::uint64_t i = 0; i < 4; ++i) {
        SampleSpec spec;
        spec.observation_clock = synthetic_clock_id("extreme-clock");
        spec.source = synthetic_source_id("extreme-source");
        spec.series = series;
        spec.path = path;
        spec.generation = generation;
        spec.sequence = SourceSequence(i + 1);
        spec.observed_ticks = ticks[i];
        spec.received_ticks = ticks[i] > 0 ? ticks[i] - 100 : ticks[i];
        auto sample = build_sample(spec);
        REQUIRE_OK(sample);
        REQUIRE_OK(window.add(sample.value()));
    }
    const WindowSelection selection = window.select();
    CHECK(selection.samples.size() <= 4);
    CHECK(selection.samples.size() >= 2);
    CHECK_EQ(window.newest_observation_ticks().value(), std::numeric_limits<std::int64_t>::max());

    // A tumbling window with the same extreme readings must not wrap either.
    WindowPolicy tumbling;
    tumbling.kind = WindowKind::Tumbling;
    tumbling.duration_ns = 100;
    tumbling.anchor_ns = std::numeric_limits<std::int64_t>::min();
    tumbling.capacity = 8;
    SampleWindow tumbling_window(tumbling);
    for (std::uint64_t i = 0; i < 4; ++i) {
        SampleSpec spec;
        spec.observation_clock = synthetic_clock_id("extreme-clock");
        spec.source = synthetic_source_id("extreme-source");
        spec.series = series;
        spec.path = path;
        spec.generation = generation;
        spec.sequence = SourceSequence(i + 1);
        spec.observed_ticks = ticks[i];
        spec.received_ticks = ticks[i] > 0 ? ticks[i] - 100 : ticks[i];
        auto sample = build_sample(spec);
        REQUIRE_OK(sample);
        REQUIRE_OK(tumbling_window.add(sample.value()));
    }
    static_cast<void>(tumbling_window.select());

    // The age model reports an unmeasurable age instead of wrapping when the runtime's
    // own clock and a reading are at opposite ends of the range.
    ClockModel clocks;
    ClockDomainDescriptor local = LocalClockDomain::descriptor();
    REQUIRE_OK(clocks.register_domain(local));
    ClockDomainDescriptor observation = local;
    observation.name = "extreme-observation-clock";
    observation.id = ClockDomainId{};
    REQUIRE_OK(clocks.register_domain(observation));
    ClockEquivalence equivalence;
    equivalence.a = observation.id;
    equivalence.b = local.id;
    equivalence.max_offset_ns = 0;
    equivalence.declared_by_authority = SourceAuthority::Authoritative;
    equivalence.justification = "unit test declaration";
    REQUIRE_OK(clocks.declare_equivalence(equivalence));

    SampleSpec spec;
    spec.observation_clock = observation.id;
    spec.source = synthetic_source_id("extreme-source");
    spec.series = series;
    spec.path = path;
    spec.generation = generation;
    spec.observed_ticks = std::numeric_limits<std::int64_t>::min();
    spec.received_ticks = std::numeric_limits<std::int64_t>::min();
    auto sample = build_sample(spec);
    REQUIRE_OK(sample);

    FreshnessPolicy freshness;
    const SampleAssessment assessment =
        assess_sample(sample.value(), clocks, std::numeric_limits<std::int64_t>::max(), freshness);
    CHECK(!is_positive_evidence(assessment.state));
    CHECK(assessment.state == EvidenceState::Unknown || assessment.state == EvidenceState::Stale);
    CHECK(assessment.reason == "ingest_age_overflowed_the_supported_range" ||
          assessment.reason == "observation_age_exceeds_expiry");
}

JITTER_TEST(adversarial, engine_bounds_reject_unbounded_growth) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;
    EngineConfig config = fixture_config();
    config.max_windows = 1;
    auto fixture = build_engine(plan, config, false, "adversarial/bounds");
    REQUIRE_OK(fixture);
    Observatory& observatory = *fixture.value().observatory;
    REQUIRE_OK(observatory.ingest(fixture.value().batches.front(), fixture.value().context));

    // A second window would exceed the bound, and the batch is refused rather than
    // silently dropped.
    const PathDescriptor* path = observatory.paths().find(fixture.value().plan.path);
    REQUIRE(path != nullptr);
    PathDescriptor changed = *path;
    changed.hops.pop_back();
    REQUIRE_OK(observatory.observe_topology(fixture.value().plan.path, make_topology(changed),
                                            fixture.value().context.now_utc_ns, "forced_change"));
    const PathGeneration* current = observatory.paths().current(fixture.value().plan.path);
    REQUIRE(current != nullptr);
    ScenarioPlan rerouted = fixture.value().plan;
    rerouted.generation = current->id;
    rerouted.ordinal = current->ordinal;
    rerouted.sequence_start = SourceSequence(200);
    const std::vector<LatencyBatch> batches = make_batches(rerouted);
    CHECK_ERR(observatory.ingest(batches.front(), fixture.value().context), ErrorCode::CapacityExceeded);
}
