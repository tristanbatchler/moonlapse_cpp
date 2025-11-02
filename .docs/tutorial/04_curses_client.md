# §04 Crafting the Curses Client Frontend

With the server orchestrating the world (§03) we now turn to the terminal client. This chapter shows how `client/main.cpp` knits together three threads of work:

1. a curses front-end that renders the grid and chat log,
2. an input loop that captures movement and chat commands,
3. a receiver thread that hydrates local state from authoritative snapshots.

Everything runs on top of the shared protocol (§01) and networking layer (§02), so the client stays thin—no business logic, just presentation plus packet plumbing.

---

## 1. Bootstrapping the client runtime

We start by connecting to the server, bringing up curses safely, and initialising the shared state objects that threads will share.


```
Main thread (UI & input)
  ├─ polls getch(), sends packets, draws frames
Receiver thread (std::jthread)
  └─ blocks on receiveExact(), updates ClientState
```

```cpp
constexpr std::string_view serverAddress = "127.0.0.1";
constexpr std::uint16_t serverPort = 40500;

auto main() -> int {
  auto socketResult = TcpSocket::connect(serverAddress, serverPort);
  if (!socketResult) {
    std::println("[client] failed to connect: {}",
                 socketResult.error().message);
    return 1;
  }

  auto connection =
      std::make_shared<TcpSocket>(std::move(socketResult.value()));
  std::string lastError;

  🧠 **Concept note:** The shared objects fall into four distinct roles. The
  `std::shared_ptr<TcpSocket>` keeps the connection alive while both the input
  loop and receiver thread use it. `ClientState` (protected by its internal mutex)
  stores gameplay data that both threads read and write. Two
  `std::atomic_bool` flags provide lock-free coordination for cooperative
  shutdown, and `errorMutex` guards `lastError`, the string we surface to the
  user after curses shuts down.

  {
    CursesSession curses;
    if (!curses.active) {
      std::println("[client] failed to initialize terminal UI");
      return 1;
    }

    ClientState state;
    std::atomic_bool running{true};
    std::atomic_bool connectionActive{true};
    std::mutex errorMutex;
    std::mutex sendMutex;

    std::jthread receiver(receiverLoop, connection, std::ref(state),
                          std::ref(running), std::ref(connectionActive),
                          std::ref(errorMutex), std::ref(lastError));

    ChatUiState chatState;
    RuntimeContext runtime{sendMutex, errorMutex, lastError, running,
                           connectionActive};

    while (running.load()) {
      if (!connectionActive.load()) {
        break;
      }

      auto inputKey = getch();
      if (handleInputKey(inputKey, chatState, connection, state, runtime) ==
          LoopAction::Stop) {
        break;
      }

      auto renderState = gatherRenderState(state);
      drawFrame(renderState.snapshot, renderState.selfId,
                std::span<const ChatEntry>{renderState.chatMessages},
                chatState);
      std::this_thread::sleep_for(refreshDelay);
    }

    running.store(false);
    connectionActive.store(false);
    receiver.request_stop();
    receiver.join();

    connection->shutdown();
    connection->close();
  }

  if (!lastError.empty()) {
    std::println("[client] {}", lastError);
  }

  return 0;
}
```

Highlights:

- `TcpSocket::connect()` and the RAII shutdown mirror the server patterns.
- `CursesSession` guards terminal state; leaving the scope tears down curses cleanly.
- Shared state lives in `ClientState`, guarded by a mutex.
- Atomic flags (`running`, `connectionActive`) coordinate the input loop with the receiver thread.
- `RuntimeContext` bundles references so helpers can log errors and trigger shutdowns without globals.
- `std::this_thread::sleep_for(refreshDelay)` yields the UI thread so we do not
  busy-spin when no input arrives.

🧠 **Concept note:** The shared objects fall into four distinct roles. The
`std::shared_ptr<TcpSocket>` keeps the connection alive while both the input
loop and receiver thread use it. `ClientState` (protected by its internal mutex)
stores gameplay data that both threads read and write. Two
`std::atomic_bool` flags provide lock-free coordination for cooperative

                🧠 **Concept note:** Curses exposes a process-wide singleton. Limiting
                `CursesSession` to a single, non-movable owner guarantees `endwin()` runs at
                most once—even if an exception or early `return` occurs.
