#include "TableGame.h"

#include <Memory.h>

#include "ConnectFour.h"
#include "DotsAndBoxes.h"
#include "LiarsDice.h"
#include "MurderMystery.h"

namespace table {

bool validGameId(const uint8_t raw) { return raw >= 1 && raw <= GAME_ID_MAX; }

std::unique_ptr<Game> createGame(const GameId id) {
  switch (id) {
    case GameId::ConnectFour:
      return makeUniqueNoThrow<ConnectFour>();
    case GameId::DotsAndBoxes:
      return makeUniqueNoThrow<DotsAndBoxes>();
    case GameId::LiarsDice:
      return makeUniqueNoThrow<LiarsDice>();
    case GameId::MurderMystery:
      return makeUniqueNoThrow<MurderMystery>();
    case GameId::None:
      break;
  }
  return nullptr;
}

}  // namespace table
