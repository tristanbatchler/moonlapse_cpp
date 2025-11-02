# §01 Stitching Together the Shared Protocol Layer

In §00 you met the project layout, build tooling, and the modern C++ mindset we rely on. Now we immediately put that mindset to work by implementing the core that both the client and server must agree on: the **protocol**. We will design packet types, encode/decode helpers, and error handling inside the `shared/` module. By the end of this chapter you will have a clean, testable static library that both sides can link against.

We will start small and iterate:

1. Set up the `shared` target in CMake.
2. Define fundamental domain types (player identifiers, directions, packet headers).
3. Implement encoding helpers that serialize to network byte order using `std::byte` and `std::span`.
4. Decode payloads safely with `std::expected` to surface errors without exceptions.
5. Build and test the shared module independently.

---

## 1. Wire up the shared target in CMake

Create `shared/CMakeLists.txt` so the root `CMakeLists.txt` can `add_subdirectory(shared)`.

```cmake
add_library(moonlapse_shared STATIC
  packets.hpp
)

set_target_properties(moonlapse_shared PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_include_directories(moonlapse_shared PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Optional (recommended for installs): CMake 3.23+ can track installed headers explicitly.
# target_sources(moonlapse_shared
#   PUBLIC
#     FILE_SET HEADERS
#     BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
#     FILES packets.hpp
# )

target_compile_features(moonlapse_shared PUBLIC cxx_std_23)
```

A few things to notice:

- `add_library(... STATIC …)` gives us a static archive the client and server can link against.
- We set the standard requirements to C++23 explicitly (redundant with the top-level definition but harmless and self-documenting).
- `target_include_directories(... PUBLIC …)` makes the headers available to dependents without leaking extra search paths.
- `target_compile_features(... PUBLIC cxx_std_23)` ensures any target that links against this one inherits the appropriate standard requirement.
- If you export or install the library, uncomment the `target_sources` block so CMake knows which headers belong to the target.

> **Try it**: run `cmake --build build --target moonlapse_shared` to make sure your CMake hierarchy is healthy before writing code. Right now the sources are empty, but once the files exist this target will build on its own.

🛠️ **Checkpoint:** if the build fails here, double-check the new `add_library` line and confirm the root `CMakeLists.txt` includes `add_subdirectory(shared)`.

---

## 2. Establish the packet scaffolding

Create `shared/packets.hpp` and start with the essential includes, namespace, and type aliases. We lean heavily on `<cstdint>`, `<span>`, `<expected>`, and `<array>` from the standard library.

```cpp
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <variant>
#include <vector>

namespace Moonlapse::Protocol {

inline constexpr std::uint16_t protocolVersion = 1;
inline constexpr std::size_t packetHeaderSize =
    sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

using PlayerId = std::uint32_t;
```

### Why `inline constexpr`?

In a header we want a single definition across translation units. `inline constexpr` gives us a guaranteed ODR-safe constant. This is similar to a `const` global in Go but scoped inside the namespace.

> **Concept note:** `constexpr` values are evaluated at compile time. The compiler substitutes the literal values everywhere they are used, so there is zero runtime overhead.

### Strong enums for clarity

Preferred enumerations use `enum class`. The scoped name avoids collisions and prevents implicit conversions.

```cpp
enum class PacketType : std::uint8_t {
  Movement = 1,
  StateSnapshot = 2,
};

enum class Direction : std::uint8_t {
  Up = 0,
  Down = 1,
  Left = 2,
  Right = 3,
};
```

Note that we specify the underlying type (`: std::uint8_t`) to control the byte layout on the wire. No magic numbers float around; each field matches the protocol spec.

Add simple structs for positions and movement commands:

```cpp
struct Position {
  std::int32_t x{};
  std::int32_t y{};
};

struct MovementPacket {
  PlayerId player{};
  Direction direction{Direction::Up};
};
```

