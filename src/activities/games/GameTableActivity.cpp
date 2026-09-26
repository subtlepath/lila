#include "GameTableActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "EspNowLink.h"
#include "GamePlayerName.h"
#include "components/UITheme.h"
#include "fontIds.h"

using table::GameId;
using table::Phase;
using table::SeatStatus;
using table::TableSession;

namespace {

// A table keeps the device awake (the radio needs full CPU clock), but not
// forever: after this long with no button presses normal auto-sleep resumes.
constexpr uint32_t TABLE_IDLE_SLEEP_MS = 20UL * 60UL * 1000UL;
// Fast refreshes ghost; a half refresh every so often keeps boards crisp.
constexpr int FAST_REFRESHES_PER_CLEAN = 14;
constexpr int SIDE_PADDING = 16;
constexpr int MAX_LOBBY_ITEMS = 8;
constexpr GameId ALL_GAMES[] = {GameId::ConnectFour, GameId::DotsAndBoxes, GameId::LiarsDice, GameId::MurderMystery};

// Touch target kinds.
constexpr uint8_t TARGET_BROWSE_ROW = 1;
constexpr uint8_t TARGET_LOBBY_ROW = 2;
// The header band stands in for Back (menu / leave) on touch boards.
constexpr uint8_t TARGET_HEADER = 3;

class MutexGuard {
 public:
  explicit MutexGuard(SemaphoreHandle_t mutex) : mutex(mutex) {
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
  }
  ~MutexGuard() {
    if (mutex) xSemaphoreGive(mutex);
  }
  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;

 private:
  SemaphoreHandle_t mutex;
};

}  // namespace

GameTableActivity::GameTableActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Mode mode,
                                     const GameId soloGame)
    : Activity("GameTable", renderer, mappedInput), mode(mode), soloGame(soloGame) {}

GameTableActivity::~GameTableActivity() {
  if (snapshotMutex) vSemaphoreDelete(snapshotMutex);
}

void GameTableActivity::onEnter() {
  Activity::onEnter();
  snapshotMutex = xSemaphoreCreateMutex();
  if (!snapshotMutex) LOG_ERR("GAME", "snapshot mutex allocation failed");

  const uint32_t now = millis();
  lastInputMs = now;
  uint8_t mac[table::MAC_LEN] = {};
  char name[table::NAME_LEN + 1];

  if (mode != Mode::Solo) {
    link = makeUniqueNoThrow<EspNowLink>();
    if (!link) {
      LOG_ERR("GAME", "OOM: ESP-NOW link");
    } else if (!link->begin()) {
      link.reset();
    }
    radioFailed = !link;
    if (link) link->localMac(mac);
  }
  if (mode == Mode::Solo) {
    snprintf(name, sizeof(name), "%s", tr(STR_GAMES_YOU));
  } else {
    gamePlayerName(name, sizeof(name));
  }
  session.begin(link.get(), mac, name, esp_random() ^ now);

  if (!radioFailed) {
    switch (mode) {
      case Mode::Host:
        session.host(now);
        break;
      case Mode::Join:
        session.browse();
        break;
      case Mode::Solo:
        session.hostSolo();
        if (soloGame != GameId::None && !session.startGame(soloGame, now)) LOG_ERR("GAME", "cannot start solo game");
        break;
    }
  }
  LOG_INF("GAME", "table open: mode=%u heap=%u", static_cast<unsigned>(mode), static_cast<unsigned>(ESP.getFreeHeap()));
  // Entered from the Library on a Confirm press; its release must not act here.
  lockConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  syncViewCursor();
  publishSnapshot();
  requestUpdate();
}

