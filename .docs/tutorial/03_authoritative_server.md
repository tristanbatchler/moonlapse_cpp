# §03 Orchestrating the Authoritative Server

With packets defined (§01) and sockets wrapped in RAII (§02), we can now *run the game itself*. This chapter turns those primitives into the living world loop on the server—accepting clients, mutating state, and broadcasting authoritative snapshots. We will unpack `server/main.cpp` with three pillars:

1. accepting clients and assigning sessions,
2. mutating the shared world in response to movement/chat packets,
3. broadcasting authoritative snapshots back to every player.

The server enforces the rules of the room—clients operate as thin terminals that render whatever the server says. Keeping the server authoritative prevents desyncs and cheats.

---

## Concurrency toolkit refresher

We lean on a small set of modern C++23 concurrency primitives. If you come from
Go or Python, here is the translation guide before diving in:

- **`std::shared_ptr<T>`** – reference-counted shared ownership. Think of it as
  an `Arc<T>` (Rust) or a ref-counted pointer; the object is destroyed when the
  last owner drops it. We use it when two threads need to share a session safely.
- **`std::mutex` + `std::scoped_lock`** – mutual exclusion. `std::scoped_lock`
  acquires the mutex in its constructor and releases it in its destructor, so we
  cannot forget to unlock—even if an exception or early return occurs.
- **`std::atomic<T>`** – a data race free integer. Calling `fetch_add(1)` is the
  C++ equivalent of `atomic.AddInt32(&value, 1)` in Go; every thread sees a
  unique player identifier without explicit locking.
- **`std::jthread`** – RAII-aware thread. Its destructor automatically joins or
  requests stop, eliminating the “forgot to join” crashes that plagued
  `std::thread`.
- **`std::variant` + `std::visit`** – sum types with exhaustive visitors.
  Combining them with the `Overloaded` helper lets us route packets to the
  correct handler with compile-time checking.

🧠 **Concept note:** We stick with blocking sockets plus a thread-per-connection
model because it mirrors the platform-neutral abstractions from §02 and keeps
the code straightforward. Later chapters can swap in event loops or coroutine
based IO without touching the gameplay logic.

---

## 1. High-level structure

`server/main.cpp` starts by importing the shared protocol and networking headers, then spins up a `GameServer` object.

```cpp
using Moonlapse::Net::TcpListener;
using Moonlapse::Net::TcpSocket;
namespace Protocol = Moonlapse::Protocol;

constexpr std::uint16_t serverPort = 40500;

auto main() -> int {
  constexpr std::string_view listenAddress = "0.0.0.0";
  auto listenerResult = TcpListener::bind(listenAddress, serverPort);
  if (!listenerResult) {
    std::println("[server] bind failed: {}", listenerResult.error().message);
    return 1;
  }

  auto listenerInstance = std::move(listenerResult.value());
  if (auto listenResult = listenerInstance.listen(); !listenResult) {
    std::println("[server] listen failed: {}", listenResult.error().message);
    return 1;
  }

  std::println("[server] listening on {}:{}", listenAddress, serverPort);
  GameServer server{std::move(listenerInstance)};
  server.run();
  return 0;
}
```

Everything interesting happens inside `GameServer`.

- `TcpListener::bind()` and `listen()` come straight from §02.
- We log and exit cleanly if the port is unavailable.
- Once the listener is ready, `GameServer::run()` enters an accept loop.

🧠 **Concept note:** `constexpr std::string_view listenAddress` bakes the
address into the binary at compile time. It behaves like a global constant in
Go or a module-level constant in Python—cheap to use and impossible to mutate
accidentally.

---

## 2. Session lifecycle

Each connected player receives a `Session` object: a shared wrapper around a `TcpSocket` plus metadata. Sessions live inside `std::shared_ptr` because the accept loop, worker threads, and snapshot broadcasts all need to hold references.

🧠 **Concept note:** `std::shared_ptr` keeps a reference count next to the
object. Copying the pointer increments the count; destroying it decrements the
count and deletes the session when it hits zero. This gives us garbage-collected
style ownership semantics with deterministic destruction.

