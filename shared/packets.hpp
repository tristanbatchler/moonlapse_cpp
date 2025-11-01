#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <variant>
#include <vector>

namespace Moonlapse::Protocol {

inline constexpr std::uint16_t PROTOCOL_VERSION = 1;
inline constexpr std::size_t PACKET_HEADER_SIZE =
    sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

enum class PacketType : std::uint8_t {
  MOVEMENT = 1,
  STATE_SNAPSHOT = 2,
};

enum class Direction : std::uint8_t {
  UP = 0,
  DOWN = 1,
  LEFT = 2,
  RIGHT = 3,
};

using PlayerId = std::uint32_t;

struct Position {
  std::int32_t x{};
  std::int32_t y{};
};

struct PacketHeader {
  std::uint16_t version{PROTOCOL_VERSION};
  PacketType type{PacketType::MOVEMENT};
  std::uint32_t payloadSize{};
};

struct MovementPacket {
  PlayerId player{};
  Direction direction{Direction::UP};
};

struct PlayerState {
  PlayerId player{};
  Position position{};
};

struct StateSnapshotPacket {
  PlayerId focusPlayer{};
  std::vector<PlayerState> players;
};

enum class PacketError : std::uint8_t {
  VERSION_MISMATCH,
  UNKNOWN_TYPE,
  TRUNCATED,
  SIZE_MISMATCH,
  INVALID_PAYLOAD,
};

template <typename T> using PacketResult = std::expected<T, PacketError>;

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

template <std::integral T, std::size_t N>
void writeIntegral(std::array<std::byte, N> &target, std::size_t &offset,
                   T value) noexcept {
  auto networkOrder = toBigEndian(value);
  auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(networkOrder);
  auto destination =
      std::span<std::byte, N>{target}.subspan(offset, bytes.size());
  std::memcpy(destination.data(), bytes.data(), bytes.size());
  offset += bytes.size();
}

template <std::integral T>
[[nodiscard]] inline auto readIntegral(std::span<const std::byte> buffer,
                                       std::size_t &offset) -> PacketResult<T> {
  if (offset + sizeof(T) > buffer.size()) {
    return std::unexpected(PacketError::TRUNCATED);
  }
  std::array<std::byte, sizeof(T)> raw{};
  auto view = buffer.subspan(offset, sizeof(T));
  std::ranges::copy(view, raw.begin());
  offset += sizeof(T);
  return fromBigEndian(std::bit_cast<T>(raw));
}

[[nodiscard]] inline auto encodeHeader(PacketHeader header) noexcept
    -> std::array<std::byte, PACKET_HEADER_SIZE> {
  std::array<std::byte, PACKET_HEADER_SIZE> buffer{};
  std::size_t offset = 0U;
  writeIntegral(buffer, offset, header.version);
  writeIntegral(buffer, offset, static_cast<std::uint16_t>(header.type));
  writeIntegral(buffer, offset, header.payloadSize);
  return buffer;
}

[[nodiscard]] inline auto decodeHeader(std::span<const std::byte> buffer)
    -> PacketResult<PacketHeader> {
  if (buffer.size() < PACKET_HEADER_SIZE) {
    return std::unexpected(PacketError::TRUNCATED);
  }

  std::size_t offset = 0U;
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

  if (*version != PROTOCOL_VERSION) {
    return std::unexpected(PacketError::VERSION_MISMATCH);
  }

  auto packetType = static_cast<PacketType>(*typeValue);
  switch (packetType) {
  case PacketType::MOVEMENT:
  case PacketType::STATE_SNAPSHOT:
    break;
  default:
    return std::unexpected(PacketError::UNKNOWN_TYPE);
  }

  return PacketHeader{*version, packetType, *payloadSize};
}

class PayloadWriter {
public:
  PayloadWriter() = default;

  template <std::integral T> void write(T value) {
    auto networkOrder = toBigEndian(value);
    auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(networkOrder);
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  }

  void writeByte(std::uint8_t value) {
    buffer_.push_back(static_cast<std::byte>(value));
  }

  void writePadding(std::size_t length) {
    buffer_.insert(buffer_.end(), length, std::byte{0});
  }

  [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte> {
    return buffer_;
  }

  [[nodiscard]] auto release() && -> std::vector<std::byte> {
    return std::move(buffer_);
  }

private:
  std::vector<std::byte> buffer_;
};

class PayloadReader {
public:
  explicit PayloadReader(std::span<const std::byte> data) noexcept
      : data_{data} {}

  template <std::integral T> auto read() -> PacketResult<T> {
    if (offset_ + sizeof(T) > data_.size()) {
      return std::unexpected(PacketError::TRUNCATED);
    }
    std::array<std::byte, sizeof(T)> raw{};
    auto view = data_.subspan(offset_, sizeof(T));
    std::ranges::copy(view, raw.begin());
    offset_ += sizeof(T);
    return fromBigEndian(std::bit_cast<T>(raw));
  }

  auto readByte() -> PacketResult<std::uint8_t> { return read<std::uint8_t>(); }

  auto skip(std::size_t length) -> PacketResult<void> {
    if (offset_ + length > data_.size()) {
      return std::unexpected(PacketError::TRUNCATED);
    }
    offset_ += length;
    return {};
  }

