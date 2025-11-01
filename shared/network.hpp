#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <limits>
#include <mutex>
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
  None = 0,
  LibraryInitFailed,
  ResolveFailed,
  ConnectFailed,
  BindFailed,
  ListenFailed,
  AcceptFailed,
  SendFailed,
  ReceiveFailed,
  ConnectionClosed,
  InvalidState,
  WouldBlock,
};

struct SocketError {
  SocketErrorCode code{SocketErrorCode::None};
  std::string message;
  std::error_code system;
};

template <typename T> using SocketResult = std::expected<T, SocketError>;

namespace Detail {

#ifdef _WIN32
using NativeHandle = SOCKET;
inline constexpr NativeHandle invalidSocketHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
inline constexpr NativeHandle invalidSocketHandle = -1;
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
  if (handle != invalidSocketHandle) {
    ::closesocket(handle);
  }
#else
  if (handle != invalidSocketHandle) {
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
    return std::unexpected(SocketError{
        .code = SocketErrorCode::ResolveFailed,
        .message = std::move(message),
        .system = std::error_code(queryResult, std::generic_category())});
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
  return SocketError{
      .code = code, .message = std::move(message), .system = systemError};
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
    return std::unexpected(Detail::makeError(SocketErrorCode::LibraryInitFailed,
                                             "WSAStartup", initResult));
  }
#endif
  return {};
}

class TcpSocket {
public:
  using NativeHandle = Detail::NativeHandle;

  TcpSocket() noexcept = default;
  explicit TcpSocket(NativeHandle nativeHandle) noexcept
      : m_handle{nativeHandle} {}

  TcpSocket(TcpSocket &&other) noexcept
      : m_handle{std::exchange(other.m_handle, Detail::invalidSocketHandle)} {}

  auto operator=(TcpSocket &&other) noexcept -> TcpSocket & {
    if (this != &other) {
      close();
      m_handle = std::exchange(other.m_handle, Detail::invalidSocketHandle);
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
      NativeHandle candidate =
          ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
      if (candidate == Detail::invalidSocketHandle) {
        continue;
      }

      int connectStatus = ::connect(candidate, entry->ai_addr,
                                    static_cast<int>(entry->ai_addrlen));
      if (connectStatus == 0) {
        return TcpSocket{candidate};
      }

      Detail::closeHandle(candidate);
    }

    return std::unexpected(
        Detail::makeError(SocketErrorCode::ConnectFailed, "connect"));
  }

  [[nodiscard]] auto isOpen() const noexcept -> bool {
    return m_handle != Detail::invalidSocketHandle;
  }

  auto close() noexcept -> void {
    if (isOpen()) {
      Detail::closeHandle(m_handle);
      m_handle = Detail::invalidSocketHandle;
    }
  }

  auto shutdown() const noexcept -> void {
    if (!isOpen()) {
      return;
    }
#ifdef _WIN32
    ::shutdown(m_handle, SD_BOTH);
#else
    ::shutdown(m_handle, SHUT_RDWR);
#endif
  }

  [[nodiscard]] auto nativeHandle() const noexcept -> NativeHandle {
    return m_handle;
  }

