// Jitter Observatory - versioned, integrity checked append only persistence.
// Copyright 2026 Summon Software Labs.
#include <jitter/persistence.hpp>

#include <array>
#include <filesystem>
#include <fstream>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::size_t kMagicBytes = 8;
constexpr std::size_t kHeaderBytes = kStoreHeaderBytes;
constexpr std::size_t kRecordPrefixBytes = 12;  // length, type, flags, payload crc
constexpr std::size_t kRecordSuffixBytes = 4 + Digest::kBytes;
constexpr std::uint32_t kRecordFlagNone = 0;

std::array<std::uint8_t, kHeaderBytes> encode_header(const StoreHeader& header) {
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    ByteWriter writer;
    writer.bytes(kStoreMagic);
    writer.u32(header.format_version);
    writer.u32(static_cast<std::uint32_t>(kHeaderBytes));
    writer.u64(header.store_epoch);
    writer.i64(header.created_at_utc_ns);
    writer.u64(header.boot_id);
    writer.raw(std::span<const std::uint8_t>(header.store_id.data(), Digest::kBytes));
    writer.u32(0);  // reserved, must stay zero
    const std::vector<std::uint8_t>& prefix = writer.data();
    const std::uint32_t checksum = crc32c(prefix);
    writer.u32(checksum);
    const std::vector<std::uint8_t>& complete = writer.data();
    for (std::size_t i = 0; i < kHeaderBytes && i < complete.size(); ++i) {
        bytes[i] = complete[i];
    }
    return bytes;
}

Result<StoreHeader> decode_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes) {
        return Result<StoreHeader>::fail(ErrorCode::Corrupt, "store header is truncated");
    }
    ByteReader reader(bytes.first(kHeaderBytes - 4));
    // The magic is a raw eight byte marker, not a length prefixed string. It is checked
    // before the checksum so that a file which is not a store at all is reported as such
    // rather than as a damaged store.
    JITTER_TRY_DECL(std::span<const std::uint8_t>, magic_bytes, reader.raw(kMagicBytes));
    const std::string_view magic(reinterpret_cast<const char*>(magic_bytes.data()),
                                 magic_bytes.size());
    if (magic != kStoreMagic) {
        return Result<StoreHeader>::fail(ErrorCode::Corrupt, "store magic does not match",
                                         std::string(magic));
    }
    const std::uint32_t expected = crc32c(bytes.first(kHeaderBytes - 4));
    ByteReader trailer_reader(bytes.subspan(kHeaderBytes - 4, 4));
    JITTER_TRY_DECL(std::uint32_t, stored, trailer_reader.u32());
    if (stored != expected) {
        return Result<StoreHeader>::fail(ErrorCode::IntegrityFailure,
                                         "store header checksum does not match");
    }
    StoreHeader header;
    JITTER_TRY_ASSIGN(header.format_version, reader.u32());
    if (header.format_version != kStoreFormatVersion) {
        return Result<StoreHeader>::fail(
            ErrorCode::VersionUnsupported, "store format version is not supported by this build",
            "found=" + std::to_string(header.format_version) +
                " expected=" + std::to_string(kStoreFormatVersion));
    }
    JITTER_TRY_DECL(std::uint32_t, header_size, reader.u32());
    if (header_size != kHeaderBytes) {
        return Result<StoreHeader>::fail(ErrorCode::Corrupt, "store header size is not supported",
                                         std::to_string(header_size));
    }
    JITTER_TRY_ASSIGN(header.store_epoch, reader.u64());
    JITTER_TRY_ASSIGN(header.created_at_utc_ns, reader.i64());
    JITTER_TRY_ASSIGN(header.boot_id, reader.u64());
    JITTER_TRY_DECL(std::span<const std::uint8_t>, id_bytes, reader.raw(Digest::kBytes));
    std::array<std::uint8_t, Digest::kBytes> id_copy{};
    for (std::size_t i = 0; i < Digest::kBytes; ++i) {
        id_copy[i] = id_bytes[i];
    }
    header.store_id = Digest(id_copy);
    JITTER_TRY_DECL(std::uint32_t, reserved, reader.u32());
    if (reserved != 0) {
        return Result<StoreHeader>::fail(ErrorCode::Corrupt, "store header reserved field is not zero");
    }
    return header;
}

