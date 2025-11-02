# §02 Building the Networking Layer with RAII Sockets

In §01 we modelled packets and proved the encode/decode pipeline. Now we have to move those octets across a wire. This chapter builds the cross-platform networking primitives that power both the server and the client. You will:

1. wrap operating-system sockets in RAII-friendly C++23 classes,
2. propagate failures with `std::expected`-style results instead of errno checks,
3. implement blocking send/receive helpers that respect partial writes,
4. compose those helpers inside the server accept loop and the client receiver thread.

By the end you will understand every line of `shared/network.hpp` and how the higher-level modules rely on it.

---

## 1. Shape the networking surface area

We hide the platform details in a small namespace: `Moonlapse::Net`. Two public classes cover the entire socket lifecycle:

- `TcpSocket` models a connected TCP endpoint (client side or accepted server connection).
- `TcpListener` models a listening socket that can accept new clients.

Both types follow RAII: their destructors close the underlying handle automatically. There is no way to leak a socket by forgetting to call `close()`.

```cpp
namespace Moonlapse::Net {

enum class SocketErrorCode : std::uint8_t {
  None,
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

template <typename T>
using SocketResult = std::expected<T, SocketError>;
```

The codecs live alongside the socket wrappers so every operation that can fail returns a `SocketResult<T>`. Callers must check the result; the compiler enforces it.

### 1.1 Cross-platform shims live in `Detail`

The `Detail` namespace stores the platform glue: native handle typedefs, RAII deleters for `addrinfo`, helpers to convert pointer types, and wrappers that translate errno/`WSAGetLastError` into `std::error_code`. You never touch those from the client or server—`TcpSocket` calls them internally.

Key helpers include:

- `resolveAddress(host, port, passive)` wraps `getaddrinfo`.
- `makeError(code, context, nativeCode)` decorates a high-level `SocketError` with the OS message.
- `ensureSocketLibrary()` runs `WSAStartup` on Windows once and registers `WSACleanup()` via `std::atexit`.

```cpp
inline auto makeError(SocketErrorCode code, std::string_view context,
                      int nativeCode = -1) -> SocketError {
  int effective = nativeCode >= 0 ? nativeCode : lastErrorCode();
  auto system = makeSystemError(effective);
  std::string message(context);
  if (effective != 0) {
    message.append(": ");
    message.append(system.message());
  }
  return SocketError{.code = code, .message = std::move(message),
                     .system = system};
}
```

That little wrapper highlights the theme for the whole layer: grab the
platform error, translate it into a portable `std::error_code`, and return an
object the caller can inspect without touching errno directly.

🧠 **Concept note:** `ensureSocketLibrary()` only does real work on Windows. It
uses `std::call_once` to invoke `WSAStartup` exactly one time and registers
`WSACleanup()` with `std::atexit`. On Linux and macOS the function returns
immediately, but the call sites stay symmetric across platforms.

The thin shims keep the public RAII classes small and readable.

---

## 2. `TcpSocket`: owned connections, safe sends

`TcpSocket` owns a single connected handle. It is move-only to prevent accidental sharing. Construction uses factory functions:

- `TcpSocket::connect(host, port)` for clients.
- `TcpListener::accept()` for servers.

```cpp
class TcpSocket {
public:
  TcpSocket() noexcept = default;
  explicit TcpSocket(NativeHandle nativeHandle) noexcept;
  TcpSocket(TcpSocket&& other) noexcept;
  auto operator=(TcpSocket&& other) noexcept -> TcpSocket&;
  ~TcpSocket() { close(); }

  [[nodiscard]] static auto connect(std::string_view host,
                                    std::uint16_t port)
      -> SocketResult<TcpSocket>;

  [[nodiscard]] auto send(std::span<const std::byte> buffer) const
      -> SocketResult<std::size_t>;
  [[nodiscard]] auto sendAll(std::span<const std::byte> buffer) const
      -> SocketResult<void>;
  [[nodiscard]] auto receive(std::span<std::byte> buffer) const
      -> SocketResult<std::size_t>;
  [[nodiscard]] auto receiveExact(std::size_t byteCount) const
      -> SocketResult<std::vector<std::byte>>;

  void shutdown() const noexcept;
  void close() noexcept;
  [[nodiscard]] auto isOpen() const noexcept -> bool;
};
```

