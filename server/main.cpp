#include "network.hpp"
#include "packets.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <print>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using Moonlapse::Net::SocketError;
using Moonlapse::Net::SocketErrorCode;
using Moonlapse::Net::SocketResult;
using Moonlapse::Net::TcpListener;
using Moonlapse::Net::TcpSocket;

namespace Protocol = Moonlapse::Protocol;

namespace {

constexpr std::uint16_t serverPort = 40500;
constexpr int gridWidth = 40;
constexpr int gridHeight = 20;

template <typename... Handlers> struct Overloaded : Handlers... {
  using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

[[nodiscard]] auto describePacketError(Protocol::PacketError error)
    -> std::string_view {
  using Protocol::PacketError;
  switch (error) {
  case PacketError::VersionMismatch:
    return "version mismatch";
  case PacketError::UnknownType:
    return "unknown packet type";
  case PacketError::Truncated:
    return "truncated payload";
  case PacketError::SizeMismatch:
    return "size mismatch";
  case PacketError::InvalidPayload:
    return "invalid payload";
  }
  return "unclassified error";
}

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

void applyMovement(Protocol::Position &position,
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

class GameServer {
public:
  explicit GameServer(TcpListener listener) noexcept
      : listener{std::move(listener)} {}

  void run() {
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

private:
  struct Session {
    Session(Protocol::PlayerId playerIdentifier, TcpSocket &&socket) noexcept
        : playerId{playerIdentifier}, socket{std::move(socket)} {}

    auto send(std::span<const std::byte> payload) -> SocketResult<void> {
      std::scoped_lock guard{sendMutex};
      return socket.sendAll(payload);
    }

    Protocol::PlayerId playerId;
    TcpSocket socket;
    std::mutex sendMutex;
  };

  struct PlayerEntry {
    Protocol::Position position;
    std::shared_ptr<Session> session;
  };

  void registerPlayer(TcpSocket socket) {
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

  void handleClient(const std::shared_ptr<Session> &session) {
    while (true) {
      auto headerBytes =
          session->socket.receiveExact(Protocol::packetHeaderSize);
      if (!headerBytes) {
        logSocketError("receive header", session->playerId,
                       headerBytes.error());
        break;
      }

      auto &headerBuffer = headerBytes.value();
      auto headerResult =
          Protocol::decodeHeader(std::span<const std::byte>{headerBuffer});
      if (!headerResult) {
        std::println("[server] packet header error for player {}: {}",
                     session->playerId,
                     describePacketError(headerResult.error()));
        break;
      }

      auto packetHeader = headerResult.value();
      auto payloadBytes =
          session->socket.receiveExact(packetHeader.payloadSize);
      if (!payloadBytes) {
        logSocketError("receive payload", session->playerId,
                       payloadBytes.error());
        break;
      }

      auto &payloadBuffer = payloadBytes.value();
      auto packetResult = Protocol::decodePacket(
          packetHeader, std::span<const std::byte>{payloadBuffer});
      if (!packetResult) {
        std::println("[server] packet decode error for player {}: {}",
                     session->playerId,
                     describePacketError(packetResult.error()));
        break;
      }

      std::visit(Overloaded{[&](const Protocol::MovementPacket &movement) {
                              handleMovement(session, movement);
                            },
                            [](const Protocol::StateSnapshotPacket &) {
                              // Clients should not send snapshots back to the
                              // server.
                            }},
                 packetResult.value());
    }

    session->socket.shutdown();
    session->socket.close();
    removePlayer(session->playerId);
    broadcastState();
    std::println("[server] player {} disconnected", session->playerId);
  }

  void handleMovement(const std::shared_ptr<Session> &session,
                      const Protocol::MovementPacket &movement) {
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
      auto &position = entry->second.position;
      Protocol::Position previous = position;
      applyMovement(position, movement.direction);
      updated = previous.x != position.x || previous.y != position.y;
    }

    if (updated) {
      broadcastState();
    }
  }

  static void logSocketError(std::string_view action,
                             Protocol::PlayerId playerIdentifier,
                             const SocketError &error) {
    if (error.code == SocketErrorCode::ConnectionClosed) {
      std::println("[server] player {} closed the connection",
                   playerIdentifier);
      return;
    }

    std::println("[server] {} failed for player {}: {}", action,
                 playerIdentifier, error.message);
  }

  [[nodiscard]] auto gatherSnapshot(Protocol::PlayerId focus) const
      -> Protocol::StateSnapshotPacket {
    Protocol::StateSnapshotPacket snapshot{};
    snapshot.focusPlayer = focus;

    std::scoped_lock guard{playersMutex};
    snapshot.players.reserve(players.size());
    for (const auto &[playerIdentifier, entry] : players) {
      snapshot.players.emplace_back(
          Protocol::PlayerState{playerIdentifier, entry.position});
    }
    return snapshot;
  }

  [[nodiscard]] auto snapshotSessions() const
      -> std::vector<std::shared_ptr<Session>> {
    std::vector<std::shared_ptr<Session>> sessions;
    std::scoped_lock guard{playersMutex};
    sessions.reserve(players.size());
    for (const auto &[playerIdentifier, entry] : players) {
      sessions.push_back(entry.session);
    }
    return sessions;
  }

  void broadcastState(Protocol::PlayerId focus = 0) {
    auto snapshot = gatherSnapshot(focus);
    auto encoded = Protocol::encode(snapshot);
    auto recipients = snapshotSessions();

    for (const auto &recipient : recipients) {
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

  SocketResult<void> sendSnapshot(const std::shared_ptr<Session> &session,
                                  Protocol::PlayerId focus) {
    auto snapshot = gatherSnapshot(focus);
    auto encoded = Protocol::encode(snapshot);
    return session->send(std::span<const std::byte>{encoded});
  }

  void removePlayer(Protocol::PlayerId playerIdentifier) {
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

  [[nodiscard]] static auto spawnPosition(Protocol::PlayerId playerIdentifier)
      -> Protocol::Position {
    auto positionIndex = static_cast<std::int32_t>(playerIdentifier - 1);
    auto coordinateX = positionIndex % gridWidth;
    auto coordinateY = (positionIndex / gridWidth) % gridHeight;
    return Protocol::Position{coordinateX, coordinateY};
  }

  TcpListener listener;
  std::atomic<Protocol::PlayerId> nextId{1};
  mutable std::mutex playersMutex;
  std::unordered_map<Protocol::PlayerId, PlayerEntry> players;
  std::vector<std::jthread> workers;
};

} // namespace

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