  [[nodiscard]] auto send(std::span<const std::byte> buffer) const
      -> SocketResult<std::size_t> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::InvalidState,
                                               "send on closed socket", 0));
    }
    if (buffer.empty()) {
      return std::size_t{0};
    }

    while (true) {
#ifdef _WIN32
      constexpr auto maxLength =
          static_cast<std::size_t>(std::numeric_limits<int>::max());
      int length = static_cast<int>(std::min(buffer.size(), maxLength));
      auto pointer = Detail::toConstCharPointer(buffer.data());
      int sendResult = ::send(m_handle, pointer, length, 0);
#else
      auto length = buffer.size();
      auto sendResult = ::send(m_handle, buffer.data(), length, 0);
#endif
      if (sendResult >= 0) {
        return static_cast<std::size_t>(sendResult);
      }

      int nativeCode = Detail::lastErrorCode();
      if (Detail::isRetryable(nativeCode)) {
        continue;
      }
      if (Detail::isWouldBlock(nativeCode)) {
        return std::unexpected(
            Detail::makeError(SocketErrorCode::WouldBlock, "send", nativeCode));
      }
      return std::unexpected(
          Detail::makeError(SocketErrorCode::SendFailed, "send", nativeCode));
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
            Detail::makeError(SocketErrorCode::ConnectionClosed, "send"));
      }
      sentTotal += result.value();
    }
    return {};
  }

  [[nodiscard]] auto receive(std::span<std::byte> buffer) const
      -> SocketResult<std::size_t> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::InvalidState,
                                               "receive on closed socket", 0));
    }
    if (buffer.empty()) {
      return std::size_t{0};
    }

    while (true) {
      if (!isOpen()) {
        return std::unexpected(Detail::makeError(
            SocketErrorCode::InvalidState, "receive on closed socket", 0));
      }
#ifdef _WIN32
      constexpr auto maxLength =
          static_cast<std::size_t>(std::numeric_limits<int>::max());
      int length = static_cast<int>(std::min(buffer.size(), maxLength));
      auto pointer = Detail::toCharPointer(buffer.data());
      int receiveResult = ::recv(m_handle, pointer, length, 0);
#else
      auto length = buffer.size();
      auto receiveResult = ::recv(m_handle, buffer.data(), length, 0);
#endif
      if (receiveResult > 0) {
        return static_cast<std::size_t>(receiveResult);
      }
      if (receiveResult == 0) {
        return std::unexpected(
            Detail::makeError(SocketErrorCode::ConnectionClosed, "receive", 0));
      }

      int nativeCode = Detail::lastErrorCode();
      if (Detail::isRetryable(nativeCode)) {
        continue;
      }
      if (Detail::isWouldBlock(nativeCode)) {
        return std::unexpected(Detail::makeError(SocketErrorCode::WouldBlock,
                                                 "receive", nativeCode));
      }
      return std::unexpected(Detail::makeError(SocketErrorCode::ReceiveFailed,
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
            Detail::makeError(SocketErrorCode::ConnectionClosed, "receive"));
      }
      received += chunk.value();
    }
    return buffer;
  }

private:
  NativeHandle m_handle{Detail::invalidSocketHandle};
};

class TcpListener {
public:
  using NativeHandle = Detail::NativeHandle;

  TcpListener() noexcept = default;
  TcpListener(TcpListener &&other) noexcept
      : m_handle{std::exchange(other.m_handle, Detail::invalidSocketHandle)},
        m_family{std::exchange(other.m_family, AF_UNSPEC)} {}

  auto operator=(TcpListener &&other) noexcept -> TcpListener & {
    if (this != &other) {
      close();
      m_handle = std::exchange(other.m_handle, Detail::invalidSocketHandle);
      m_family = std::exchange(other.m_family, AF_UNSPEC);
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
      NativeHandle candidate =
          ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
      if (candidate == Detail::invalidSocketHandle) {
        continue;
      }

      Detail::setReuseAddress(candidate);

      int bindStatus = ::bind(candidate, entry->ai_addr,
                              static_cast<int>(entry->ai_addrlen));
      if (bindStatus == 0) {
        TcpListener listener;
        listener.m_handle = candidate;
        listener.m_family = entry->ai_family;
        return listener;
      }

      Detail::closeHandle(candidate);
    }

    return std::unexpected(
        Detail::makeError(SocketErrorCode::BindFailed, "bind"));
  }

  [[nodiscard]] auto isOpen() const noexcept -> bool {
    return m_handle != Detail::invalidSocketHandle;
  }

  auto close() noexcept -> void {
    if (isOpen()) {
      Detail::closeHandle(m_handle);
      m_handle = Detail::invalidSocketHandle;
      m_family = AF_UNSPEC;
    }
  }

  [[nodiscard]] auto listen(int backlog = SOMAXCONN) const
      -> SocketResult<void> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::InvalidState,
                                               "listen on closed socket", 0));
    }
    if (::listen(m_handle, backlog) != 0) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::ListenFailed, "listen"));
    }
    return {};
  }

  [[nodiscard]] auto accept() const -> SocketResult<TcpSocket> {
    if (!isOpen()) {
      return std::unexpected(Detail::makeError(SocketErrorCode::InvalidState,
                                               "accept on closed socket", 0));
    }

#ifdef _WIN32
    NativeHandle client = ::accept(m_handle, nullptr, nullptr);
    if (client == Detail::invalidSocketHandle) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::AcceptFailed, "accept"));
    }
#else
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    auto *addressPointer = std::bit_cast<sockaddr *>(std::addressof(storage));
    NativeHandle client = ::accept(m_handle, addressPointer, &length);
    if (client == Detail::invalidSocketHandle) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::AcceptFailed, "accept"));
    }
#endif

    return TcpSocket{client};
  }

private:
  NativeHandle m_handle{Detail::invalidSocketHandle};
  int m_family{AF_UNSPEC};
};

} // namespace Moonlapse::Net
