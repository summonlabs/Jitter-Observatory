// Jitter Observatory - real TCP ingest transport.
// Copyright 2026 Summon Software Labs.
#include <jitter/transport.hpp>

#include <jitter/codec.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

ErrorMessage to_error_message(const Error& error) {
    ErrorMessage message;
    message.code = std::string(to_string(error.code));
    message.message = error.message;
    if (!error.context.empty()) {
        message.message.append(" [");
        message.message.append(error.context);
        message.message.append("]");
    }
    return message;
}

Status send_error(FrameStream& stream, ErrorCode code, const std::string& text) {
    ErrorMessage message;
    message.code = std::string(to_string(code));
    message.message = text;
    ByteWriter writer;
    write_error(writer, message);
    return stream.write_frame(WireMessageType::Error, writer.data());
}

}  // namespace

FrameStream::FrameStream(platform::SocketHandle handle, std::size_t read_chunk)
    : handle_(handle), read_chunk_(read_chunk == 0 ? 16384 : read_chunk) {}

Result<WireFrame> FrameStream::read_frame() {
    for (;;) {
        if (buffer_.size() > buffered_start_) {
            const std::span<const std::uint8_t> available(buffer_.data() + buffered_start_,
                                                          buffer_.size() - buffered_start_);
            auto decoded = decode_frame(available);
            if (decoded.ok()) {
                buffered_start_ += decoded.value().consumed;
                if (buffered_start_ == buffer_.size()) {
                    buffer_.clear();
                    buffered_start_ = 0;
                } else if (buffered_start_ > 65536) {
                    buffer_.erase(buffer_.begin(),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_start_));
                    buffered_start_ = 0;
                }
                return decoded.value().frame;
            }
            if (decoded.code() != ErrorCode::NotReady) {
                return decoded.error();
            }
        }
        if (buffer_.size() - buffered_start_ > Limits::kMaxWireFrameBytes) {
            return Result<WireFrame>::fail(ErrorCode::LimitExceeded,
                                           "peer exceeded the maximum frame size",
                                           std::to_string(buffer_.size()));
        }
        std::vector<std::uint8_t> chunk(read_chunk_);
        auto received = platform::receive_some(handle_, chunk);
        if (!received.ok()) {
            return received.error();
        }
        if (received.value() == 0) {
            return Result<WireFrame>::fail(ErrorCode::IoFailure, "peer closed the connection",
                                           "peer_closed");
        }
        bytes_received_ += received.value();
        buffer_.insert(buffer_.end(), chunk.begin(),
                       chunk.begin() + static_cast<std::ptrdiff_t>(received.value()));
    }
}

Status FrameStream::write_frame(WireMessageType type, std::span<const std::uint8_t> payload) {
    const std::vector<std::uint8_t> encoded = encode_frame(type, payload);
    JITTER_TRY(platform::send_all(handle_, encoded));
    bytes_sent_ += encoded.size();
    return Status::success();
}

TcpIngestServer::TcpIngestServer(Observatory& observatory, IngestServerConfig config,
                                 IngestContextSource context_source)
    : observatory_(observatory), config_(std::move(config)),
      context_source_(std::move(context_source)) {}

Status TcpIngestServer::start() {
    if (!context_source_) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "ingest server requires an ingest context source");
    }
    if (!observatory_.initialized()) {
        return Status::failure(ErrorCode::NotReady, "ingest server requires an initialised engine");
    }
    auto listener = platform::listen_tcp(config_.bind_address, config_.port);
    if (!listener.ok()) {
        return listener.error();
    }
    listener_ = listener.value();
    return Status::success();
}

void TcpIngestServer::stop() { platform::close_listener(listener_); }

Status TcpIngestServer::serve(std::uint64_t max_connections) {
    for (std::uint64_t i = 0; i < max_connections; ++i) {
        JITTER_TRY(serve_once());
    }
    return Status::success();
}

