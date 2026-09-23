// Jitter Observatory - real TCP ingest transport.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/error.hpp>
#include <jitter/limits.hpp>
#include <jitter/platform/socket.hpp>
#include <jitter/wire.hpp>

namespace jitter {

// Supplies the ingest context (notably the current instant) for each accepted batch.
// The transport itself never reads the ambient clock; the caller decides what "now"
// means, which is what keeps replay driven runs reproducible.
using IngestContextSource = std::function<IngestContext()>;

struct IngestServerConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0;  // 0 lets the operating system choose a free port
    std::uint64_t max_batches_per_connection = 1000000;
    bool require_hello = true;
    std::string advertised_agent = "jitter-observatory";
};

struct IngestServerStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_completed = 0;
    std::uint64_t connections_failed = 0;
    std::uint64_t hellos_accepted = 0;
    std::uint64_t hellos_rejected = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t batches_accepted = 0;
    std::uint64_t batches_rejected = 0;
    std::uint64_t samples_accepted = 0;
    std::uint64_t samples_rejected = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_sent = 0;
    std::string last_error;
};

// Blocking, framed, request/response reader and writer over one connected socket.
class FrameStream {
public:
    explicit FrameStream(platform::SocketHandle handle, std::size_t read_chunk = 16384);

    // Reads one frame. A peer that closes in an orderly way yields IoFailure with the
    // detail "peer_closed".
    Result<WireFrame> read_frame();
    Status write_frame(WireMessageType type, std::span<const std::uint8_t> payload);

    std::uint64_t bytes_received() const noexcept { return bytes_received_; }
    std::uint64_t bytes_sent() const noexcept { return bytes_sent_; }

private:
    platform::SocketHandle handle_;
    std::vector<std::uint8_t> buffer_;
    std::size_t buffered_start_ = 0;
    std::size_t read_chunk_;
    std::uint64_t bytes_received_ = 0;
    std::uint64_t bytes_sent_ = 0;
};

// Synchronous ingest listener. serve_once() handles exactly one connection from the
// first byte to the client's disconnect, so a test can drive a real client process and
// then wait for the server without any timer.
class TcpIngestServer {
public:
    TcpIngestServer(Observatory& observatory, IngestServerConfig config,
                    IngestContextSource context_source);

    Status start();
    void stop();
    bool listening() const noexcept { return listener_.valid(); }
    std::uint16_t port() const noexcept { return listener_.port; }

    Status serve_once();
    // Serves at most max_connections connections, then returns. Used by the command
    // line server so that its lifetime is bounded by work rather than by a timer.
    Status serve(std::uint64_t max_connections);

    const IngestServerStats& stats() const noexcept { return stats_; }

private:
    Status handle_connection(platform::SocketHandle handle);

    Observatory& observatory_;
    IngestServerConfig config_;
    IngestContextSource context_source_;
    platform::Listener listener_;
    IngestServerStats stats_;
};

class TcpIngestClient {
public:
    static Result<TcpIngestClient> connect(const std::string& host, std::uint16_t port,
                                           HelloMessage hello);

    Result<HelloAckMessage> handshake();
    Result<BatchAckMessage> send_batch(const LatencyBatch& batch);
    Result<ByeMessage> send_bye(std::uint64_t batches_sent, std::uint64_t batches_acknowledged);
    void close();

    std::uint64_t bytes_received() const noexcept { return stream_.bytes_received(); }
    std::uint64_t bytes_sent() const noexcept { return stream_.bytes_sent(); }

private:
    TcpIngestClient(platform::SocketHandle handle) : handle_(handle), stream_(handle) {}

    platform::SocketHandle handle_;
    FrameStream stream_;
    HelloMessage hello_;
    HelloAckMessage ack_;
    bool handshaken_ = false;
};

}  // namespace jitter