We will add more packet types (e.g., state snapshots, chat) over time. For now this is enough to get the encoder/decoder pipeline in place.

Close the namespace at the bottom of the file for now; we will keep appending to this header throughout the chapter.

---

## 3. Encoding helpers: thinking in bytes

When you talk to the network you have to convert structs into contiguous byte sequences. We want to perform this without dangerous pointer casting or manual buffer math. Enter `std::array<std::byte, N>` plus `std::span`.

### 3.1 Network byte order

The protocol uses big-endian byte order. C++23’s `<bit>` provides `std::byteswap` to help. Let’s add two small helpers:

```cpp
template <std::integral T>
[[nodiscard]] constexpr auto toBigEndian(T value) noexcept -> T {
  if constexpr (std::endian::native == std::endian::big) {
    return value;
  }
  return std::byteswap(value);
}

template <std::integral T>
[[nodiscard]] constexpr auto fromBigEndian(T value) noexcept -> T {
  return toBigEndian(value);
}
```

These templates accept any integral type (`std::integral` is a C++20 concept) and either return the original value (on big-endian hosts) or byteswap (on little-endian machines like x86-64).

> **Concept note:** the `std::integral` concept keeps these helpers honest—only integer-like types compile.

> **Concept note:** `[[nodiscard]]` asks the compiler to warn if you ignore the returned value, which is invaluable for functions that return converted data or `std::expected` objects.

### 3.2 Serializing into a buffer

Define a helper that writes integrals into an existing array. We design it with spans so it works with any buffer size—`std::span` is the C++ equivalent of a Go slice or Python `memoryview`.

```cpp
template <std::integral T, std::size_t N>
void writeIntegral(std::array<std::byte, N>& target, std::size_t& offset,
                   T value) noexcept {
  auto networkOrder = toBigEndian(value);
  auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(networkOrder);
  auto destination =
      std::span<std::byte, N>{target}.subspan(offset, bytes.size());
  std::copy_n(bytes.begin(), bytes.size(), destination.begin());
  offset += bytes.size();
}

template <std::integral T>
[[nodiscard]] inline auto readIntegral(std::span<const std::byte> buffer,
                                       std::size_t& offset)
    -> PacketResult<T> {
  if (offset + sizeof(T) > buffer.size()) {
    return std::unexpected(PacketError::Truncated);
  }

  std::array<std::byte, sizeof(T)> raw{};
  std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(offset), sizeof(T),
              raw.begin());
  offset += sizeof(T);
  return fromBigEndian(std::bit_cast<T>(raw));
}
```

Callers pass a buffer and an offset by reference; the function writes the bytes and bumps the offset. No raw `char*` required.

> **Concept note:** `std::bit_cast` is the safe, constexpr alternative to `reinterpret_cast`. We bit-cast the integral into a byte array, copy the bytes, and let the compiler optimise away any temporaries.

> **Why two APIs?** The free functions keep header encoding simple, while `PayloadReader` manages its own offset when parsing payloads. Both rely on the same underlying idioms.

### 3.3 Encoding the packet header

Every packet starts with a header describing version, type, and payload size. Add this struct and encoder:

```cpp
struct PacketHeader {
  std::uint16_t version{protocolVersion};
  PacketType type{PacketType::Movement};
  std::uint32_t payloadSize{};
};

[[nodiscard]] inline auto encodeHeader(PacketHeader header) noexcept
    -> std::array<std::byte, packetHeaderSize> {
  std::array<std::byte, packetHeaderSize> buffer{};
  std::size_t offset = 0;
  writeIntegral(buffer, offset, header.version);
  writeIntegral(buffer, offset, static_cast<std::uint16_t>(header.type));
  writeIntegral(buffer, offset, header.payloadSize);
  return buffer;
}
```

### 3.4 Payload writer utility

For packet payloads we often need to append bytes dynamically. A small helper class that wraps `std::vector<std::byte>` keeps the code neat.

