// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "transport/tcp.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rack_fabric::transport {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

std::once_flag g_socket_layer_once;
bool g_socket_layer_ok = false;

[[nodiscard]] NativeSocket to_native(std::uintptr_t handle) noexcept {
  return static_cast<NativeSocket>(handle);
}

[[nodiscard]] std::uintptr_t from_native(NativeSocket socket) noexcept {
  return static_cast<std::uintptr_t>(socket);
}

void close_socket(NativeSocket socket) noexcept {
  if (socket == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

[[nodiscard]] std::string last_socket_error(std::string_view prefix) {
#ifdef _WIN32
  return std::string(prefix) + ": winsock error " + std::to_string(::WSAGetLastError());
#else
  return std::string(prefix) + ": " + std::strerror(errno);
#endif
}

/// Waits until the socket is readable, writable or closed.
[[nodiscard]] int wait_socket(NativeSocket socket, bool for_write,
                              std::chrono::milliseconds timeout) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(socket, &set);
  timeval value{};
  value.tv_sec = static_cast<long>(timeout.count() / 1000);
  value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#ifdef _WIN32
  const int result = ::select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr,
                              &value);
#else
  const int result = ::select(socket + 1, for_write ? nullptr : &set, for_write ? &set : nullptr,
                              nullptr, &value);
#endif
  return result;
}

void set_nodelay(NativeSocket socket) {
  const int enabled = 1;
  ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
               sizeof(enabled));
}

}  // namespace

bool ensure_socket_layer(std::string& error) {
  std::call_once(g_socket_layer_once, []() {
#ifdef _WIN32
    WSADATA data{};
    g_socket_layer_ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    g_socket_layer_ok = true;
#endif
  });
  if (!g_socket_layer_ok) {
    error = "the platform socket layer could not be initialized";
  }
  return g_socket_layer_ok;
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : handle_(other.handle_.exchange(kInvalidHandle)) {}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_.store(other.handle_.exchange(kInvalidHandle));
  }
  return *this;
}

TcpConnection::~TcpConnection() { close(); }

bool TcpConnection::send(const Frame& frame, const ResourceLimits& limits, std::string& error) {
  if (!valid()) {
    error = "connection is not open";
    return false;
  }
  const std::optional<std::vector<std::byte>> encoded = encode_frame(frame, limits);
  if (!encoded.has_value()) {
    error = "frame exceeds the configured bound";
    return false;
  }
  const NativeSocket socket = to_native(handle_.load());
  std::size_t sent = 0;
  while (sent < encoded->size()) {
    const int ready = wait_socket(socket, true, std::chrono::milliseconds{5000});
    if (ready <= 0) {
      error = ready == 0 ? "send timed out" : last_socket_error("send wait failed");
      return false;
    }
#ifdef _WIN32
    const int written = ::send(socket, reinterpret_cast<const char*>(encoded->data() + sent),
                               static_cast<int>(encoded->size() - sent), 0);
#else
    const ssize_t written = ::send(socket, encoded->data() + sent, encoded->size() - sent, 0);
#endif
    if (written <= 0) {
      error = last_socket_error("send failed");
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

bool TcpConnection::receive(Frame& frame, const ResourceLimits& limits,
                            std::chrono::milliseconds timeout, std::string& error) {
  if (!valid()) {
    error = "connection is not open";
    return false;
  }
  const NativeSocket socket = to_native(handle_.load());
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  std::vector<std::byte> buffer(kFrameHeaderBytes);
  std::size_t received = 0;
  while (received < kFrameHeaderBytes) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      error = "receive timed out";
      return false;
    }
    const int ready = wait_socket(socket, false, remaining);
    if (ready == 0) {
      error = "receive timed out";
      return false;
    }
    if (ready < 0) {
      error = last_socket_error("receive wait failed");
      return false;
    }
#ifdef _WIN32
    const int read = ::recv(socket, reinterpret_cast<char*>(buffer.data() + received),
                            static_cast<int>(buffer.size() - received), 0);
#else
    const ssize_t read = ::recv(socket, buffer.data() + received, buffer.size() - received, 0);
#endif
    if (read <= 0) {
      error = "connection closed by peer";
      return false;
    }
    received += static_cast<std::size_t>(read);
  }

  // The header is validated before the declared payload length is trusted.
  const DecodeResult header = decode_frame(buffer.data(), kFrameHeaderBytes, limits);
  if (header.status != DecodeStatus::TruncatedFrame) {
    // A complete header either decodes (empty payload) or is malformed.
    if (header.ok()) {
      frame = header.frame;
      return true;
    }
    error = std::string("malformed frame header: ") + std::string(to_string(header.status));
    return false;
  }

  std::uint32_t payload_length = 0;
  std::memcpy(&payload_length, buffer.data() + 20, sizeof(payload_length));
  if (payload_length > limits.max_payload_bytes) {
    error = "declared payload length exceeds the configured bound";
    return false;
  }
  const std::size_t total = kFrameOverheadBytes + static_cast<std::size_t>(payload_length);
  if (total > limits.max_frame_bytes) {
    error = "declared frame length exceeds the configured bound";
    return false;
  }
  buffer.resize(total);
  while (received < total) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      error = "receive timed out";
      return false;
    }
    const int ready = wait_socket(socket, false, remaining);
    if (ready == 0) {
      error = "receive timed out";
      return false;
    }
    if (ready < 0) {
      error = last_socket_error("receive wait failed");
      return false;
    }
#ifdef _WIN32
    const int read = ::recv(socket, reinterpret_cast<char*>(buffer.data() + received),
                            static_cast<int>(buffer.size() - received), 0);
#else
    const ssize_t read = ::recv(socket, buffer.data() + received, buffer.size() - received, 0);
#endif
    if (read <= 0) {
      error = "connection closed by peer";
      return false;
    }
    received += static_cast<std::size_t>(read);
  }

  const DecodeResult decoded = decode_frame(buffer.data(), buffer.size(), limits);
  if (!decoded.ok()) {
    error = std::string("malformed frame: ") + std::string(to_string(decoded.status));
    return false;
  }
  frame = decoded.frame;
  return true;
}

