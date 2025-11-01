#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdlib>

namespace Moonlapse::Net {

enum class SocketErrorCode : std::uint8_t {
  NONE = 0,
  LIBRARY_INIT_FAILED,
  RESOLVE_FAILED,
  CONNECT_FAILED,
  BIND_FAILED,
  LISTEN_FAILED,
  ACCEPT_FAILED,
  SEND_FAILED,
  RECEIVE_FAILED,
  CONNECTION_CLOSED,
  INVALID_STATE,
  WOULD_BLOCK,
};

struct SocketError {
  SocketErrorCode code{SocketErrorCode::NONE};
  std::string message;
  std::error_code system;
};

template <typename T> using SocketResult = std::expected<T, SocketError>;

namespace Detail {

#ifdef _WIN32
using NativeHandle = SOCKET;
inline constexpr NativeHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
using NativeHandle = int;
inline constexpr NativeHandle INVALID_SOCKET_HANDLE = -1;
#endif

struct AddrInfoDeleter {
  void operator()(addrinfo *info) const noexcept {
    if (info != nullptr) {
      ::freeaddrinfo(info);
    }
  }
};

using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

inline auto lastErrorCode() noexcept -> int {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

inline auto makeSystemError(int nativeCode) -> std::error_code {
  return std::error_code(nativeCode, std::system_category());
}

inline auto isRetryable(int nativeCode) noexcept -> bool {
#ifdef _WIN32
  return nativeCode == WSAEINTR;
#else
  return nativeCode == EINTR;
#endif
}

inline auto isWouldBlock(int nativeCode) noexcept -> bool {
#ifdef _WIN32
  return nativeCode == WSAEWOULDBLOCK;
#else
  return nativeCode == EWOULDBLOCK || nativeCode == EAGAIN;
#endif
}

inline void closeHandle(NativeHandle handle) noexcept {
#ifdef _WIN32
  if (handle != INVALID_SOCKET_HANDLE) {
    ::closesocket(handle);
  }
#else
  if (handle != INVALID_SOCKET_HANDLE) {
    ::close(handle);
  }
#endif
}

inline void setReuseAddress(NativeHandle handle) noexcept {
  int enable = 1;
#ifdef _WIN32
  auto pointer = std::bit_cast<const char *>(&enable);
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, pointer, sizeof(enable));
#else
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif
}

template <typename Pointer>
[[nodiscard]] auto toConstCharPointer(const Pointer *pointer) noexcept -> const
    char * {
  return std::bit_cast<const char *>(pointer);
}

template <typename Pointer>
[[nodiscard]] auto toCharPointer(Pointer *pointer) noexcept -> char * {
  return std::bit_cast<char *>(pointer);
}

inline auto resolveAddress(std::string_view host, std::uint16_t port,
                           bool passive) -> SocketResult<AddrInfoPtr> {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = passive ? AI_PASSIVE : 0;

  addrinfo *rawInfo = nullptr;
  std::string hostString(host);
  std::string portString = std::to_string(port);
  const char *hostPointer = hostString.empty() ? nullptr : hostString.c_str();
  if (passive && hostString.empty()) {
    hostPointer = nullptr;
  }

  int queryResult =
      ::getaddrinfo(hostPointer, portString.c_str(), &hints, &rawInfo);
  if (queryResult != 0) {
#ifdef _WIN32
    std::string message =
        std::string{"getaddrinfo: "} + ::gai_strerrorA(queryResult);
#else
    std::string message =
        std::string{"getaddrinfo: "} + ::gai_strerror(queryResult);
#endif
    return std::unexpected(
        SocketError{SocketErrorCode::RESOLVE_FAILED, std::move(message),
                    std::error_code(queryResult, std::generic_category())});
  }

  return AddrInfoPtr{rawInfo};
}

inline auto makeError(SocketErrorCode code, std::string_view context,
                      int nativeCode = -1) -> SocketError {
  int effectiveCode = nativeCode >= 0 ? nativeCode : lastErrorCode();
  std::error_code systemError = makeSystemError(effectiveCode);
  std::string message(context);
  if (effectiveCode != 0) {
    message.append(": ");
    message.append(systemError.message());
  }
  return SocketError{code, std::move(message), systemError};
}

} // namespace Detail

[[nodiscard]] inline auto ensureSocketLibrary() -> SocketResult<void> {
#ifdef _WIN32
  static std::once_flag initFlag;
  static int initResult = 0;
  std::call_once(initFlag, []() {
    WSADATA data{};
    initResult = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (initResult == 0) {
      std::atexit([]() { ::WSACleanup(); });
    }
  });
  if (initResult != 0) {
    auto error = Detail::makeError(SocketErrorCode::LIBRARY_INIT_FAILED,
                                   "WSAStartup", initResult);
    return std::unexpected(std::move(error));
  }
#endif
  return {};
}

class TcpSocket {
public:
  using NativeHandle = Detail::NativeHandle;

  TcpSocket() noexcept = default;
  explicit TcpSocket(NativeHandle handle) noexcept : handle_{handle} {}

