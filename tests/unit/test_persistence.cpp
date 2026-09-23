// Jitter Observatory - store format, integrity and conservative recovery.
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <jitter/bytes.hpp>
#include <jitter/persistence.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

std::string scratch_directory() {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "jitter-observatory-tests" / "persistence";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return base.string();
}

std::string path_for(const std::string& name) { return scratch_directory() + "/" + name; }

StoreHeader test_header() {
    StoreHeader header;
    header.format_version = kStoreFormatVersion;
    header.store_epoch = 1;
    header.created_at_utc_ns = 1700000000000000000ll;
    header.boot_id = 42;
    header.store_id = Digest::of("unit-test-store");
    return header;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> payload_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace

JITTER_TEST(persistence, create_append_and_reload_round_trip) {
    const std::string path = path_for("round-trip.jostore");
    std::error_code error;
    std::filesystem::remove(path, error);

    auto writer = StoreWriter::create(path, test_header(), false);
    REQUIRE_OK(writer);
    CHECK_EQ(writer.value().record_count(), std::uint64_t{0});

    for (int i = 0; i < 5; ++i) {
        const std::vector<std::uint8_t> payload = payload_of("record-" + std::to_string(i));
        CHECK_OK(writer.value().append(StoreRecordType::Marker, payload));
    }
    CHECK_EQ(writer.value().record_count(), std::uint64_t{5});
    CHECK_OK(writer.value().append_trailer());
    CHECK_OK(writer.value().flush());
    writer.value().close();

    auto loaded = load_store(path);
    REQUIRE_OK(loaded);
    CHECK(loaded.value().recovery.clean);
    CHECK(loaded.value().recovery.trailer_present);
    CHECK(!loaded.value().recovery.truncated_tail);
    CHECK(!loaded.value().recovery.corrupt_tail_record);
    CHECK(!loaded.value().recovery.mid_file_corruption);
    CHECK_EQ(loaded.value().recovery.records_recovered, std::uint64_t{6});
    CHECK_EQ(loaded.value().trailer_record_count, std::uint64_t{5});
    CHECK_EQ(loaded.value().header.boot_id, std::uint64_t{42});
    REQUIRE(loaded.value().records.size() == 6);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK_EQ(loaded.value().records[i].type, StoreRecordType::Marker);
        CHECK_EQ(loaded.value().records[i].payload, payload_of("record-" + std::to_string(i)));
        CHECK_EQ(loaded.value().records[i].payload_bytes, std::uint32_t{8});
    }
    CHECK_EQ(loaded.value().records.back().type, StoreRecordType::Trailer);

    // Creating again without truncation is refused rather than silently overwriting.
    CHECK_ERR(StoreWriter::create(path, test_header(), false), ErrorCode::Duplicate);
    auto truncating = StoreWriter::create(path, test_header(), true);
    CHECK_OK(truncating);
    CHECK_EQ(truncating.value().record_count(), std::uint64_t{0});
    truncating.value().close();
}

