// Jitter Observatory - core primitives: hashing, identities, encodings, arithmetic.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/metrics.hpp>
#include <jitter/text.hpp>

#include "support/harness.hpp"

using namespace jitter;

namespace {

std::string sha256_hex(std::string_view input) {
    return Digest(Sha256::hash(input)).hex();
}

}  // namespace

JITTER_TEST(core, sha256_known_vectors) {
    CHECK_EQ(sha256_hex(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256_hex("The quick brown fox jumps over the lazy dog"),
             std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
}

JITTER_TEST(core, sha256_incremental_matches_one_shot) {
    const std::string payload(1000, 'a');
    const Digest one_shot = Digest::of(payload);
    Sha256 incremental;
    for (std::size_t i = 0; i < payload.size(); i += 7) {
        const std::size_t length = std::min<std::size_t>(7, payload.size() - i);
        incremental.update(std::string_view(payload).substr(i, length));
    }
    CHECK_EQ(Digest(incremental.finish()), one_shot);

    // The classic million 'a' vector, fed in blocks.
    Sha256 million;
    const std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        million.update(block);
    }
    CHECK_EQ(Digest(million.finish()).hex(),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

JITTER_TEST(core, crc32c_known_vector) {
    CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
    CHECK_EQ(crc32c(std::string_view("")), 0u);
    const std::string_view digits = "123456789";
    CHECK_EQ(crc32c_extend(0u, std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(digits.data()),
                                  digits.size())),
             0xE3069283u);
}

JITTER_TEST(core, hash_detects_single_bit_flip) {
    std::string payload = "the quick brown fox";
    const Digest before = Digest::of(payload);
    payload[3] = static_cast<char>(payload[3] ^ 0x01);
    CHECK(before != Digest::of(payload));
}

JITTER_TEST(core, strong_id_format_parse_and_nil) {
    const SourceId nil;
    CHECK(nil.is_nil());
    CHECK(!nil.bytes().empty());

    DigestBuilder builder("jitter.source.v1");
    builder.str("alpha");
    const SourceId id = builder.as_id<SourceTag>();
    CHECK(!id.is_nil());
    CHECK_EQ(id.hex().size(), std::size_t{32});
    CHECK_EQ(id.label(), std::string("source:") + id.hex());

    auto parsed = SourceId::parse(id.hex());
    REQUIRE_OK(parsed);
    CHECK_EQ(parsed.value(), id);

    auto labelled = SourceId::parse(id.label());
    REQUIRE_OK(labelled);
    CHECK_EQ(labelled.value(), id);

    CHECK_ERR(SourceId::parse("source:zz"), ErrorCode::InvalidArgument);
    CHECK_ERR(SourceId::parse("series:" + id.hex()), ErrorCode::InvalidArgument);
    CHECK_ERR(SourceId::parse("00"), ErrorCode::InvalidArgument);
}

JITTER_TEST(core, digest_builder_never_blurs_fields) {
    // ("ab", "c") and ("a", "bc") must never hash to the same identity.
    DigestBuilder left("jitter.test.v1");
    left.str("ab");
    left.str("c");
    DigestBuilder right("jitter.test.v1");
    right.str("a");
    right.str("bc");
    CHECK(left.digest() != right.digest());

    // Domain separation must matter.
    DigestBuilder other_domain("jitter.test.v2");
    other_domain.str("ab");
    other_domain.str("c");
    CHECK(other_domain.digest() != left.digest());

    // Optionals must be distinguishable from zero values.
    DigestBuilder present("jitter.test.v1");
    present.optional_i64(0);
    DigestBuilder absent("jitter.test.v1");
    absent.optional_i64(std::nullopt);
    CHECK(present.digest() != absent.digest());
}

JITTER_TEST(core, checked_arithmetic_detects_overflow) {
    std::uint64_t unsigned_result = 0;
    CHECK(checked_add_u64(1, 2, unsigned_result));
    CHECK_EQ(unsigned_result, std::uint64_t{3});
    CHECK(!checked_add_u64(kU64Max, 1, unsigned_result));
    CHECK(checked_mul_u64(3, 4, unsigned_result));
    CHECK_EQ(unsigned_result, std::uint64_t{12});
    CHECK(!checked_mul_u64(kU64Max, 2, unsigned_result));

    std::int64_t signed_result = 0;
    CHECK(checked_add_i64(kI64Max, 1, signed_result) == false);
    CHECK(checked_sub_i64(kI64Min, 1, signed_result) == false);
    CHECK(checked_add_i64(-5, 3, signed_result));
    CHECK_EQ(signed_result, std::int64_t{-2});
    CHECK(checked_mul_i64(kI64Max, 1, signed_result));
    CHECK(!checked_mul_i64(kI64Max, 2, signed_result));

    CHECK_ERR(add_u64(kU64Max, 5, "test"), ErrorCode::Overflow);
    CHECK_ERR(mul_u64(kU64Max, 5, "test"), ErrorCode::Overflow);
    CHECK_ERR(add_i64(kI64Max, 5, "test"), ErrorCode::Overflow);
    CHECK_ERR(sub_i64(kI64Min, 5, "test"), ErrorCode::Overflow);
    CHECK_ERR(require_within(11, 10, "bound"), ErrorCode::LimitExceeded);
    CHECK_OK(require_within(10, 10, "bound"));
}

JITTER_TEST(core, canonical_encoding_round_trip) {
    ByteWriter writer;
    writer.u8(0x7f);
    writer.boolean(true);
    writer.u16(0x1234);
    writer.u32(0xdeadbeef);
    writer.u64(0x0123456789abcdefull);
    writer.i64(-42);
    writer.f64_bytes(-0.5);
    writer.str("hello");

    ByteReader reader(writer.data());
    CHECK_EQ(reader.u8().value(), std::uint8_t{0x7f});
    CHECK_EQ(reader.boolean().value(), true);
    CHECK_EQ(reader.u16().value(), std::uint16_t{0x1234});
    CHECK_EQ(reader.u32().value(), std::uint32_t{0xdeadbeef});
    CHECK_EQ(reader.u64().value(), std::uint64_t{0x0123456789abcdefull});
    CHECK_EQ(reader.i64().value(), std::int64_t{-42});
    CHECK_EQ(reader.f64_bytes().value(), -0.5);
    CHECK_EQ(reader.str().value(), std::string("hello"));
    CHECK(reader.at_end());
}

JITTER_TEST(core, canonical_encoding_is_big_endian_and_bounded) {
    ByteWriter writer;
    writer.u32(0x01020304);
    CHECK_EQ(writer.data().size(), std::size_t{4});
    CHECK_EQ(writer.data()[0], std::uint8_t{0x01});
    CHECK_EQ(writer.data()[3], std::uint8_t{0x04});

    ByteReader reader(writer.data());
    CHECK_EQ(reader.remaining(), std::uint64_t{4});
    CHECK_ERR(reader.u64(), ErrorCode::ProtocolViolation);
    CHECK_EQ(reader.remaining(), std::uint64_t{4});

    ByteWriter text;
    text.str("abcd");
    ByteReader text_reader(text.data());
    CHECK_ERR(text_reader.raw(64), ErrorCode::ProtocolViolation);
    CHECK_OK(text_reader.raw(4));

    ByteWriter flag;
    flag.u8(7);
    ByteReader flag_reader(flag.data());
    CHECK_ERR(flag_reader.boolean(), ErrorCode::ProtocolViolation);
}

JITTER_TEST(core, text_helpers_are_locale_independent) {
    CHECK_EQ(text::u64_to_string(0), std::string("0"));
    CHECK_EQ(text::u64_to_string(18446744073709551615ull), std::string("18446744073709551615"));
    CHECK_EQ(text::i64_to_string(-9223372036854775807ll - 1), std::string("-9223372036854775808"));
    CHECK_EQ(text::double_to_string(0.5), std::string("0.5"));
    CHECK_EQ(text::double_to_string(-0.0), std::string("-0"));
    CHECK_EQ(text::double_to_string(std::numeric_limits<double>::quiet_NaN()), std::string("nan"));
    CHECK_EQ(text::escape_json("a\"b\\c\n"), std::string("a\\\"b\\\\c\\n"));

    CHECK_EQ(text::parse_u64("42").value(), std::uint64_t{42});
    CHECK(!text::parse_u64("-1").has_value());
    CHECK(!text::parse_u64("4x").has_value());
    CHECK_EQ(text::parse_i64("-42").value(), std::int64_t{-42});
    CHECK_EQ(text::trim("  x \t").size(), std::size_t{1});
    CHECK(text::equals_ascii_ci("AbC", "aBc"));
    CHECK(text::starts_with("jitter.absolute", "jitter."));
    CHECK(text::ends_with("jitter.absolute", "absolute"));
}

JITTER_TEST(core, metric_table_is_consistent_and_identities_are_distinct) {
    CHECK_OK(verify_metric_table());

    const MetricRegistry& registry = metric_registry();
    CHECK(registry.size() >= std::size_t{15});

    // Two metrics with different definitions never share an identity, even when they
    // describe the same family of quantity.
    const MetricDefinition& absolute_delta = registry.definition(MetricKey::AbsoluteDeltaMean);
    const MetricDefinition& ipdv = registry.definition(MetricKey::IpdvMean);
    CHECK(absolute_delta.id != ipdv.id);
    CHECK_EQ(absolute_delta.unit, MetricUnit::Nanoseconds);
    CHECK_EQ(ipdv.unit, MetricUnit::Nanoseconds);

    const MetricDefinition& variance = registry.definition(MetricKey::SampleVariance);
    const MetricDefinition& stddev = registry.definition(MetricKey::SampleStdDev);
    CHECK_EQ(variance.unit, MetricUnit::NanosecondsSquared);
    CHECK_EQ(stddev.unit, MetricUnit::Nanoseconds);
    CHECK(variance.id != stddev.id);

    // A different definition version yields a different identity.
    MetricDefinition altered = absolute_delta;
    altered.version = absolute_delta.version + 1;
    CHECK(make_metric_id(altered) != absolute_delta.id);
    altered = absolute_delta;
    altered.formula = "some other formula";
    CHECK(make_metric_id(altered) != absolute_delta.id);

    for (const MetricDefinition& definition : registry.definitions()) {
        CHECK(!definition.name.empty());
        CHECK(!definition.formula.empty());
        CHECK(definition.min_samples >= 1);
        CHECK_EQ(definition.id, make_metric_id(definition));
    }
}

JITTER_TEST(core, metric_values_of_different_metrics_are_not_comparable) {
    const std::vector<std::int64_t> samples{100, 120, 90, 130, 110};
    auto delta = metric_registry().compute(MetricKey::AbsoluteDeltaMean, samples);
    auto variance = metric_registry().compute(MetricKey::SampleVariance, samples);
    REQUIRE_OK(delta);
    REQUIRE_OK(variance);

    CHECK_ERR(check_comparable(delta.value(), variance.value()), ErrorCode::MetricMismatch);
    CHECK_OK(check_comparable(delta.value(), delta.value()));

    MetricValue forged = delta.value();
    forged.version = delta.value().version + 1;
    CHECK_ERR(check_comparable(delta.value(), forged), ErrorCode::VersionUnsupported);

    MetricValue wrong_unit = delta.value();
    wrong_unit.unit = MetricUnit::NanosecondsSquared;
    CHECK_ERR(check_comparable(delta.value(), wrong_unit), ErrorCode::UnitMismatch);
}

JITTER_TEST(core, limits_are_finite_and_ordered) {
    CHECK(Limits::kMaxSamplesPerBatch > 0);
    CHECK(Limits::kMaxWindowSamples >= Limits::kMaxSamplesPerBatch);
    CHECK(Limits::kMaxStoreBytes > Limits::kMaxStoreRecords || Limits::kMaxStoreRecords > 0);
    CHECK(Limits::kMaxWireFrameBytes > 0);
    CHECK(Limits::kMinLatencyNs < 0);
    CHECK(Limits::kMaxLatencyNs > 0);
}