bool TcpConnection::has_buffered_frame() const noexcept { return false; }

void TcpConnection::close() noexcept {
  const std::uintptr_t previous = handle_.exchange(kInvalidHandle);
  if (previous != kInvalidHandle) {
    const NativeSocket socket = to_native(previous);
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
    close_socket(socket);
  }
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_.exchange(TcpConnection::kInvalidHandle)), port_(other.port_) {
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_.store(other.handle_.exchange(TcpConnection::kInvalidHandle));
    port_ = other.port_;
    other.port_ = 0;
  }
  return *this;
}

TcpListener::~TcpListener() { close(); }

std::optional<TcpListener> TcpListener::bind_loopback(std::uint16_t port, int backlog,
                                                      std::string& error) {
  if (!ensure_socket_layer(error)) {
    return std::nullopt;
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    error = last_socket_error("socket creation failed");
    return std::nullopt;
  }
  const int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  address.sin_port = ::htons(port);
  if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    error = last_socket_error("bind failed");
    close_socket(socket);
    return std::nullopt;
  }
  if (::listen(socket, backlog) != 0) {
    error = last_socket_error("listen failed");
    close_socket(socket);
    return std::nullopt;
  }
  sockaddr_in bound{};
#ifdef _WIN32
  int bound_length = static_cast<int>(sizeof(bound));
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    error = last_socket_error("getsockname failed");
    close_socket(socket);
    return std::nullopt;
  }
  TcpListener listener{from_native(socket)};
  listener.port_ = ::ntohs(bound.sin_port);
  return listener;
}

std::optional<TcpConnection> TcpListener::accept(std::chrono::milliseconds timeout,
                                                 std::string& error) {
  if (!valid()) {
    error = "listener is not open";
    return std::nullopt;
  }
  const NativeSocket socket = to_native(handle_.load());
  const int ready = wait_socket(socket, false, timeout);
  if (ready == 0) {
    error = "accept timed out";
    return std::nullopt;
  }
  if (ready < 0) {
    error = last_socket_error("accept wait failed");
    return std::nullopt;
  }
  const NativeSocket accepted = ::accept(socket, nullptr, nullptr);
  if (accepted == kInvalidSocket) {
    error = last_socket_error("accept failed");
    return std::nullopt;
  }
  set_nodelay(accepted);
  return TcpConnection{from_native(accepted)};
}

void TcpListener::close() noexcept {
  const std::uintptr_t previous = handle_.exchange(TcpConnection::kInvalidHandle);
  if (previous != TcpConnection::kInvalidHandle) {
    close_socket(to_native(previous));
  }
}

std::optional<TcpConnection> connect_loopback(std::uint16_t port,
                                              std::chrono::milliseconds timeout,
                                              std::string& error) {
  if (!ensure_socket_layer(error)) {
    return std::nullopt;
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    error = last_socket_error("socket creation failed");
    return std::nullopt;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  address.sin_port = ::htons(port);
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    error = last_socket_error("connect failed");
    close_socket(socket);
    return std::nullopt;
  }
  set_nodelay(socket);
  (void)timeout;
  return TcpConnection{from_native(socket)};
}

}  // namespace rack_fabric::transport