shutdown, and `errorMutex` guards `lastError`, the string we surface to the
user after curses shuts down.

🧠 **Concept note:** `std::ref(...)` wraps arguments in a reference wrapper so
the `std::jthread` constructor passes the original object to `receiverLoop`
instead of copying it. Without `std::ref`, the thread would see its own private
copy of `ClientState`, `running`, and `lastError`, and changes would never make
it back to the main loop.

---

## 2. The curses session as RAII

Curses requires matching `initscr()`/`endwin()` calls. We wrap that requirement in a move-proof RAII struct so the rest of the code can treat curses like any other resource.

```cpp
struct CursesSession {
  CursesSession() : window{initscr()} {
    if (window == nullptr) {
      return;
    }
    active = true;
    cbreak();
    noecho();
    keypad(window, true);
    nodelay(window, true);
    curs_set(0);
  }

  ~CursesSession() {
    if (active) {
      endwin();
    }
  }

  CursesSession(const CursesSession&) = delete;
  auto operator=(const CursesSession&) -> CursesSession& = delete;
  CursesSession(CursesSession&&) = delete;
  auto operator=(CursesSession&&) -> CursesSession& = delete;

  WINDOW* window{};
  bool active{false};
};
```

The constructor configures a non-blocking UI (`nodelay`) and disables echo so we can manage the cursor ourselves. The type is intentionally non-movable; owning the curses session twice would lead to double-shutdown bugs.

---

## 3. Local state and render pipeline

`ClientState` stores the authoritative snapshot as last received plus chat history. Rendering extracts immutable copies so we never hold the mutex during drawing.

```cpp
struct ChatEntry {
  Protocol::PlayerId player{};
  std::string message;
};

struct ChatUiState {
  bool active{false};
  std::string input;
};

struct ClientState {
  std::unordered_map<Protocol::PlayerId, Protocol::Position> players;
  std::optional<Protocol::PlayerId> selfId;
  std::deque<ChatEntry> chatLog;
  mutable std::mutex mutex;
};

struct RenderState {
  std::vector<Protocol::PlayerState> snapshot;
  std::vector<ChatEntry> chatMessages;
  std::optional<Protocol::PlayerId> selfId;
};

🧠 **Concept note:** `std::optional` gives us an explicit “maybe” value without
resorting to sentinel IDs like `0` or `-1`. When `selfId` lacks a value, the
client simply has not received its focus packet yet.

🧠 **Concept note:** We keep chat history in `std::deque` so pushing new entries
and trimming old ones stay `O(1)` at both ends. Snapshots copy into
`std::vector` because we iterate them linearly when drawing the grid.
```

`gatherRenderState()` copies the data out under lock:

```cpp
auto gatherRenderState(ClientState& state) -> RenderState {
  RenderState render;
  std::scoped_lock guard{state.mutex};

  render.snapshot.reserve(state.players.size());
  for (const auto& [playerIdentifier, position] : state.players) {
    render.snapshot.emplace_back(Protocol::PlayerState{
        .player = playerIdentifier, .position = position});
  }

  render.chatMessages.reserve(state.chatLog.size());
  for (const auto& entry : state.chatLog) {
    render.chatMessages.push_back(entry);
  }

  render.selfId = state.selfId;
  return render;
}

🧠 **Concept note:** The render pipeline follows the *lock, copy, release*
pattern. We hold the mutex only long enough to snapshot the data, then draw from
immutable copies. Slow terminal I/O can never stall the receiver thread that
keeps `ClientState` fresh.
```

### 3.1 Drawing the grid

The draw helpers translate `RenderState` into curses calls. They are pure functions with no shared state, making them easy to reason about.

```cpp
void drawBorder() {
  for (int row = 0; row <= gridHeight + 1; ++row) {
    for (int column = 0; column <= gridWidth + 1; ++column) {
      const bool borderCell = row == 0 || row == gridHeight + 1 ||
                              column == 0 || column == gridWidth + 1;
      const auto glyph = static_cast<unsigned char>(borderCell ? '#' : ' ');
      mvaddch(row, column, static_cast<chtype>(glyph));
    }
  }
}
```