```cpp
class GameServer {
public:
  explicit GameServer(TcpListener listener) noexcept
      : listener{std::move(listener)} {}

  void run();

private:
  struct Session {
    Session(Protocol::PlayerId playerIdentifier, TcpSocket&& socket) noexcept
        : playerId{playerIdentifier}, socket{std::move(socket)} {}

    auto send(std::span<const std::byte> payload) -> SocketResult<void> {
      std::scoped_lock guard{sendMutex};
      return socket.sendAll(payload);
    }

    Protocol::PlayerId playerId;
    TcpSocket socket;
    std::mutex sendMutex;
  };

  // std::scoped_lock enters the critical section in its constructor and
  // releases it automatically when the guard goes out of scope.

  struct PlayerEntry {
    Protocol::Position position;
    std::shared_ptr<Session> session;
  };

  TcpListener listener;
  std::atomic<Protocol::PlayerId> nextId{1};
  mutable std::mutex playersMutex;
  std::unordered_map<Protocol::PlayerId, PlayerEntry> players;
  std::vector<std::jthread> workers;
};
```

Key choices:

- `Session::send()` guards the socket with a mutex because multiple threads (movement broadcasts, chat broadcasts) may write simultaneously.
- Worker threads live in a `std::vector<std::jthread>`. Thanks to RAII, the destructor joins them automatically when the server shuts down.
- `mutable std::mutex playersMutex;` lets const-qualified helpers lock the map
  even though they do not mutate observable state. It is similar to marking a
  field as interiorly mutable in Rust.

🧠 **Concept note:** `std::jthread` fixes the classic `std::thread` footgun:
destroying an unjoined thread calls `std::terminate()`. With `std::jthread`,
the destructor joins (or stop-requests) automatically, so even error paths shut
down cleanly.

🧠 **Concept note:** `std::make_shared<Session>(...)` allocates the control block
and the session in one step. It is exception-safe and avoids the double
allocation that `std::shared_ptr<Session>{new Session(...)}` would incur.

### 2.1 Accepting clients

`GameServer::run()` loops forever, calling `listener.accept()` for each connection. The accept loop stays on the main thread; each client runs on its own worker thread. This keeps networking responsive while the world map and player registry remain synchronised behind a single mutex. On success, we create a new session and hand it off to `registerPlayer()`.

```cpp
void GameServer::run() {
  std::println("[server] waiting for players...");
  while (true) {
    auto connection = listener.accept();
    if (!connection) {
      std::println("[server] accept failed: {}", connection.error().message);
      continue;
    }
    registerPlayer(std::move(connection.value()));
  }
}
```

### 2.2 Registering a player

`registerPlayer()` assigns a fresh identifier, chooses a spawn point, records the session, and sends the initial snapshot.

```cpp
void GameServer::registerPlayer(TcpSocket socket) {
  auto playerIdentifier = nextId.fetch_add(1);
  auto session =
      std::make_shared<Session>(playerIdentifier, std::move(socket));
  auto position = spawnPosition(playerIdentifier);

  {
    std::scoped_lock guard{playersMutex};
    players.emplace(playerIdentifier,
                    PlayerEntry{.position = position, .session = session});
  }

  std::println("[server] player {} connected at ({}, {})", playerIdentifier,
               position.x, position.y);

  if (auto snapshotResult = sendSnapshot(session, playerIdentifier);
      !snapshotResult) {
    std::println("[server] failed to initialize player {}: {}",
                 playerIdentifier, snapshotResult.error().message);
    session->socket.shutdown();
    session->socket.close();
    removePlayer(playerIdentifier);
    return;
  }

  broadcastState();

  workers.emplace_back([this, session]() { handleClient(session); });
}
```

Steps in order:

1. **Allocate ID:** `nextId` is an `std::atomic`. Each new player increments it without data races.
2. **Spawn position:** `spawnPosition()` spreads players across the grid without collisions by deriving coordinates from the ID.
3. **Store session:** we grab `playersMutex`, insert state, and release the lock quickly.
4. **Send initial snapshot:** the new player learns its ID and the current world.
5. **Notify everyone else:** `broadcastState()` shares the updated roster.
6. **Launch worker thread:** `handleClient()` runs in a dedicated `std::jthread`.

