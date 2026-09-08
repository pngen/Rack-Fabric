// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Bounded TCP transport over a loopback endpoint.
//
// A connection is owned by exactly one thread at a time. No lock is held
// across a socket wait, and every read is bounded by the frame limit before
// memory is allocated.

#ifndef RACK_FABRIC_TRANSPORT_TCP_HPP
#define RACK_FABRIC_TRANSPORT_TCP_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/limits.hpp"
#include "rack_fabric/protocol.hpp"

namespace rack_fabric::transport {

/// Initializes the platform socket layer once per process. Returns false when
/// the platform refuses to provide sockets.
[[nodiscard]] bool ensure_socket_layer(std::string& error);

class TcpConnection {
 public:
  TcpConnection() = default;
  explicit TcpConnection(std::uintptr_t socket_handle) : handle_(socket_handle) {}
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;
  ~TcpConnection();

  [[nodiscard]] bool valid() const noexcept { return handle_.load() != kInvalidHandle; }

  /// Sends one framed message. Returns false on any transport error.
  [[nodiscard]] bool send(const Frame& frame, const ResourceLimits& limits, std::string& error);

  /// Receives exactly one framed message. Returns false on transport error,
  /// timeout or a malformed frame; the error string distinguishes them.
  [[nodiscard]] bool receive(Frame& frame, const ResourceLimits& limits,
                             std::chrono::milliseconds timeout, std::string& error);

  /// True when a complete frame is already buffered.
  [[nodiscard]] bool has_buffered_frame() const noexcept;

  /// Closes the connection. Safe to call from another thread while a read or
  /// write is blocked: the blocked call fails with a transport error.
  void close() noexcept;

  [[nodiscard]] std::uintptr_t handle() const noexcept { return handle_.load(); }

  static constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~0ULL);

 private:
  /// Atomic so that close() may be called from another thread while a read or
  /// write is in progress, and so that a double close is a no-op.
  std::atomic<std::uintptr_t> handle_{kInvalidHandle};
};

class TcpListener {
 public:
  TcpListener() = default;
  explicit TcpListener(std::uintptr_t socket_handle) : handle_(socket_handle) {}
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  ~TcpListener();

  /// Binds and listens on a loopback address. port 0 selects an ephemeral
  /// port, which is reported by port().
  [[nodiscard]] static std::optional<TcpListener> bind_loopback(std::uint16_t port, int backlog,
                                                               std::string& error);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept {
    return handle_.load() != TcpConnection::kInvalidHandle;
  }

  /// Accepts one connection. Returns nullopt when the wait expires or the
  /// listener was closed.
  [[nodiscard]] std::optional<TcpConnection> accept(std::chrono::milliseconds timeout,
                                                    std::string& error);

  void close() noexcept;

 private:
  std::atomic<std::uintptr_t> handle_{TcpConnection::kInvalidHandle};
  std::uint16_t port_ = 0;
};

/// Connects to a loopback port with a bounded wait.
[[nodiscard]] std::optional<TcpConnection> connect_loopback(std::uint16_t port,
                                                            std::chrono::milliseconds timeout,
                                                            std::string& error);

}  // namespace rack_fabric::transport

#endif  // RACK_FABRIC_TRANSPORT_TCP_HPP
