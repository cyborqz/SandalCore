# AocAdmin — a game master's desktop dashboard for the Sandal Core server

Date: 2026-11-01
Status: design approved, ready for the implementation plan
Author: MCode, with M7MD

## 1. Goal

The server has a complete administrative gRPC API, but no way to use it that a
game master can actually work with: today every action is a `worldctl.exe`
command typed by hand, and there is nothing for the stats a game master wants
to change (damage, resistances, health). This adds a desktop application that
puts the whole server under one window: the players, their stats, their
inventory, the money, the experience, the living world and the cities.

The dashboard is a personal tool for this machine. It talks to the server on
localhost only.

## 2. The decisions

| Question | Decision |
|---|---|
| Shape | A desktop application with its own window (not a browser tab, not a tab inside the launcher) |
| Stack | Go + Wails (a WebView2 window; the UI is HTML/CSS/JS) |
| Combat stats | Direct overrides on the player's stats, not the game's own buffs |
| Access | localhost only (127.0.0.1), no network listener |
| Language | Arabic UI (RTL); the game's own names (stats, items, places) stay English, because the game data is English |

## 3. What exists already (nothing here is built from scratch)

| Piece | Where | State |
|---|---|---|
| The administrative API | `proto/internal/aoc/worldadmin/v1/worldadmin.proto` | 22 RPCs: players, places, teleport, move speed, money, items, stations, craft, recipes, tutorials, weapon XP, NPC control |
| The live world stream | the same proto, `WorldStreamService` | subscribe, inspect, search, status: the entities, the deltas, the density grid |
| The character API | `proto/internal/aoc/character/v1/character.proto` | create, list, get, save state/inventory/progress (level, experience rows, skill ranks, professions, attunements) |
| The city node API | `proto/internal/aoc/citynode/v1/citynode.proto` | the nodes, their levels, their experience, the clock |
| The Go clients | `services/gen/aoc/**` | generated, ready to use |
| The service token | `services/internal/svcauth` | how the services authenticate to each other |
| The C++ admin service | `server/worldd/svc/world_admin.cpp` | the handlers, with `authorize` and `on_net_thread` |
| The stat engine | `server/worldd/stats/character_stats.{h,cpp}` | the player's stats: class, level, worn gear, effects, ranks |
| The C++ protobuf generation | a CMake rule | `build/x64/server/worldd/generated/**` — a proto change needs no manual step |

## 4. Architecture

```
┌─ aocadmin.exe (Go + Wails, a WebView2 window) ────────────────────┐
│  frontend/  index.html + app.js + style.css (Arabic RTL, dark)     │
│      │ Wails bindings                                             │
│      ▼                                                             │
│  app.go ──→ admin_client.go ──→ gRPC over 127.0.0.1 + token       │
└────────────────────────────────────────────────────────────────────┘
        │ uses services/gen/aoc/** (generated, no codegen for the app)
   ┌────────────┬──────────────┬──────────────┬──────────────┬────────────┐
   │ worldadmin │ worldstream  │  character   │  citynode    │   quest    │
   │  :50056    │   :50057     │   :50062     │   :50055     │   :50058   │
   └────────────┴──────────────┴──────────────┴──────────────┴────────────┘
                                 │
                        worldd (C++, server/worldd)
```

The application is a new Go command, `services/cmd/aocadmin`, next to the
other services, so it uses the same generated clients, the same `svcauth`
token helper and the same module.

### 4.1 The files

```
services/cmd/aocadmin/
  main.go            the Wails bootstrap: the window, the dark theme, the RTL direction
  app.go             the methods the UI calls (the Wails bindings)
  client.go          the gRPC clients and the service token
  client_test.go     the client's unit tests (a fake gRPC server)
  frontend/
    index.html       the shell and the five tabs
    app.js           the state, the calls, the tables
    style.css        the dark theme
proto/internal/aoc/worldadmin/v1/worldadmin.proto   the four new RPCs
server/worldd/stats/character_stats.{h,cpp}         the GM stat overrides
server/worldd/svc/world_admin.{h,cpp}               the four new handlers
```