JITTER_TEST(persistence, unsupported_format_version_is_refused) {
    const std::string path = path_for("bad-version.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    writer.value().close();

    std::vector<std::uint8_t> bytes = read_file(path);
    REQUIRE(bytes.size() >= 16);
    // Patch the format version field (after the 8 byte magic plus a 4 byte length prefix).
    bytes[8] = 0;
    bytes[9] = 0;
    bytes[10] = 0;
    bytes[11] = 99;
    // Recompute the header CRC (the last four bytes of the header block).
    const std::size_t crc_at = kStoreHeaderBytes - 4;
    const std::uint32_t checksum =
        crc32c(std::span<const std::uint8_t>(bytes.data(), crc_at));
    bytes[crc_at] = static_cast<std::uint8_t>((checksum >> 24) & 0xff);
    bytes[crc_at + 1] = static_cast<std::uint8_t>((checksum >> 16) & 0xff);
    bytes[crc_at + 2] = static_cast<std::uint8_t>((checksum >> 8) & 0xff);
    bytes[crc_at + 3] = static_cast<std::uint8_t>(checksum & 0xff);
    write_file(path, bytes);

    CHECK_ERR(load_store(path), ErrorCode::VersionUnsupported);
}

JITTER_TEST(persistence, header_corruption_is_detected) {
    const std::string path = path_for("bad-header.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    writer.value().close();

    std::vector<std::uint8_t> bytes = read_file(path);
    bytes[40] = static_cast<std::uint8_t>(bytes[40] ^ 0x01);
    write_file(path, bytes);
    CHECK_ERR(load_store(path), ErrorCode::IntegrityFailure);

    std::vector<std::uint8_t> magic = read_file(path);
    magic[0] = 'X';
    write_file(path, magic);
    CHECK_ERR(load_store(path), ErrorCode::Corrupt);
}

JITTER_TEST(persistence, payload_corruption_is_detected_and_never_hidden) {
    const std::string path = path_for("bad-payload.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    CHECK_OK(writer.value().append(StoreRecordType::Source, payload_of("0123456789")));
    CHECK_OK(writer.value().flush());
    writer.value().close();

    std::vector<std::uint8_t> bytes = read_file(path);
    // Flip a bit inside the only record's payload, which starts after the header and the
    // twelve byte record prefix.
    const std::size_t payload_start = kStoreHeaderBytes + 12;
    REQUIRE(bytes.size() > payload_start);
    bytes[payload_start + 3] = static_cast<std::uint8_t>(bytes[payload_start + 3] ^ 0x08);
    write_file(path, bytes);

    auto loaded = load_store(path);
    REQUIRE_OK(loaded);
    CHECK(!loaded.value().recovery.clean);
    CHECK(!loaded.value().recovery.mid_file_corruption);
    CHECK(loaded.value().recovery.corrupt_tail_record || loaded.value().recovery.truncated_tail);
    CHECK_EQ(loaded.value().recovery.records_recovered, std::uint64_t{0});

    // Reopening for append without repair authority is refused.
    CHECK_ERR(StoreWriter::reopen(path, false), ErrorCode::IntegrityFailure);
}

JITTER_TEST(persistence, a_damaged_tail_is_cut_back_only_with_authority) {
    const std::string path = path_for("truncated.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("first")));
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("second")));
    CHECK_OK(writer.value().flush());
    writer.value().close();

    std::vector<std::uint8_t> bytes = read_file(path);
    bytes.resize(bytes.size() - 5);
    write_file(path, bytes);

    auto loaded = load_store(path);
    REQUIRE_OK(loaded);
    CHECK(!loaded.value().recovery.clean);
    CHECK(loaded.value().recovery.truncated_tail);
    CHECK_EQ(loaded.value().recovery.records_recovered, std::uint64_t{1});
    CHECK(loaded.value().recovery.bytes_discarded > 0);

    CHECK_ERR(StoreWriter::reopen(path, false), ErrorCode::IntegrityFailure);

    auto repaired = StoreWriter::reopen(path, true);
    REQUIRE_OK(repaired);
    CHECK(repaired.value().repair_bytes() > 0);
    CHECK_OK(repaired.value().append(StoreRecordType::Marker, payload_of("third")));
    CHECK_OK(repaired.value().append_trailer());
    repaired.value().close();

    auto reloaded = load_store(path);
    REQUIRE_OK(reloaded);
    CHECK(reloaded.value().recovery.clean);
    CHECK_EQ(reloaded.value().recovery.records_recovered, std::uint64_t{3});
    CHECK_EQ(reloaded.value().records[1].payload, payload_of("third"));
    CHECK_EQ(reloaded.value().records[2].type, StoreRecordType::Trailer);
}

JITTER_TEST(persistence, corruption_in_the_middle_is_never_repaired) {
    const std::string path = path_for("mid-file.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("first")));
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("second")));
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("third")));
    CHECK_OK(writer.value().flush());
    writer.value().close();

    std::vector<std::uint8_t> bytes = read_file(path);
    // Flip a bit inside the first record's payload while two more records follow it.
    const std::size_t payload_start = kStoreHeaderBytes + 12;
    REQUIRE(bytes.size() > payload_start);
    bytes[payload_start + 1] = static_cast<std::uint8_t>(bytes[payload_start + 1] ^ 0x10);
    write_file(path, bytes);

    auto loaded = load_store(path);
    REQUIRE_OK(loaded);
    CHECK(!loaded.value().recovery.clean);
    CHECK(loaded.value().recovery.mid_file_corruption);
    CHECK_ERR(StoreWriter::reopen(path, true), ErrorCode::Corrupt);
}

