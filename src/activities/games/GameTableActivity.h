#pragma once

#include <TableGame.h>
#include <TableSession.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>

#include "activities/Activity.h"
#include "activities/games/GameView.h"
#include "components/OptionPopup.h"

class EspNowLink;

// The Games "table": hosts or joins a table over ESP-NOW (or plays solo
// against CPU seats) and runs whichever game the host picks on it. The table
// session lives exactly as long as this activity, so switching games never
// drops anyone; leaving the activity stands up (guest) or closes the table
// (host).
//
// Sub-screens are drawn in place rather than pushed as activities: a pushed
// activity would stop this loop from pumping the radio and every guest would
// time out behind a dialog.
class GameTableActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Host, Join, Solo };

  GameTableActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode,
                    table::GameId soloGame = table::GameId::None);
  ~GameTableActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override;

 private:
  enum class Screen : uint8_t { RadioFailed, Browse, Joining, Notice, Lobby, Game };
  enum class LobbyAction : uint8_t { Play, AddCpu, RemoveCpu, Close };

  struct LobbyItem {
    LobbyAction action;
    table::GameId game;
  };

  struct SeatView {
    table::SeatStatus status;
    char name[table::NAME_LEN + 1];
  };

  struct TableView {
    char host[table::NAME_LEN + 1];
    uint8_t players;
    uint8_t maxPlayers;
    table::GameId game;
    table::Phase phase;
  };

  // Everything render() draws, published by the loop task under
  // snapshotMutex. The render task never touches the session itself.
  struct Snapshot {
    Screen screen = Screen::Browse;
    table::TableSession::Role role = table::TableSession::Role::None;
    table::TableSession::Status status = table::TableSession::Status::Idle;
    table::Phase phase = table::Phase::Lobby;
    table::GameId gameId = table::GameId::None;
    uint8_t localSlot = table::NO_SLOT;
    int8_t localSeat = -1;
    bool actionPending = false;
    bool viewCurrent = false;
    bool botTakeover = false;
    uint8_t gameSeats = 0;
    uint8_t gameSlots[table::MAX_SLOTS] = {};
    SeatView seats[table::MAX_SLOTS] = {};
    uint8_t tableCount = 0;
    TableView tables[table::MAX_TABLES] = {};
    uint8_t view[table::MAX_GAME_STATE] = {};
    uint16_t viewLen = 0;
    GameCursor cursor;
    int8_t selection = 0;
    StrId notice = StrId::STR_GAMES_CONNECTION_LOST;
  };

  // Tap targets recorded while drawing, so touch routing always matches the
  // frame on screen. kind is one of the TARGET_* values in the .cpp.
  struct TouchTarget {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint8_t kind;
    int16_t value;
  };
  static constexpr int MAX_TARGETS = 16;

  // Loop task
  Screen currentScreen() const;
  void handleTap(int x, int y, uint32_t nowMs);
  void handleInput(uint32_t nowMs);
  void handleBrowseInput(uint32_t nowMs);
  void handleLobbyInput(uint32_t nowMs);
  void handleGameInput(uint32_t nowMs);
  void runLobbyItem(const LobbyItem& item, uint32_t nowMs);
  // Lobby rows for a role; shared by input handling and drawing so the two
  // can never disagree about what row N is.
  static int lobbyItemsFor(table::TableSession::Role role, bool anyBot, LobbyItem* items, int capacity);
  int buildLobbyItems(LobbyItem* items, int capacity) const;
  void showGameMenu(bool gameOver);
  void confirmLeave();
  void exitToLibrary();
  void syncViewCursor();
  void publishSnapshot();

  // Render task
  void drawBrowse(const Snapshot& s);
  void drawJoining(const Snapshot& s);
  void drawNotice(const Snapshot& s);
  void drawLobby(const Snapshot& s);
  void drawGame(const Snapshot& s);
  void drawCenteredLines(const char* primary, const char* secondary);
  void seatLabel(const Snapshot& s, uint8_t slot, char* out, size_t size) const;
  const char* statusLine(const Snapshot& s, const table::Game& game, const char* const* names, char* buf,
                         size_t size) const;
  const char* tableTitle(const Snapshot& s, char* buf, size_t size) const;

  Mode mode;
  table::GameId soloGame;
  table::TableSession session;
  std::unique_ptr<EspNowLink> link;
  bool radioFailed = false;
  bool exiting = false;

  // Loop-task UI state.
  GameCursor cursor;
  table::GameId cursorGame = table::GameId::None;
  int selection = 0;
  uint32_t seenRevision = 0xFFFFFFFFu;
  bool uiDirty = true;
  uint32_t lastInputMs = 0;
  // Swallows the Confirm release that launched this screen from the Library.
  bool lockConfirmRelease = false;
  // Set from inside a popup callback: the popup cannot re-show itself while
  // its own callback is running, so the leave prompt opens next pass.
  bool leavePromptPending = false;
  OptionPopup popup;

  // Shared with the render task.
  SemaphoreHandle_t snapshotMutex = nullptr;
  Snapshot shared;
  TouchTarget sharedTargets[MAX_TARGETS] = {};
  uint8_t sharedTargetCount = 0;
  GameTargets sharedGameTargets;

  // Render-task state.
  void addTarget(int x, int y, int w, int h, uint8_t kind, int value);
  TouchTarget renderTargets[MAX_TARGETS] = {};
  uint8_t renderTargetCount = 0;
  GameTargets renderGameTargets;
  Snapshot renderSnap;
  std::unique_ptr<table::Game> renderGame;
  Screen lastRenderedScreen = Screen::RadioFailed;
  int fastRefreshes = 0;
};