```cpp
class PayloadWriter {
public:
  template <std::integral T> void write(T value) {
    auto networkOrder = toBigEndian(value);
    auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(networkOrder);
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
  }

  void writeByte(std::uint8_t value) {
    m_buffer.push_back(static_cast<std::byte>(value));
  }

  [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte> {
    return m_buffer;
  }

  [[nodiscard]] auto release() && -> std::vector<std::byte> {
    return std::move(m_buffer);
  }

private:
  std::vector<std::byte> m_buffer;
};
```

🧠 **Concept note**: `std::bit_cast` gives us a safe, constexpr way to treat bytes as another trivially copyable type. It replaces old `reinterpret_cast` hacks with defined behaviour.

Notice that `release()` is an rvalue-qualified member; it returns the vector by move only when called on a temporary (`std::move(writer).release()`). This expresses ownership transfer clearly.

> **Concept note:** Move semantics let us transfer ownership without copying. After `std::move(writer)`, the writer keeps a valid but unspecified state while the caller takes over the buffer—much like handing off a Go slice’s backing array instead of cloning it.

### 3.5 Putting it together: encode a movement command

Add this function to the header:

```cpp
[[nodiscard]] inline auto encodePayload(const MovementPacket& packet)
    -> std::vector<std::byte> {
  PayloadWriter writer;
  writer.write(packet.player);
  writer.writeByte(static_cast<std::uint8_t>(packet.direction));
  return std::move(writer).release();
}

[[nodiscard]] inline auto encode(const MovementPacket& packet)
    -> std::vector<std::byte> {
  auto payload = encodePayload(packet);
  PacketHeader header{.type = PacketType::Movement,
                      .payloadSize =
                          static_cast<std::uint32_t>(payload.size())};
  auto headerBytes = encodeHeader(header);

  std::vector<std::byte> buffer;
  buffer.reserve(packetHeaderSize + payload.size());
  buffer.insert(buffer.end(), headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());
  return buffer;
}
```

We reserve capacity up front, append header then payload, and return a single buffer ready for sending over the socket.

🛠️ **Checkpoint:** rebuild `moonlapse_shared` now. The target should compile without warnings. If you see template errors, they usually point to a missing include or a typo in the helper signatures.

🔁 **Round-trip demo:** drop the snippet below into a scratch file (or a doctest case) to verify encoder/decoder symmetry.

```cpp
const Protocol::MovementPacket original{
  .player = 42,
  .direction = Protocol::Direction::Down};
auto encoded = Protocol::encode(original);
auto header = Protocol::decodeHeader(std::span<const std::byte>{encoded});
assert(header);
auto payloadSpan =
  std::span<const std::byte>{encoded}.subspan(Protocol::packetHeaderSize);
auto decoded = Protocol::decodePacket(*header, payloadSpan);
assert(decoded);
auto* movement = std::get_if<Protocol::MovementPacket>(&decoded.value());
assert(movement != nullptr);
assert(movement->player == original.player);
assert(movement->direction == original.direction);
```

---

## 4. Decoding with safety guarantees

Decoding is mirror work, but we must guard against truncated payloads or invalid data. This is where `std::expected` shines.

### 4.1 Packet errors

Define an enum for failure modes and a handy alias for `std::expected`:

```cpp
enum class PacketError : std::uint8_t {
  VersionMismatch,
  UnknownType,
  Truncated,
  SizeMismatch,
  InvalidPayload,
};

template <typename T> using PacketResult = std::expected<T, PacketError>;

Unlike `std::optional`, `std::expected` carries a strongly typed error. Callers can inspect `.error()` for the precise reason, keeping control flow explicit without exceptions.
```

### 4.2 Reading integrals with bounds checking

Create a small reader that tracks offsets and returns `std::expected` when data runs out:

```cpp
class PayloadReader {
public:
  explicit PayloadReader(std::span<const std::byte> data) noexcept
      : m_payload{data} {}

  template <std::integral T> auto read() -> PacketResult<T> {
    if (m_offset + sizeof(T) > m_payload.size()) {
      return std::unexpected(PacketError::Truncated);
    }

    std::array<std::byte, sizeof(T)> raw{};
    std::copy_n(m_payload.begin() + static_cast<std::ptrdiff_t>(m_offset),
                sizeof(T), raw.begin());
    m_offset += sizeof(T);
    return fromBigEndian(std::bit_cast<T>(raw));
  }

  auto readByte() -> PacketResult<std::uint8_t> { return read<std::uint8_t>(); }

  [[nodiscard]] auto remaining() const noexcept -> std::size_t {
    return m_payload.size() - m_offset;
  }

private:
  std::span<const std::byte> m_payload;
  std::size_t m_offset{};
};
```

No raw pointers, no undefined behaviour: either we return a successfully decoded value or we surface an error.

### 4.3 Header decoding

Reassemble the header carefully. Callers must check the error condition before using the result.

```cpp
[[nodiscard]] inline auto decodeHeader(std::span<const std::byte> buffer)
    -> PacketResult<PacketHeader> {
  if (buffer.size() < packetHeaderSize) {
    return std::unexpected(PacketError::Truncated);
  }

  std::size_t offset = 0;
  auto version = readIntegral<std::uint16_t>(buffer, offset);
  if (!version) {
    return std::unexpected(version.error());
  }

  auto typeValue = readIntegral<std::uint16_t>(buffer, offset);
  if (!typeValue) {
    return std::unexpected(typeValue.error());
  }

  auto payloadSize = readIntegral<std::uint32_t>(buffer, offset);
  if (!payloadSize) {
    return std::unexpected(payloadSize.error());
  }

  if (*version != protocolVersion) {
    return std::unexpected(PacketError::VersionMismatch);
  }

  auto decodedType = static_cast<PacketType>(*typeValue);
  switch (decodedType) {
  case PacketType::Movement:
  case PacketType::StateSnapshot:
    break;
  default:
    return std::unexpected(PacketError::UnknownType);
  }

  return PacketHeader{
      .version = *version, .type = decodedType, .payloadSize = *payloadSize};
}
```

> `readIntegral` here is a convenience wrapper identical in spirit to the `PayloadReader` implementation. You can either reuse the reader class or keep separate free functions—the key point is that each conversion is bounds-checked and returns `std::expected`.

> **Pattern reminder:** checking `if (!result)` is the C++ analogue of Go’s `if err != nil`. Use `return std::unexpected(result.error());` to propagate the failure immediately.

### 4.4 Decoding a movement payload

We validate each field and guard against unknown directions.

```cpp
[[nodiscard]] inline auto decodeMovement(std::span<const std::byte> payload)
    -> PacketResult<MovementPacket> {
  PayloadReader reader{payload};
  auto playerId = reader.read<PlayerId>();
  if (!playerId) {
    return std::unexpected(playerId.error());
  }

  auto directionRaw = reader.readByte();
  if (!directionRaw) {
    return std::unexpected(directionRaw.error());
  }

  auto directionValue = *directionRaw;
  if (directionValue > static_cast<std::uint8_t>(Direction::Right)) {
    return std::unexpected(PacketError::InvalidPayload);
  }

  if (reader.remaining() != 0) {
    return std::unexpected(PacketError::SizeMismatch);
  }

  return MovementPacket{.player = *playerId,
                        .direction = static_cast<Direction>(directionValue)};
}
```

### 4.5 Stitching decoded variants

As the protocol grows, we will hold different packet structs inside a `std::variant`. For now it holds a single type, but wiring it up early keeps the interface stable.

