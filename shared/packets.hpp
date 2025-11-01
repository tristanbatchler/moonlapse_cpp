#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Moonlapse::Protocol {

inline constexpr std::uint16_t protocolVersion = 1;
inline constexpr std::size_t packetHeaderSize =
    sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

enum class PacketType : std::uint8_t {
  Movement = 1,
  StateSnapshot = 2,
  Chat = 3,
};

enum class Direction : std::uint8_t {
  Up = 0,
  Down = 1,
  Left = 2,
  Right = 3,
};

using PlayerId = std::uint32_t;

struct Position {
  std::int32_t x{};
  std::int32_t y{};
};

struct PacketHeader {
  std::uint16_t version{protocolVersion};
  PacketType type{PacketType::Movement};
  std::uint32_t payloadSize{};
};

struct MovementPacket {
  PlayerId player{};
  Direction direction{Direction::Up};
};

struct PlayerState {
  PlayerId player{};
  Position position{};
};

struct StateSnapshotPacket {
  PlayerId focusPlayer{};
  std::vector<PlayerState> players;
};

struct ChatPacket {
  PlayerId player{};
  std::string message;
};

enum class PacketError : std::uint8_t {
  VersionMismatch,
  UnknownType,
  Truncated,
  SizeMismatch,
  InvalidPayload,
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
    return std::unexpected(PacketError::Truncated);
  }

  std::array<std::byte, sizeof(T)> raw{};
  std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(offset), sizeof(T),
              raw.begin());
  offset += sizeof(T);
  return fromBigEndian(std::bit_cast<T>(raw));
}

[[nodiscard]] inline auto encodeHeader(PacketHeader header) noexcept
    -> std::array<std::byte, packetHeaderSize> {
  std::array<std::byte, packetHeaderSize> buffer{};
  std::size_t offset = 0;
  writeIntegral(buffer, offset, header.version);
  writeIntegral(buffer, offset, static_cast<std::uint16_t>(header.type));
  writeIntegral(buffer, offset, header.payloadSize);
  return buffer;
}

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
  case PacketType::Chat:
    break;
  default:
    return std::unexpected(PacketError::UnknownType);
  }

  return PacketHeader{
      .version = *version, .type = decodedType, .payloadSize = *payloadSize};
}

class PayloadWriter {
public:
  PayloadWriter() = default;

  template <std::integral T> void write(T value) {
    auto networkOrder = toBigEndian(value);
    auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(networkOrder);
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
  }

  void writeByte(std::uint8_t value) {
    m_buffer.push_back(static_cast<std::byte>(value));
  }