If any step fails—say, a socket write error—the session tears down cleanly while the rest of the server keeps running.

🧠 **Concept note:** `fetch_add(1)` on `std::atomic` is equivalent to Go’s
`atomic.AddUint32`. The operation is indivisible, so two threads can never hand
out the same `PlayerId` even if they accept connections at the same moment.

🧠 **Concept note:** `workers.emplace_back(...)` constructs the `std::jthread`
in-place inside the vector, avoiding an extra move. It mirrors Rust’s
`vec.push(Thread::new(...))` but without temporary objects.

```cpp
[[nodiscard]] auto GameServer::spawnPosition(Protocol::PlayerId playerIdentifier)
    -> Protocol::Position {
  auto index = static_cast<std::int32_t>(playerIdentifier - 1);
  auto x = index % gridWidth;
  auto y = (index / gridWidth) % gridHeight;
  return Protocol::Position{x, y};
}
```

🧠 **Concept note:** The modulo arithmetic walks the grid row by row, ensuring
every new player spawns on a distinct tile until the grid fills. The helper is
pure math—no locks required—which keeps registration fast.

### 2.3 Threading model overview

```
Main thread:
  listener.accept() → registerPlayer()

Worker thread (per player):
  handleClient() → receive loop → handleMovement()/handleChat()
```

The `std::jthread` pool guarantees each worker joins automatically when the server shuts down, so we never leak threads.

⚖️ **Design trade-off:** A thread per connection keeps the walkthrough
approachable and works well for dozens of players. At larger scales you would
switch to an event loop (epoll/kqueue) or a pool of worker threads consuming a
queue of socket jobs. The rest of the architecture—sessions, protocol decoding,
and broadcast helpers—remains valid if you later adopt those models.

---

## 3. Handling client packets

`handleClient()` performs the canonical read loop:

```cpp
void GameServer::handleClient(const std::shared_ptr<Session>& session) {
  while (true) {
    auto headerBytes =
        session->socket.receiveExact(Protocol::packetHeaderSize);
    if (!headerBytes) {
      logSocketError("receive header", session->playerId,
                     headerBytes.error());
      break;
    }

    auto headerResult =
        Protocol::decodeHeader(std::span<const std::byte>{headerBytes.value()});
    if (!headerResult) {
      std::println("[server] packet header error for player {}: {}",
                   session->playerId,
                   describePacketError(headerResult.error()));
      break;
    }

    auto payloadBytes =
        session->socket.receiveExact(headerResult->payloadSize);
    if (!payloadBytes) {
      logSocketError("receive payload", session->playerId,
                     payloadBytes.error());
      break;
    }

    auto packetResult = Protocol::decodePacket(
        *headerResult, std::span<const std::byte>{payloadBytes.value()});
    if (!packetResult) {
      std::println("[server] packet decode error for player {}: {}",
                   session->playerId,
                   describePacketError(packetResult.error()));
      break;
    }

    std::visit(Overloaded{
                  [&](const Protocol::MovementPacket& movement) {
                    handleMovement(session, movement);
                  },
                  [](const Protocol::StateSnapshotPacket&) {
                    // Clients never send snapshots back.
                  },
                  [&](const Protocol::ChatPacket& chat) {
                    handleChat(session, chat);
                  }},
               *packetResult);
  }

  session->socket.shutdown();
  session->socket.close();
  removePlayer(session->playerId);
  broadcastState();
  std::println("[server] player {} disconnected", session->playerId);
}
```

Highlights:

- `receiveExact()` and `decodeHeader()` come from earlier chapters.
- Each failure path logs context and breaks the loop; cleanup happens after the loop.
- `Overloaded` is the standard helper pattern for visiting `std::variant` with lambdas.

```cpp
template <typename... Handlers> struct Overloaded : Handlers... {
  using Handlers::operator()...;
};

template <typename... Handlers> Overloaded(Handlers...)
    -> Overloaded<Handlers...>;
```

🧠 **Concept note:** The helper inherits from each lambda and exposes all
`operator()` overloads at once. Passing `Overloaded{...}` into `std::visit`
mimics pattern-matching: the compiler checks that we handle every variant
alternative.

