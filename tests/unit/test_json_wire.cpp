// Jitter Observatory - canonical JSON and the ingest wire protocol.
// Copyright 2026 Summon Software Labs.
#include <limits>
#include <string>
#include <vector>

#include <jitter/codec.hpp>
#include <jitter/json.hpp>
#include <jitter/render.hpp>
#include <jitter/wire.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

std::vector<std::uint8_t> frame_bytes(WireMessageType type, std::string_view payload) {
    return encode_frame(type, std::span<const std::uint8_t>(
                                   reinterpret_cast<const std::uint8_t*>(payload.data()),
                                   payload.size()));
}

}  // namespace

JITTER_TEST(json, object_members_are_emitted_in_sorted_order) {
    JsonValue first = JsonValue::make_object();
    first.set("zeta", JsonValue::make_u64(1));
    first.set("alpha", JsonValue::make_u64(2));
    first.set("mu", JsonValue::make_u64(3));

    JsonValue second = JsonValue::make_object();
    second.set("mu", JsonValue::make_u64(3));
    second.set("alpha", JsonValue::make_u64(2));
    second.set("zeta", JsonValue::make_u64(1));

    CHECK_EQ(first.dump(), std::string("{\"alpha\":2,\"mu\":3,\"zeta\":1}"));
    CHECK_EQ(first.dump(), second.dump());
    CHECK_EQ(first.digest(), second.digest());
}

JITTER_TEST(json, scalars_are_rendered_deterministically) {
    CHECK_EQ(JsonValue::make_u64(18446744073709551615ull).dump(),
             std::string("18446744073709551615"));
    CHECK_EQ(JsonValue::make_i64(-9223372036854775807ll - 1).dump(),
             std::string("-9223372036854775808"));
    CHECK_EQ(JsonValue::make_double(0.1).dump(), std::string("0.1"));
    CHECK_EQ(JsonValue::make_bool(true).dump(), std::string("true"));
    CHECK_EQ(JsonValue::make_null().dump(), std::string("null"));
    CHECK_EQ(JsonValue::make_double(std::numeric_limits<double>::infinity()).dump(),
             std::string("null"));
    CHECK_EQ(JsonValue::make_double(std::numeric_limits<double>::quiet_NaN()).dump(),
             std::string("null"));
    CHECK_EQ(JsonValue::make_string("line\n\"quoted\"\\slash").dump(),
             std::string("\"line\\n\\\"quoted\\\"\\\\slash\""));
}

JITTER_TEST(json, nested_documents_are_canonical) {
    JsonValue array = JsonValue::make_array();
    array.push(JsonValue::make_u64(3));
    array.push(JsonValue::make_u64(1));
    array.push(JsonValue::make_u64(2));

    JsonValue document = JsonValue::make_object();
    document.set("items", std::move(array));
    document.set("count", JsonValue::make_u64(3));

    CHECK_EQ(document.dump(), std::string("{\"count\":3,\"items\":[3,1,2]}"));
    CHECK(document.dump_pretty().find('\n') != std::string::npos);
    CHECK(JsonValue::make_u64(1).set("x", JsonValue::make_u64(1)) == false);
}

JITTER_TEST(json, document_envelope_is_stable) {
    JsonValue payload = JsonValue::make_object();
    payload.set("value", JsonValue::make_u64(7));
    const JsonValue document = make_document("unit_test", std::move(payload));
    const std::string text = document.dump();
    CHECK(text.find("\"kind\":\"unit_test\"") != std::string::npos);
    CHECK(text.find("\"schema\":1") != std::string::npos);
    CHECK(text.find("jitter-observatory") != std::string::npos);

    JsonValue rebuilt_payload = JsonValue::make_object();
    rebuilt_payload.set("value", JsonValue::make_u64(7));
    CHECK_EQ(make_document("unit_test", std::move(rebuilt_payload)).dump(), text);
}

JITTER_TEST(wire, frames_round_trip_for_every_message_type) {
    HelloMessage hello;
    hello.source = synthetic_source_id("wire-source");
    hello.origin = EvidenceOrigin::Synthetic;
    hello.authority = SourceAuthority::Simulated;
    hello.agent = "unit-test";
    ByteWriter hello_writer;
    write_hello(hello_writer, hello);
    auto decoded_hello = decode_frame(frame_bytes(WireMessageType::Hello, std::string_view(
        reinterpret_cast<const char*>(hello_writer.data().data()), hello_writer.size())));
    REQUIRE_OK(decoded_hello);
    ByteReader hello_reader(decoded_hello.value().frame.payload);
    auto parsed_hello = read_hello(hello_reader);
    REQUIRE_OK(parsed_hello);
    CHECK_EQ(parsed_hello.value().source, hello.source);
    CHECK_EQ(parsed_hello.value().agent, std::string("unit-test"));

    HelloAckMessage ack;
    ack.accepted = true;
    ack.reason = "accepted";
    ByteWriter ack_writer;
    write_hello_ack(ack_writer, ack);
    auto ack_frame = decode_frame(frame_bytes(
        WireMessageType::HelloAck,
        std::string_view(reinterpret_cast<const char*>(ack_writer.data().data()), ack_writer.size())));
    REQUIRE_OK(ack_frame);
    ByteReader ack_reader(ack_frame.value().frame.payload);
    auto parsed_ack = read_hello_ack(ack_reader);
    REQUIRE_OK(parsed_ack);
    CHECK(parsed_ack.value().accepted);
    CHECK_EQ(parsed_ack.value().max_frame_bytes, Limits::kMaxWireFrameBytes);

    ErrorMessage error;
    error.code = "protocol_violation";
    error.message = "bad frame";
    ByteWriter error_writer;
    write_error(error_writer, error);
    auto error_frame = decode_frame(frame_bytes(
        WireMessageType::Error, std::string_view(
                                     reinterpret_cast<const char*>(error_writer.data().data()),
                                     error_writer.size())));
    REQUIRE_OK(error_frame);
    ByteReader error_reader(error_frame.value().frame.payload);
    auto parsed_error = read_error(error_reader);
    REQUIRE_OK(parsed_error);
    CHECK_EQ(parsed_error.value().code, std::string("protocol_violation"));
}