Digest chain_step(const Digest& previous, std::span<const std::uint8_t> record_bytes) {
    Sha256 hasher;
    hasher.update(previous.data(), Digest::kBytes);
    hasher.update(record_bytes.data(), record_bytes.size());
    return Digest(hasher.finish());
}

std::uint64_t file_size_of(std::ifstream& stream) {
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    return size < 0 ? 0 : static_cast<std::uint64_t>(size);
}

void ensure_parent_directory(const std::string& path) {
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(parent, error);
}

}  // namespace

class StoreFileSink {
public:
    std::ofstream stream;
};

std::string_view to_string(StoreRecordType type) noexcept {
    switch (type) {
        case StoreRecordType::ClockDomain: return "clock_domain";
        case StoreRecordType::Equivalence: return "equivalence";
        case StoreRecordType::Source: return "source";
        case StoreRecordType::Series: return "series";
        case StoreRecordType::Path: return "path";
        case StoreRecordType::Generation: return "generation";
        case StoreRecordType::Sample: return "sample";
        case StoreRecordType::Baseline: return "baseline";
        case StoreRecordType::Episode: return "episode";
        case StoreRecordType::Conflict: return "conflict";
        case StoreRecordType::Marker: return "marker";
        case StoreRecordType::Trailer: return "trailer";
        case StoreRecordType::SourceGuard: return "source_guard";
    }
    return "unknown";
}

bool is_known_record_type(std::uint16_t raw) noexcept {
    switch (static_cast<StoreRecordType>(raw)) {
        case StoreRecordType::ClockDomain:
        case StoreRecordType::Equivalence:
        case StoreRecordType::Source:
        case StoreRecordType::Series:
        case StoreRecordType::Path:
        case StoreRecordType::Generation:
        case StoreRecordType::Sample:
        case StoreRecordType::Baseline:
        case StoreRecordType::Episode:
        case StoreRecordType::Conflict:
        case StoreRecordType::Marker:
        case StoreRecordType::Trailer:
        case StoreRecordType::SourceGuard:
            return true;
    }
    return false;
}

std::string StoreRecovery::describe() const {
    std::string out;
    out.append(clean ? "clean" : "recovered");
    out.append(" records=");
    out.append(text::u64_to_string(records_recovered));
    out.append(" valid_bytes=");
    out.append(text::u64_to_string(valid_bytes));
    out.append(" file_bytes=");
    out.append(text::u64_to_string(file_bytes));
    if (truncated_tail) out.append(" truncated_tail");
    if (corrupt_tail_record) out.append(" corrupt_tail_record");
    if (mid_file_corruption) out.append(" mid_file_corruption");
    if (trailer_present) out.append(" trailer_present");
    if (trailer_mismatch) out.append(" trailer_mismatch");
    if (bytes_discarded > 0) {
        out.append(" discarded=");
        out.append(text::u64_to_string(bytes_discarded));
    }
    if (!detail.empty()) {
        out.append(" detail=");
        out.append(detail);
    }
    return out;
}