Status TcpIngestServer::serve_once() {
    if (!listener_.valid()) {
        return Status::failure(ErrorCode::NotReady, "ingest server is not listening");
    }
    auto accepted = platform::accept_one(listener_);
    if (!accepted.ok()) {
        stats_.connections_failed++;
        stats_.last_error = accepted.error().message;
        return accepted.error();
    }
    ++stats_.connections_accepted;
    static_cast<void>(platform::set_no_delay(accepted.value()));
    const Status handled = handle_connection(accepted.value());
    platform::close_socket(accepted.value());
    if (!handled.ok()) {
        ++stats_.connections_failed;
        stats_.last_error = handled.error().message;
        return handled;
    }
    ++stats_.connections_completed;
    return Status::success();
}

Status TcpIngestServer::handle_connection(platform::SocketHandle handle) {
    FrameStream stream(handle);
    bool hello_accepted = !config_.require_hello;
    HelloMessage hello;
    std::uint64_t batches = 0;

    for (;;) {
        auto frame = stream.read_frame();
        if (!frame.ok()) {
            stats_.bytes_received = stream.bytes_received();
            stats_.bytes_sent = stream.bytes_sent();
            if (frame.error().context == "peer_closed") {
                return Status::success();
            }
            return frame.error();
        }
        ++stats_.frames_received;

        if (frame.value().type == WireMessageType::Bye) {
            stats_.bytes_received = stream.bytes_received();
            stats_.bytes_sent = stream.bytes_sent();
            return Status::success();
        }

        if (frame.value().type == WireMessageType::Hello) {
            ByteReader reader(frame.value().payload);
            auto parsed = read_hello(reader);
            if (!parsed.ok()) {
                ++stats_.frames_rejected;
                ++stats_.hellos_rejected;
                static_cast<void>(send_error(stream, parsed.code(), parsed.error().message));
                return Status::success();
            }
            hello = parsed.value();
            HelloAckMessage ack;
            ack.protocol_version = kWireProtocolVersion;
            ack.max_frame_bytes = Limits::kMaxWireFrameBytes;
            ack.max_samples_per_batch = Limits::kMaxSamplesPerBatch;
            if (hello.protocol_version != kWireProtocolVersion) {
                ack.accepted = false;
                ack.reason = "protocol_version_unsupported";
                ++stats_.hellos_rejected;
                hello_accepted = false;
            } else {
                ack.accepted = true;
                ack.reason = "accepted";
                ++stats_.hellos_accepted;
                hello_accepted = true;
            }
            ByteWriter writer;
            write_hello_ack(writer, ack);
            JITTER_TRY(stream.write_frame(WireMessageType::HelloAck, writer.data()));
            continue;
        }

        if (frame.value().type == WireMessageType::Batch) {
            if (!hello_accepted) {
                ++stats_.frames_rejected;
                ++stats_.batches_rejected;
                static_cast<void>(send_error(stream, ErrorCode::ProtocolViolation,
                                             "a batch arrived before an accepted hello"));
                return Status::success();
            }
            ++batches;
            if (batches > config_.max_batches_per_connection) {
                ++stats_.batches_rejected;
                static_cast<void>(send_error(stream, ErrorCode::LimitExceeded,
                                             "connection exceeded its batch budget"));
                return Status::success();
            }
            ByteReader reader(frame.value().payload);
            auto batch = codec::read_batch(reader);
            if (!batch.ok()) {
                ++stats_.frames_rejected;
                ++stats_.batches_rejected;
                static_cast<void>(send_error(stream, batch.code(), batch.error().message));
                return Status::success();
            }

            BatchAckMessage ack;
            ack.batch = batch.value().content;
            const IngestContext context = context_source_();
            auto outcome = observatory_.ingest(batch.value(), context);
            if (!outcome.ok()) {
                ack.accepted = false;
                ack.reason_code = static_cast<std::uint32_t>(outcome.code());
                ack.verdict = std::string(to_string(outcome.code()));
                ack.reason = outcome.error().message;
                ack.rejected_samples = batch.value().samples.size();
                ++stats_.batches_rejected;
                stats_.samples_rejected += batch.value().samples.size();
            } else {
                ack.accepted = outcome.value().admitted();
                ack.reason_code = static_cast<std::uint32_t>(outcome.code());
                ack.verdict = std::string(to_string(outcome.value().verdict));
                ack.reason = outcome.value().reason;
                ack.accepted_samples = outcome.value().accepted;
                ack.rejected_samples = outcome.value().rejected;
                ack.window_retained = outcome.value().window_retained;
                if (outcome.value().admitted()) {
                    ++stats_.batches_accepted;
                    stats_.samples_accepted += outcome.value().accepted;
                } else {
                    ++stats_.batches_rejected;
                    stats_.samples_rejected += batch.value().samples.size();
                }
            }
            ByteWriter writer;
            write_batch_ack(writer, ack);
            JITTER_TRY(stream.write_frame(WireMessageType::BatchAck, writer.data()));
            continue;
        }

        ++stats_.frames_rejected;
        static_cast<void>(send_error(stream, ErrorCode::ProtocolViolation,
                                     "unexpected message type on an ingest connection"));
        return Status::success();
    }
}