### 3.1 Movement handling

Movement packets reflect the client’s arrow-key input, but the server revalidates every command, clamps movement to the grid, and only trusts packets from the owning player.

```cpp
void GameServer::handleMovement(const std::shared_ptr<Session>& session,
                                const Protocol::MovementPacket& movement) {
  if (movement.player != session->playerId) {
    std::println("[server] ignoring spoofed movement for player {}",
                 session->playerId);
    return;
  }

  bool updated = false;
  {
    std::scoped_lock guard{playersMutex};
    auto entry = players.find(session->playerId);
    if (entry == players.end()) {
      return;
    }
    auto& position = entry->second.position;
    Protocol::Position previous = position;
    applyMovement(position, movement.direction);
    updated = previous.x != position.x || previous.y != position.y;
  }

  if (updated) {
    broadcastState();
  }
}
```

- Validation happens first—anything spoofed gets logged and ignored.
- We take the lock, adjust the player’s position via `applyMovement()`, and remember if anything changed.
- When the character moves, we broadcast a fresh snapshot to everyone.

Movement uses helper functions:

```cpp
[[nodiscard]] auto clampCoordinate(std::int32_t value, std::int32_t ceiling)
    -> std::int32_t {
  if (value < 0) {
    return 0;
  }
  if (value >= ceiling) {
    return ceiling - 1;
  }
  return value;
}

void applyMovement(Protocol::Position& position,
                   Protocol::Direction direction) {
  switch (direction) {
  case Protocol::Direction::Up:
    --position.y;
    break;
  case Protocol::Direction::Down:
    ++position.y;
    break;
  case Protocol::Direction::Left:
    --position.x;
    break;
  case Protocol::Direction::Right:
    ++position.x;
    break;
  }

  position.x = clampCoordinate(position.x, gridWidth);
  position.y = clampCoordinate(position.y, gridHeight);
}
```

These helpers keep the main handler tidy and express grid rules in one place.

🧠 **Concept note:** `gridWidth` and `gridHeight` are compile-time constants at
the top of `server/main.cpp`. Editing them instantly changes the playfield size
for every new session.

### 3.2 Chat handling

Chat packets carry `{player, message}`. The server verifies the sender and rebroadcasts.

```cpp
void GameServer::handleChat(const std::shared_ptr<Session>& session,
                            const Protocol::ChatPacket& chat) {
  if (chat.player != session->playerId) {
    std::println("[server] ignoring spoofed chat for player {}",
                 session->playerId);
    return;
  }

  broadcastChat(chat);
}
```

Chat reuses the same send guard as movement broadcasts. Every packet travels through `Protocol::encode(chat)` before hitting the wire.

---

## 4. Broadcasting authoritative state

Two broadcasting helpers keep the main logic compact:

```cpp
void GameServer::broadcastState(Protocol::PlayerId focus = 0) {
  auto snapshot = gatherSnapshot(focus);
  auto encoded = Protocol::encode(snapshot);
  auto recipients = snapshotSessions();

  for (const auto& recipient : recipients) {
    if (!recipient) {
      continue;
    }
    if (auto result = recipient->send(std::span<const std::byte>{encoded});
        !result) {
      std::println("[server] broadcast failed for player {}: {}",
                   recipient->playerId, result.error().message);
      removePlayer(recipient->playerId);
    }
  }
}

void GameServer::broadcastChat(const Protocol::ChatPacket& chat) {
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

> **Future tweak:** Both broadcast functions follow the same template by
> design—encode, snapshot sessions, iterate. As the game grows you can extract a
> generic helper or introduce delta compression so idle players receive fewer
> bytes. For now the duplication keeps the walkthrough explicit.

Supporting helpers gather state safely:

```cpp
[[nodiscard]] auto GameServer::gatherSnapshot(Protocol::PlayerId focus) const
    -> Protocol::StateSnapshotPacket {
  Protocol::StateSnapshotPacket snapshot{};
  snapshot.focusPlayer = focus;

  std::scoped_lock guard{playersMutex};
  snapshot.players.reserve(players.size());
  for (const auto& [playerIdentifier, entry] : players) {
    snapshot.players.emplace_back(
        Protocol::PlayerState{playerIdentifier, entry.position});
  }
  return snapshot;
}