void GameTableActivity::onExit() {
  session.end();
  if (link) {
    // Let the leave/close frames clear the radio before tearing it down.
    vTaskDelay(pdMS_TO_TICKS(60));
    link->end();
    link.reset();
  }
  renderGame.reset();
  LOG_INF("GAME", "table closed: heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  Activity::onExit();
}

bool GameTableActivity::preventAutoSleep() {
  // Solo play is an ordinary screen; a live table must keep the radio up.
  return link && millis() - lastInputMs < TABLE_IDLE_SLEEP_MS;
}

// --- loop task -------------------------------------------------------------------

GameTableActivity::Screen GameTableActivity::currentScreen() const {
  if (radioFailed) return Screen::RadioFailed;
  switch (session.status()) {
    case TableSession::Status::Idle:
    case TableSession::Status::Browsing:
      return Screen::Browse;
    case TableSession::Status::Joining:
      return Screen::Joining;
    case TableSession::Status::Closed:
    case TableSession::Status::Denied:
    case TableSession::Status::Lost:
      return Screen::Notice;
    case TableSession::Status::Seated:
    case TableSession::Status::Reconnecting:
      break;
  }
  return session.phase() == Phase::Playing && session.view() ? Screen::Game : Screen::Lobby;
}

void GameTableActivity::loop() {
  if (exiting) return;
  const uint32_t now = millis();
  if (link) link->pump(session, now);
  session.tick(now);

  handleInput(now);
  if (exiting) return;

  if (session.revision() != seenRevision || uiDirty) {
    // Other players' moves keep a live table awake as much as our own.
    if (session.revision() != seenRevision) lastInputMs = now;
    seenRevision = session.revision();
    uiDirty = false;
    syncViewCursor();
    publishSnapshot();
    requestUpdate();
  }
}

void GameTableActivity::syncViewCursor() {
  const table::Game* view = session.view();
  if (!view || session.phase() != Phase::Playing) return;
  const GameView* ops = gameViewFor(view->id());
  if (!ops) return;
  if (cursorGame != view->id()) {
    cursorGame = view->id();
    cursor = GameCursor{};
  }
  ops->syncCursor(*view, session.localSeat(), cursor);
}

void GameTableActivity::handleInput(const uint32_t nowMs) {
  if (mappedInput.wasAnyPressed()) lastInputMs = nowMs;
  if (leavePromptPending && !popup.isActive()) {
    leavePromptPending = false;
    confirmLeave();
    return;
  }
  if (popup.handleInput(mappedInput, [this] { uiDirty = true; })) return;

  if (lockConfirmRelease) {
    // A long-press launch has its release consumed before this loop runs, so
    // also drop the lock once Confirm is simply no longer held.
    const bool released = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (released || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) lockConfirmRelease = false;
    if (released) return;
  }

  if (mappedInput.hasTouch()) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      lastInputMs = nowMs;
      handleTap(tx, ty, nowMs);
      return;
    }
  }

  const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture();
  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  switch (currentScreen()) {
    case Screen::RadioFailed:
      if (back || confirm) exitToLibrary();
      break;
    case Screen::Browse:
      handleBrowseInput(nowMs);
      break;
    case Screen::Joining:
      if (back) session.browse();
      break;
    case Screen::Notice:
      if (back || confirm) {
        if (mode == Mode::Join) {
          session.browse();
          selection = 0;
        } else {
          exitToLibrary();
        }
      }
      break;
    case Screen::Lobby:
      handleLobbyInput(nowMs);
      break;
    case Screen::Game:
      handleGameInput(nowMs);
      break;
  }
}

void GameTableActivity::handleTap(const int x, const int y, const uint32_t nowMs) {
  TouchTarget hit{};
  bool found = false;
  int gameValue = -1;
  {
    MutexGuard guard(snapshotMutex);
    gameValue = sharedGameTargets.hit(x, y);
    // Later targets are drawn on top, so they win.
    for (int i = sharedTargetCount - 1; i >= 0 && !found; i--) {
      const TouchTarget& t = sharedTargets[i];
      if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
        hit = t;
        found = true;
      }
    }
  }
  const Screen screen = currentScreen();
  const table::Game* view = session.view();
  if (gameValue >= 0 && screen == Screen::Game && view && !view->over()) {
    const GameView* ops = gameViewFor(view->id());
    if (!ops) return;
    const int seat = session.localSeat();
    const bool myTurn = seat >= 0 && view->currentSeat() == seat && session.status() == TableSession::Status::Seated &&
                        session.viewIsCurrent();
    uint8_t action[table::MAX_ACTION];
    size_t actionLen = 0;
    if (ops->handleTap(*view, seat, myTurn, gameValue, cursor, action, actionLen)) uiDirty = true;
    if (actionLen > 0 && !session.submitAction(action, actionLen, nowMs)) LOG_DBG("GAME", "tapped move not accepted");
    return;
  }
  if (!found) return;
  switch (hit.kind) {
    case TARGET_BROWSE_ROW:
      if (screen == Screen::Browse && hit.value < session.tableCount()) {
        selection = hit.value;
        uiDirty = true;
        session.join(static_cast<uint8_t>(hit.value), nowMs);
      }
      break;
    case TARGET_LOBBY_ROW: {
      if (screen != Screen::Lobby) break;
      LobbyItem items[MAX_LOBBY_ITEMS];
      const int count = buildLobbyItems(items, MAX_LOBBY_ITEMS);
      if (hit.value < count) {
        selection = hit.value;
        uiDirty = true;
        runLobbyItem(items[hit.value], nowMs);
      }
      break;
    }
    case TARGET_HEADER:
      if (screen == Screen::Game && session.view()) {
        showGameMenu(session.view()->over());
      } else if (screen == Screen::Lobby) {
        confirmLeave();
      } else if (screen == Screen::Browse || screen == Screen::RadioFailed) {
        exitToLibrary();
      } else if (screen == Screen::Notice || screen == Screen::Joining) {
        if (mode == Mode::Join) {
          session.browse();
        } else {
          exitToLibrary();
        }
      }
      break;
    default:
      break;
  }
}