  TcpSocket(TcpSocket &&other) noexcept
      : handle_{std::exchange(other.handle_, Detail::INVALID_SOCKET_HANDLE)} {}

  auto operator=(TcpSocket &&other) noexcept -> TcpSocket & {
    if (this != &other) {
      close();
      handle_ = std::exchange(other.handle_, Detail::INVALID_SOCKET_HANDLE);
    }
    return *this;
  }

  TcpSocket(const TcpSocket &) = delete;
  auto operator=(const TcpSocket &) -> TcpSocket & = delete;

  ~TcpSocket() { close(); }

  [[nodiscard]] static auto connect(std::string_view host, std::uint16_t port)
      -> SocketResult<TcpSocket> {
    auto initResult = ensureSocketLibrary();
    if (!initResult) {
      return std::unexpected(initResult.error());
    }

    auto addresses = Detail::resolveAddress(host, port, false);
    if (!addresses) {
      return std::unexpected(addresses.error());
    }
    auto info = std::move(addresses.value());

    for (auto *entry = info.get(); entry != nullptr; entry = entry->ai_next) {
      NativeHandle handle =
          ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
      if (handle == Detail::INVALID_SOCKET_HANDLE) {
        continue;
      }

      int connectStatus = ::connect(handle, entry->ai_addr,
                                    static_cast<int>(entry->ai_addrlen));
      if (connectStatus == 0) {
        return TcpSocket{handle};
      }

      Detail::closeHandle(handle);
    }

    return std::unexpected(
        Detail::makeError(SocketErrorCode::CONNECT_FAILED, "connect"));
  }

  [[nodiscard]] auto isOpen() const noexcept -> bool {
    return handle_ != Detail::INVALID_SOCKET_HANDLE;
  }

  auto close() noexcept -> void {
    if (isOpen()) {
      Detail::closeHandle(handle_);
      handle_ = Detail::INVALID_SOCKET_HANDLE;
    }
  }

  auto shutdown() const noexcept -> void {
    if (!isOpen()) {
      return;
    }
#ifdef _WIN32
    ::shutdown(handle_, SD_BOTH);
#else
    ::shutdown(handle_, SHUT_RDWR);
#endif
  }

  [[nodiscard]] auto nativeHandle() const noexcept -> NativeHandle {
    return handle_;
  }

  [[nodiscard]] auto send(std::span<const std::byte> buffer) const
      -> SocketResult<std::size_t> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::INVALID_STATE,
                                               "send on closed socket", 0));
    }
    if (buffer.empty()) {
      return std::size_t{0};
    }

    while (true) {
#ifdef _WIN32
      const auto maxLength =
          static_cast<std::size_t>(std::numeric_limits<int>::max());
      int length = static_cast<int>(std::min(buffer.size(), maxLength));
      auto pointer = Detail::toConstCharPointer(buffer.data());
      int sendResult = ::send(handle_, pointer, length, 0);
#else
      auto length = buffer.size();
      auto sendResult = ::send(handle_, buffer.data(), length, 0);
#endif
      if (sendResult >= 0) {
        return static_cast<std::size_t>(sendResult);
      }

      int nativeCode = Detail::lastErrorCode();
      if (Detail::isRetryable(nativeCode)) {
        continue;
      }
      if (Detail::isWouldBlock(nativeCode)) {
        return std::unexpected(Detail::makeError(SocketErrorCode::WOULD_BLOCK,
                                                 "send", nativeCode));
      }
      return std::unexpected(
          Detail::makeError(SocketErrorCode::SEND_FAILED, "send", nativeCode));
    }
  }

  [[nodiscard]] auto sendAll(std::span<const std::byte> buffer) const
      -> SocketResult<void> {
    std::size_t sentTotal = 0;
    while (sentTotal < buffer.size()) {
      auto remaining = buffer.subspan(sentTotal);
      auto result = send(remaining);
      if (!result) {
        return std::unexpected(result.error());
      }
      if (result.value() == 0) {
        return std::unexpected(
            Detail::makeError(SocketErrorCode::CONNECTION_CLOSED, "send"));
      }
      sentTotal += result.value();
    }
    return {};
  }

  [[nodiscard]] auto receive(std::span<std::byte> buffer) const
      -> SocketResult<std::size_t> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::INVALID_STATE,
                                               "receive on closed socket", 0));
    }
    if (buffer.empty()) {
      return std::size_t{0};
    }

    while (true) {
      if (!isOpen()) {
        return std::unexpected(Detail::makeError(
            SocketErrorCode::INVALID_STATE, "receive on closed socket", 0));
      }
#ifdef _WIN32
      const auto maxLength =
          static_cast<std::size_t>(std::numeric_limits<int>::max());
      int length = static_cast<int>(std::min(buffer.size(), maxLength));
      auto pointer = Detail::toCharPointer(buffer.data());
      int receiveResult = ::recv(handle_, pointer, length, 0);
#else
      auto length = buffer.size();
      auto receiveResult = ::recv(handle_, buffer.data(), length, 0);
#endif
      if (receiveResult > 0) {
        return static_cast<std::size_t>(receiveResult);
      }
      if (receiveResult == 0) {
        return std::unexpected(Detail::makeError(
            SocketErrorCode::CONNECTION_CLOSED, "receive", 0));
      }

      int nativeCode = Detail::lastErrorCode();
      if (Detail::isRetryable(nativeCode)) {
        continue;
      }
      if (Detail::isWouldBlock(nativeCode)) {
        return std::unexpected(Detail::makeError(SocketErrorCode::WOULD_BLOCK,
                                                 "receive", nativeCode));
      }
      return std::unexpected(Detail::makeError(SocketErrorCode::RECEIVE_FAILED,
                                               "receive", nativeCode));
    }
  }

  [[nodiscard]] auto receiveExact(std::size_t byteCount) const
      -> SocketResult<std::vector<std::byte>> {
    std::vector<std::byte> buffer(byteCount);
    std::size_t received = 0;
    while (received < byteCount) {
      auto span = std::span<std::byte>{buffer}.subspan(received);
      auto chunk = receive(span);
      if (!chunk) {
        return std::unexpected(chunk.error());
      }
      if (chunk.value() == 0) {
        return std::unexpected(
            Detail::makeError(SocketErrorCode::CONNECTION_CLOSED, "receive"));
      }
      received += chunk.value();
    }
    return buffer;
  }