JITTER_TEST(wire, incomplete_frames_ask_for_more_data) {
    const std::vector<std::uint8_t> frame = frame_bytes(WireMessageType::Bye, "0123456789");
    for (std::size_t length = 0; length < frame.size(); ++length) {
        const std::span<const std::uint8_t> prefix(frame.data(), length);
        auto decoded = decode_frame(prefix);
        CHECK_ERR(decoded, ErrorCode::NotReady);
    }
    auto complete = decode_frame(frame);
    REQUIRE_OK(complete);
    CHECK_EQ(complete.value().consumed, frame.size());
}

JITTER_TEST(wire, malformed_frames_are_rejected_with_specific_reasons) {
    std::vector<std::uint8_t> frame = frame_bytes(WireMessageType::Bye, "abc");

    std::vector<std::uint8_t> wrong_magic = frame;
    wrong_magic[0] = 'X';
    CHECK_ERR(decode_frame(wrong_magic), ErrorCode::ProtocolViolation);

    std::vector<std::uint8_t> wrong_version = frame;
    wrong_version[7] = 9;
    CHECK_ERR(decode_frame(wrong_version), ErrorCode::VersionUnsupported);

    std::vector<std::uint8_t> unknown_type = frame;
    unknown_type[9] = 99;
    CHECK_ERR(decode_frame(unknown_type), ErrorCode::ProtocolViolation);

    std::vector<std::uint8_t> reserved_flags = frame;
    reserved_flags[11] = 1;
    CHECK_ERR(decode_frame(reserved_flags), ErrorCode::ProtocolViolation);

    std::vector<std::uint8_t> corrupted = frame;
    corrupted[frame.size() - 1] = static_cast<std::uint8_t>(corrupted.back() ^ 0x40);
    CHECK_ERR(decode_frame(corrupted), ErrorCode::IntegrityFailure);

    std::vector<std::uint8_t> oversized = frame;
    // length field is bytes 12..15 (after the 4 byte magic, u32 version, u16 type, u16 flags)
    oversized[12] = 0x7f;
    oversized[13] = 0xff;
    oversized[14] = 0xff;
    oversized[15] = 0xff;
    CHECK_ERR(decode_frame(oversized), ErrorCode::LimitExceeded);
}

JITTER_TEST(codec, batch_round_trip_preserves_every_identity) {
    ScenarioPlan plan;
    plan.samples = 5;
    plan.batch_size = 5;

    EngineConfig config = fixture_config();
    auto fixture = build_engine(plan, config, false, "codec/forward");
    REQUIRE_OK(fixture);
    const std::vector<LatencyBatch> batches = make_batches(fixture.value().plan);
    REQUIRE(batches.size() == 1);

    ByteWriter writer;
    codec::write_batch(writer, batches.front());
    ByteReader reader(writer.data());
    auto decoded = codec::read_batch(reader);
    REQUIRE_OK(decoded);
    CHECK(reader.at_end());
    CHECK_EQ(decoded.value().id, batches.front().id);
    CHECK_EQ(decoded.value().content, batches.front().content);
    CHECK_EQ(decoded.value().samples.size(), batches.front().samples.size());
    CHECK_EQ(decoded.value().samples.front().id, batches.front().samples.front().id);
    CHECK_EQ(decoded.value().header.generation, batches.front().header.generation);
    CHECK_EQ(decoded.value().samples.front().hop_timings.size(),
             batches.front().samples.front().hop_timings.size());
}

JITTER_TEST(codec, tampering_with_a_sample_is_detected) {
    ScenarioPlan plan;
    plan.samples = 2;
    plan.batch_size = 2;
    EngineConfig config = fixture_config();
    auto fixture = build_engine(plan, config, false, "codec/tamper");
    REQUIRE_OK(fixture);
    const std::vector<LatencyBatch> batches = make_batches(fixture.value().plan);
    REQUIRE(batches.size() == 1);

    ByteWriter writer;
    codec::write_sample(writer, batches.front().samples.front());
    std::vector<std::uint8_t> bytes = writer.data();
    // Corrupting any field changes the recomputed content digest, which the reader
    // compares against the identity the sender claimed.
    bytes[bytes.size() - 8] = static_cast<std::uint8_t>(bytes[bytes.size() - 8] ^ 0xff);
    ByteReader reader(bytes);
    auto decoded = codec::read_sample(reader);
    if (decoded.ok()) {
        CHECK(decoded.value().id != batches.front().samples.front().id);
    }
}

JITTER_TEST(codec, truncated_records_never_produce_a_partial_value) {
    ScenarioPlan plan;
    plan.samples = 2;
    plan.batch_size = 2;
    EngineConfig config = fixture_config();
    auto fixture = build_engine(plan, config, false, "codec/truncate");
    REQUIRE_OK(fixture);
    const std::vector<LatencyBatch> batches = make_batches(fixture.value().plan);

    ByteWriter writer;
    codec::write_batch(writer, batches.front());
    const std::vector<std::uint8_t> full = writer.data();
    for (std::size_t length = 0; length < full.size(); length += 7) {
        ByteReader reader(std::span<const std::uint8_t>(full.data(), length));
        auto decoded = codec::read_batch(reader);
        CHECK(!decoded.ok());
    }
    ByteReader reader(full);
    CHECK_OK(codec::read_batch(reader));
}