void GameTableActivity::handleBrowseInput(const uint32_t nowMs) {
  using Button = MappedInputManager::Button;
  const int count = session.tableCount();
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasBackGesture()) {
    exitToLibrary();
    return;
  }
  if (count == 0) {
    selection = 0;
    return;
  }
  if (selection >= count) {
    selection = count - 1;
    uiDirty = true;
  }
  if (mappedInput.wasPressed(Button::NavNext)) {
    selection = (selection + 1) % count;
    uiDirty = true;
  } else if (mappedInput.wasPressed(Button::NavPrevious)) {
    selection = (selection + count - 1) % count;
    uiDirty = true;
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (!session.join(static_cast<uint8_t>(selection), nowMs)) LOG_ERR("GAME", "join %d refused", selection);
  }
}

int GameTableActivity::lobbyItemsFor(const TableSession::Role role, const bool anyBot, LobbyItem* items,
                                     const int capacity) {
  int n = 0;
  if (role == TableSession::Role::Host || role == TableSession::Role::Solo) {
    for (const GameId game : ALL_GAMES) {
      if (n < capacity) items[n++] = {LobbyAction::Play, game};
    }
    if (n < capacity) items[n++] = {LobbyAction::AddCpu, GameId::None};
    if (anyBot && n < capacity) items[n++] = {LobbyAction::RemoveCpu, GameId::None};
  }
  if (n < capacity) items[n++] = {LobbyAction::Close, GameId::None};
  return n;
}

int GameTableActivity::buildLobbyItems(LobbyItem* items, const int capacity) const {
  bool anyBot = false;
  for (uint8_t slot = 0; slot < table::MAX_SLOTS; slot++) {
    anyBot = anyBot || session.seat(slot).status == SeatStatus::Bot;
  }
  return lobbyItemsFor(session.role(), anyBot, items, capacity);
}

void GameTableActivity::handleLobbyInput(const uint32_t nowMs) {
  using Button = MappedInputManager::Button;
  LobbyItem items[MAX_LOBBY_ITEMS];
  const int count = buildLobbyItems(items, MAX_LOBBY_ITEMS);
  if (selection >= count) {
    selection = count - 1;
    uiDirty = true;
  }
  if (mappedInput.wasPressed(Button::NavNext)) {
    selection = (selection + 1) % count;
    uiDirty = true;
  } else if (mappedInput.wasPressed(Button::NavPrevious)) {
    selection = (selection + count - 1) % count;
    uiDirty = true;
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    runLobbyItem(items[selection], nowMs);
  } else if (mappedInput.wasReleased(Button::Back) || mappedInput.wasBackGesture()) {
    confirmLeave();
  }
}

void GameTableActivity::runLobbyItem(const LobbyItem& item, const uint32_t nowMs) {
  switch (item.action) {
    case LobbyAction::Play:
      if (!session.startGame(item.game, nowMs))
        LOG_ERR("GAME", "cannot start game %u", static_cast<unsigned>(item.game));
      break;
    case LobbyAction::AddCpu:
      session.addBot();
      break;
    case LobbyAction::RemoveCpu:
      session.removeBot();
      break;
    case LobbyAction::Close:
      confirmLeave();
      return;
  }
  uiDirty = true;
}