  [[nodiscard]] auto remaining() const noexcept -> std::size_t {
    return data_.size() - offset_;
  }

private:
  std::span<const std::byte> data_;
  std::size_t offset_{};
};

[[nodiscard]] inline auto encodePayload(const MovementPacket &packet)
    -> std::vector<std::byte> {
  constexpr std::size_t RESERVED_BYTES = 3U;
  PayloadWriter writer;
  writer.write<std::uint32_t>(packet.player);
  writer.writeByte(static_cast<std::uint8_t>(packet.direction));
  writer.writePadding(RESERVED_BYTES);
  return std::move(writer).release();
}

[[nodiscard]] inline auto encodePayload(const StateSnapshotPacket &packet)
    -> std::vector<std::byte> {
  PayloadWriter writer;
  writer.write<std::uint32_t>(packet.focusPlayer);
  writer.write<std::uint32_t>(
      static_cast<std::uint32_t>(packet.players.size()));
  for (const auto &entry : packet.players) {
    writer.write<std::uint32_t>(entry.player);
    writer.write<std::int32_t>(entry.position.x);
    writer.write<std::int32_t>(entry.position.y);
  }
  return std::move(writer).release();
}

[[nodiscard]] inline auto encode(const MovementPacket &packet)
    -> std::vector<std::byte> {
  auto payload = encodePayload(packet);
  const PacketHeader header{PROTOCOL_VERSION, PacketType::MOVEMENT,
                            static_cast<std::uint32_t>(payload.size())};
  auto headerBytes = encodeHeader(header);

  std::vector<std::byte> buffer;
  buffer.reserve(PACKET_HEADER_SIZE + payload.size());
  buffer.insert(buffer.end(), headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());
  return buffer;
}

[[nodiscard]] inline auto encode(const StateSnapshotPacket &packet)
    -> std::vector<std::byte> {
  auto payload = encodePayload(packet);
  const PacketHeader header{PROTOCOL_VERSION, PacketType::STATE_SNAPSHOT,
                            static_cast<std::uint32_t>(payload.size())};
  auto headerBytes = encodeHeader(header);

  std::vector<std::byte> buffer;
  buffer.reserve(PACKET_HEADER_SIZE + payload.size());
  buffer.insert(buffer.end(), headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());
  return buffer;
}

[[nodiscard]] inline auto decodeMovement(std::span<const std::byte> payload)
    -> PacketResult<MovementPacket> {
  constexpr std::size_t RESERVED_BYTES = 3U;
  PayloadReader reader{payload};
  auto playerId = reader.read<std::uint32_t>();
  if (!playerId) {
    return std::unexpected(playerId.error());
  }
  auto directionRaw = reader.readByte();
  if (!directionRaw) {
    return std::unexpected(directionRaw.error());
  }
  auto skipResult = reader.skip(RESERVED_BYTES);
  if (!skipResult) {
    return std::unexpected(skipResult.error());
  }

  auto directionValue = *directionRaw;
  if (directionValue > static_cast<std::uint8_t>(Direction::RIGHT)) {
    return std::unexpected(PacketError::INVALID_PAYLOAD);
  }

  return MovementPacket{*playerId, static_cast<Direction>(directionValue)};
}

[[nodiscard]] inline auto
decodeStateSnapshot(std::span<const std::byte> payload)
    -> PacketResult<StateSnapshotPacket> {
  PayloadReader reader{payload};
  auto focusId = reader.read<std::uint32_t>();
  if (!focusId) {
    return std::unexpected(focusId.error());
  }
  auto count = reader.read<std::uint32_t>();
  if (!count) {
    return std::unexpected(count.error());
  }

  StateSnapshotPacket packet{};
  packet.focusPlayer = *focusId;
  packet.players.reserve(*count);

  for (std::uint32_t index = 0; index < *count; ++index) {
    auto playerId = reader.read<std::uint32_t>();
    if (!playerId) {
      return std::unexpected(playerId.error());
    }
    auto positionX = reader.read<std::int32_t>();
    if (!positionX) {
      return std::unexpected(positionX.error());
    }
    auto positionY = reader.read<std::int32_t>();
    if (!positionY) {
      return std::unexpected(positionY.error());
    }

    packet.players.emplace_back(
        PlayerState{*playerId, Position{*positionX, *positionY}});
  }

  if (reader.remaining() != 0) {
    return std::unexpected(PacketError::SIZE_MISMATCH);
  }

  return packet;
}

using PacketVariant = std::variant<MovementPacket, StateSnapshotPacket>;

[[nodiscard]] inline auto decodePacket(const PacketHeader &header,
                                       std::span<const std::byte> payload)
    -> PacketResult<PacketVariant> {
  if (payload.size() != header.payloadSize) {
    return std::unexpected(PacketError::SIZE_MISMATCH);
  }

  switch (header.type) {
  case PacketType::MOVEMENT: {
    auto packet = decodeMovement(payload);
    if (!packet) {
      return std::unexpected(packet.error());
    }
    return PacketVariant{*packet};
  }
  case PacketType::STATE_SNAPSHOT: {
    auto packet = decodeStateSnapshot(payload);
    if (!packet) {
      return std::unexpected(packet.error());
    }
    return PacketVariant{*packet};
  }
  default:
    return std::unexpected(PacketError::UNKNOWN_TYPE);
  }
}

} // namespace Moonlapse::Protocol
