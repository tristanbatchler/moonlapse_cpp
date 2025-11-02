# §00 Build a Modern Curses MMO with idiomatic, safe C++23

Welcome to the start of a hands-on series where we build a small multiplayer terminal game while learning the *modern* way to write C++23. If you already ship code in C, Go, or Python but feel uneasy about references, templates, or CMake files, this is for you. You will write real production-grade C++ as we go—no toy examples—and every concept earns its place in working code.

We will grow a real project—the `moonlapse_cpp` codebase—step by step. Along the way you will learn how to:

- model packets and game state with strong types and value semantics,
- use RAII (Resource Acquisition Is Initialization) to keep sockets, threads, and terminal state safe,
- harness C++23 language and library features (concepts, `std::expected`, `std::jthread`, `<print>`),
- structure a CMake workspace that targets Linux and Windows with curses-based clients,
- keep server and client code testable and maintainable.

This first instalment orients you, shows the tools we rely on, and highlights the C++ idioms you will meet repeatedly.

> **Try this now:** skim the repository root and open `shared/packets.hpp` in your editor. Notice how every type lives inside a namespace and avoids raw pointers. Keep that file open while you read; we will refer to it often.

---

## 1. Big-picture architecture

`moonlapse_cpp` consists of three CMake targets. Each target compiles into its own executable or static library, but they share headers and idioms, just like a Go module with a shared package or a Rust workspace split into multiple crates.

```
project-root/
├─ client/    # Terminal UI + input + client networking
├─ server/    # Authoritative game state + broadcast
└─ shared/    # Packets, protocol helpers, socket wrappers
```

The server listens for TCP connections, assigns players positions on a 2D grid, and broadcasts snapshots. The client is a curses-driven frontend that renders the grid and sends movement commands. The `shared` directory contains the protocol definitions and the networking abstraction we use on both sides.

```
##########################
# @  o                 o #
#                        #
#           o            #
##########################
Chat> hello world_
```

That snapshot is the end goal for the first milestone: two terminals, one server, all state synchronized.

Unlike the classic C style of sprinkling manual `malloc` and `free`, we let objects manage their own lifetimes. That principle (RAII) underpins everything in this series.

---

## 2. Required toolchain

You will need:

- A modern C++ compiler (Clang 17+, GCC 13+, or MSVC 2022) with full C++23 support.
- CMake ≥ 3.26.
- `ncurses` (Linux/macOS) or PDCurses (Windows) for terminal rendering.
- A recent `clang-tidy` and `clang-format` to keep style consistent (optional but recommended).

Install curses with your platform package manager:

- **Ubuntu/Debian:** `sudo apt-get install libncurses5-dev`
- **Fedora:** `sudo dnf install ncurses-devel`
- **macOS (Homebrew):** `brew install ncurses`
- **Windows:** PDCurses is fetched automatically via CMake when you configure the project; ensure you generate either a Visual Studio or Ninja build.

Clone the repository and generate a build tree:

```bash
cmake -S . -B build
cmake --build build
```

If you are unsure about tool versions, double-check before the first configure step:

```bash
g++ --version
clang++ --version
cmake --version
```

On success you should see CMake finish with something like:

```
[100%] Linking CXX executable moonlapse_client
[100%] Built target moonlapse_client
```

CMake uses *out-of-source* builds, so all generated files live inside `build/`. You can delete that directory at any time without touching your code.

> **Why CMake?** Because CMake is all about **targets, not variables**. Each module declares only what it consumes (`target_link_libraries(client PRIVATE moonlapse_shared Threads::Threads ${CURSES_LIBRARIES})`) and CMake handles include paths, definitions, and platform-specific switches.

⚠️ **Common configure issues**

- *Missing curses library:* install the package listed above, then rerun `cmake -S . -B build`.
- *Old compiler:* GCC 13+ or Clang 17+ is required. On Ubuntu, `sudo apt-get install g++-13` and configure with `-DCMAKE_CXX_COMPILER=g++-13`.
- *Windows SDK missing:* ensure you ran the “x64 Native Tools” developer prompt before configuring.

---

## 3. A modern C++ mindset

Coming from C, Go, or Python, the most jarring differences in modern C++ are about *ownership* and *expression*. Modern C++ delivers deterministic memory safety **without** a garbage collector; RAII (Resource Acquisition Is Initialization) ensures resources release the moment they fall out of scope.

### 3.1 Value-first design

In `shared/packets.hpp` you will see structs like:

```cpp
struct MovementPacket {
  PlayerId player{};
  Direction direction{Direction::Up};
};
```

This is plain old data, but we initialise members at the point of declaration and rely on aggregate initialisers (`MovementPacket packet{.player = id, .direction = dir};`). Values move efficiently when needed and copy when cheap. Value semantics make ownership explicit: you always know who owns what, and the compiler tells you when something is copied or moved.

> **Move semantics in one sentence:** when a type is cheap to move but expensive to copy (e.g., `std::vector`), C++ lets you transfer ownership instead of duplicating data by calling `std::move`—the object on the right-hand side becomes “moved-from” but remains valid. We will call this out whenever the codebase relies on moves for efficiency.

### 3.2 RAII everywhere

Any resource—socket, thread, window—is wrapped in a type whose destructor releases it. For example, the client owns a `CursesSession`:

```cpp
struct CursesSession {
  CursesSession() : window{initscr()} { /* configure curses */ }
  ~CursesSession() { if (active) endwin(); }
  // ...
};
```

Once the object goes out of scope, we are guaranteed that `endwin()` runs, even during exceptions. No manual `finally` blocks required. In Go you would `defer endwin()`; in C++ the destructor plays that role automatically at the end of scope, giving you Go-like determinism without a runtime or GC.