`drawPlayers()` highlights the local player with `@` and renders others as `o`. The function clamps coordinates defensively in case future gameplay tweaks move players off-grid.

```cpp
void drawPlayers(const std::vector<Protocol::PlayerState>& snapshot,
                 std::optional<Protocol::PlayerId> selfId) {
  for (const auto& player : snapshot) {
    auto positionRow = static_cast<int>(player.position.y) + 1;
    auto positionColumn = static_cast<int>(player.position.x) + 1;
    if (positionColumn < 1 || positionColumn > gridWidth || positionRow < 1 ||
        positionRow > gridHeight) {
      continue;
    }
    const bool isSelf = selfId.has_value() && player.player == *selfId;
    const auto glyph = static_cast<unsigned char>(isSelf ? '@' : 'o');
    mvaddch(positionRow, positionColumn, static_cast<chtype>(glyph));
  }
}
```

The rest of the UI is simple text: controls hint, player ID, chat log, and an input prompt with a trailing underscore when the chat box has focus. We assemble it all inside `drawFrame()`:

```cpp
void drawFrame(const std::vector<Protocol::PlayerState>& snapshot,
               std::optional<Protocol::PlayerId> selfId,
               std::span<const ChatEntry> chatMessages,
               const ChatUiState& chatUi) {
  erase();
  drawBorder();
  drawPlayers(snapshot, selfId);
  auto nextRow = drawInfoLines(selfId, chatUi);
  nextRow = drawChatLog(nextRow, chatMessages);
  drawChatPrompt(nextRow, chatUi);
  refresh();
}
```

Because curses is immediate-mode, we redraw the whole frame each tick. The `refreshDelay` constant (50 ms) throttles the loop to keep CPU usage reasonable.

🧠 **Concept note:** Immediate-mode means the screen is just another buffer.
Every frame we paint the full scene, then `refresh()` flushes it. All of the
timing constants—`gridWidth`, `gridHeight`, `refreshDelay`, and key codes like
`escapeKeyCode`—are defined near the top of `client/main.cpp` so tweaking the UI
never requires hunting through the tutorial.

> The helper functions referenced here—`drawInfoLines`, `drawChatLog`,
> `drawChatPrompt`, and the key constants—live alongside the snippet in
> `client/main.cpp`. They are thin wrappers over curses primitives in the same
> style as `drawBorder()`.

---

## 4. Input handling and packet sends

Input is split into two modes: movement (default) and chat (activated with the Enter key). We keep the logic modular with small helpers.

```cpp
enum class LoopAction : std::uint8_t { Continue, Stop };

auto handleInputKey(int inputKey, ChatUiState& chatState,
                    const std::shared_ptr<TcpSocket>& connection,
                    ClientState& state, RuntimeContext& runtime)
    -> LoopAction {
  if (inputKey == ERR) {
    return LoopAction::Continue;
  }

  if (chatState.active) {
    return handleChatInputKey(inputKey, chatState, connection, state, runtime);
  }

  return handleMovementInputKey(inputKey, chatState, connection, state,
                                runtime);
}
```

### 4.1 Movement keys

Arrow keys map to `Protocol::Direction` values. The helper sends a movement packet guarded by the shared `sendMutex`.

```cpp
auto handleMovementInputKey(int inputKey, ChatUiState& chatState,
                            const std::shared_ptr<TcpSocket>& connection,
                            ClientState& state, RuntimeContext& runtime)
    -> LoopAction {
  if (inputKey == 'q' || inputKey == 'Q') {
    runtime.running.get().store(false);
    return LoopAction::Stop;
  }

  if (inputKey == '\n' || inputKey == KEY_ENTER) {
    chatState.active = true;
    chatState.input.clear();
    return LoopAction::Continue;
  }

  if (auto direction = keyToDirection(inputKey); direction) {
    if (auto result = sendMovement(connection, state, *direction,
                                   runtime.sendMutex.get());
        !result) {
      const auto failureMessage = result.error().message;
      recordSocketFailure(runtime, failureMessage);
      return LoopAction::Stop;
    }
  }

  return LoopAction::Continue;
}
```

