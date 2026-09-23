// Jitter Observatory - canonical byte encodings.
// Copyright 2026 Summon Software Labs.
#include <jitter/bytes.hpp>

#include <cstring>

namespace jitter {
namespace {

void put_u16_bytes(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    out[1] = static_cast<std::uint8_t>(value & 0xffu);
}

void put_u32_bytes(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    out[3] = static_cast<std::uint8_t>(value & 0xffu);
}

void put_u64_bytes(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        const auto shift = static_cast<unsigned>((7 - i) * 8);
        out[i] = static_cast<std::uint8_t>((value >> shift) & 0xffull);
    }
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    std::uint8_t buffer[2];
    put_u16_bytes(buffer, value);
    out.insert(out.end(), buffer, buffer + 2);
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t buffer[4];
    put_u32_bytes(buffer, value);
    out.insert(out.end(), buffer, buffer + 4);
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    std::uint8_t buffer[8];
    put_u64_bytes(buffer, value);
    out.insert(out.end(), buffer, buffer + 8);
}

}  // namespace

void ByteWriter::u8(std::uint8_t value) { data_.push_back(value); }

void ByteWriter::boolean(bool value) { data_.push_back(value ? 0x01u : 0x00u); }

void ByteWriter::u16(std::uint16_t value) { append_u16(data_, value); }

void ByteWriter::u32(std::uint32_t value) { append_u32(data_, value); }

void ByteWriter::u64(std::uint64_t value) { append_u64(data_, value); }

void ByteWriter::i64(std::int64_t value) { append_u64(data_, static_cast<std::uint64_t>(value)); }

void ByteWriter::f64_bytes(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(data_, bits);
}

void ByteWriter::str(std::string_view value) {
    append_u32(data_, static_cast<std::uint32_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
}

void ByteWriter::raw(std::span<const std::uint8_t> value) {
    data_.insert(data_.end(), value.begin(), value.end());
}

void ByteWriter::bytes(std::string_view value) {
    data_.insert(data_.end(), value.begin(), value.end());
}

Status ByteReader::need(std::uint64_t count) const {
    if (count > remaining()) {
        return Status::failure(ErrorCode::ProtocolViolation, "truncated canonical encoding",
                               "need=" + std::to_string(count) +
                                   " remaining=" + std::to_string(remaining()));
    }
    return Status::success();
}

Result<std::uint8_t> ByteReader::u8() {
    JITTER_TRY(need(1));
    return data_[offset_++];
}

Result<bool> ByteReader::boolean() {
    const auto raw_value = u8();
    if (!raw_value.ok()) {
        return raw_value.error();
    }
    if (raw_value.value() > 1u) {
        return Result<bool>::fail(ErrorCode::ProtocolViolation, "invalid boolean encoding",
                                  std::to_string(raw_value.value()));
    }
    return raw_value.value() == 1u;
}

Result<std::uint16_t> ByteReader::u16() {
    JITTER_TRY(need(2));
    std::uint16_t value = 0;
    for (int i = 0; i < 2; ++i) {
        value = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(value) << 8) |
            static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)]));
    }
    offset_ += 2;
    return value;
}

Result<std::uint32_t> ByteReader::u32() {
    JITTER_TRY(need(4));
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)]);
    }
    offset_ += 4;
    return value;
}

Result<std::uint64_t> ByteReader::u64() {
    JITTER_TRY(need(8));
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]);
    }
    offset_ += 8;
    return value;
}

Result<std::int64_t> ByteReader::i64() {
    const auto raw_value = u64();
    if (!raw_value.ok()) {
        return raw_value.error();
    }
    return static_cast<std::int64_t>(raw_value.value());
}

Result<double> ByteReader::f64_bytes() {
    const auto bits = u64();
    if (!bits.ok()) {
        return bits.error();
    }
    double value = 0.0;
    const std::uint64_t raw = bits.value();
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

Result<std::string> ByteReader::str() {
    const auto length = u32();
    if (!length.ok()) {
        return length.error();
    }
    JITTER_TRY(need(length.value()));
    std::string value(reinterpret_cast<const char*>(data_.data() + offset_), length.value());
    offset_ += length.value();
    return value;
}

Result<std::span<const std::uint8_t>> ByteReader::raw(std::uint64_t count) {
    JITTER_TRY(need(count));
    const auto length = static_cast<std::size_t>(count);
    const auto slice = data_.subspan(offset_, length);
    offset_ += length;
    return slice;
}

DigestBuilder::DigestBuilder(std::string_view domain) { tag(domain); }

void DigestBuilder::u8(std::uint8_t value) { hasher_.update(&value, 1); }

void DigestBuilder::boolean(bool value) {
    const std::uint8_t encoded = value ? 0x01u : 0x00u;
    hasher_.update(&encoded, 1);
}

void DigestBuilder::u16(std::uint16_t value) {
    std::uint8_t buffer[2];
    put_u16_bytes(buffer, value);
    hasher_.update(buffer, sizeof(buffer));
}

void DigestBuilder::u32(std::uint32_t value) {
    std::uint8_t buffer[4];
    put_u32_bytes(buffer, value);
    hasher_.update(buffer, sizeof(buffer));
}

void DigestBuilder::u64(std::uint64_t value) {
    std::uint8_t buffer[8];
    put_u64_bytes(buffer, value);
    hasher_.update(buffer, sizeof(buffer));
}

void DigestBuilder::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void DigestBuilder::f64(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
}

void DigestBuilder::tag(std::string_view value) { str(value); }

void DigestBuilder::str(std::string_view value) {
    std::uint8_t length_buffer[4];
    put_u32_bytes(length_buffer, static_cast<std::uint32_t>(value.size()));
    hasher_.update(length_buffer, sizeof(length_buffer));
    hasher_.update(value.data(), value.size());
}

void DigestBuilder::raw(std::span<const std::uint8_t> value) {
    hasher_.update(value.data(), value.size());
}

void DigestBuilder::id_bytes(std::span<const std::uint8_t> value) {
    hasher_.update(value.data(), value.size());
}

void DigestBuilder::optional_i64(const std::optional<std::int64_t>& value) {
    if (value.has_value()) {
        u8(1u);
        i64(value.value());
    } else {
        u8(0u);
    }
}

void DigestBuilder::optional_u64(const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
        u8(1u);
        u64(value.value());
    } else {
        u8(0u);
    }
}

Digest DigestBuilder::digest() const {
    Sha256 copy = hasher_;
    return Digest(copy.finish());
}

}  // namespace jitter