// Leaving a solo table affects nobody; standing up from or closing a shared
// table asks first.
void GameTableActivity::confirmLeave() {
  if (session.role() == TableSession::Role::Solo) {
    exitToLibrary();
    return;
  }
  const bool host = session.role() == TableSession::Role::Host;
  const char* options[] = {tr(STR_CANCEL), host ? tr(STR_GAMES_CLOSE_TABLE) : tr(STR_GAMES_LEAVE_TABLE)};
  popup.show(host ? tr(STR_GAMES_CLOSE_TABLE) : tr(STR_GAMES_LEAVE_TABLE), options, 2, 0, [this](const int choice) {
    if (choice == 1) exitToLibrary();
  });
  uiDirty = true;
}

void GameTableActivity::handleGameInput(const uint32_t nowMs) {
  using Button = MappedInputManager::Button;
  const table::Game* view = session.view();
  if (!view) return;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasBackGesture()) {
    showGameMenu(view->over());
    return;
  }
  if (view->over()) {
    // The host decides what the table plays next.
    if (session.isHostLike() && mappedInput.wasReleased(Button::Confirm)) showGameMenu(true);
    return;
  }
  const GameView* ops = gameViewFor(view->id());
  if (!ops) return;
  const int seat = session.localSeat();
  // A guest holding the previous match's state must not build moves from it.
  const bool myTurn = seat >= 0 && view->currentSeat() == seat && session.status() == TableSession::Status::Seated &&
                      session.viewIsCurrent();
  uint8_t action[table::MAX_ACTION];
  size_t actionLen = 0;
  if (ops->handleInput(mappedInput, *view, seat, myTurn, cursor, action, actionLen)) uiDirty = true;
  if (actionLen > 0 && !session.submitAction(action, actionLen, nowMs)) {
    LOG_DBG("GAME", "move not accepted (pending=%d)", session.actionPending() ? 1 : 0);
  }
}

void GameTableActivity::showGameMenu(const bool gameOver) {
  enum Choice : uint8_t { RESUME, PLAY_AGAIN, CHANGE_GAME, CPU_COVERS, LEAVE };
  Choice choices[5];
  const char* labels[5];
  int count = 0;
  if (!gameOver) {
    choices[count] = RESUME;
    labels[count++] = tr(STR_GAMES_RESUME);
  }
  if (session.isHostLike()) {
    choices[count] = PLAY_AGAIN;
    labels[count++] = tr(STR_GAMES_PLAY_AGAIN);
    choices[count] = CHANGE_GAME;
    labels[count++] = tr(STR_GAMES_CHANGE_GAME);
  }
  if (session.role() == TableSession::Role::Host) {
    choices[count] = CPU_COVERS;
    labels[count++] = session.botTakeoverEnabled() ? tr(STR_GAMES_CPU_COVERS_ON) : tr(STR_GAMES_CPU_COVERS_OFF);
  }
  choices[count] = LEAVE;
  switch (session.role()) {
    case TableSession::Role::Host:
      labels[count++] = tr(STR_GAMES_CLOSE_TABLE);
      break;
    case TableSession::Role::Solo:
      labels[count++] = tr(STR_GAMES_QUIT);
      break;
    default:
      labels[count++] = tr(STR_GAMES_LEAVE_TABLE);
      break;
  }
  const GameId game = session.gameId();
  popup.show(gameOver ? tr(STR_GAMES_GAME_OVER) : gameName(game), labels, count, 0,
             [this, choices, game](const int index) {
               const uint32_t now = millis();
               switch (choices[index]) {
                 case RESUME:
                   break;
                 case PLAY_AGAIN:
                   session.startGame(game, now);
                   break;
                 case CHANGE_GAME:
                   session.returnToLobby();
                   selection = 0;
                   break;
                 case CPU_COVERS:
                   session.setBotTakeover(!session.botTakeoverEnabled());
                   break;
                 case LEAVE:
                   if (session.role() == TableSession::Role::Solo) {
                     exitToLibrary();
                   } else {
                     leavePromptPending = true;
                   }
                   return;
               }
               uiDirty = true;
             });
  uiDirty = true;
}