Result<LoadedStore> load_store(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Result<LoadedStore>::fail(ErrorCode::IoFailure, "store file could not be opened", path);
    }
    const std::uint64_t total = file_size_of(stream);
    if (total < kHeaderBytes) {
        return Result<LoadedStore>::fail(ErrorCode::Corrupt, "store file is smaller than its header",
                                         path);
    }

    LoadedStore loaded;
    loaded.recovery.file_bytes = total;

    std::array<std::uint8_t, kHeaderBytes> header_bytes{};
    stream.read(reinterpret_cast<char*>(header_bytes.data()),
                static_cast<std::streamsize>(kHeaderBytes));
    if (stream.gcount() != static_cast<std::streamsize>(kHeaderBytes)) {
        return Result<LoadedStore>::fail(ErrorCode::IoFailure, "store header could not be read", path);
    }
    auto header = decode_header(header_bytes);
    if (!header.ok()) {
        return header.error();
    }
    loaded.header = header.value();

    Digest chain = Digest::of(std::span<const std::uint8_t>(header_bytes.data(), kHeaderBytes));
    std::uint64_t offset = kHeaderBytes;
    bool stopped = false;

    while (!stopped) {
        if (offset == total) {
            break;
        }
        if (total - offset < kRecordPrefixBytes) {
            loaded.recovery.truncated_tail = true;
            loaded.recovery.bytes_discarded = total - offset;
            loaded.recovery.detail = "incomplete record prefix at the end of the file";
            break;
        }
        std::array<std::uint8_t, kRecordPrefixBytes> prefix{};
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.read(reinterpret_cast<char*>(prefix.data()),
                    static_cast<std::streamsize>(kRecordPrefixBytes));
        if (stream.gcount() != static_cast<std::streamsize>(kRecordPrefixBytes)) {
            loaded.recovery.truncated_tail = true;
            loaded.recovery.bytes_discarded = total - offset;
            loaded.recovery.detail = "record prefix could not be read";
            break;
        }

        ByteReader prefix_reader(prefix);
        auto length = prefix_reader.u32();
        auto type = prefix_reader.u16();
        auto flags = prefix_reader.u16();
        auto payload_crc = prefix_reader.u32();
        if (!length.ok() || !type.ok() || !flags.ok() || !payload_crc.ok()) {
            loaded.recovery.corrupt_tail_record = true;
            loaded.recovery.detail = "record prefix could not be decoded";
            break;
        }

        const std::uint64_t needed = static_cast<std::uint64_t>(kRecordPrefixBytes) +
                                     static_cast<std::uint64_t>(length.value()) +
                                     kRecordSuffixBytes;
        if (length.value() > Limits::kMaxRecordPayloadBytes) {
            loaded.recovery.bytes_discarded = total - offset;
            loaded.recovery.detail = "record declares a payload beyond the supported bound";
            if (total - offset > needed) {
                loaded.recovery.mid_file_corruption = true;
            } else {
                loaded.recovery.corrupt_tail_record = true;
            }
            break;
        }
        if (total - offset < needed) {
            loaded.recovery.truncated_tail = true;
            loaded.recovery.bytes_discarded = total - offset;
            loaded.recovery.detail = "record body is incomplete at the end of the file";
            break;
        }

        std::vector<std::uint8_t> body(static_cast<std::size_t>(needed));
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(needed));
        if (stream.gcount() != static_cast<std::streamsize>(needed)) {
            loaded.recovery.truncated_tail = true;
            loaded.recovery.bytes_discarded = total - offset;
            loaded.recovery.detail = "record body could not be read";
            break;
        }

        const std::span<const std::uint8_t> payload_span(body.data() + kRecordPrefixBytes,
                                                         length.value());
        const std::uint32_t computed_payload_crc = crc32c(payload_span);
        const std::span<const std::uint8_t> checked_span(body.data(),
                                                         kRecordPrefixBytes + length.value());
        const std::uint32_t computed_record_crc = crc32c(checked_span);
        ByteReader suffix_reader(std::span<const std::uint8_t>(
            body.data() + kRecordPrefixBytes + length.value(), kRecordSuffixBytes));
        auto stored_record_crc = suffix_reader.u32();
        auto stored_chain_bytes = suffix_reader.raw(Digest::kBytes);
        if (!stored_record_crc.ok() || !stored_chain_bytes.ok()) {
            loaded.recovery.corrupt_tail_record = true;
            loaded.recovery.detail = "record suffix could not be decoded";
            break;
        }
        std::array<std::uint8_t, Digest::kBytes> stored_chain_array{};
        for (std::size_t i = 0; i < Digest::kBytes; ++i) {
            stored_chain_array[i] = stored_chain_bytes.value()[i];
        }
        const Digest stored_chain(stored_chain_array);

        const bool payload_ok = computed_payload_crc == payload_crc.value();
        const bool record_ok = computed_record_crc == stored_record_crc.value();
        const Digest expected_chain = chain_step(chain, checked_span);
        const bool chain_ok = expected_chain == stored_chain;

        if (!payload_ok || !record_ok || !chain_ok) {
            const std::uint64_t after = total - (offset + needed);
            if (after > 0) {
                loaded.recovery.mid_file_corruption = true;
                loaded.recovery.detail = "record failed its integrity check and data follows it";
            } else {
                loaded.recovery.corrupt_tail_record = true;
                loaded.recovery.detail = payload_ok ? (record_ok ? "integrity chain mismatch"
                                                                 : "record checksum mismatch")
                                                    : "payload checksum mismatch";
            }
            loaded.recovery.bytes_discarded = total - offset;
            break;
        }

        StoredRecord record;
        record.type = static_cast<StoreRecordType>(type.value());
        record.offset = offset;
        record.payload_bytes = length.value();
        record.chain = stored_chain;
        record.payload.assign(payload_span.begin(), payload_span.end());

        if (!is_known_record_type(type.value())) {
            loaded.recovery.detail = "record type is not known to this build";
            loaded.recovery.bytes_discarded = total - offset;
            loaded.recovery.mid_file_corruption = total - offset > needed;
            if (!loaded.recovery.mid_file_corruption) {
                loaded.recovery.corrupt_tail_record = true;
            }
            break;
        }

        offset += needed;
        chain = stored_chain;
        ++loaded.recovery.records_recovered;
        loaded.recovery.valid_bytes = offset;

        if (record.type == StoreRecordType::Trailer) {
            loaded.recovery.trailer_present = true;
            ByteReader trailer_reader(record.payload);
            auto count = trailer_reader.u64();
            auto root = trailer_reader.raw(Digest::kBytes);
            if (!count.ok() || !root.ok() || !trailer_reader.at_end()) {
                loaded.recovery.trailer_mismatch = true;
                loaded.recovery.detail = "trailer record could not be decoded";
            } else {
                loaded.trailer_record_count = count.value();
                std::array<std::uint8_t, Digest::kBytes> root_array{};
                for (std::size_t i = 0; i < Digest::kBytes; ++i) {
                    root_array[i] = root.value()[i];
                }
                loaded.trailer_chain_root = Digest(root_array);
                const Digest previous_chain =
                    loaded.records.empty()
                        ? Digest::of(std::span<const std::uint8_t>(header_bytes.data(), kHeaderBytes))
                        : loaded.records.back().chain;
                if (count.value() != loaded.records.size() ||
                    loaded.trailer_chain_root != previous_chain) {
                    loaded.recovery.trailer_mismatch = true;
                    loaded.recovery.detail =
                        "trailer record count or chain root does not match the records read";
                }
            }
            loaded.records.push_back(std::move(record));
            break;
        }

        loaded.records.push_back(std::move(record));
    }

    if (loaded.recovery.valid_bytes == 0) {
        loaded.recovery.valid_bytes = kHeaderBytes;
    }
    loaded.recovery.clean = !loaded.recovery.truncated_tail &&
                            !loaded.recovery.corrupt_tail_record &&
                            !loaded.recovery.mid_file_corruption &&
                            !loaded.recovery.trailer_mismatch;
    if (loaded.recovery.clean && loaded.recovery.detail.empty()) {
        loaded.recovery.detail = "no integrity deviation detected";
    }
    return loaded;
}

