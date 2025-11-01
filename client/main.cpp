#include "network.hpp"
#include "packets.hpp"

#ifdef _WIN32
#include <curses.h>
#else
#include <ncurses.h>
#endif

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <deque>
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
constexpr std::size_t maxChatMessages = 8;
constexpr std::size_t maxChatInputLength = 200;
constexpr int escapeKeyCode = 27;
constexpr int deleteKeyCode = 127;
constexpr int backspaceKeyCode = 8;
constexpr int printableAsciiMin = 32;
constexpr int printableAsciiMax = 126;

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

enum class LoopAction : std::uint8_t { Continue, Stop };

struct RenderState {
  std::vector<Protocol::PlayerState> snapshot;
  std::vector<ChatEntry> chatMessages;
  std::optional<Protocol::PlayerId> selfId;
};

struct RuntimeContext {
  std::reference_wrapper<std::mutex> sendMutex;
  std::reference_wrapper<std::mutex> errorMutex;
  std::reference_wrapper<std::string> lastError;
  std::reference_wrapper<std::atomic_bool> running;
  std::reference_wrapper<std::atomic_bool> connectionActive;
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

void drawPlayers(const std::vector<Protocol::PlayerState> &snapshot,
                 std::optional<Protocol::PlayerId> selfId) {
  for (const auto &player : snapshot) {
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

auto drawInfoLines(std::optional<Protocol::PlayerId> selfId,
                   const ChatUiState &chatUi) -> int {
  const int infoRowIndex = gridHeight + 3;
  const std::string controlsMessage =
      "Controls: arrow keys to move, q to quit, Enter to chat.";
  mvaddnstr(infoRowIndex, 1, controlsMessage.c_str(),
            static_cast<int>(controlsMessage.size()));

  int nextRow = infoRowIndex + 1;
  if (selfId) {
    const auto idText =
        std::format("You are player {}", static_cast<unsigned>(*selfId));
    mvaddnstr(nextRow, 1, idText.c_str(), static_cast<int>(idText.size()));
    ++nextRow;
  }

  const std::string chatHint = chatUi.active
                                   ? "Chat mode: Enter to send, Esc to cancel."
                                   : "Press Enter to chat with other players.";
  mvaddnstr(nextRow, 1, chatHint.c_str(), static_cast<int>(chatHint.size()));
  return nextRow + 2;
}

auto drawChatLog(int startRow, std::span<const ChatEntry> chatMessages) -> int {
  mvaddnstr(startRow, 1, "Recent chat:", 12);
  int row = startRow + 1;
  for (const auto &entry : chatMessages) {
    const auto line =
        std::format("[{}] {}", static_cast<unsigned>(entry.player),
                    entry.message);
    mvaddnstr(row, 1, line.c_str(), static_cast<int>(line.size()));
    ++row;
  }
  return row + 1;
}

void drawChatPrompt(int row, const ChatUiState &chatUi) {
  constexpr std::string_view promptPrefix = "Chat> ";
  auto prompt = std::format("{}{}", promptPrefix, chatUi.input);
  if (chatUi.active) {
    prompt.push_back('_');
  }

  mvaddnstr(row, 1, prompt.c_str(), static_cast<int>(prompt.size()));
  if (chatUi.active) {
    move(row, static_cast<int>(promptPrefix.size() + chatUi.input.size()));
  }
  curs_set(chatUi.active ? 1 : 0);
}

void drawFrame(const std::vector<Protocol::PlayerState> &snapshot,
               std::optional<Protocol::PlayerId> selfId,
               std::span<const ChatEntry> chatMessages,
               const ChatUiState &chatUi) {
  erase();
  drawBorder();
  drawPlayers(snapshot, selfId);
  auto nextRow = drawInfoLines(selfId, chatUi);
  nextRow = drawChatLog(nextRow, chatMessages);
  drawChatPrompt(nextRow, chatUi);
  refresh();
}

auto sendMovement(const std::shared_ptr<TcpSocket> &socket, ClientState &state,
          Protocol::Direction direction, std::mutex &sendMutex)
  -> SocketResult<void>;

auto sendChat(const std::shared_ptr<TcpSocket> &socket, ClientState &state,
        std::string_view message, std::mutex &sendMutex)
  -> SocketResult<void>;

auto handleSnapshot(ClientState &state,
                    const Protocol::StateSnapshotPacket &snapshot) -> void {
  std::unordered_map<Protocol::PlayerId, Protocol::Position> updated;
  updated.reserve(snapshot.players.size());
  for (const auto &player : snapshot.players) {
    updated.emplace(player.player, player.position);
  }

  std::scoped_lock guard{state.mutex};
  state.players = std::move(updated);
  if (snapshot.focusPlayer != 0) {
    state.selfId = snapshot.focusPlayer;
  }
}

auto handleChat(ClientState &state, const Protocol::ChatPacket &chat) -> void {
  ChatEntry entry{.player = chat.player, .message = chat.message};
  std::scoped_lock guard{state.mutex};
  state.chatLog.push_back(std::move(entry));
  while (state.chatLog.size() > maxChatMessages) {
    state.chatLog.pop_front();
  }
}

void recordSocketFailure(RuntimeContext &runtime, const std::string &message) {
  {
    std::scoped_lock guard{runtime.errorMutex.get()};
    runtime.lastError.get() = message;
  }
  runtime.running.get().store(false);
  runtime.connectionActive.get().store(false);
}

auto handleChatInputKey(int inputKey, ChatUiState &chatState,
                        const std::shared_ptr<TcpSocket> &connection,
                        ClientState &state, RuntimeContext &runtime)
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

auto handleMovementInputKey(int inputKey, ChatUiState &chatState,
                            const std::shared_ptr<TcpSocket> &connection,
                            ClientState &state, RuntimeContext &runtime)
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

auto handleInputKey(int inputKey, ChatUiState &chatState,
                    const std::shared_ptr<TcpSocket> &connection,
                    ClientState &state, RuntimeContext &runtime)
    -> LoopAction {
  if (inputKey == ERR) {
    return LoopAction::Continue;
  }

  if (chatState.active) {
    return handleChatInputKey(inputKey, chatState, connection, state,
                              runtime);
  }

  return handleMovementInputKey(inputKey, chatState, connection, state,
                                runtime);
}

auto gatherRenderState(ClientState &state) -> RenderState {
  RenderState render;
  std::scoped_lock guard{state.mutex};

  render.snapshot.reserve(state.players.size());
  for (const auto &[playerIdentifier, position] : state.players) {
    render.snapshot.emplace_back(
        Protocol::PlayerState{.player = playerIdentifier, .position = position});
  }

  render.chatMessages.reserve(state.chatLog.size());
  for (const auto &entry : state.chatLog) {
    render.chatMessages.push_back(entry);
  }

  render.selfId = state.selfId;
  return render;
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
                     handleSnapshot(state, snapshot);
                   },
                   [](const Protocol::MovementPacket &) {
                     // Movement updates are broadcast as state
                     // snapshots; ignore stray packets.
                   },
                  [&](const Protocol::ChatPacket &chat) {
                    handleChat(state, chat);
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

  Protocol::MovementPacket packet{.player = *playerIdentifier,
                                  .direction = direction};
  auto encoded = Protocol::encode(packet);
  std::scoped_lock guard{sendMutex};
  return socket->sendAll(std::span<const std::byte>{encoded});
}

auto sendChat(const std::shared_ptr<TcpSocket> &socket, ClientState &state,
              std::string_view message, std::mutex &sendMutex)
    -> SocketResult<void> {
  if (message.empty()) {
    return {};
  }

  std::optional<Protocol::PlayerId> playerIdentifier;
  {
    std::scoped_lock guard{state.mutex};
    playerIdentifier = state.selfId;
  }

  if (!playerIdentifier) {
    return {};
  }

  Protocol::ChatPacket packet{.player = *playerIdentifier,
                              .message = std::string{message}};
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