```cpp
using PacketVariant = std::variant<MovementPacket>;

[[nodiscard]] inline auto decodePacket(const PacketHeader& header,
                                       std::span<const std::byte> payload)
    -> PacketResult<PacketVariant> {
  if (payload.size() != header.payloadSize) {
    return std::unexpected(PacketError::SizeMismatch);
  }

  switch (header.type) {
  case PacketType::Movement: {
    auto packet = decodeMovement(payload);
    if (!packet) {
      return std::unexpected(packet.error());
    }
    return PacketVariant{*packet};
  }
  default:
    return std::unexpected(PacketError::UnknownType);
  }
}
```

When the enum gains additional entries, we expand both the variant and the switch statement. Exhaustive handling makes it impossible to “forget” a new packet type in one code path.

> **Concept note:** `std::variant` behaves like a Rust `enum` with data—it stores exactly one alternative. `std::visit` forces you to handle every case, so the compiler catches missing packet handlers.

🧠 **Concept note**: `std::variant` is a type-safe tagged union. Combined with `std::visit`, it enforces exhaustive handling at compile time.

---

## 5. Testing the shared module in isolation

Because `moonlapse_shared` is a static library with no outside dependencies, you can build and test it without touching the client or server. Two recommendations:

1. **Use `cmake --build build --target moonlapse_shared`** during early development. This gives fast feedback whenever you change protocol code.
2. **Add a simple executable under `shared/tests/`** driven by Catch2, doctest, or the testing framework of your choice. Point the new test target at `moonlapse_shared` to exercise encoding/decoding round-trips and edge cases.

Example `CMakeLists.txt` snippet for a doctest-based test:

```cmake
add_executable(shared_protocol_tests
  tests/protocol_roundtrip.cpp
)

target_link_libraries(shared_protocol_tests PRIVATE moonlapse_shared)
```

Then run:

```bash
cmake --build build --target shared_protocol_tests
ctest --tests-regex shared_protocol
```

Even if you postpone formal tests for later, the standalone build target means you can iterate on `shared/` logic quickly with a single command. Great early tests include truncated headers, oversized payload declarations, and invalid direction codes to exercise each error branch.

🏁 **Example doctest case**

```cpp
#include <doctest/doctest.h>
#include "packets.hpp"

TEST_CASE("movement packet round-trip") {
  using Protocol::Direction;
  using Protocol::MovementPacket;

  const MovementPacket original{.player = 7, .direction = Direction::Left};
  auto encoded = Protocol::encode(original);
  auto header = Protocol::decodeHeader(std::span<const std::byte>{encoded});
  REQUIRE(header);
  auto payloadSpan =
      std::span<const std::byte>{encoded}.subspan(Protocol::packetHeaderSize);
  auto decoded = Protocol::decodePacket(*header, payloadSpan);
  REQUIRE(decoded);
  REQUIRE(std::holds_alternative<MovementPacket>(decoded.value()));
  const auto& restored = std::get<MovementPacket>(decoded.value());
  CHECK(restored.player == original.player);
  CHECK(restored.direction == original.direction);
}
```

Compile this under `shared/tests/` and you have executable assurance that the helpers behave.

---

## 6. Checklist

- [x] `shared/CMakeLists.txt` defines a C++23 static library with clean include semantics.
- [x] Strongly typed enums describe packet types and directions.
- [x] Packet header encoding uses `std::byte` buffers and network byte order helpers.
- [x] `PayloadWriter` and `PayloadReader` encapsulate serialization without raw pointers.
- [x] `std::expected` communicates errors explicitly when decoding fails.
- [x] A `std::variant` keeps packet handling extensible.

At this point, both client and server can include `packets.hpp`, encode movement commands, and decode them safely.

---

## What’s next (§02)

In the next chapter we will layer networking primitives on top of the protocol. §02 introduces the cross-platform socket wrapper, showcases RAII for system handles, and demonstrates how to integrate `std::expected`-style error reporting with blocking/non-blocking I/O. By the end of §02 you will be able to connect the client to the server and swap packets through the shared protocol you just built.