[[nodiscard]] auto GameServer::snapshotSessions() const
    -> std::vector<std::shared_ptr<Session>> {
  std::vector<std::shared_ptr<Session>> sessions;
  std::scoped_lock guard{playersMutex};
  sessions.reserve(players.size());
  for (const auto& [playerIdentifier, entry] : players) {
    sessions.push_back(entry.session);
  }
  return sessions;
}
```

Design notes:

- We copy `shared_ptr` instances out of the map while holding the lock, then release the lock before performing network I/O. This avoids holding the mutex during potentially slow `send()` calls.
- The optional `focus` argument carries the joining player’s ID so the client can highlight itself.
- If a send fails, we log and call `removePlayer()`.

🧠 **Concept note:** Never hold a mutex while performing blocking I/O. The
`snapshotSessions()` helper minimises lock duration: we grab the map, copy the
`std::shared_ptr` handles, release the lock, and only then call `sendAll`. If a
client stalls, the rest of the world keeps moving.

### 4.1 Removing players safely

```cpp
void GameServer::removePlayer(Protocol::PlayerId playerIdentifier) {
  std::shared_ptr<Session> removed;
  {
    std::scoped_lock guard{playersMutex};
    auto entry = players.find(playerIdentifier);
    if (entry == players.end()) {
      return;
    }
    removed = std::move(entry->second.session);
    players.erase(entry);
  }

  if (removed) {
    removed->socket.shutdown();
    removed->socket.close();
  }
}
```

We grab the session pointer while locked, erase the map entry, then release the lock before shutting down the socket. This prevents deadlocks caused by calling into networking code while holding `playersMutex`.

🧠 **Concept note:** Returning the `std::shared_ptr` outside the lock guarantees
the session object stays alive until cleanup finishes—even if other threads drop
their copies during removal. Once every reference disappears, the RAII
destructor runs and closes the socket one final time.

---

## 5. Error logging and diagnostics

The server wraps socket errors in a helper to keep logs consistent:

```cpp
static void GameServer::logSocketError(std::string_view action,
                                       Protocol::PlayerId playerIdentifier,
                                       const SocketError& error) {
  if (error.code == SocketErrorCode::ConnectionClosed) {
    std::println("[server] player {} closed the connection",
                 playerIdentifier);
    return;
  }

  std::println("[server] {} failed for player {}: {}", action,
               playerIdentifier, error.message);
}
```

`describePacketError()` translates `Protocol::PacketError` codes into strings, mirroring the client’s UX. If decoding fails due to version mismatch or malformed payloads, we can see it instantly in the logs.

---

## 6. Try it now

🛠️ **Build checkpoint:**

```bash
cmake --build build --target moonlapse_server
```

Then launch the server from one terminal:

```bash
./build/server/moonlapse_server
```

From a second terminal run the `moonlapse_client` binary (you will build it in
§04; if you already compiled it, re-use that executable). Watch the server log
as you connect, move, chat, and disconnect.

📋 **Sample log**

```
[server] listening on 0.0.0.0:40500
[server] waiting for players...
[server] player 1 connected at (0, 0)
[server] player 1 disconnected
```

Seeing the connect/disconnect messages proves the accept loop, session
registration, and cleanup path are wired correctly.

---

## 7. Checklist

- [x] `GameServer` owns the listener, player map, and worker threads.
- [x] Each player session guards its outgoing socket with a mutex.
- [x] Movement and chat handlers validate the sender before mutating state.
- [x] Snapshots broadcast the authoritative world after every change.
- [x] Player removal shuts down sockets outside the mutex to avoid deadlocks.
- [x] Logging distinguishes between clean disconnects and unexpected errors.

With the server orchestrating the world, every movement and chat line flows through a single authority.

```
[Client A] →
             \
              [GameServer] ↔ (Authoritative State)
             /
[Client B] →
```

In §04 we will switch perspectives to the client: rendering the grid with curses, managing chat focus, and coordinating the receiver thread with the input loop.