`keyToDirection()` simply maps curses key constants (`KEY_UP`, etc.) to the protocol enum. The actual send is handled by `sendMovement()`:

```cpp
auto sendMovement(const std::shared_ptr<TcpSocket>& socket, ClientState& state,
                  Protocol::Direction direction, std::mutex& sendMutex)
    -> SocketResult<void> {
  std::optional<Protocol::PlayerId> playerIdentifier;
  {
    std::scoped_lock guard{state.mutex};
    playerIdentifier = state.selfId;
  }

  if (!playerIdentifier) {
    return {};
  }

  Protocol::MovementPacket packet{.player = *playerIdentifier,
                                  .direction = direction};
  auto encoded = Protocol::encode(packet);
  std::scoped_lock guard{sendMutex};
  return socket->sendAll(std::span<const std::byte>{encoded});
}
```

We look up the local player ID under lock, build a packet, encode it with the shared protocol, and send it. If the ID is unknown (e.g., before the first snapshot arrives) the function returns early.

🧠 **Concept note:** `RuntimeContext` is a lightweight bundle of references the
handlers share. Passing one struct keeps the signatures tidy and avoids the
global variables that are so common in traditional curses programs.

### 4.2 Chat mode

When chat mode is active, keystrokes append to the input string, handle backspace/delete, and submit with Enter.

```cpp
auto handleChatInputKey(int inputKey, ChatUiState& chatState,
                        const std::shared_ptr<TcpSocket>& connection,
                        ClientState& state, RuntimeContext& runtime)
    -> LoopAction {
  switch (inputKey) {
  case escapeKeyCode:
    chatState.active = false;
    chatState.input.clear();
    return LoopAction::Continue;
  case '\n':
  case KEY_ENTER:
    if (chatState.input.empty()) {
      chatState.active = false;
      return LoopAction::Continue;
    }

    if (auto result = sendChat(connection, state, chatState.input,
                               runtime.sendMutex.get());
        !result) {
      const auto failureMessage = result.error().message;
      recordSocketFailure(runtime, failureMessage);
      return LoopAction::Stop;
    }

    chatState.input.clear();
    chatState.active = false;
    return LoopAction::Continue;
  case KEY_BACKSPACE:
  case deleteKeyCode:
  case backspaceKeyCode:
    if (!chatState.input.empty()) {
      chatState.input.pop_back();
    }
    return LoopAction::Continue;
  default:
    if (inputKey >= printableAsciiMin && inputKey <= printableAsciiMax &&
        chatState.input.size() < maxChatInputLength) {
      chatState.input.push_back(static_cast<char>(inputKey));
    }
    return LoopAction::Continue;
  }
}
```

`sendChat()` mirrors `sendMovement()`, encoding `Protocol::ChatPacket` and writing it under the send mutex. The chat log itself is capped at `maxChatMessages` to keep the UI tidy.

---

## 5. Receiver thread: keeping state fresh

`receiverLoop()` runs as a `std::jthread`, so its destructor requests stop automatically. The loop blocks on `receiveExact()` to pull headers and payloads from the server, then dispatches the decoded packets.

```cpp
void receiverLoop(const std::shared_ptr<TcpSocket>& socket, ClientState& state,
                  std::atomic_bool& running, std::atomic_bool& connectionActive,
                  std::mutex& errorMutex, std::string& lastError) {
  while (running.load()) {
    auto headerBytes = socket->receiveExact(Protocol::packetHeaderSize);
    if (!headerBytes) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = headerBytes.error().message;
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    auto headerResult =
        Protocol::decodeHeader(std::span<const std::byte>{headerBytes.value()});
    if (!headerResult) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = std::string{describePacketError(headerResult.error())};
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    auto payloadBytes = socket->receiveExact(headerResult->payloadSize);
    if (!payloadBytes) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = payloadBytes.error().message;
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    auto packetResult = Protocol::decodePacket(
        *headerResult, std::span<const std::byte>{payloadBytes.value()});
    if (!packetResult) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = std::string{describePacketError(packetResult.error())};
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    std::visit(Overloaded{
                  [&](const Protocol::StateSnapshotPacket& snapshot) {
                    handleSnapshot(state, snapshot);
                  },
                  [](const Protocol::MovementPacket&) {
                    // Movement packets are redundant; snapshots carry state.
                  },
                  [&](const Protocol::ChatPacket& chat) {
                    handleChat(state, chat);
                  }},
               *packetResult);
  }
}
```

