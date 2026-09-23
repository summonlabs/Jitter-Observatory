// Jitter Observatory - versioned ingest wire protocol.
// Copyright 2026 Summon Software Labs.
#include <jitter/wire.hpp>

#include <jitter/codec.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::uint16_t kFlagsNone = 0;

}  // namespace

std::string_view to_string(WireMessageType type) noexcept {
    switch (type) {
        case WireMessageType::Hello: return "hello";
        case WireMessageType::HelloAck: return "hello_ack";
        case WireMessageType::Batch: return "batch";
        case WireMessageType::BatchAck: return "batch_ack";
        case WireMessageType::Bye: return "bye";
        case WireMessageType::Error: return "error";
    }
    return "unknown";
}

bool is_known_message_type(std::uint16_t raw) noexcept {
    switch (static_cast<WireMessageType>(raw)) {
        case WireMessageType::Hello:
        case WireMessageType::HelloAck:
        case WireMessageType::Batch:
        case WireMessageType::BatchAck:
        case WireMessageType::Bye:
        case WireMessageType::Error:
            return true;
    }
    return false;
}

std::vector<std::uint8_t> encode_frame(WireMessageType type, std::span<const std::uint8_t> payload) {
    ByteWriter writer;
    writer.bytes(kWireMagic);
    writer.u32(kWireProtocolVersion);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u16(kFlagsNone);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(crc32c(payload));
    writer.raw(payload);
    return writer.take();
}

Result<FrameDecodeResult> decode_frame(std::span<const std::uint8_t> buffer) {
    if (buffer.size() < kWireHeaderBytes) {
        return Result<FrameDecodeResult>::fail(ErrorCode::NotReady, "frame header is incomplete",
                                               std::to_string(buffer.size()));
    }
    ByteReader header(buffer.first(kWireHeaderBytes));
    // The magic is a raw four byte marker, not a length prefixed string.
    JITTER_TRY_DECL(std::span<const std::uint8_t>, magic_bytes, header.raw(4));
    const std::string_view magic(reinterpret_cast<const char*>(magic_bytes.data()),
                                 magic_bytes.size());
    if (magic != kWireMagic) {
        return Result<FrameDecodeResult>::fail(ErrorCode::ProtocolViolation,
                                               "frame magic does not match", std::string(magic));
    }
    JITTER_TRY_DECL(std::uint32_t, version, header.u32());
    if (version != kWireProtocolVersion) {
        return Result<FrameDecodeResult>::fail(
            ErrorCode::VersionUnsupported, "frame protocol version is not supported",
            "found=" + std::to_string(version));
    }
    JITTER_TRY_DECL(std::uint16_t, type_raw, header.u16());
    if (!is_known_message_type(type_raw)) {
        return Result<FrameDecodeResult>::fail(ErrorCode::ProtocolViolation,
                                               "frame message type is not known",
                                               std::to_string(type_raw));
    }
    JITTER_TRY_DECL(std::uint16_t, flags, header.u16());
    if (flags != kFlagsNone) {
        return Result<FrameDecodeResult>::fail(ErrorCode::ProtocolViolation,
                                               "frame flags are reserved and must be zero",
                                               std::to_string(flags));
    }
    JITTER_TRY_DECL(std::uint32_t, length, header.u32());
    if (length > Limits::kMaxWireFrameBytes) {
        return Result<FrameDecodeResult>::fail(ErrorCode::LimitExceeded,
                                               "frame payload exceeds the supported bound",
                                               std::to_string(length));
    }
    JITTER_TRY_DECL(std::uint32_t, checksum, header.u32());
    const std::uint64_t total = static_cast<std::uint64_t>(kWireHeaderBytes) + length;
    if (buffer.size() < total) {
        return Result<FrameDecodeResult>::fail(ErrorCode::NotReady, "frame payload is incomplete",
                                               std::to_string(buffer.size()) + "/" +
                                                   std::to_string(total));
    }
    const std::span<const std::uint8_t> payload = buffer.subspan(kWireHeaderBytes, length);
    if (crc32c(payload) != checksum) {
        return Result<FrameDecodeResult>::fail(ErrorCode::IntegrityFailure,
                                               "frame payload checksum does not match");
    }
    FrameDecodeResult result;
    result.frame.type = static_cast<WireMessageType>(type_raw);
    result.frame.payload.assign(payload.begin(), payload.end());
    result.consumed = static_cast<std::size_t>(total);
    return result;
}

