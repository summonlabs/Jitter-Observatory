// Jitter Observatory - versioned, integrity checked append only persistence.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/limits.hpp>
#include <jitter/version.hpp>

namespace jitter {

// Every record type the store understands. The numeric values are part of the on disk
// format and must never be reordered.
enum class StoreRecordType : std::uint16_t {
    ClockDomain = 1,
    Equivalence = 2,
    Source = 3,
    Series = 4,
    Path = 5,
    Generation = 6,
    Sample = 7,
    Baseline = 8,
    Episode = 9,
    Conflict = 10,
    Marker = 11,
    Trailer = 12,
    // Per source monotonic guard, so that a replay after a restart is still recognised.
    SourceGuard = 13,
};

std::string_view to_string(StoreRecordType type) noexcept;
bool is_known_record_type(std::uint16_t raw) noexcept;

// Fixed size of the on disk header block. Layout:
//   magic(8) version(u32) header_size(u32) store_epoch(u64) created_at(i64)
//   boot_id(u64) store_id(32) reserved(u32) header_crc32c(u32)
inline constexpr std::size_t kStoreHeaderBytes = 80;

struct StoreHeader {
    std::uint32_t format_version = kStoreFormatVersion;
    // Increments whenever the store file is rewritten from scratch. Evidence recorded
    // under an older epoch is reported as such and never silently treated as current.
    std::uint64_t store_epoch = 0;
    std::int64_t created_at_utc_ns = 0;
    // Identity of the process incarnation that created the store.
    std::uint64_t boot_id = 0;
    Digest store_id;
};

struct StoredRecord {
    StoreRecordType type = StoreRecordType::Marker;
    std::uint64_t offset = 0;
    std::uint32_t payload_bytes = 0;
    Digest chain;
    std::vector<std::uint8_t> payload;
};

// The explicit outcome of reading a store. Every deviation from a pristine file is
// named here; nothing is repaired silently.
struct StoreRecovery {
    bool clean = true;
    bool truncated_tail = false;
    bool corrupt_tail_record = false;
    bool mid_file_corruption = false;
    bool trailer_present = false;
    bool trailer_mismatch = false;
    std::uint64_t records_recovered = 0;
    std::uint64_t bytes_discarded = 0;
    std::uint64_t valid_bytes = 0;
    std::uint64_t file_bytes = 0;
    std::string detail;

    std::string describe() const;
};

struct LoadedStore {
    StoreHeader header;
    std::vector<StoredRecord> records;
    StoreRecovery recovery;
    std::uint64_t trailer_record_count = 0;
    Digest trailer_chain_root;
};

// Streaming reader. The whole file is never materialised, so a store is bounded by the
// record and byte limits rather than by available memory.
Result<LoadedStore> load_store(const std::string& path);

// Append only writer. It keeps the integrity chain in memory between appends and
// closes the file when destroyed.
class StoreWriter {
public:
    StoreWriter() = default;
    StoreWriter(StoreWriter&& other) noexcept;
    StoreWriter& operator=(StoreWriter&& other) noexcept;
    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;
    ~StoreWriter();

    // Creates a new store. When truncate_existing is false an existing file is refused.
    static Result<StoreWriter> create(const std::string& path, const StoreHeader& header,
                                      bool truncate_existing);

    // Reopens an existing store for appending. When repair_truncated_tail is true a
    // damaged tail is cut back to the last valid record and that action is reported by
    // repair_bytes(). Mid file corruption is never repaired.
    static Result<StoreWriter> reopen(const std::string& path, bool repair_truncated_tail);

    Status append(StoreRecordType type, std::span<const std::uint8_t> payload);
    Status append_trailer();

    const StoreHeader& header() const noexcept { return header_; }
    std::uint64_t record_count() const noexcept { return record_count_; }
    std::uint64_t byte_size() const noexcept { return byte_size_; }
    std::uint64_t repair_bytes() const noexcept { return repair_bytes_; }
    const StoreRecovery& last_recovery() const noexcept { return recovery_; }
    bool is_open() const noexcept { return file_ != nullptr; }
    const std::string& path() const noexcept { return path_; }

    Status flush();
    void close();

private:
    void reset() noexcept;

    std::string path_;
    std::unique_ptr<class StoreFileSink> file_;
    StoreHeader header_;
    Digest chain_;
    std::uint64_t record_count_ = 0;
    std::uint64_t byte_size_ = 0;
    std::uint64_t repair_bytes_ = 0;
    StoreRecovery recovery_;
};

}  // namespace jitter