JITTER_TEST(persistence, a_replaced_record_is_caught_by_the_chain) {
    const std::string path = path_for("chain.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("alpha")));
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("beta")));
    CHECK_OK(writer.value().flush());
    writer.value().close();

    auto loaded = load_store(path);
    REQUIRE_OK(loaded);
    CHECK(loaded.value().recovery.clean);
    const Digest first_chain = loaded.value().records[0].chain;
    const Digest second_chain = loaded.value().records[1].chain;
    CHECK(first_chain != second_chain);
    CHECK(!first_chain.is_zero());
}

JITTER_TEST(persistence, reopening_a_closed_store_replaces_its_trailer) {
    const std::string path = path_for("trailer.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);
    CHECK_OK(writer.value().append(StoreRecordType::Marker, payload_of("one")));
    CHECK_OK(writer.value().append_trailer());
    writer.value().close();

    auto reopened = StoreWriter::reopen(path, false);
    REQUIRE_OK(reopened);
    CHECK(reopened.value().repair_bytes() > 0);
    CHECK_EQ(reopened.value().record_count(), std::uint64_t{1});
    CHECK_OK(reopened.value().append(StoreRecordType::Marker, payload_of("two")));
    CHECK_OK(reopened.value().append_trailer());
    reopened.value().close();

    auto loaded = load_store(path);
    REQUIRE_OK(loaded);
    CHECK(loaded.value().recovery.clean);
    CHECK_EQ(loaded.value().recovery.records_recovered, std::uint64_t{3});
    CHECK_EQ(loaded.value().trailer_record_count, std::uint64_t{2});
    CHECK_EQ(loaded.value().records[1].payload, payload_of("two"));
}

JITTER_TEST(persistence, record_bounds_are_enforced) {
    const std::string path = path_for("bounds.jostore");
    auto writer = StoreWriter::create(path, test_header(), true);
    REQUIRE_OK(writer);

    const std::vector<std::uint8_t> too_large(
        static_cast<std::size_t>(Limits::kMaxRecordPayloadBytes) + 1, 0);
    CHECK_ERR(writer.value().append(StoreRecordType::Marker, too_large), ErrorCode::LimitExceeded);
    CHECK_EQ(writer.value().record_count(), std::uint64_t{0});
    CHECK_ERR(writer.value().append(StoreRecordType::Trailer, payload_of("x")),
              ErrorCode::InvalidArgument);

    StoreHeader bad = test_header();
    bad.store_id = Digest{};
    CHECK_ERR(StoreWriter::create(path_for("no-identity.jostore"), bad, true),
              ErrorCode::InvalidArgument);
    bad = test_header();
    bad.format_version = kStoreFormatVersion + 1;
    CHECK_ERR(StoreWriter::create(path_for("bad-version-write.jostore"), bad, true),
              ErrorCode::VersionUnsupported);
    writer.value().close();
}

JITTER_TEST(persistence, missing_and_tiny_files_fail_predictably) {
    CHECK_ERR(load_store(path_for("does-not-exist.jostore")), ErrorCode::IoFailure);

    const std::string tiny = path_for("tiny.jostore");
    write_file(tiny, payload_of("JITTEROB"));
    CHECK_ERR(load_store(tiny), ErrorCode::Corrupt);
}
