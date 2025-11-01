#include "network.hpp"
#include "packets.hpp"

#ifdef _WIN32
#include <curses.h>
#else
#include <ncurses.h>
#endif

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using Moonlapse::Net::SocketError;
using Moonlapse::Net::SocketErrorCode;
using Moonlapse::Net::SocketResult;
using Moonlapse::Net::TcpSocket;

namespace Protocol = Moonlapse::Protocol;

namespace {

constexpr std::string_view serverAddress = "127.0.0.1";
constexpr std::uint16_t serverPort = 40500;
constexpr int gridWidth = 40;
constexpr int gridHeight = 20;
constexpr auto refreshDelay = std::chrono::milliseconds{50};

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
  return "unclassified packet error";
}

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

  CursesSession(const CursesSession &) = delete;
  auto operator=(const CursesSession &) -> CursesSession & = delete;
  CursesSession(CursesSession &&) = delete;
  auto operator=(CursesSession &&) -> CursesSession & = delete;

  WINDOW *window{};
  bool active{false};
};

struct ClientState {
  std::unordered_map<Protocol::PlayerId, Protocol::Position> players;
  std::optional<Protocol::PlayerId> selfId;
  mutable std::mutex mutex;
};

[[nodiscard]] auto keyToDirection(int key)
    -> std::optional<Protocol::Direction> {
  switch (key) {
  case KEY_UP:
    return Protocol::Direction::Up;
  case KEY_DOWN:
    return Protocol::Direction::Down;
  case KEY_LEFT:
    return Protocol::Direction::Left;
  case KEY_RIGHT:
    return Protocol::Direction::Right;
  default:
    return std::nullopt;
  }
}

void drawFrame(const std::vector<Protocol::PlayerState> &snapshot,
               std::optional<Protocol::PlayerId> selfId) {
  erase();

  for (int row = 0; row <= gridHeight + 1; ++row) {
    for (int column = 0; column <= gridWidth + 1; ++column) {
      auto borderCell = row == 0 || row == gridHeight + 1 || column == 0 ||
                        column == gridWidth + 1;
      auto borderGlyphChar = static_cast<unsigned char>(borderCell ? '#' : ' ');
      auto borderCharacter = static_cast<chtype>(borderGlyphChar);
      mvaddch(row, column, borderCharacter);
    }
  }

  for (const auto &player : snapshot) {
    auto positionRow = static_cast<int>(player.position.y) + 1;
    auto positionColumn = static_cast<int>(player.position.x) + 1;
    if (positionColumn < 1 || positionColumn > gridWidth || positionRow < 1 ||
        positionRow > gridHeight) {
      continue;
    }
    auto isSelfPlayer = selfId.has_value() && player.player == *selfId;
    auto glyphCharacter = static_cast<unsigned char>(isSelfPlayer ? '@' : 'o');
    auto playerGlyph = static_cast<chtype>(glyphCharacter);
    mvaddch(positionRow, positionColumn, playerGlyph);
  }

  auto infoRowIndex = gridHeight + 3;
  std::string controlsMessage = "Controls: arrow keys to move, q to quit.";
  mvaddnstr(infoRowIndex, 1, controlsMessage.c_str(),
            static_cast<int>(controlsMessage.size()));
  if (selfId) {
    auto idText =
        std::format("You are player {}", static_cast<unsigned>(*selfId));
    mvaddnstr(infoRowIndex + 1, 1, idText.c_str(),
              static_cast<int>(idText.size()));
  }

  refresh();
}

void receiverLoop(const std::shared_ptr<TcpSocket> &socket, ClientState &state,
                  std::atomic_bool &running, std::atomic_bool &connectionActive,
                  std::mutex &errorMutex, std::string &lastError) {
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

    auto &headerBuffer = headerBytes.value();
    auto headerResult =
        Protocol::decodeHeader(std::span<const std::byte>{headerBuffer});
    if (!headerResult) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = std::string{describePacketError(headerResult.error())};
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    auto packetHeader = headerResult.value();
    auto payloadBytes = socket->receiveExact(packetHeader.payloadSize);
    if (!payloadBytes) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = payloadBytes.error().message;
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    auto &payloadBuffer = payloadBytes.value();
    auto packetResult = Protocol::decodePacket(
        packetHeader, std::span<const std::byte>{payloadBuffer});
    if (!packetResult) {
      {
        std::scoped_lock guard{errorMutex};
        lastError = std::string{describePacketError(packetResult.error())};
      }
      connectionActive.store(false);
      running.store(false);
      return;
    }

    std::visit(
        Overloaded{[&](const Protocol::StateSnapshotPacket &snapshot) {
                     std::unordered_map<Protocol::PlayerId, Protocol::Position>
                         updated;
                     updated.reserve(snapshot.players.size());
                     for (const auto &player : snapshot.players) {
                       updated.emplace(player.player, player.position);
                     }

                     std::scoped_lock guard{state.mutex};
                     state.players = std::move(updated);
                     if (snapshot.focusPlayer != 0) {
                       state.selfId = snapshot.focusPlayer;
                     }
                   },
                   [](const Protocol::MovementPacket &) {
                     // Movement updates are broadcast as state snapshots;
                     // ignore stray packets.
                   }},
        packetResult.value());
  }
}

auto sendMovement(const std::shared_ptr<TcpSocket> &socket, ClientState &state,
                  Protocol::Direction direction, std::mutex &sendMutex)
    -> SocketResult<void> {
  std::optional<Protocol::PlayerId> playerIdentifier;
  {
    std::scoped_lock guard{state.mutex};
    playerIdentifier = state.selfId;
  }

  if (!playerIdentifier) {
    return {};
  }

  Protocol::MovementPacket packet{*playerIdentifier, direction};
  auto encoded = Protocol::encode(packet);
  std::scoped_lock guard{sendMutex};
  return socket->sendAll(std::span<const std::byte>{encoded});
}

} // namespace

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

    while (running.load()) {
      if (!connectionActive.load()) {
        break;
      }

      auto inputKey = getch();
      if (inputKey == 'q' || inputKey == 'Q') {
        running.store(false);
        break;
      }

      if (auto direction = keyToDirection(inputKey); direction) {
        if (auto result =
                sendMovement(connection, state, *direction, sendMutex);
            !result) {
          {
            std::scoped_lock guard{errorMutex};
            lastError = result.error().message;
          }
          running.store(false);
          connectionActive.store(false);
          break;
        }
      }

      std::vector<Protocol::PlayerState> snapshot;
      std::optional<Protocol::PlayerId> selfId;
      {
        std::scoped_lock guard{state.mutex};
        snapshot.reserve(state.players.size());
        for (const auto &[playerIdentifier, position] : state.players) {
          snapshot.emplace_back(
              Protocol::PlayerState{playerIdentifier, position});
        }
        selfId = state.selfId;
      }

      drawFrame(snapshot, selfId);
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