void GameTableActivity::exitToLibrary() {
  exiting = true;
  activityManager.goToLibrary(true);
}

void GameTableActivity::publishSnapshot() {
  MutexGuard guard(snapshotMutex);
  Snapshot& s = shared;
  s.screen = currentScreen();
  s.role = session.role();
  s.status = session.status();
  s.phase = session.phase();
  // The game the view bytes belong to, which briefly trails the roster's
  // game on a guest while a switch is in flight.
  s.gameId = session.view() ? session.view()->id() : session.gameId();
  s.localSlot = session.localSlot();
  s.localSeat = static_cast<int8_t>(session.localSeat());
  s.actionPending = session.actionPending();
  s.viewCurrent = session.viewIsCurrent();
  s.botTakeover = session.botTakeoverEnabled();
  s.gameSeats = session.gameSeatCount();
  for (uint8_t i = 0; i < table::MAX_SLOTS; i++) {
    s.gameSlots[i] = session.slotOfSeat(i);
    const table::SeatInfo& seat = session.seat(i);
    s.seats[i].status = seat.status;
    memcpy(s.seats[i].name, seat.name, sizeof(s.seats[i].name));
  }
  s.tableCount = session.tableCount();
  for (uint8_t i = 0; i < s.tableCount; i++) {
    const table::TableInfo& t = session.tableAt(i);
    memcpy(s.tables[i].host, t.hostName, sizeof(s.tables[i].host));
    s.tables[i].players = t.players;
    s.tables[i].maxPlayers = t.maxPlayers;
    s.tables[i].game = t.game;
    s.tables[i].phase = t.phase;
  }
  s.viewLen = static_cast<uint16_t>(session.viewBytes(s.view, sizeof(s.view)));
  s.cursor = cursor;
  s.selection = static_cast<int8_t>(selection);
  switch (session.status()) {
    case TableSession::Status::Denied:
      s.notice = StrId::STR_GAMES_TABLE_FULL;
      break;
    case TableSession::Status::Closed:
      s.notice = StrId::STR_GAMES_TABLE_CLOSED;
      break;
    default:
      s.notice = StrId::STR_GAMES_CONNECTION_LOST;
      break;
  }
}

// --- render task -------------------------------------------------------------------

void GameTableActivity::render(RenderLock&&) {
  {
    MutexGuard guard(snapshotMutex);
    renderSnap = shared;
  }
  const Snapshot& s = renderSnap;
  renderTargetCount = 0;
  renderGameTargets.count = 0;
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    addTarget(0, 0, renderer.getScreenWidth(), metrics.topPadding + metrics.headerHeight, TARGET_HEADER, 0);
  }

  renderer.clearScreen();
  switch (s.screen) {
    case Screen::RadioFailed:
      drawCenteredLines(tr(STR_GAMES_RADIO_FAILED), nullptr);
      break;
    case Screen::Browse:
      drawBrowse(s);
      break;
    case Screen::Joining:
      drawJoining(s);
      break;
    case Screen::Notice:
      drawNotice(s);
      break;
    case Screen::Lobby:
      drawLobby(s);
      break;
    case Screen::Game:
      drawGame(s);
      break;
  }

  // A new screen (or a long run of fast updates) gets a cleaning refresh.
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (s.screen != lastRenderedScreen || ++fastRefreshes >= FAST_REFRESHES_PER_CLEAN) {
    mode = HalDisplay::HALF_REFRESH;
    fastRefreshes = 0;
  }
  lastRenderedScreen = s.screen;
  {
    MutexGuard guard(snapshotMutex);
    memcpy(sharedTargets, renderTargets, sizeof(sharedTargets));
    sharedTargetCount = renderTargetCount;
    sharedGameTargets = renderGameTargets;
  }
  if (popup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(mode);
}

void GameTableActivity::addTarget(const int x, const int y, const int w, const int h, const uint8_t kind,
                                  const int value) {
  if (renderTargetCount >= MAX_TARGETS) return;
  renderTargets[renderTargetCount++] = {
      static_cast<int16_t>(x),    static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h), kind,
      static_cast<int16_t>(value)};
}

