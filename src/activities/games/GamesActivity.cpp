#include "GamesActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/games/GamePlayerName.h"
#include "activities/games/GameTableActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {

struct GameRow {
  StrId label;
  StrId subtitle;
  UIIcon icon;
  // Rows that do not launch a table edit the player name instead.
  bool launches;
  GameTableActivity::Mode mode;
  table::GameId game;
};

constexpr GameRow GAME_ROWS[] = {
    {StrId::STR_GAMES_HOST, StrId::STR_GAMES_HOST_DESC, UIIcon::Hotspot, true, GameTableActivity::Mode::Host,
     table::GameId::None},
    {StrId::STR_GAMES_JOIN, StrId::STR_GAMES_JOIN_DESC, UIIcon::Wifi, true, GameTableActivity::Mode::Join,
     table::GameId::None},
    {StrId::STR_GAMES_YOUR_NAME, StrId::STR_GAMES_YOUR_NAME_DESC, UIIcon::Bookmark, false,
     GameTableActivity::Mode::Host, table::GameId::None},
    {StrId::STR_GAME_CONNECT_FOUR, StrId::STR_GAME_CONNECT_FOUR_DESC, UIIcon::Blocks, true,
     GameTableActivity::Mode::Solo, table::GameId::ConnectFour},
    {StrId::STR_GAME_DOTS_AND_BOXES, StrId::STR_GAME_DOTS_AND_BOXES_DESC, UIIcon::Blocks, true,
     GameTableActivity::Mode::Solo, table::GameId::DotsAndBoxes},
    {StrId::STR_GAME_LIARS_DICE, StrId::STR_GAME_LIARS_DICE_DESC, UIIcon::Blocks, true, GameTableActivity::Mode::Solo,
     table::GameId::LiarsDice},
    {StrId::STR_GAME_MURDER_MYSTERY, StrId::STR_GAME_MURDER_MYSTERY_DESC, UIIcon::Blocks, true,
     GameTableActivity::Mode::Solo, table::GameId::MurderMystery},
};
static_assert(sizeof(GAME_ROWS) / sizeof(GAME_ROWS[0]) == GamesActivity::ROW_COUNT, "Games row storage");
constexpr int FIRST_SOLO_ROW = 3;

}  // namespace

GamesActivity::GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Games", renderer, mappedInput) {}

void GamesActivity::onEnter() {
  UiListActivity::onEnter();
  swallowHeldReleases();
}

void GamesActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

bool GamesActivity::handleCustomInput() {
  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }
  return false;
}

int GamesActivity::listCount() const { return ROW_COUNT; }

const char* GamesActivity::headerTitle() const { return tr(STR_GAMES); }

void GamesActivity::onBackButton() { onGoHome(); }

void GamesActivity::activateIndex(const int index) {
  if (index < 0 || index >= ROW_COUNT) return;
  const GameRow& game = GAME_ROWS[index];
  if (!game.launches) {
    editPlayerName();
    return;
  }
  auto activity = makeUniqueNoThrow<GameTableActivity>(renderer, mappedInput, game.mode, game.game);
  if (!activity) {
    LOG_ERR("GAMES", "OOM: game table");
    return;
  }
  app.clearTapFlash();
  activityManager.replaceActivity(std::move(activity));
}

// Named here rather than at the table: the keyboard is a pushed activity, and
// at a live table that would stop the radio loop and time out every guest.
void GamesActivity::editPlayerName() {
  app.clearTapFlash();
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GAMES_YOUR_NAME),
                                               std::string(SETTINGS.gamesPlayerName), table::NAME_LEN, InputType::Text);
  if (!keyboard) {
    LOG_ERR("GAMES", "OOM: player name keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (!result.isCancelled) setGamePlayerName(std::get<KeyboardResult>(result.data).text.c_str());
    requestUpdate();
  });
}

void GamesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  for (int row = 0; row < ROW_COUNT; row++) {
    fui::ListItem item;
    item.label = I18N.get(GAME_ROWS[row].label);
    item.subtitle = I18N.get(GAME_ROWS[row].subtitle);
    item.icon = listIconFor(GAME_ROWS[row].icon, 32);
    item.actionValue = static_cast<int16_t>(row);
    if (!GAME_ROWS[row].launches) {
      gamePlayerName(playerNameBuf, sizeof(playerNameBuf));
      item.value = playerNameBuf;
    }
    if (row == 0) item.sectionHeading = tr(STR_GAMES_SECTION_TABLE);
    if (row == FIRST_SOLO_ROW) item.sectionHeading = tr(STR_GAMES_SECTION_SOLO);
    rowItems[row] = item;
  }
  props.items = rowItems;
  syncListViewport(screen, props);
  screen.list(props);
}
