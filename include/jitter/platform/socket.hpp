// Jitter Observatory - minimal blocking TCP socket layer.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <jitter/error.hpp>

namespace jitter::platform {

// Process wide socket subsystem lifetime. Constructing one of these is idempotent and
// cheap; it exists so that Winsock is initialised before any socket call.
class SocketSubsystem {
public:
    SocketSubsystem();
    ~SocketSubsystem();
    SocketSubsystem(const SocketSubsystem&) = delete;
    SocketSubsystem& operator=(const SocketSubsystem&) = delete;
    bool ok() const noexcept { return ok_; }
    const std::string& detail() const noexcept { return detail_; }

private:
    bool ok_ = false;
    std::string detail_;
};

// Process wide socket subsystem handle. It is created on first use and lives until the
// process exits, which is what a listening socket needs: tearing Winsock down while a
// listener is open leaves that listener unable to accept connections.
SocketSubsystem& socket_subsystem();

using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kInvalidSocket = -1;

struct Listener {
    SocketHandle handle = kInvalidSocket;
    std::uint16_t port = 0;
    std::string address;

    bool valid() const noexcept { return handle != kInvalidSocket; }
};

Result<Listener> listen_tcp(const std::string& bind_address, std::uint16_t port);
Result<SocketHandle> accept_one(const Listener& listener);
Result<SocketHandle> connect_tcp(const std::string& host, std::uint16_t port);

Status send_all(SocketHandle handle, std::span<const std::uint8_t> data);
// Returns the number of bytes read; 0 means the peer closed the connection in an
// orderly way.
Result<std::size_t> receive_some(SocketHandle handle, std::span<std::uint8_t> buffer);

void close_socket(SocketHandle handle) noexcept;
void close_listener(Listener& listener) noexcept;

// Disables Nagle so that request/response exchanges are not delayed.
Status set_no_delay(SocketHandle handle);

}  // namespace jitter::platform