void GameTableActivity::drawCenteredLines(const char* primary, const char* secondary) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_LIBRARY_TAB_GAMES));
  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = height - metrics.buttonHintsHeight;
  const Rect box{SIDE_PADDING, top, width - 2 * SIDE_PADDING, (bottom - top) / 2};
  UITheme::drawCenteredWrappedText(renderer, box, UI_12_FONT_ID, primary, 3, true, EpdFontFamily::BOLD,
                                   UITheme::TextVerticalAlignment::BOTTOM);
  if (secondary) {
    const Rect below{SIDE_PADDING, top + (bottom - top) / 2 + 12, width - 2 * SIDE_PADDING, (bottom - top) / 2 - 12};
    UITheme::drawCenteredWrappedText(renderer, below, UI_10_FONT_ID, secondary, 4, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::TOP);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GameTableActivity::drawBrowse(const Snapshot& s) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  if (s.tableCount == 0) {
    drawCenteredLines(tr(STR_GAMES_SEARCHING), tr(STR_GAMES_SEARCH_HINT));
    return;
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_GAMES_TABLES_NEARBY));

  const int titleFont = UI_12_FONT_ID;
  const int subFont = UI_10_FONT_ID;
  const int rowH = renderer.getLineHeight(titleFont) + renderer.getLineHeight(subFont) + 18;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 4;
  const int rowW = width - 2 * SIDE_PADDING;
  for (uint8_t i = 0; i < s.tableCount; i++) {
    const TableView& t = s.tables[i];
    const bool selected = i == s.selection;
    if (selected) renderer.fillRoundedRect(SIDE_PADDING, y, rowW, rowH - 6, 10, Color::Black);
    addTarget(SIDE_PADDING, y, rowW, rowH - 6, TARGET_BROWSE_ROW, i);
    char title[48];
    snprintf(title, sizeof(title), tr(STR_GAMES_TABLE_OF), t.host);
    gameui::drawFittedText(renderer, titleFont, SIDE_PADDING + 14, y + 6, title, rowW - 28, !selected,
                           EpdFontFamily::BOLD);
    char players[24];
    snprintf(players, sizeof(players), tr(STR_GAMES_PLAYER_COUNT), t.players, t.maxPlayers);
    char detail[80];
    if (t.phase == Phase::Playing && t.game != GameId::None) {
      char playing[48];
      snprintf(playing, sizeof(playing), tr(STR_GAMES_NOW_PLAYING), gameName(t.game));
      snprintf(detail, sizeof(detail), "%s  ·  %s", players, playing);
    } else {
      snprintf(detail, sizeof(detail), "%s  ·  %s", players, tr(STR_GAMES_IN_LOBBY));
    }
    gameui::drawFittedText(renderer, subFont, SIDE_PADDING + 14, y + 8 + renderer.getLineHeight(titleFont), detail,
                           rowW - 28, !selected);
    y += rowH;
    if (y + rowH > height - metrics.buttonHintsHeight) break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GameTableActivity::drawJoining(const Snapshot&) { drawCenteredLines(tr(STR_GAMES_JOINING), nullptr); }

void GameTableActivity::drawNotice(const Snapshot& s) { drawCenteredLines(I18N.get(s.notice), nullptr); }

void GameTableActivity::seatLabel(const Snapshot& s, const uint8_t slot, char* out, const size_t size) const {
  const SeatView& seat = s.seats[slot];
  const char* name = slot == s.localSlot ? tr(STR_GAMES_YOU) : seat.name;
  const char* tag = nullptr;
  if (seat.status == SeatStatus::Bot) {
    tag = tr(STR_GAMES_TAG_CPU);
  } else if (seat.status == SeatStatus::Away) {
    tag = tr(STR_GAMES_TAG_AWAY);
  } else if (seat.status == SeatStatus::Empty) {
    tag = tr(STR_GAMES_TAG_LEFT);
  } else if (slot == table::HOST_SLOT && s.role != TableSession::Role::Solo) {
    tag = tr(STR_GAMES_TAG_HOST);
  }
  if (tag && seat.status != SeatStatus::Bot) {
    snprintf(out, size, "%s  (%s)", name, tag);
  } else {
    snprintf(out, size, "%s", name);
  }
}

const char* GameTableActivity::tableTitle(const Snapshot& s, char* buf, const size_t size) const {
  if (s.role == TableSession::Role::Solo) return tr(STR_LIBRARY_TAB_GAMES);
  snprintf(buf, size, tr(STR_GAMES_TABLE_OF), s.seats[table::HOST_SLOT].name);
  return buf;
}

void GameTableActivity::drawLobby(const Snapshot& s) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  char titleBuf[48];
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight},
                 tableTitle(s, titleBuf, sizeof(titleBuf)));

  const int font = UI_10_FONT_ID;
  const int bold = UI_12_FONT_ID;
  const int lineH = renderer.getLineHeight(font);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 4;

  // Roster.
  uint8_t players = 0;
  for (const SeatView& seat : s.seats) players += seat.status != SeatStatus::Empty ? 1 : 0;
  char heading[32];
  snprintf(heading, sizeof(heading), tr(STR_GAMES_PLAYER_COUNT), players, table::MAX_SLOTS);
  renderer.drawText(font, SIDE_PADDING, y, heading, true, EpdFontFamily::BOLD);
  y += lineH + 6;
  for (uint8_t slot = 0; slot < table::MAX_SLOTS; slot++) {
    if (s.seats[slot].status == SeatStatus::Empty) continue;
    char label[48];
    seatLabel(s, slot, label, sizeof(label));
    renderer.fillRoundedRect(SIDE_PADDING + 4, y + lineH / 2 - 3, 6, 6, 3, Color::Black);
    gameui::drawFittedText(renderer, font, SIDE_PADDING + 18, y, label, width - 2 * SIDE_PADDING - 18, true,
                           slot == s.localSlot ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += lineH + 4;
  }
  y += 10;
  renderer.drawLine(SIDE_PADDING, y, width - SIDE_PADDING, y, true);
  y += 12;

  if (s.role == TableSession::Role::Guest) {
    const char* waiting =
        s.status == TableSession::Status::Reconnecting ? tr(STR_GAMES_RECONNECTING) : tr(STR_GAMES_WAITING_HOST);
    UITheme::drawCenteredWrappedText(renderer, Rect{SIDE_PADDING, y, width - 2 * SIDE_PADDING, lineH * 2}, font,
                                     waiting, 2, true, EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
    y += lineH * 2 + 12;
  }

  bool anyBot = false;
  for (const SeatView& seat : s.seats) anyBot = anyBot || seat.status == SeatStatus::Bot;
  LobbyItem items[MAX_LOBBY_ITEMS];
  const int count = lobbyItemsFor(s.role, anyBot, items, MAX_LOBBY_ITEMS);

  const int rowH = renderer.getLineHeight(bold) + 16;
  const int bottom = height - metrics.buttonHintsHeight - 4;
  // Keep the selected row on screen when the menu is taller than the space.
  const int visible = std::max(1, (bottom - y) / rowH);
  const int first = std::clamp(s.selection - visible + 1, 0, std::max(0, count - visible));
  for (int i = first; i < count && y + rowH <= bottom; i++) {
    const LobbyItem& item = items[i];
    char text[64];
    switch (item.action) {
      case LobbyAction::Play:
        snprintf(text, sizeof(text), tr(STR_GAMES_PLAY), gameName(item.game));
        break;
      case LobbyAction::AddCpu:
        snprintf(text, sizeof(text), "%s", tr(STR_GAMES_ADD_CPU));
        break;
      case LobbyAction::RemoveCpu:
        snprintf(text, sizeof(text), "%s", tr(STR_GAMES_REMOVE_CPU));
        break;
      case LobbyAction::Close:
        snprintf(text, sizeof(text), "%s",
                 s.role == TableSession::Role::Host   ? tr(STR_GAMES_CLOSE_TABLE)
                 : s.role == TableSession::Role::Solo ? tr(STR_GAMES_QUIT)
                                                      : tr(STR_GAMES_LEAVE_TABLE));
        break;
    }
    const bool selected = i == s.selection;
    if (selected) renderer.fillRoundedRect(SIDE_PADDING, y, width - 2 * SIDE_PADDING, rowH - 6, 10, Color::Black);
    addTarget(SIDE_PADDING, y, width - 2 * SIDE_PADDING, rowH - 6, TARGET_LOBBY_ROW, i);
    gameui::drawFittedText(renderer, bold, SIDE_PADDING + 14, y + (rowH - 6 - renderer.getLineHeight(bold)) / 2, text,
                           width - 2 * SIDE_PADDING - 28, !selected);
    y += rowH;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

const char* GameTableActivity::statusLine(const Snapshot& s, const table::Game& game, const char* const* names,
                                          char* buf, const size_t size) const {
  if (s.status == TableSession::Status::Reconnecting) return tr(STR_GAMES_RECONNECTING);
  if (game.over()) {
    const uint8_t winners = game.winnerMask();
    if (winners == 0) return tr(STR_GAMES_TIE);
    if (s.localSeat >= 0 && (winners >> s.localSeat) & 1u) return tr(STR_GAMES_YOU_WIN);
    for (uint8_t seat = 0; seat < game.seatCount(); seat++) {
      if ((winners >> seat) & 1u) {
        snprintf(buf, size, tr(STR_GAMES_WINS), names[seat]);
        return buf;
      }
    }
    return tr(STR_GAMES_GAME_OVER);
  }
  if (s.actionPending) return tr(STR_GAMES_SENDING);
  const int current = game.currentSeat();
  if (current >= 0 && current == s.localSeat) return tr(STR_GAMES_YOUR_TURN);
  if (current >= 0 && current < game.seatCount()) {
    const uint8_t slot = s.gameSlots[current];
    const bool away = slot < table::MAX_SLOTS && s.seats[slot].status == SeatStatus::Away;
    snprintf(buf, size, away ? tr(STR_GAMES_WAITING_FOR) : tr(STR_GAMES_TURN_OF), names[current]);
    return buf;
  }
  return s.localSeat < 0 ? tr(STR_GAMES_WATCHING) : "";
}

void GameTableActivity::drawGame(const Snapshot& s) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const GameView* ops = gameViewFor(s.gameId);

  if (!ops || s.viewLen == 0) {
    drawCenteredLines(tr(STR_GAMES_JOINING), nullptr);
    return;
  }
  if (!renderGame || renderGame->id() != s.gameId) renderGame = table::createGame(s.gameId);
  if (!renderGame || !renderGame->deserialize(s.view, s.viewLen)) {
    LOG_ERR("GAME", "cannot load view for game %u", static_cast<unsigned>(s.gameId));
    drawCenteredLines(tr(STR_GAMES_CONNECTION_LOST), nullptr);
    return;
  }
  const table::Game& game = *renderGame;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, gameName(s.gameId));

  // Names per game seat: "You" for this reader, the roster name otherwise.
  const char* names[table::MAX_SLOTS] = {};
  for (uint8_t seat = 0; seat < table::MAX_SLOTS; seat++) {
    const uint8_t slot = seat < s.gameSeats ? s.gameSlots[seat] : table::NO_SLOT;
    if (slot >= table::MAX_SLOTS) {
      names[seat] = "?";
    } else {
      names[seat] = slot == s.localSlot ? tr(STR_GAMES_YOU) : s.seats[slot].name;
    }
  }

  const int statusFont = UI_12_FONT_ID;
  const int statusH = renderer.getLineHeight(statusFont);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  char buf[64];
  const char* status = statusLine(s, game, names, buf, sizeof(buf));
  const int statusW = renderer.getTextWidth(statusFont, status, EpdFontFamily::BOLD);
  renderer.drawText(statusFont, std::max(SIDE_PADDING, (width - statusW) / 2), y, status, true, EpdFontFamily::BOLD);
  y += statusH + 8;

  const bool myTurn = s.localSeat >= 0 && game.currentSeat() == s.localSeat && !s.actionPending && s.viewCurrent &&
                      s.status == TableSession::Status::Seated;
  const GameViewContext ctx{game, s.localSeat, names, s.cursor, myTurn, renderGameTargets};
  const int bottom = height - metrics.buttonHintsHeight - 6;
  ops->render(renderer, Rect{SIDE_PADDING / 2, y, width - SIDE_PADDING, bottom - y}, ctx);

  const char* confirm = "";
  if (game.over()) {
    confirm = s.role == TableSession::Role::Guest ? "" : tr(STR_GAMES_MENU);
  } else if (myTurn || (s.localSeat >= 0 && game.showingResult())) {
    confirm = ops->confirmLabel(game, s.localSeat, s.cursor);
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_GAMES_MENU), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                                       tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
