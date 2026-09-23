// Jitter Observatory - minimal blocking TCP socket layer.
// Copyright 2026 Summon Software Labs.
#include <jitter/platform/socket.hpp>

#include <cstring>

#include <jitter/text.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jitter::platform {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kNativeInvalid = INVALID_SOCKET;

NativeSocket to_native(SocketHandle handle) { return static_cast<NativeSocket>(handle); }
SocketHandle from_native(NativeSocket socket) { return static_cast<SocketHandle>(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kNativeInvalid = -1;

NativeSocket to_native(SocketHandle handle) { return static_cast<NativeSocket>(handle); }
SocketHandle from_native(NativeSocket socket) { return static_cast<SocketHandle>(socket); }
#endif

}  // namespace

SocketSubsystem::SocketSubsystem() {
#if defined(_WIN32)
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    ok_ = result == 0;
    if (!ok_) {
        detail_ = "WSAStartup failed with code " + text::i64_to_string(result);
    } else {
        detail_ = "winsock 2.2 initialised";
    }
#else
    ok_ = true;
    detail_ = "posix sockets available";
#endif
}

SocketSubsystem::~SocketSubsystem() {
#if defined(_WIN32)
    if (ok_) {
        WSACleanup();
    }
#endif
}

SocketSubsystem& socket_subsystem() {
    static SocketSubsystem instance;
    return instance;
}

Result<Listener> listen_tcp(const std::string& bind_address, std::uint16_t port) {
    SocketSubsystem& subsystem = socket_subsystem();
    if (!subsystem.ok()) {
        return Result<Listener>::fail(ErrorCode::IoFailure, "socket subsystem is unavailable",
                                      subsystem.detail());
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const std::string port_text = text::u64_to_string(port);
    const int resolved = getaddrinfo(bind_address.empty() ? "127.0.0.1" : bind_address.c_str(),
                                     port_text.c_str(), &hints, &results);
    if (resolved != 0 || results == nullptr) {
        return Result<Listener>::fail(ErrorCode::IoFailure, "bind address could not be resolved",
                                      bind_address);
    }

    NativeSocket listener = kNativeInvalid;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        listener = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (listener == kNativeInvalid) {
            continue;
        }
        const int reuse = 1;
        static_cast<void>(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                                     reinterpret_cast<const char*>(&reuse), sizeof(reuse)));
        if (::bind(listener, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
            ::listen(listener, 8) == 0) {
            break;
        }
#if defined(_WIN32)
        closesocket(listener);
#else
        ::close(listener);
#endif
        listener = kNativeInvalid;
    }
    freeaddrinfo(results);

    if (listener == kNativeInvalid) {
        return Result<Listener>::fail(ErrorCode::IoFailure, "listening socket could not be created",
                                      bind_address + ":" + port_text);
    }

    sockaddr_in bound{};
#if defined(_WIN32)
    int bound_length = sizeof(bound);
#else
    socklen_t bound_length = sizeof(bound);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
        close_socket(from_native(listener));
        return Result<Listener>::fail(ErrorCode::IoFailure, "bound socket address could not be read");
    }

    Listener result;
    result.handle = from_native(listener);
    result.port = ntohs(bound.sin_port);
    result.address = bind_address.empty() ? "127.0.0.1" : bind_address;
    return result;
}

Result<SocketHandle> accept_one(const Listener& listener) {
    if (!listener.valid()) {
        return Result<SocketHandle>::fail(ErrorCode::NotReady, "listener is not valid");
    }
    sockaddr_storage peer{};
#if defined(_WIN32)
    int peer_length = sizeof(peer);
#else
    socklen_t peer_length = sizeof(peer);
#endif
    const NativeSocket accepted =
        ::accept(to_native(listener.handle), reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (accepted == kNativeInvalid) {
        return Result<SocketHandle>::fail(ErrorCode::IoFailure, "accept failed");
    }
    return from_native(accepted);
}

Result<SocketHandle> connect_tcp(const std::string& host, std::uint16_t port) {
    SocketSubsystem& subsystem = socket_subsystem();
    if (!subsystem.ok()) {
        return Result<SocketHandle>::fail(ErrorCode::IoFailure, "socket subsystem is unavailable",
                                          subsystem.detail());
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const std::string port_text = text::u64_to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results) != 0 || results == nullptr) {
        return Result<SocketHandle>::fail(ErrorCode::IoFailure, "connect address could not be resolved",
                                          host + ":" + port_text);
    }
    NativeSocket socket = kNativeInvalid;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == kNativeInvalid) {
            continue;
        }
        if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
            break;
        }
#if defined(_WIN32)
        closesocket(socket);
#else
        ::close(socket);
#endif
        socket = kNativeInvalid;
    }
    freeaddrinfo(results);
    if (socket == kNativeInvalid) {
        return Result<SocketHandle>::fail(ErrorCode::IoFailure, "connection could not be established",
                                          host + ":" + port_text);
    }
    return from_native(socket);
}

Status send_all(SocketHandle handle, std::span<const std::uint8_t> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int chunk = static_cast<int>(data.size() - sent);
#else
        const std::size_t chunk = data.size() - sent;
#endif
        const auto written = ::send(to_native(handle),
                                    reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
#if defined(_WIN32)
        if (written == SOCKET_ERROR) {
#else
        if (written < 0) {
#endif
            return Status::failure(ErrorCode::IoFailure, "socket send failed");
        }
        if (written == 0) {
            return Status::failure(ErrorCode::IoFailure, "socket send reported zero bytes");
        }
        sent += static_cast<std::size_t>(written);
    }
    return Status::success();
}

Result<std::size_t> receive_some(SocketHandle handle, std::span<std::uint8_t> buffer) {
    if (buffer.empty()) {
        return Result<std::size_t>::fail(ErrorCode::InvalidArgument, "receive buffer is empty");
    }
#if defined(_WIN32)
    const int chunk = static_cast<int>(buffer.size());
#else
    const std::size_t chunk = buffer.size();
#endif
    const auto read = ::recv(to_native(handle), reinterpret_cast<char*>(buffer.data()), chunk, 0);
#if defined(_WIN32)
    if (read == SOCKET_ERROR) {
#else
    if (read < 0) {
#endif
        return Result<std::size_t>::fail(ErrorCode::IoFailure, "socket receive failed");
    }
    return static_cast<std::size_t>(read);
}

void close_socket(SocketHandle handle) noexcept {
    if (handle == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(to_native(handle));
#else
    ::close(to_native(handle));
#endif
}

void close_listener(Listener& listener) noexcept {
    if (!listener.valid()) {
        return;
    }
    close_socket(listener.handle);
    listener.handle = kInvalidSocket;
}

Status set_no_delay(SocketHandle handle) {
    const int enabled = 1;
    if (setsockopt(to_native(handle), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
        return Status::failure(ErrorCode::IoFailure, "TCP_NODELAY could not be set");
    }
    return Status::success();
}

}  // namespace jitter::platform