StoreWriter::StoreWriter(StoreWriter&& other) noexcept
    : path_(std::move(other.path_)),
      file_(std::move(other.file_)),
      header_(other.header_),
      chain_(other.chain_),
      record_count_(other.record_count_),
      byte_size_(other.byte_size_),
      repair_bytes_(other.repair_bytes_),
      recovery_(other.recovery_) {
    other.file_.reset();
    other.record_count_ = 0;
    other.byte_size_ = 0;
}

StoreWriter& StoreWriter::operator=(StoreWriter&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        file_ = std::move(other.file_);
        header_ = other.header_;
        chain_ = other.chain_;
        record_count_ = other.record_count_;
        byte_size_ = other.byte_size_;
        repair_bytes_ = other.repair_bytes_;
        recovery_ = other.recovery_;
        other.file_.reset();
        other.record_count_ = 0;
        other.byte_size_ = 0;
    }
    return *this;
}

StoreWriter::~StoreWriter() { close(); }

void StoreWriter::reset() noexcept {
    file_.reset();
    record_count_ = 0;
    byte_size_ = 0;
}

void StoreWriter::close() {
    if (file_ != nullptr && file_->stream.is_open()) {
        file_->stream.flush();
        file_->stream.close();
    }
    file_.reset();
}

Status StoreWriter::flush() {
    if (file_ == nullptr) {
        return Status::failure(ErrorCode::NotReady, "store writer is not open", path_);
    }
    file_->stream.flush();
    if (!file_->stream) {
        return Status::failure(ErrorCode::IoFailure, "store flush failed", path_);
    }
    return Status::success();
}