void write_hello(ByteWriter& writer, const HelloMessage& hello) {
    writer.u32(hello.protocol_version);
    codec::write_id(writer, hello.source);
    writer.u8(static_cast<std::uint8_t>(hello.origin));
    writer.u8(static_cast<std::uint8_t>(hello.authority));
    writer.str(hello.agent);
    writer.u32(hello.protocol_revision);
}

Result<HelloMessage> read_hello(ByteReader& reader) {
    HelloMessage hello;
    JITTER_TRY_ASSIGN(hello.protocol_version, reader.u32());
    JITTER_TRY_ASSIGN(hello.source, codec::read_id<SourceTag>(reader));
    JITTER_TRY_DECL(std::uint8_t, origin, reader.u8());
    if (origin > static_cast<std::uint8_t>(EvidenceOrigin::Unsupported)) {
        return Result<HelloMessage>::fail(ErrorCode::ProtocolViolation,
                                          "hello declares an unknown evidence origin");
    }
    hello.origin = static_cast<EvidenceOrigin>(origin);
    JITTER_TRY_DECL(std::uint8_t, authority, reader.u8());
    if (authority > static_cast<std::uint8_t>(SourceAuthority::Authoritative)) {
        return Result<HelloMessage>::fail(ErrorCode::ProtocolViolation,
                                          "hello declares an unknown source authority");
    }
    hello.authority = static_cast<SourceAuthority>(authority);
    JITTER_TRY_ASSIGN(hello.agent, reader.str());
    JITTER_TRY_ASSIGN(hello.protocol_revision, reader.u32());
    return hello;
}

void write_hello_ack(ByteWriter& writer, const HelloAckMessage& ack) {
    writer.boolean(ack.accepted);
    writer.u32(ack.protocol_version);
    writer.str(ack.reason);
    writer.u64(ack.max_frame_bytes);
    writer.u64(ack.max_samples_per_batch);
}

Result<HelloAckMessage> read_hello_ack(ByteReader& reader) {
    HelloAckMessage ack;
    JITTER_TRY_ASSIGN(ack.accepted, reader.boolean());
    JITTER_TRY_ASSIGN(ack.protocol_version, reader.u32());
    JITTER_TRY_ASSIGN(ack.reason, reader.str());
    JITTER_TRY_ASSIGN(ack.max_frame_bytes, reader.u64());
    JITTER_TRY_ASSIGN(ack.max_samples_per_batch, reader.u64());
    return ack;
}

void write_batch_ack(ByteWriter& writer, const BatchAckMessage& ack) {
    writer.boolean(ack.accepted);
    writer.u32(ack.reason_code);
    writer.str(ack.verdict);
    writer.str(ack.reason);
    writer.u64(ack.accepted_samples);
    writer.u64(ack.rejected_samples);
    writer.u64(ack.window_retained);
    codec::write_digest(writer, ack.batch);
}

Result<BatchAckMessage> read_batch_ack(ByteReader& reader) {
    BatchAckMessage ack;
    JITTER_TRY_ASSIGN(ack.accepted, reader.boolean());
    JITTER_TRY_ASSIGN(ack.reason_code, reader.u32());
    JITTER_TRY_ASSIGN(ack.verdict, reader.str());
    JITTER_TRY_ASSIGN(ack.reason, reader.str());
    JITTER_TRY_ASSIGN(ack.accepted_samples, reader.u64());
    JITTER_TRY_ASSIGN(ack.rejected_samples, reader.u64());
    JITTER_TRY_ASSIGN(ack.window_retained, reader.u64());
    JITTER_TRY_ASSIGN(ack.batch, codec::read_digest(reader));
    return ack;
}

void write_error(ByteWriter& writer, const ErrorMessage& message) {
    writer.str(message.code);
    writer.str(message.message);
}

Result<ErrorMessage> read_error(ByteReader& reader) {
    ErrorMessage message;
    JITTER_TRY_ASSIGN(message.code, reader.str());
    JITTER_TRY_ASSIGN(message.message, reader.str());
    return message;
}

void write_bye(ByteWriter& writer, const ByeMessage& bye) {
    writer.u64(bye.batches_sent);
    writer.u64(bye.batches_acknowledged);
}

Result<ByeMessage> read_bye(ByteReader& reader) {
    ByeMessage bye;
    JITTER_TRY_ASSIGN(bye.batches_sent, reader.u64());
    JITTER_TRY_ASSIGN(bye.batches_acknowledged, reader.u64());
    return bye;
}

}  // namespace jitter
