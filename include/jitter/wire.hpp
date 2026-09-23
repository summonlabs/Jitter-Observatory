// Jitter Observatory - versioned ingest wire protocol.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/bytes.hpp>
#include <jitter/error.hpp>
#include <jitter/limits.hpp>
#include <jitter/provenance.hpp>
#include <jitter/sample.hpp>
#include <jitter/version.hpp>

namespace jitter {

// Frame layout, all integers big-endian:
//   magic      4 bytes  "JOW1"
//   version    u32      protocol version, currently 1
//   type       u16      WireMessageType
//   flags      u16      reserved, must be zero
//   length     u32      payload length
//   checksum   u32      CRC-32C over the payload
//   payload    length bytes
// The receiver validates the magic, version, reserved flags, payload bound and
// checksum before it decodes anything.
enum class WireMessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    Batch = 3,
    BatchAck = 4,
    Bye = 5,
    Error = 6,
};

std::string_view to_string(WireMessageType type) noexcept;
bool is_known_message_type(std::uint16_t raw) noexcept;

inline constexpr std::size_t kWireHeaderBytes = 20;

struct WireFrame {
    WireMessageType type = WireMessageType::Hello;
    std::vector<std::uint8_t> payload;
};

// Serialises a frame into a contiguous buffer ready to be written to a byte stream.
std::vector<std::uint8_t> encode_frame(WireMessageType type, std::span<const std::uint8_t> payload);

// Decodes one frame from the front of a buffer. Returns the number of bytes consumed,
// or an error when the buffer holds a prefix of a frame or a malformed frame.
struct FrameDecodeResult {
    WireFrame frame;
    std::size_t consumed = 0;
};

Result<FrameDecodeResult> decode_frame(std::span<const std::uint8_t> buffer);

struct HelloMessage {
    std::uint32_t protocol_version = kWireProtocolVersion;
    SourceId source;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    SourceAuthority authority = SourceAuthority::Unknown;
    std::string agent;
    std::uint32_t protocol_revision = 1;
};

void write_hello(ByteWriter& writer, const HelloMessage& hello);
Result<HelloMessage> read_hello(ByteReader& reader);

struct HelloAckMessage {
    bool accepted = false;
    std::uint32_t protocol_version = kWireProtocolVersion;
    std::string reason;
    std::uint64_t max_frame_bytes = Limits::kMaxWireFrameBytes;
    std::uint64_t max_samples_per_batch = Limits::kMaxSamplesPerBatch;
};

void write_hello_ack(ByteWriter& writer, const HelloAckMessage& ack);
Result<HelloAckMessage> read_hello_ack(ByteReader& reader);

struct BatchAckMessage {
    bool accepted = false;
    std::uint32_t reason_code = 0;
    std::string verdict;
    std::string reason;
    std::uint64_t accepted_samples = 0;
    std::uint64_t rejected_samples = 0;
    std::uint64_t window_retained = 0;
    Digest batch;
};

void write_batch_ack(ByteWriter& writer, const BatchAckMessage& ack);
Result<BatchAckMessage> read_batch_ack(ByteReader& reader);

struct ErrorMessage {
    std::string code;
    std::string message;
};

void write_error(ByteWriter& writer, const ErrorMessage& message);
Result<ErrorMessage> read_error(ByteReader& reader);

struct ByeMessage {
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_acknowledged = 0;
};

void write_bye(ByteWriter& writer, const ByeMessage& bye);
Result<ByeMessage> read_bye(ByteReader& reader);

}  // namespace jitter