Result<StoreWriter> StoreWriter::create(const std::string& path, const StoreHeader& header,
                                        bool truncate_existing) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (exists && !truncate_existing) {
        return Result<StoreWriter>::fail(ErrorCode::Duplicate,
                                         "store already exists and truncation was not requested", path);
    }
    ensure_parent_directory(path);

    StoreHeader normalized = header;
    if (normalized.format_version != kStoreFormatVersion) {
        return Result<StoreWriter>::fail(ErrorCode::VersionUnsupported,
                                         "refusing to write an unsupported store format version");
    }
    if (normalized.store_id.is_zero()) {
        return Result<StoreWriter>::fail(ErrorCode::InvalidArgument,
                                         "store header requires a non zero store identity");
    }

    StoreWriter writer;
    writer.path_ = path;
    writer.file_ = std::make_unique<StoreFileSink>();
    writer.file_->stream.open(path, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!writer.file_->stream.is_open()) {
        return Result<StoreWriter>::fail(ErrorCode::IoFailure, "store file could not be created", path);
    }

    const auto header_bytes = encode_header(normalized);
    writer.file_->stream.write(reinterpret_cast<const char*>(header_bytes.data()),
                               static_cast<std::streamsize>(header_bytes.size()));
    if (!writer.file_->stream) {
        return Result<StoreWriter>::fail(ErrorCode::IoFailure, "store header could not be written", path);
    }
    writer.header_ = normalized;
    writer.chain_ = Digest::of(std::span<const std::uint8_t>(header_bytes.data(), header_bytes.size()));
    writer.byte_size_ = kHeaderBytes;
    writer.record_count_ = 0;
    writer.recovery_.valid_bytes = kHeaderBytes;
    writer.recovery_.clean = true;
    writer.recovery_.detail = "created";
    return writer;
}

Result<StoreWriter> StoreWriter::reopen(const std::string& path, bool repair_truncated_tail) {
    auto loaded = load_store(path);
    if (!loaded.ok()) {
        return loaded.error();
    }
    if (loaded.value().recovery.mid_file_corruption) {
        return Result<StoreWriter>::fail(
            ErrorCode::Corrupt,
            "store has corruption before the end of the file and will not be modified", path);
    }

    StoreWriter writer;
    writer.path_ = path;
    writer.header_ = loaded.value().header;
    writer.recovery_ = loaded.value().recovery;
    writer.record_count_ = 0;
    std::uint64_t trailer_offset = 0;
    bool trailer_present = false;
    for (const StoredRecord& record : loaded.value().records) {
        if (record.type == StoreRecordType::Trailer) {
            trailer_offset = record.offset;
            trailer_present = true;
            continue;
        }
        ++writer.record_count_;
        writer.chain_ = record.chain;
    }
    if (writer.record_count_ == 0) {
        const auto header_bytes = encode_header(writer.header_);
        writer.chain_ =
            Digest::of(std::span<const std::uint8_t>(header_bytes.data(), header_bytes.size()));
    }

    const std::uint64_t valid = loaded.value().recovery.valid_bytes;
    const std::uint64_t total = loaded.value().recovery.file_bytes;
    if (valid < total && !repair_truncated_tail) {
        return Result<StoreWriter>::fail(
            ErrorCode::IntegrityFailure,
            "store tail is damaged; reopen without repair authority was refused",
            path + " " + loaded.value().recovery.describe());
    }

    // Bytes removed at reopen: a damaged tail, or a trailer that a newly appended
    // record would otherwise sit behind.
    std::uint64_t target = valid;
    if (trailer_present && trailer_offset < target) {
        target = trailer_offset;
    }
    if (target < total) {
        std::error_code error;
        std::filesystem::resize_file(path, target, error);
        if (error) {
            return Result<StoreWriter>::fail(ErrorCode::IoFailure,
                                             "store could not be truncated for append", path);
        }
        writer.repair_bytes_ = total - target;
        writer.recovery_.detail = loaded.value().recovery.detail +
                                  " ; replaced_on_reopen=" + text::u64_to_string(total - target);
    }

    writer.file_ = std::make_unique<StoreFileSink>();
    writer.file_->stream.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    if (!writer.file_->stream.is_open()) {
        return Result<StoreWriter>::fail(ErrorCode::IoFailure,
                                         "store file could not be reopened for append", path);
    }
    // The writer accounts for the file as it now stands, not as it stood before the
    // superseded trailer was removed.
    writer.byte_size_ = target;
    return writer;
}