🧠 **Concept note:** Methods such as `send(...) const` look odd if you come
from Go or Python. In C++ `const` signals that the observable state of the
object does not change—writing to the socket is an external side effect, but
the wrapper’s data members stay untouched, so we can safely mark the method
`const`.

### 2.1 Deterministic ownership in a GC-free world

Go and Python developers normally lean on garbage collection or `defer` / context
managers for cleanup. C++ has no GC, so RAII (Resource Acquisition Is
Initialization) is how we guarantee deterministic destruction. When a
`TcpSocket` instance leaves scope its destructor immediately calls `close()`,
which in turn invokes `Detail::closeHandle`. There is no window where the
operating system handle lingers because a collector has not run yet.

> **Contrast:** In Go you might write `defer conn.Close()` and trust the runtime
> to run it eventually. In this codebase, the compiler inserts the call at scope
> exit, so the handle closes even if we return early on error.

```c
// C-style sockets demand manual cleanup on every exit path.
int fd = socket(AF_INET, SOCK_STREAM, 0);
if (fd == -1) { return -1; }
// ... configure fd ...
close(fd); // must remember this for every return statement
```

RAII turns the closing logic into a destructor so every code path is covered
without duplicated `close()` calls.

### 2.2 Factory functions instead of raw constructors

The static `connect()` function hides the platform discovery (`getaddrinfo`),
performs the OS call, and returns either a fully initialized socket or an
error. A plain constructor cannot report failure, so we favour factory
functions that return `SocketResult<TcpSocket>`. Callers spell
`auto socket = TcpSocket::connect(...);` and must check the result before they
obtain a usable object.

### 2.3 A move semantics primer

Sockets represent unique ownership: duplicating the handle would let two
objects close it twice. We therefore delete copy operations and implement
move-only behaviour:

```cpp
TcpSocket(TcpSocket&& other) noexcept
  : m_handle{std::exchange(other.m_handle, Detail::invalidSocketHandle)} {}

auto operator=(TcpSocket&& other) noexcept -> TcpSocket& {
  if (this != &other) {
    close();
    m_handle =
        std::exchange(other.m_handle, Detail::invalidSocketHandle);
  }
  return *this;
}
```

🧠 **Concept note:** `std::exchange` swaps the current handle with
`invalidSocketHandle` in one expression, leaving the moved-from object in a
safe, closed state. That is the C++23 way to spell “take ownership and null out
the source” without manual `std::swap` plumbing.

### 2.4 Blocking sockets and partial transfers

TCP sockets are stream oriented. A single call to `send` or `recv` talks to the
kernel, which may accept fewer bytes than requested because its internal buffer
is full. We loop until the entire message is transferred:

```cpp
[[nodiscard]] auto TcpSocket::sendAll(std::span<const std::byte> buffer) const
    -> SocketResult<void> {
  std::size_t sentTotal = 0;
  while (sentTotal < buffer.size()) {
    auto chunk = send(buffer.subspan(sentTotal));
    if (!chunk) {
      return std::unexpected(chunk.error());
    }
    if (chunk.value() == 0) {
      return std::unexpected(
          Detail::makeError(SocketErrorCode::ConnectionClosed, "send"));
    }
    sentTotal += chunk.value();
  }
  return {};
}
```

Both `send()` and `receive()` retry automatically on interruptible errors such
as `EINTR`. When the OS reports `EWOULDBLOCK`/`WSAEWOULDBLOCK` we bubble up
`SocketErrorCode::WouldBlock`, which lets us extend the wrappers to
non-blocking mode later.

🧠 **Concept note:** *Blocking I/O* means the call parks the current thread
until data is ready. `receiveExact(n)` will therefore wait for all `n` bytes or
return an error if the peer disconnects. That predictability keeps the protocol
code simple.

### 2.5 Build checkpoint

🛠️ **Checkpoint:** After adding `TcpSocket` run
`cmake --build build --target moonlapse_shared`. The target is header-only, but
this quick build recompiles everything that includes `network.hpp` and ensures
we did not miss an include or introduce a syntax error before wiring the
listener.

On failure every function returns `std::unexpected(SocketError{...})`. The
server’s accept loop logs `error.message`, and the client UI stores the string
to show the user after curses shuts down.

---

## 3. `TcpListener`: RAII for `bind`/`listen`

Servers construct a listener, bind it to an address, and accept clients. `TcpListener` mirrors that lifecycle.