## 5. The server side: the GM stat overrides

### 5.1 The mechanism

A player's stats are computed from their class, their level, their worn gear,
their effects and their ranks. The engine takes the gear's contribution as a
list of `StatMod` (a stat's guid and a value) and applies each mod in the
stat's own form: an additive stat adds the value, a multiplying stat
multiplies by it (2 doubles, 0.5 halves).

The GM override is that same list, added after the gear's:

```cpp
// stats/character_stats.h
/// The game master's overrides: stat mods applied on top of the computed
/// stats (the gear's, the level's, the effects'). They are part of the next
/// prepare(), so the client sees them like any other stat change.
void set_gm_mods(std::vector<StatMod> mods) { gm_mods_ = std::move(mods); }
const std::vector<StatMod>& gm_mods() const { return gm_mods_; }
```

```cpp
// stats/character_stats.cpp, in compute()
in.mods = g.mods;
in.mods.insert(in.mods.end(), gm_mods_.begin(), gm_mods_.end());   // the GM's, after the gear's
```

Because the override goes through the engine, the change is computed, clamped
and sent to the client by the same code path as a gear change: the player sees
the new numbers immediately, with no reconnect.

The override lives on the session, so it lasts as long as the player is
online and is gone when they log out. It is not written to the character: the
dashboard sets it again after a reconnect.

### 5.2 The four new RPCs

```proto
// One stat of the bundle, with the player's current value.
message StatInfo {
  int64  guid    = 1;   // the stat's guid in the bundle
  string name    = 2;   // the bundle's name (English)
  uint32 flags   = 3;   // the bundle's kStat* flags: kStatMulMods (multiplies), kStatBuckets, kStatSpawnMax, kStatResource
  double value   = 4;   // the player's current value (0 when the player is offline)
  double max     = 5;   // its maximum (0 when the stat has none)
  bool   has_override = 6;
  double override = 7;  // the GM override on it (0 when there is none)
}

message GetPlayerStatsRequest  { string character = 1; }
message GetPlayerStatsResponse { repeated StatInfo stats = 1; }

message SetStatModsRequest  { string character = 1; repeated StatMod mods = 2; }
message SetStatModsResponse { repeated StatInfo stats = 1; }   // the stats after the change

message ClearStatModsRequest  { string character = 1; }
message ClearStatModsResponse { repeated StatInfo stats = 1; }

message SetCurrentStatRequest  { string character = 1; int64 stat = 2; double value = 3; }
message SetCurrentStatResponse { double value = 1; }
```

`StatMod` is the engine's own type (`stat_engine.h`): a stat's guid and a
value. The proto reuses the name for the wire message.

`GetPlayerStats` returns the whole catalog of the bundle with the player's
values, so the dashboard shows every stat the game has, not a hand-picked
list. The dashboard classifies a stat from its name and its flags (a name with
"Resistance" is a resistance; the `kStatMulMods` flag means the value
multiplies).

`SetCurrentStat` uses the engine's `set_current(guid, value)`: it sets the
current value of a stat that is held (Health, Mana, Stamina), which is how
"fill my health" works.

### 5.3 The handlers

The four handlers follow `GiveMoney` exactly: authorize, then run the work on
worldd's network thread, between two ticks of the world. The session is never
touched from the gRPC thread.

```cpp
grpc::Status SetStatMods(grpc::ServerContext* ctx, const adminv1::SetStatModsRequest* req,
                          adminv1::SetStatModsResponse* resp) override {
    if (auto s = authorize(ctx, "SetStatMods"); !s.ok()) return s;
    const std::string by = "the admin API (" + ctx->peer() + ")";
    GmStatMods out;
    if (auto s = on_net_thread([&] { out = owner.gm_.set_stat_mods(req->character(), mods, by); }); !s.ok()) return s;
    ...
}
```

`owner.gm_` is the game master's helper on worldd, the one the other GM
actions (the money, the items, the NPC) already use. The stat override adds
two methods to it: `set_stat_mods` and `clear_stat_mods`. The helper reaches
the player's session the way the other player-scoped helpers do, and returns
`GmStatus::NotFound` when the character is not online (the dashboard only
offers these actions for the players it sees in the world).

### 5.4 The codegen

- The C++ stubs: CMake's rule generates them from the proto — a rebuild is
  enough.
- The Go stubs: `buf generate` in `services/` (needs `buf`, `protoc-gen-go`
  and `protoc-gen-go-grpc`; all three install with `go install`).

## 6. The application

### 6.1 The tabs

1. **اللاعبون** — the live player table: the name, the guid, the level, the
   position, the mounted flag, the move speed. A row's buttons: teleport (a
   place or coordinates), the move speed, the money, the items, the
   experience, the recipes, the tutorials. Selecting a player opens the
   detail panel.