Result<TcpIngestClient> TcpIngestClient::connect(const std::string& host, std::uint16_t port,
                                                 HelloMessage hello) {
    auto handle = platform::connect_tcp(host, port);
    if (!handle.ok()) {
        return handle.error();
    }
    static_cast<void>(platform::set_no_delay(handle.value()));
    TcpIngestClient client(handle.value());
    client.hello_ = std::move(hello);
    return client;
}

Result<HelloAckMessage> TcpIngestClient::handshake() {
    ByteWriter writer;
    write_hello(writer, hello_);
    JITTER_TRY(stream_.write_frame(WireMessageType::Hello, writer.data()));
    auto frame = stream_.read_frame();
    if (!frame.ok()) {
        return frame.error();
    }
    if (frame.value().type == WireMessageType::Error) {
        ByteReader reader(frame.value().payload);
        auto message = read_error(reader);
        if (!message.ok()) {
            return message.error();
        }
        return Result<HelloAckMessage>::fail(ErrorCode::ProtocolViolation,
                                             "server rejected the handshake: " + message.value().message);
    }
    if (frame.value().type != WireMessageType::HelloAck) {
        return Result<HelloAckMessage>::fail(ErrorCode::ProtocolViolation,
                                             "server did not acknowledge the handshake");
    }
    ByteReader reader(frame.value().payload);
    auto ack = read_hello_ack(reader);
    if (!ack.ok()) {
        return ack.error();
    }
    ack_ = ack.value();
    handshaken_ = ack.value().accepted;
    return ack_;
}

Result<BatchAckMessage> TcpIngestClient::send_batch(const LatencyBatch& batch) {
    if (!handshaken_) {
        return Result<BatchAckMessage>::fail(ErrorCode::NotReady,
                                             "client must complete its handshake before sending batches");
    }
    ByteWriter writer;
    codec::write_batch(writer, batch);
    JITTER_TRY(stream_.write_frame(WireMessageType::Batch, writer.data()));
    auto frame = stream_.read_frame();
    if (!frame.ok()) {
        return frame.error();
    }
    if (frame.value().type != WireMessageType::BatchAck) {
        return Result<BatchAckMessage>::fail(ErrorCode::ProtocolViolation,
                                             "server did not acknowledge the batch");
    }
    ByteReader reader(frame.value().payload);
    return read_batch_ack(reader);
}

Result<ByeMessage> TcpIngestClient::send_bye(std::uint64_t batches_sent,
                                             std::uint64_t batches_acknowledged) {
    ByeMessage bye;
    bye.batches_sent = batches_sent;
    bye.batches_acknowledged = batches_acknowledged;
    ByteWriter writer;
    write_bye(writer, bye);
    auto status = stream_.write_frame(WireMessageType::Bye, writer.data());
    if (!status.ok()) {
        return status.error();
    }
    return bye;
}

void TcpIngestClient::close() { platform::close_socket(handle_); }

}  // namespace jitter