> `Overloaded` is the same helper introduced in §03—it aggregates lambdas so
> `std::visit` can route each packet type to the correct handler with compile
time checks.

🧠 **Concept note:** The receiver thread performs blocking I/O on purpose. It
owns the socket read-side exclusively, so parking on `receiveExact()` keeps the
design simple. When data arrives we update `ClientState`; when shutdown starts,
the main thread calls `shutdown()` to break the wait.

Two helpers mutate `ClientState` under lock:

```cpp
auto handleSnapshot(ClientState& state,
                    const Protocol::StateSnapshotPacket& snapshot) -> void {
  std::unordered_map<Protocol::PlayerId, Protocol::Position> updated;
  updated.reserve(snapshot.players.size());
  for (const auto& player : snapshot.players) {
    updated.emplace(player.player, player.position);
  }

  std::scoped_lock guard{state.mutex};
  state.players = std::move(updated);
  if (snapshot.focusPlayer != 0) {
    state.selfId = snapshot.focusPlayer;
  }
}

auto handleChat(ClientState& state, const Protocol::ChatPacket& chat) -> void {
  ChatEntry entry{.player = chat.player, .message = chat.message};
  std::scoped_lock guard{state.mutex};
  state.chatLog.push_back(std::move(entry));
  while (state.chatLog.size() > maxChatMessages) {
    state.chatLog.pop_front();
  }
}
```

Snapshots replace the entire position table to avoid drift. Chat messages append to a deque, trimming old entries once the cap is exceeded.

### 5.1 Handling failures

Any failure while receiving or decoding sets `lastError`, flips both atomic flags, and exits the loop. After curses shuts down, `main()` prints the final error (if any). This keeps the UI responsive even when the server disappears unexpectedly.

`recordSocketFailure()` provides the same failure path for send-side errors, so the whole runtime unwinds consistently.

🧠 **Concept note:** Calling `connection->shutdown()` in `main()` wakes any
thread blocked in `receiveExact()`. On POSIX it causes `recv()` to return `0` or
`ECONNRESET`; on Windows it surfaces `WSAECONNRESET`. Either way the receiver
thread notices, records the error, and stops without hanging.

---

## 6. Try it now

🛠️ **Build checkpoint:**

```bash
cmake --build build --target moonlapse_client
```

With the server from §03 already running, launch the client:

```bash
./build/client/moonlapse_client
```

Use arrow keys to move, `Enter` to focus the chat box, type a short message, and
press `Enter` again to send. `Esc` leaves chat mode, and `q` exits the client.

📋 **Sample log**

```
[client] failed to connect: connection refused   # when the server is down
[client] connection closed by peer               # when the server exits cleanly
```

Seeing movement echoed across two client windows confirms the receiver thread,
shared state, and rendering pipeline are in sync.

---

## 7. Checklist

- [x] `CursesSession` wraps terminal setup/teardown with RAII.
- [x] `ClientState` stores player positions, self ID, and chat history behind a mutex.
- [x] Rendering copies state snapshots under lock, then draws without holding the mutex.
- [x] Input loop supports movement, chat input, and quit semantics.
- [x] Movement and chat packets reuse the shared protocol encoders.
- [x] Receiver thread decodes authoritative snapshots and updates local state atomically.
- [x] Errors propagate through `RuntimeContext`, shutting down cleanly and reporting to the user.

At this point the “terminal MMO” is fully playable: run the server, launch two clients, and watch the grid update in real time. In §05 we will sew everything together with build/test workflows, plus suggestions for extending the game (animations, persistence, matchmaking, and beyond).