2. **الإحصائيات** — the stat table of the selected player: the name, the kind
   (derived from the name and the flags), the current value, the maximum, the
   override and an input for the new override. The input's unit follows the
   stat's own rule: `+` for an additive stat, `×` for a multiplying one.
   "حفظ" sends `SetStatMods` (it replaces every override of that player),
   "امسح الكل" sends `ClearStatMods`, and "املأ الدم" sends
   `SetCurrentStat` with the maximum.
3. **العالم** — the live world: the entities (players, NPCs, resource nodes),
   the density grid as a map, the NPC control (kill, respawn, reset, heal,
   despawn, move), the search and the inspection (the health, the AI state,
   the threat table, the leash).
4. **المدن** — the city nodes: the level, the experience, the clock (advance,
   speed) and the seed.
5. **السجل** — the live log of the services and of the game client.

### 6.2 The live updates

One subscription to the world stream feeds the world tab and the player list.
The stats and the cities are polled every two seconds while their tab is
open. Nothing polls while the window is closed.

### 6.3 The errors

| Case | What the user sees |
|---|---|
| An RPC fails | A toast with the server's own message and a retry button |
| The server is down | A red banner with a "شغّل الستاك" button that runs `scripts\start-stack.ps1` |
| The service token is missing | A clear message: the token is written by `scripts\services.ps1 run` |
| A value is out of range | The input is refused before the call (0 to 1e7) |

## 7. The tests

- **Go**: the client's unit tests against a fake gRPC server (the calls, the
  token, the error mapping).
- **C++**: a new test in `server/worldd/stats/tests`: the GM mods land in the
  computed stats and show up in the changed list, and clearing them restores
  the gear's numbers.
- **worldd**: a test for the new handlers, the way the other admin handlers
  are tested.
- **The dashboard**: a manual smoke test against the running stack.

## 8. The phases

1. The proto, the C++ (the GM mods and the four handlers), the codegen, the
   C++ tests. The worldd builds and the stack still runs.
2. The Go command: the Wails bootstrap, the client, the bindings.
3. The interface: the five tabs.
4. The tests, the build, the run.

## 9. The risks

| Risk | Fallback |
|---|---|
| The `wails` CLI or the npm install fails | The same UI in a hand-written WebView2 shell (`go-webview2`, no npm) |
| `buf` or the protoc plugins fail to install | The Go client files are generated once and kept in the tree |
| The stat override is refused by the engine (a stat the bundle does not have) | The error is returned as-is; the dashboard shows which stat was refused |

## 10. Out of scope

- A web dashboard that other machines can reach (the tool is local).
- Writing the overrides to the character, so they survive a reconnect (the
  dashboard sets them again).
- A 3D view of the world: the density grid is a 2D map.
- Editing the design data (the content database) itself.
