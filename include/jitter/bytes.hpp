// Jitter Observatory - canonical byte encodings and digest construction.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/version.hpp>

namespace jitter {

// Canonical writer used for wire frames, persisted records and export interoperability.
// All multi-byte integers are big-endian so that digests are byte-identical on every
// platform and endianness.
class ByteWriter {
public:
    void u8(std::uint8_t value);
    void boolean(bool value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    // IEEE-754 binary64 bit pattern, big-endian. Used where a value must round-trip
    // bit exactly rather than approximately.
    void f64_bytes(double value);

    // Length prefixed (u32) UTF-8 text.
    void str(std::string_view value);
    void raw(std::span<const std::uint8_t> value);
    void bytes(std::string_view value);

    const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    std::vector<std::uint8_t> take() noexcept { return std::move(data_); }
    std::size_t size() const noexcept { return data_.size(); }
    bool empty() const noexcept { return data_.empty(); }
    void clear() noexcept { data_.clear(); }

private:
    std::vector<std::uint8_t> data_;
};

// Canonical, bounds checked reader. Every read is validated against the remaining
// buffer; a truncated stream yields an explicit error rather than a partial value.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    Result<std::uint8_t> u8();
    Result<bool> boolean();
    Result<std::uint16_t> u16();
    Result<std::uint32_t> u32();
    Result<std::uint64_t> u64();
    Result<std::int64_t> i64();
    Result<double> f64_bytes();
    Result<std::string> str();
    Result<std::span<const std::uint8_t>> raw(std::uint64_t count);

    bool at_end() const noexcept { return offset_ == data_.size(); }
    std::uint64_t remaining() const noexcept {
        return static_cast<std::uint64_t>(data_.size() - offset_);
    }
    std::uint64_t position() const noexcept { return static_cast<std::uint64_t>(offset_); }

private:
    Status need(std::uint64_t count) const;

    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
};

// Incremental canonical digest builder. Every field is length delimited or length
// prefixed, so two different field sequences can never produce the same byte stream.
class DigestBuilder {
public:
    explicit DigestBuilder(std::string_view domain);

    void u8(std::uint8_t value);
    void boolean(bool value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void f64(double value);

    void tag(std::string_view value);
    void str(std::string_view value);
    void raw(std::span<const std::uint8_t> value);
    void id_bytes(std::span<const std::uint8_t> value);
    void optional_i64(const std::optional<std::int64_t>& value);
    void optional_u64(const std::optional<std::uint64_t>& value);

    template <class Tag>
    void id(const StrongId<Tag>& value) {
        id_bytes(value.bytes());
    }

    Digest digest() const;
    template <class Tag>
    StrongId<Tag> as_id() const {
        return StrongId<Tag>::from_digest(digest());
    }
    std::string hex() const { return digest().hex(); }

private:
    Sha256 hasher_;
};

}  // namespace jitter