  void writePadding(std::size_t length) {
    m_buffer.insert(m_buffer.end(), length, std::byte{0});
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

  auto skip(std::size_t length) -> PacketResult<void> {
    if (m_offset + length > m_payload.size()) {
      return std::unexpected(PacketError::Truncated);
    }

    m_offset += length;
    return {};
  }

  [[nodiscard]] auto remaining() const noexcept -> std::size_t {
    return m_payload.size() - m_offset;
  }

private:
  std::span<const std::byte> m_payload;
  std::size_t m_offset{};
};

[[nodiscard]] inline auto encodePayload(const MovementPacket &packet)
    -> std::vector<std::byte> {
  constexpr std::size_t reservedBytes = 3;
  PayloadWriter writer;
  writer.write<PlayerId>(packet.player);
  writer.writeByte(static_cast<std::uint8_t>(packet.direction));
  writer.writePadding(reservedBytes);
  return std::move(writer).release();
}

[[nodiscard]] inline auto encodePayload(const StateSnapshotPacket &packet)
    -> std::vector<std::byte> {
  PayloadWriter writer;
  writer.write<PlayerId>(packet.focusPlayer);
  writer.write<std::uint32_t>(
      static_cast<std::uint32_t>(packet.players.size()));
  for (const auto &entry : packet.players) {
    writer.write<std::uint32_t>(entry.player);
    writer.write<std::int32_t>(entry.position.x);
    writer.write<std::int32_t>(entry.position.y);
  }
  return std::move(writer).release();
}

[[nodiscard]] inline auto encodePayload(const ChatPacket &packet)
    -> std::vector<std::byte> {
  PayloadWriter writer;
  writer.write<PlayerId>(packet.player);
  for (char chr : packet.message) {
    writer.writeByte(static_cast<std::uint8_t>(chr));
  }
  return std::move(writer).release();
}

[[nodiscard]] inline auto encode(const MovementPacket &packet)
    -> std::vector<std::byte> {
  auto payload = encodePayload(packet);
  PacketHeader header{.version = protocolVersion,
                      .type = PacketType::Movement,
                      .payloadSize =
                          static_cast<std::uint32_t>(payload.size())};
  auto headerBytes = encodeHeader(header);

  std::vector<std::byte> buffer;
  buffer.reserve(packetHeaderSize + payload.size());
  buffer.insert(buffer.end(), headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());
  return buffer;
}

[[nodiscard]] inline auto encode(const StateSnapshotPacket &packet)
    -> std::vector<std::byte> {
  auto payload = encodePayload(packet);
  PacketHeader header{.version = protocolVersion,
                      .type = PacketType::StateSnapshot,
                      .payloadSize =
                          static_cast<std::uint32_t>(payload.size())};
  auto headerBytes = encodeHeader(header);

  std::vector<std::byte> buffer;
  buffer.reserve(packetHeaderSize + payload.size());
  buffer.insert(buffer.end(), headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());
  return buffer;
}

[[nodiscard]] inline auto encode(const ChatPacket &packet)
    -> std::vector<std::byte> {
  auto payload = encodePayload(packet);
  PacketHeader header{.version = protocolVersion,
                      .type = PacketType::Chat,
                      .payloadSize =
                          static_cast<std::uint32_t>(payload.size())};
  auto headerBytes = encodeHeader(header);

  std::vector<std::byte> buffer;
  buffer.reserve(packetHeaderSize + payload.size());
  buffer.insert(buffer.end(), headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());
  return buffer;
}

[[nodiscard]] inline auto decodeMovement(std::span<const std::byte> payload)
    -> PacketResult<MovementPacket> {
  constexpr std::size_t reservedBytes = 3;
  PayloadReader reader{payload};
  auto playerId = reader.read<std::uint32_t>();
  if (!playerId) {
    return std::unexpected(playerId.error());
  }

  auto directionRaw = reader.readByte();
  if (!directionRaw) {
    return std::unexpected(directionRaw.error());
  }

  auto skipResult = reader.skip(reservedBytes);
  if (!skipResult) {
    return std::unexpected(skipResult.error());
  }

  auto directionValue = *directionRaw;
  if (directionValue > static_cast<std::uint8_t>(Direction::Right)) {
    return std::unexpected(PacketError::InvalidPayload);
  }

  return MovementPacket{.player = *playerId,
                        .direction = static_cast<Direction>(directionValue)};
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
        PlayerState{.player = *playerId,
                    .position = Position{.x = *positionX, .y = *positionY}});
  }

  if (reader.remaining() != 0) {
    return std::unexpected(PacketError::SizeMismatch);
  }

  return packet;
}

[[nodiscard]] inline auto decodeChat(std::span<const std::byte> payload)
    -> PacketResult<ChatPacket> {
  PayloadReader reader{payload};
  auto playerId = reader.read<std::uint32_t>();
  if (!playerId) {
    return std::unexpected(playerId.error());
  }

  std::size_t messageLength = reader.remaining();
  std::string message;
  message.resize(messageLength);
  for (std::size_t i = 0; i < messageLength; ++i) {
    auto byteResult = reader.readByte();
    if (!byteResult) {
      return std::unexpected(byteResult.error());
    }
    message[i] = static_cast<char>(*byteResult);
  }

  return ChatPacket{.player = *playerId, .message = std::move(message)};
}

using PacketVariant =
    std::variant<MovementPacket, StateSnapshotPacket, ChatPacket>;

[[nodiscard]] inline auto decodePacket(const PacketHeader &header,
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
  case PacketType::StateSnapshot: {
    auto packet = decodeStateSnapshot(payload);
    if (!packet) {
      return std::unexpected(packet.error());
    }
    return PacketVariant{*packet};
  }
  case PacketType::Chat: {
    auto packet = decodeChat(payload);
    if (!packet) {
      return std::unexpected(packet.error());
    }
    return PacketVariant{*packet};
  }
  default:
    return std::unexpected(PacketError::UnknownType);
  }
}

} // namespace Moonlapse::Protocol