```cpp
class TcpListener {
public:
  [[nodiscard]] static auto bind(std::string_view host, std::uint16_t port)
      -> SocketResult<TcpListener>;
  [[nodiscard]] auto listen(int backlog = SOMAXCONN) const
      -> SocketResult<void>;
  [[nodiscard]] auto accept() const -> SocketResult<TcpSocket>;

  void close() noexcept;
  [[nodiscard]] auto isOpen() const noexcept -> bool;
};
```

Design notes:

- `bind()` runs `ensureSocketLibrary()`, resolves the host/port, enables `SO_REUSEADDR`, and returns a listener that owns the native handle.
- `listen()` fails fast if you call it on a closed listener.
- `accept()` wraps the platform call and returns a new `TcpSocket` already in RAII form.

> **Ownership reminder:** Just like `TcpSocket`, the listener’s destructor calls
> `close()`, so the operating system port is released the moment the object
> leaves scope. No separate `defer` or `finally` block is required.

🧠 **Concept note:** `SOMAXCONN` is the OS-defined upper bound for the pending
connection queue. Passing it through preserves the platform default while still
allowing explicit tuning later.

This separation keeps the server’s `main()` readable:

```cpp
auto listenerResult = TcpListener::bind("0.0.0.0", 40500);
if (!listenerResult) {
  std::println("[server] bind failed: {}", listenerResult.error().message);
  return 1;
}

auto listenerInstance = std::move(listenerResult.value());
if (auto listenResult = listenerInstance.listen(); !listenResult) {
  std::println("[server] listen failed: {}", listenResult.error().message);
  return 1;
}
```

No manual cleanup is required—the RAII wrapper closes the socket on scope exit even if an early `return` fires.

🛠️ **Checkpoint:** Build just the server now with
`cmake --build build --target moonlapse_server`. If it fails, double-check the
new include order in `server/main.cpp` and ensure you added `#include
"network.hpp"` from the shared module.

---

## 4. Wiring sockets into the server loop

Before the full game loop, sanity-check the wrappers with a minimal echo
server. It accepts a single client, reads a header, and sends it straight back:

```cpp
auto listenerResult = TcpListener::bind("127.0.0.1", 40500);
if (!listenerResult) {
  throw std::runtime_error(listenerResult.error().message);
}
auto listener = std::move(listenerResult.value());
listener.listen().value();
while (true) {
  auto connection = listener.accept();
  if (!connection) {
    std::println("[demo] accept failed: {}", connection.error().message);
    continue;
  }
  auto socket = std::move(connection.value());
  auto bytes = socket.receiveExact(Protocol::packetHeaderSize);
  if (!bytes) {
    std::println("[demo] read failed: {}", bytes.error().message);
    continue;
  }
  socket.sendAll(std::span<const std::byte>{bytes.value()}).value();
}
```

The pattern is the same everywhere: call a helper, check the
`std::expected`, and only use `.value()` once you are sure the call succeeded.

🧪 **Try it:** Drop that loop into a scratch executable under `server/tests/`
if you want to watch `netcat` bounce packets back. It exercises both
`receiveExact` and `sendAll` before we add concurrency.

With the guardrails proven, the production `GameServer` thread-spawns
`handleClient()` for every accepted connection:

1. Receive a fixed-size header with `receiveExact(packetHeaderSize)`.
2. Decode it using the §01 protocol helpers.
3. Allocate and receive the payload with `receiveExact(payloadSize)`.
4. Visit the decoded variant and dispatch to movement, chat, or snapshot handlers.

Every step checks the returned `SocketResult`. An error triggers a break,
closing the socket and removing the player from the registry.

`TcpSocket` itself does not implement internal locking—simultaneous `sendAll`
calls from multiple threads would race on the underlying handle and interleave
payloads. POSIX technically allows concurrent writes, but games want packet
boundaries preserved, so we guard the send path with a `std::mutex`. The server
wraps the socket inside a `Session` struct for that reason, while the global
player table is protected by another mutex.

🧠 **Concept note:** `std::scoped_lock` acquires every mutex passed to it and
releases them automatically on scope exit. It is the multi-mutex, RAII-friendly
successor to `std::lock_guard`.

When broadcasting, we re-use the protocol encoder and the session wrapper:

```cpp
void broadcastChat(const Protocol::ChatPacket& chat) {
  auto encoded = Protocol::encode(chat);
  auto recipients = snapshotSessions();

  for (const auto& recipient : recipients) {
    if (!recipient) {
      continue;
    }
    if (auto result = recipient->send(std::span<const std::byte>{encoded});
        !result) {
      std::println("[server] chat broadcast failed for player {}: {}",
                   recipient->playerId, result.error().message);
      removePlayer(recipient->playerId);
    }
  }
}
```

