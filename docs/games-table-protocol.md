# Games table protocol

Home › Games runs a *table*: one host reader plus up to five guests (or CPU
players) that play one game after another without reconnecting. Readers talk
over ESP-NOW on WiFi channel 1 without joining an access point.

Code map:

| Piece | Where |
| --- | --- |
| Wire format (`Reader`/`Writer`, header, message types) | `lib/TableGames/TableWire.h` |
| Session state machine (roles, join/leave, heartbeats, relay) | `lib/TableGames/TableSession.*` |
| Game rules, CPU players, per-viewer serialization | `lib/TableGames/{ConnectFour,DotsAndBoxes,LiarsDice,MurderMystery}.*` |
| ESP-NOW binding (SDK `EspNowTransport`) | `src/activities/games/EspNowLink.*` |
| Screens and per-game views | `src/activities/games/` |
| Host tests (in-memory lossy radio) | `test/table_games/` |

## Model

The host is authoritative. Guests send moves; the host validates them against
its own game instance and sends each guest the state *as that guest may see
it* (`Game::serialize(viewerSeat)`), so hidden information such as Liar's Dice
cups never leaves the host. The host renders from the same kind of view, so
it cannot peek either.

There are no per-message acks. Each guest's 1 Hz `Ping` carries the roster and
state versions it holds. The host re-sends a stale roster or state in reply to
that ping, and again on its own 1 Hz beacon tick. A lost packet therefore heals
within about a second, including under loss patterns that line up with the
heartbeat. Moves are the exception: a guest retries its pending move every
400 ms until a `State` echoes the move's sequence number back.

## Packets

All packets fit ESP-NOW's v1 250-byte payload so every IDF version interoperates.
Multi-byte fields are little-endian and read byte-wise (no unaligned loads).

Header (10 bytes): `'C' 'G' version(1) type(1) tableId(u32) srcSlot(u8) flags(u8)`.
Other traffic on the channel, other versions and unknown types are ignored.

| Type | Direction | Payload |
| --- | --- | --- |
| `Beacon` | host → broadcast, 1 Hz | hostName[12] players maxPlayers gameId phase presentMask |
| `Join` | guest → host | name[12] (also used to rejoin) |
| `Welcome` | host → guest | slot, lastActionSeq(u16) |
| `Deny` | host → guest | reason (`Full`); a seated guest that gets one rejoins |
| `Roster` | host → guest | rosterVersion(u16) gameId phase matchId seatCount gameSlots[6] then 6 × (status, name[12]) |
| `Leave` | guest → host | — |
| `Close` | host → guest/broadcast | — |
| `Ping` | guest → host, 1 Hz | rosterVersion(u16) stateVersion(u16) |
| `State` | host → guest | stateVersion(u16) gameId matchId ackActionSeq(u16) len bytes[len] |
| `Action` | guest → host | matchId actionSeq(u16) len bytes[len] (≤ 8) |

Names are fixed 12-byte UTF-8 fields, zero-padded and never cut inside a
character.

`matchId` increments with every game the host starts. A move stamped with an
older match (one retried across "play again" or a game switch) is
acknowledged but never applied. Guests drop a pending move when the roster
announces a new match, and accept no input until their view belongs to it.

`presentMask` has a bit per slot the host currently hears. A beacon only
proves the host is alive, so a seated guest counts itself heard only through
its bit or a unicast from the host. A guest the host has lost therefore notices
within 4.5 s and rejoins, even when it can still hear the host.

## Seats and lifetimes

* Slot 0 is the host. Guests are identified by MAC address, so a reader that
  drops out (sleep, out of range) and comes back gets the same slot and game
  seat.
* The host marks a guest silent for 4.5 s as *away*. Away seats outside the
  running match are freed after 30 s; seats in the match are kept, and the host
  can let the CPU play them.
* A guest the host has not acknowledged for 4.5 s shows *Reconnecting* and
  re-sends `Join`. It gives up after 30 s without hearing the host at all. If
  its seat was freed and the table has since filled, the host's `Deny` ends
  the attempt.
* A guest who leaves mid-match forfeits: the game's `seatLeft()` decides what
  that means (Connect Four: the opponent wins; Dots & Boxes and Liar's Dice:
  the seat is skipped; Murder Mystery: the seat is skipped and its cards are
  laid face up for everyone).
* Starting a game seats the humans present (host first), then CPU players, up to
  the game's seat limit. It adds CPU players if needed to reach the minimum.
  Readers who join mid-match watch until the next one.

## Adding a game

1. Implement `table::Game` in `lib/TableGames/` and give it a `GameId`. Keep
   its serialized view within `MAX_GAME_STATE` (200 bytes) and its moves within
   `MAX_ACTION` (8 bytes), and hide anything secret in `serialize()` for other
   viewers.
2. Add it to `createGame()` and cover it in `test/table_games/`.
3. Add a `GameView` (input, cursor defaults, drawing) in `src/activities/games/`
   and register it in `gameViewFor()`.
4. List it in `ALL_GAMES` (lobby) and `GAME_ROWS` (Games screen).

Bump `PROTOCOL_VERSION` whenever an existing packet or game serialization
changes shape. Readers on other versions then ignore each other's tables
instead of misreading them.
