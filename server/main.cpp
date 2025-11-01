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

constexpr std::uint16_t SERVER_PORT = 40500;
constexpr int GRID_WIDTH = 40;
constexpr int GRID_HEIGHT = 20;

template <typename... Handlers> struct Overloaded : Handlers... {
  using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

[[nodiscard]] auto describePacketError(Protocol::PacketError error)
    -> std::string_view {
  using Protocol::PacketError;
  switch (error) {
  case PacketError::VERSION_MISMATCH:
    return "version mismatch";
  case PacketError::UNKNOWN_TYPE:
    return "unknown packet type";
  case PacketError::TRUNCATED:
    return "truncated payload";
  case PacketError::SIZE_MISMATCH:
    return "size mismatch";
  case PacketError::INVALID_PAYLOAD:
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
  case Protocol::Direction::UP:
    --position.y;
    break;
  case Protocol::Direction::DOWN:
    ++position.y;
    break;
  case Protocol::Direction::LEFT:
    --position.x;
    break;
  case Protocol::Direction::RIGHT:
    ++position.x;
    break;
  }

  position.x = clampCoordinate(position.x, GRID_WIDTH);
  position.y = clampCoordinate(position.y, GRID_HEIGHT);
}

class GameServer {
public:
  explicit GameServer(TcpListener listener) noexcept
      : listener_{std::move(listener)} {}

  void run() {
    std::println("[server] waiting for players...");
    while (true) {
      auto connection = listener_.accept();
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

    SocketResult<void> send(std::span<const std::byte> payload) {
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
    auto playerIdentifier = nextId_.fetch_add(1);
    auto session =
        std::make_shared<Session>(playerIdentifier, std::move(socket));
    auto position = spawnPosition(playerIdentifier);

    {
      std::scoped_lock guard{playersMutex_};
      players_.emplace(playerIdentifier, PlayerEntry{position, session});
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

    workers_.emplace_back([this, session]() { handleClient(session); });
  }

  void handleClient(const std::shared_ptr<Session> &session) {
    while (true) {
      auto headerBytes =
          session->socket.receiveExact(Protocol::PACKET_HEADER_SIZE);
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
      std::scoped_lock guard{playersMutex_};
      auto entry = players_.find(session->playerId);
      if (entry == players_.end()) {
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
    if (error.code == SocketErrorCode::CONNECTION_CLOSED) {
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

    std::scoped_lock guard{playersMutex_};
    snapshot.players.reserve(players_.size());
    for (const auto &[playerIdentifier, entry] : players_) {
      snapshot.players.emplace_back(
          Protocol::PlayerState{playerIdentifier, entry.position});
    }
    return snapshot;
  }

  [[nodiscard]] auto snapshotSessions() const
      -> std::vector<std::shared_ptr<Session>> {
    std::vector<std::shared_ptr<Session>> sessions;
    std::scoped_lock guard{playersMutex_};
    sessions.reserve(players_.size());
    for (const auto &[playerIdentifier, entry] : players_) {
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
      std::scoped_lock guard{playersMutex_};
      auto entry = players_.find(playerIdentifier);
      if (entry == players_.end()) {
        return;
      }
      removed = std::move(entry->second.session);
      players_.erase(entry);
    }

    if (removed) {
      removed->socket.shutdown();
      removed->socket.close();
    }
  }

  [[nodiscard]] static auto spawnPosition(Protocol::PlayerId playerIdentifier)
      -> Protocol::Position {
    auto positionIndex = static_cast<std::int32_t>(playerIdentifier - 1);
    auto coordinateX = positionIndex % GRID_WIDTH;
    auto coordinateY = (positionIndex / GRID_WIDTH) % GRID_HEIGHT;
    return Protocol::Position{coordinateX, coordinateY};
  }

  TcpListener listener_;
  std::atomic<Protocol::PlayerId> nextId_{1};
  mutable std::mutex playersMutex_;
  std::unordered_map<Protocol::PlayerId, PlayerEntry> players_;
  std::vector<std::jthread> workers_;
};

} // namespace

auto main() -> int {
  constexpr std::string_view LISTEN_ADDRESS = "0.0.0.0";
  auto listenerResult = TcpListener::bind(LISTEN_ADDRESS, SERVER_PORT);
  if (!listenerResult) {
    std::println("[server] bind failed: {}", listenerResult.error().message);
    return 1;
  }

  auto listener = std::move(listenerResult.value());
  if (auto listenResult = listener.listen(); !listenResult) {
    std::println("[server] listen failed: {}", listenResult.error().message);
    return 1;
  }

  std::println("[server] listening on {}:{}", LISTEN_ADDRESS, SERVER_PORT);
  GameServer server{std::move(listener)};
  server.run();
  return 0;
}