private:
  NativeHandle handle_{Detail::INVALID_SOCKET_HANDLE};
};

class TcpListener {
public:
  using NativeHandle = Detail::NativeHandle;

  TcpListener() noexcept = default;
  TcpListener(TcpListener &&other) noexcept
      : handle_{std::exchange(other.handle_, Detail::INVALID_SOCKET_HANDLE)},
        family_{std::exchange(other.family_, AF_UNSPEC)} {}

  auto operator=(TcpListener &&other) noexcept -> TcpListener & {
    if (this != &other) {
      close();
      handle_ = std::exchange(other.handle_, Detail::INVALID_SOCKET_HANDLE);
      family_ = std::exchange(other.family_, AF_UNSPEC);
    }
    return *this;
  }

  TcpListener(const TcpListener &) = delete;
  auto operator=(const TcpListener &) -> TcpListener & = delete;

  ~TcpListener() { close(); }

  [[nodiscard]] static auto bind(std::string_view host, std::uint16_t port)
      -> SocketResult<TcpListener> {
    auto initResult = ensureSocketLibrary();
    if (!initResult) {
      return std::unexpected(initResult.error());
    }

    auto addresses = Detail::resolveAddress(host, port, true);
    if (!addresses) {
      return std::unexpected(addresses.error());
    }
    auto info = std::move(addresses.value());

    for (auto *entry = info.get(); entry != nullptr; entry = entry->ai_next) {
      NativeHandle handle =
          ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
      if (handle == Detail::INVALID_SOCKET_HANDLE) {
        continue;
      }

      Detail::setReuseAddress(handle);

      int bindStatus =
          ::bind(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
      if (bindStatus == 0) {
        TcpListener listener;
        listener.handle_ = handle;
        listener.family_ = entry->ai_family;
        return listener;
      }

      Detail::closeHandle(handle);
    }

    return std::unexpected(
        Detail::makeError(SocketErrorCode::BIND_FAILED, "bind"));
  }

  [[nodiscard]] auto isOpen() const noexcept -> bool {
    return handle_ != Detail::INVALID_SOCKET_HANDLE;
  }

  auto close() noexcept -> void {
    if (isOpen()) {
      Detail::closeHandle(handle_);
      handle_ = Detail::INVALID_SOCKET_HANDLE;
      family_ = AF_UNSPEC;
    }
  }

  [[nodiscard]] auto listen(int backlog = SOMAXCONN) const
      -> SocketResult<void> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::INVALID_STATE,
                                               "listen on closed socket", 0));
    }
    if (::listen(handle_, backlog) != 0) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::LISTEN_FAILED, "listen"));
    }
    return {};
  }

  [[nodiscard]] auto accept() const -> SocketResult<TcpSocket> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::INVALID_STATE,
                                               "accept on closed socket", 0));
    }

#ifdef _WIN32
    NativeHandle client = ::accept(handle_, nullptr, nullptr);
    if (client == Detail::INVALID_SOCKET_HANDLE) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::ACCEPT_FAILED, "accept"));
    }
#else
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    auto *addressPointer = std::bit_cast<sockaddr *>(std::addressof(storage));
    NativeHandle client = ::accept(handle_, addressPointer, &length);
    if (client == Detail::INVALID_SOCKET_HANDLE) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::ACCEPT_FAILED, "accept"));
    }
#endif

    return TcpSocket{client};
  }

private:
  NativeHandle handle_{Detail::INVALID_SOCKET_HANDLE};
  int family_{AF_UNSPEC};
};

} // namespace Moonlapse::Net