Every failure becomes a structured `SocketError` with context. No raw errno checks clutter the gameplay code.

`snapshotSessions()` copies the shared pointers while holding the player mutex
and releases the lock before sending, so long broadcasts never block new
movement updates from being processed.

🧠 **Concept note:** `std::visit` is the C++ equivalent of pattern-matching on a
`std::variant`. The server builds an `Overloaded` helper (see `server/main.cpp`)
so each lambda handles one packet alternative. Missing a case becomes a compile
error the next time we extend `PacketVariant`.

The receive loop also showcases idiomatic error propagation: `if (!result)
return std::unexpected(result.error());` mirrors Go’s `if err != nil { return
err }` and keeps failure paths explicit without exceptions.

---

## 5. Feeding the client from a receiver thread

The client mirrors the server’s read loop inside `receiverLoop` (a `std::jthread`):

```cpp
void receiverLoop(const std::shared_ptr<TcpSocket>& socket, ClientState& state,
                  std::atomic_bool& running,
                  std::atomic_bool& connectionActive,
                  std::mutex& errorMutex, std::string& lastError) {
  while (running.load()) {
    auto headerBytes = socket->receiveExact(Protocol::packetHeaderSize);
    if (!headerBytes) {
      std::scoped_lock guard{errorMutex};
      lastError = headerBytes.error().message;
      connectionActive.store(false);
      running.store(false);
      return;
    }
    // ... decode header, receive payload, std::visit the packet variant ...
  }
}
```

🧠 **Concept note:** `std::jthread` automatically joins in its destructor and
exposes a `std::stop_token`. You no longer need to remember to call `join()` in
every exit path—the object handles cooperative shutdown for you.

Because `TcpSocket` must stay alive while both the UI loop and the receiver
loop run, we wrap it in `std::shared_ptr`. Neither thread is the clear “owner,”
so shared ownership ensures the socket only closes once both references drop
out of scope. We still guard the actual send path with a mutex; shared pointers
manage lifetime, not thread safety.

> **Analogy:** In Go you might pass a pointer to a goroutine and trust the GC to
> keep it alive. In C++ we explicitly model shared lifetime with
> `std::shared_ptr`, and the runtime deletes the socket as soon as the last
> reference disappears.

The receiver thread stops gracefully by observing an atomic `running` flag and
by calling `receiver.request_stop()` when the main loop exits. Any error gets
stored in `lastError` under a mutex, then shown in the UI after curses shuts
down.

On exit, the client shuts down and closes the socket explicitly. The destructor would run anyway, but calling `shutdown()` tells the peer that no more data will be sent, reducing the chance of lingering half-closed connections.

---

## 6. Handling errors without exceptions

The networking layer never throws. Every public method returns `SocketResult<T>`. Higher layers translate these into user-facing logs or UI messages.

- Server: `std::println("[server] accept failed: {}", error.message);`
- Client: stash the message in `lastError` and display it after curses tears down.

`SocketError` always carries three fields: a domain-specific `SocketErrorCode`, a human-readable message, and the original `std::error_code` for deeper inspection or logging. That keeps debugging straightforward—especially useful when a failure happens on Windows but not on Linux.

> **Why not just `std::error_code`?** The extra enum lets higher layers branch
> on semantic states (e.g., `ConnectionClosed` vs `WouldBlock`) without parsing
> strings, while the embedded `std::error_code` preserves the OS-specific
> details for logs.

---

## 7. Checklist before moving on

- [x] `shared/network.hpp` exposes `TcpSocket` and `TcpListener` as move-only RAII wrappers.
- [x] All networking functions return `SocketResult<T>` with rich error information.
- [x] Client and server use `receiveExact()` and `sendAll()` to avoid partial-transfer bugs.
- [x] Server sessions guard outgoing sends with a mutex; client guards chat/movement sends similarly.
- [x] Threads use `std::jthread` and atomic flags for cooperative shutdown.

With packets and sockets in place the pipeline is complete: the server accepts clients, decodes movement commands, and broadcasts consistent snapshots; the client renders the shared state and relays chat. In §03 we will turn to the terminal UI and input handling: rendering the grid, managing chat focus, and keeping the frame loop responsive without wasting CPU.