Status StoreWriter::append(StoreRecordType type, std::span<const std::uint8_t> payload) {
    if (file_ == nullptr) {
        return Status::failure(ErrorCode::NotReady, "store writer is not open", path_);
    }
    if (type == StoreRecordType::Trailer) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "the trailer record is written by append_trailer");
    }
    JITTER_TRY(require_within(payload.size(), Limits::kMaxRecordPayloadBytes, "store record payload"));
    const std::uint64_t next_count = record_count_ + 1u;
    JITTER_TRY(require_within(next_count, Limits::kMaxStoreRecords, "store record count"));

    std::uint64_t record_bytes = 0;
    JITTER_TRY_ASSIGN(record_bytes, add_u64(static_cast<std::uint64_t>(kRecordPrefixBytes),
                                            static_cast<std::uint64_t>(payload.size()), "store record"));
    JITTER_TRY_ASSIGN(record_bytes, add_u64(record_bytes, kRecordSuffixBytes, "store record"));
    std::uint64_t next_size = 0;
    JITTER_TRY_ASSIGN(next_size, add_u64(byte_size_, record_bytes, "store size"));
    JITTER_TRY(require_within(next_size, Limits::kMaxStoreBytes, "store byte size"));

    ByteWriter prefix;
    prefix.u32(static_cast<std::uint32_t>(payload.size()));
    prefix.u16(static_cast<std::uint16_t>(type));
    prefix.u16(kRecordFlagNone);
    prefix.u32(crc32c(payload));

    std::vector<std::uint8_t> body;
    body.reserve(static_cast<std::size_t>(record_bytes));
    body.insert(body.end(), prefix.data().begin(), prefix.data().end());
    body.insert(body.end(), payload.begin(), payload.end());
    const std::span<const std::uint8_t> checked(body.data(), body.size());
    const std::uint32_t record_crc = crc32c(checked);
    const Digest next_chain = chain_step(chain_, checked);

    ByteWriter suffix;
    suffix.u32(record_crc);
    suffix.raw(std::span<const std::uint8_t>(next_chain.data(), Digest::kBytes));
    body.insert(body.end(), suffix.data().begin(), suffix.data().end());

    file_->stream.write(reinterpret_cast<const char*>(body.data()),
                        static_cast<std::streamsize>(body.size()));
    if (!file_->stream) {
        return Status::failure(ErrorCode::IoFailure, "store append failed", path_);
    }

    chain_ = next_chain;
    record_count_ = next_count;
    byte_size_ = next_size;
    return Status::success();
}

Status StoreWriter::append_trailer() {
    ByteWriter payload;
    payload.u64(record_count_);
    payload.raw(std::span<const std::uint8_t>(chain_.data(), Digest::kBytes));

    if (file_ == nullptr) {
        return Status::failure(ErrorCode::NotReady, "store writer is not open", path_);
    }
    std::uint64_t record_bytes = 0;
    JITTER_TRY_ASSIGN(record_bytes,
                      add_u64(static_cast<std::uint64_t>(kRecordPrefixBytes) + payload.size(),
                              kRecordSuffixBytes, "store trailer"));
    std::uint64_t next_size = 0;
    JITTER_TRY_ASSIGN(next_size, add_u64(byte_size_, record_bytes, "store size"));
    JITTER_TRY(require_within(next_size, Limits::kMaxStoreBytes, "store byte size"));

    ByteWriter prefix;
    prefix.u32(static_cast<std::uint32_t>(payload.size()));
    prefix.u16(static_cast<std::uint16_t>(StoreRecordType::Trailer));
    prefix.u16(kRecordFlagNone);
    prefix.u32(crc32c(payload.data()));

    std::vector<std::uint8_t> body;
    body.insert(body.end(), prefix.data().begin(), prefix.data().end());
    body.insert(body.end(), payload.data().begin(), payload.data().end());
    const std::span<const std::uint8_t> checked(body.data(), body.size());
    const std::uint32_t record_crc = crc32c(checked);
    const Digest next_chain = chain_step(chain_, checked);
    ByteWriter suffix;
    suffix.u32(record_crc);
    suffix.raw(std::span<const std::uint8_t>(next_chain.data(), Digest::kBytes));
    body.insert(body.end(), suffix.data().begin(), suffix.data().end());

    file_->stream.write(reinterpret_cast<const char*>(body.data()),
                        static_cast<std::streamsize>(body.size()));
    if (!file_->stream) {
        return Status::failure(ErrorCode::IoFailure, "store trailer could not be written", path_);
    }
    file_->stream.flush();
    chain_ = next_chain;
    byte_size_ = next_size;
    return Status::success();
}

}  // namespace jitter
