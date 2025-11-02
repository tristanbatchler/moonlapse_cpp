# §05 Ship It and Extend It

We now have a functioning terminal MMO: protocol helpers (§01), portable sockets (§02), an authoritative server (§03), and a curses client (§04). This final chapter ties the project together with build/test tips, end-to-end run instructions, and ideas for expanding the game while keeping the codebase maintainable.

---

## 1. Building every target

The root `CMakeLists.txt` wires together `shared`, `server`, and `client`. After configuring (`cmake -S . -B build`), rebuild everything with:

```bash
cmake --build build
```

Useful single-target builds when iterating locally:

```bash
cmake --build build --target moonlapse_shared
cmake --build build --target moonlapse_server
cmake --build build --target moonlapse_client
```

When `shared/packets.hpp` changes, rebuild the shared library first so dependent targets pick up new headers.

### 1.1 Optional: compile commands for tooling

The top-level `CMakeLists.txt` enables `CMAKE_EXPORT_COMPILE_COMMANDS`, so tools like clangd and clang-tidy can ingest `build/compile_commands.json` directly. This file is a universal manifest of compiler flags and include paths. Point VS Code, CLion, or Neovim’s clangd extension at it and you get accurate completions, diagnostics, and cross references without hand-maintaining settings—a quality-of-life boost compared with the guesswork older C++ workflows required.

---

## 2. Running server and clients together

In separate terminals:

```bash
./build/server/moonlapse_server
./build/client/moonlapse_client
```

The server logs connections, packet errors, and disconnects. Each client renders the grid, shows chat entries, and exits gracefully on `q`. You can run multiple clients from the same machine—the server assigns unique IDs and positions using `spawnPosition()`.

### 2.1 Verifying end-to-end behaviour

Try this workflow after every major change:

1. Start the server.
2. Launch two clients.
3. Move each avatar across the grid; confirm both screens update.
4. Type chat messages in each client; confirm they appear in both logs.
5. Close one client; verify the other still runs and the server logs the disconnect cleanly.

Any failure should surface either in the server logs or in the client’s terminal output after curses shuts down.

---

## 3. Testing strategy

We kept the runtime simple, but the architecture invites targeted tests:

- **Shared protocol:** add `shared/tests/protocol_roundtrip.cpp` and link against `moonlapse_shared`. Doctest or Catch2 work well. Focus on boundary cases: truncated headers, invalid directions, oversize chat payloads.
- **Server logic:** extract pure helpers (`applyMovement`, `spawnPosition`) into their own file for unit tests. For integration, consider spinning up the server in a test harness, then driving it with sockets using std::thread.
- **Client render helpers:** treat them as pure functions (they already are). If you want automated coverage, abstract the curses calls behind an interface so tests can assert on a fake framebuffer. For now, manual testing plus sanitizers is sufficient.

Example doctest for the movement packet round-trip:

```cpp
#include <doctest/doctest.h>
#include "packets.hpp"

TEST_CASE("movement packet round-trip") {
	using Protocol::Direction;
	using Protocol::MovementPacket;

	const MovementPacket original{.player = 42, .direction = Direction::Left};
	auto encoded = Protocol::encode(original);
	auto header = Protocol::decodeHeader(std::span<const std::byte>{encoded});
	REQUIRE(header);
	auto payload = std::span<const std::byte>{encoded}.subspan(Protocol::packetHeaderSize);
	auto decoded = Protocol::decodePacket(*header, payload);
	REQUIRE(decoded);
	REQUIRE(std::holds_alternative<MovementPacket>(decoded.value()));
	const auto &restored = std::get<MovementPacket>(decoded.value());
	CHECK(restored.player == original.player);
	CHECK(restored.direction == original.direction);
}
```

Drop this under `shared/tests/`, add a CMake target that links against `moonlapse_shared`, and wire it into CI so protocol regressions fail fast.

### 3.1 Sanitizers and static analysis

Coming from Go or Python, the runtime guards you against use-after-free and
buffer overflows. In C++ you lean on **sanitizers** instead—they insert
aggressive runtime checks that catch memory and UB bugs the moment they occur.

Add an option near the top of your root `CMakeLists.txt`:

```cmake
option(MOONLAPSE_ENABLE_SANITIZERS "Enable address/UB sanitizers" OFF)
if(MOONLAPSE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address,undefined)
endif()
```

Then configure a dedicated build when you want the extra checks:

```bash
cmake -S . -B build-sanitize -DMOONLAPSE_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize
```

Pair that with `clang-tidy` (either via your editor or a dedicated CMake
target) to flag stylistic or API usage issues before they reach `main`.

---

## 4. Deployment and packaging

The server and client are single binaries with minimal dependencies:

- For Linux/macOS, ensure `ncurses` is installed (e.g., `apt-get install libncurses5-dev` or `brew install ncurses`).
- On Windows, bundle PDCurses (already handled in `client/CMakeLists.txt`) and ship the `.exe` alongside `pdcurses.dll` if necessary.
- Ship the server with a simple systemd service or a Docker container. Since the process is lightweight, a single core handles small rooms easily.

When distributing builds, provide a README snippet with the same run commands from §2.

---

## 5. Where to go next

The architecture leaves plenty of room to grow. Stick to the shared protocol + RAII foundations and you can add features without fighting the existing code.

### 5.1 Gameplay ideas

1. **Diagonals and speed:** extend `Protocol::Direction` with diagonals; adjust `applyMovement()` accordingly.
2. **Persistent chat history:** add a server-side ring buffer and send it in `StateSnapshotPacket` (remember to update the checklist in `.github/instructions/add_new_packet.instructions.md`).
3. **Rooms or channels:** namespace players into rooms; include the room ID in movement/chat packets.
4. **NPCs or obstacles:** treat non-player entities as extra entries in the snapshot, maybe with distinct glyphs.

Each feature will touch the shared protocol, server dispatch, and client rendering—use the packet checklist to stay consistent.

That checklist lives in `.github/instructions/add_new_packet.instructions.md`.
It walks through updating `PacketType`, encoding/decoding helpers, server
dispatch, and client visitors so new wire formats stay synchronized across all
three binaries.

### 5.2 Networking polish

- **Non-blocking sockets:** adjust `TcpSocket` to expose non-blocking mode and integrate polling (epoll/kqueue) for scaling past handfuls of clients.
- **TLS support:** wrap sockets with an mbedTLS or OpenSSL RAII layer.
- **Heartbeat/ping:** add a timer thread that sends heartbeat packets and drops idle clients.

### 5.3 Testing and CI

Because the shared protocol, server helpers, and client rendering are decoupled,
CI can exercise each layer independently. A minimal GitHub Actions workflow
could:

```yaml
name: ci
on: [push, pull_request]
jobs:
	build:
		runs-on: ubuntu-latest
		steps:
			- uses: actions/checkout@v4
			- name: Configure
				run: cmake -S . -B build
			- name: Build
				run: cmake --build build --target moonlapse_shared moonlapse_server moonlapse_client
			- name: Protocol tests
				run: ctest --test-dir build --output-on-failure
```

Layer on `clang-format`/`clang-tidy` steps or a sanitizer build as needed. The
goal is to keep the protocol and state-management guarantees verifiable on
every push, so feature work never silently drifts from the design we built in
§§01–04.

### 5.4 UI improvements

- **Color and animations:** curses supports color pairs—highlight the local player or chat mentions.
- **Scrolling chat:** increase `maxChatMessages` and add page-up/page-down navigation.
- **Command palette:** Adopt a slash-command system (`/nick`, `/whisper`, etc.) by extending the protocol.

---

## 6. What you learned

- RAII wrappers (`TcpSocket`, `CursesSession`) give deterministic cleanup across
	OS APIs, replacing manual `close()` calls.
- `std::expected`, `std::variant`, and visitors keep protocol flows explicit and
	type-safe.
- Thread ownership is spelled with `std::jthread`, `std::shared_ptr`, mutexes,
	and atomics—always pairing blocking I/O with clear lifetime rules.
- Modern CMake (targets, `compile_commands.json`, sanitizer toggles) removes the
	friction of “big C++” builds and makes IDE integration dependable.

## 7. Checklist recap

- [x] Build instructions cover all targets and tooling hooks.
- [x] Runbook demonstrates the full server + multi-client workflow.
- [x] Testing guidance highlights shared protocol tests and sanitizer setup.
- [x] Deployment section notes platform dependencies.
- [x] Extension ideas map to concrete protocol/server/client changes.

Thank you for following along. You now have a modern C++23 codebase that demonstrates RAII, safe networking, and a curses UI working in concert. Keep the packet checklist handy, lean on `std::expected` when adding error paths, and continue building features one module at a time. Happy hacking! 