### 3.3 `std::expected` over error codes

Functions that can fail return `std::expected<T, Error>`. It behaves like Go’s `(value, error)` multiple return values or Rust’s `Result`, but the compiler enforces handling. The networking layer uses:

```cpp
using SocketResult<T> = std::expected<T, SocketError>;
```

Callers inspect `.has_value()` or use the `if (!result) { /* handle */ }` pattern. Unlike exceptions, `std::expected` keeps control flow visible and local—no invisible stack unwinding—while still avoiding error-prone sentinel codes.

```cpp
// Before (C-style):
int send_packet(int sock, const void* data, size_t size);
if (send_packet(fd, buffer, length) != 0) {
  // who knows why it failed?
}

// After (std::expected):
auto result = socket.sendAll(buffer);
if (!result) {
  std::println("send failed: {}", result.error().message);
}
```

### 3.4 Ranges and spans

Where you might reach for raw pointer arithmetic in C, we reach for `std::span`. Think of it as **C++’s Go slice** or **Python’s `memoryview`**: a non-owning view over a contiguous block of memory. Packet decoding uses spans to iterate over byte buffers safely, and spans bridge nicely between STL containers and legacy C arrays.

---

## 4. CMake primer for this project

The top-level `CMakeLists.txt` declares the project standard and adds three subdirectories. Each subdirectory registers a target and its dependencies. Modern CMake is about **targets, not variables**: every target states what it needs and CMake wires the dependency graph. The key ideas:

- `add_executable(moonlapse_client main.cpp)` defines a target.
- `target_include_directories` lists include paths. Visibility matters: use `PRIVATE` for internal headers, `PUBLIC` when dependents also need them, and `INTERFACE` for headers only (no sources).
- `target_link_libraries` pulls in the shared library, threading library, and curses vendor library—again with `PRIVATE`/`PUBLIC` keywords to control propagation.
- Conditional blocks (`if (WIN32) ... else() ...`) let us link PDCurses on Windows and `ncurses` elsewhere.

A minimalist client target looks like this:

```cmake
add_executable(moonlapse_client main.cpp)
target_link_libraries(moonlapse_client PRIVATE moonlapse_shared Threads::Threads ${CURSES_LIBRARIES})
target_compile_features(moonlapse_client PRIVATE cxx_std_23)
```

Think of targets as modules. By keeping include paths and compile definitions private to a target, we avoid the global include soup you might remember from old CMake setups. If you come from Makefiles, think of this as a typed build graph where each node declares its needs explicitly.

🛠️ **Hands-on mini exercise**

1. Comment out the `target_link_libraries(moonlapse_client PRIVATE moonlapse_shared ...)` line in `client/CMakeLists.txt`.
2. Run `cmake --build build`.
3. Observe the link error complaining about missing protocol symbols.
4. Restore the line and rebuild—the error disappears. This is the core CMake feedback loop: targets declare dependencies and the build either succeeds or fails with precise diagnostics.

---

## 5. How to read modern C++ code

You will encounter syntax that looks alien at first glance. Here are the recurring patterns we will demystify in upcoming posts, along with rough translations to other languages:

| Syntax | Meaning | Analogy | Why it matters |
| ------ | ------- | ------- | --------------- |
| `auto main() -> int` | Trailing return type; stylistic | Go function declarations | Keeps complex return types readable |
| `std::jthread receiver(...)` | Joinable thread with cooperative stop | Go goroutine + `context.Context` | Ensures threads terminate when scope ends |
| `std::scoped_lock guard{mutex};` | RAII mutex guard | Go `sync.Mutex` + `defer` | Eliminates forgotten unlocks |
| `std::variant` + `std::visit` | Exhaustive sum type | Rust `enum` with data | Forces you to handle every packet kind |
| `constexpr` values | Compile-time constants | `const` evaluated at compile time | Eliminates runtime overhead |
| `std::unique_ptr<T>` | Exclusive owner with automatic cleanup | Owning pointer + deterministic `defer close()` | Prevents leaks without GC |

If you keep function and type declarations narrow, C++ reads more like a strongly typed scripting language than the template jungle you may fear.

---

## 6. What comes next

The next post will set up the shared protocol headers from scratch. You will learn how to:

- model packets with strongly typed enums (`enum class`),
- implement safe encoding/decoding helpers with `std::span`,
- use `std::expected` to propagate errors without exceptions,
- unit-test packet logic by compiling only the `shared` target.

From there we will flesh out the networking layer, build the curses UI, and eventually add features like chat, all while reinforcing modern C++ idioms.

🚧 **Common C++ pitfalls to watch for**

- Forgetting `&` in range-for loops (`for (auto player : players)` copies; use `auto&` instead).
- Falling back to raw pointers—reach for `std::unique_ptr`, `std::shared_ptr`, or `gsl::not_null` instead.
- Omitting `const` qualifiers, which can cause accidental mutations. Err on the side of `const` until mutation is required.
- Ignoring compiler warnings. Treat warnings as errors; the project enables high warning levels by default.

Until then, explore the repository:

```bash
# Build both client and server
cmake --build build

# Run the server
./build/server/moonlapse_server

# In another terminal, run the client
./build/client/moonlapse_client
```

You now have a mental map of the playground. In §01 we will write the protocol layer together, introduce templates and concepts where they earn their keep, and see how modern C++ expresses the same ideas you already know from other languages—safely and succinctly.

📚 **Homework:** run `cmake --build build --target moonlapse_shared`, then open `shared/packets.hpp` and identify every place `std::span` appears. Try explaining to yourself how each usage corresponds to a Go slice or Python `memoryview`. We will build on that understanding in the next chapter.
