# AocAdmin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A desktop application that puts the whole Sandal Core server under one window, including four new administrative calls that let a game master override a player's stats (damage, resistances, health) through the stat engine itself.

**Architecture:** The server side adds a "GM stat mod" list to `stats::CharacterStats`, applied on top of the gear's mods in `compute()`, so an override travels the same path as a gear change and reaches the client with no new send path. Four new RPCs on `WorldAdminService` expose it. The application is a new Go command `services/cmd/aocadmin` (Wails): a WebView2 window whose HTML/CSS/JS calls the generated gRPC clients over localhost with the service token.

**Tech Stack:** C++20 (MSVC v143, CMake), Go 1.27, protobuf/gRPC (buf), Wails v2, HTML/CSS/JS.

## Global Constraints

- The repository is `D:\SandalCore-main`. The git clone used for the pull request is `D:\SandalCore-main\build\upstream` (branch `fix/windows-sdk-vcpkg-toolset-build`).
- The Windows SDK is repaired: `C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64` has its tools. `scripts\build.ps1` puts the SDK tools on PATH.
- The build must keep working with the VS 17 2022 generator (toolset v143) and the overlay triplet `vcpkg-triplets\x64-windows.cmake` (`VCPKG_PLATFORM_TOOLSET=v143`). Never let the libraries and the programs use different toolsets.
- The C++ sources use the project's conventions: four-space indent, `_` suffix for members, `k` prefix for constants, the `// (C.O.A)` comment markers, and a header comment block with the SPDX line in every file.
- The C++ tests use the project's harness: `CHECK(cond, msg)` and `CHECKF(cond, ...)` with the `g_pass` / `g_fail` counters, printing `[FAIL] msg` lines. There is no gtest.
- The Go code follows the services' conventions: package `main` in `services/cmd/<name>`, `gofmt` clean, errors wrapped with `%w`.
- The service token is `dist\services\service.token`; the Go side reads it with `services/internal/svcauth`.
- The UI is Arabic with `dir="rtl"`; the game's own names (stats, items, places) stay English because the game data is English.
- The application talks to `127.0.0.1` only. It opens no listening port.
- Every task ends with a commit.

### The names this plan uses (all read from the code, not invented)

| Name | Where |
|---|---|
| `stats::StatMod { int64_t stat; double value; }` | `server/worldd/stats/stat_engine.h` |
| `stats::CharacterStats` | `server/worldd/stats/character_stats.h` |
| `CharacterStats::values()`, `::find(guid)`, `::started()`, `::prepare(worn, changed, next, err)`, `::commit(next)`, `::update(worn, changed, err)`, `::start(worn, err)` | same |
| `b.stat_name(i)`, `b.stats[i].guid` | `server/worldd/stats/stat_program.h` (`StatBundle`) |
| `guid_named(b, "MaxHealth")` | the stats test's own helper (`stats/tests/test_worldd_stats.cpp`) |
| `PlayerSession::apply_gear_stats(const char* what)` | `server/worldd/session/player_session.h` |
| `PlayerSession::character_stats()` | same |
| `PlayerHandle { PlayerSession* player; Connection* connection; }` | `server/worldd/session/player_session.h` |
| `GmTravel::find_player(name)`, `GmTravel::give_money(character, copper, by)` | `server/worldd/session/gm_travel.h` |
| `GmStatus { Ok, NotFound, InvalidArgument, FailedPrecondition }` | same |
| `WorldAdminServer`, `AdminQueue::run(task, timeout)` | `server/worldd/svc/world_admin.h` |
| `Harness` (the gm_travel test's rig), `h.s` (the session) | `server/worldd/tests/test_worldd_gm_travel.cpp` |
| `STATS_CORPUS_PATH`, the corpus's bundle "tiny" and "main" | `server/worldd/tests/stats_fixture.h` |
| `worldadmin.Player { name, character_id, position, yaw_degrees, mounted, dead, move_speed_multiplier }` | the proto |
| `ListPlayersResponse.players`, `ListPlacesResponse.places` | the proto |

---

### Task 1: The GM stat overrides in the stat engine

The engine applies the **effects** in each stat's own form: an additive stat adds the value, a multiplying stat multiplies by it (the gear's own `in.mods` path is always additive, so it cannot carry the override). The GM override therefore travels as an `ActiveEffect`: one effect per (stat, value) pair, merged with the other effects in `compute()` and in the run the expression programs read.

**Files:**
- Modify: `server/worldd/stats/character_stats.h`
- Modify: `server/worldd/stats/character_stats.cpp`
- Test: `server/worldd/stats/tests/test_worldd_stats.cpp`

**Interfaces:**
- Consumes: `stats::ActiveEffect` (`server/worldd/stats/stat_engine.h`), `stats::EffectMod { int64_t stat; double value; }`, `stats::kRefTypeEffect`.
- Produces on `aoc::worldd::stats::CharacterStats`:
  ```cpp
  void set_gm_effects(std::vector<ActiveEffect> effects) { gm_effects_ = std::move(effects); }
  const std::vector<ActiveEffect>& gm_effects() const { return gm_effects_; }
  ```

- [ ] **Step 1: Write the failing test**

In `server/worldd/stats/tests/test_worldd_stats.cpp`, inside `void test_character_stats(const StatBundle& b)` (the function that already builds the gear catalog `cat` and the stat guids `kCon`, `kHealth`, `kMaxHealth`, ...), add at the end of the function, right before its closing brace:

```cpp
    // ── the game master's stat overrides ───────────────────────────────────
    // The override travels as an effect, so it follows the stat's own rule: an
    // additive stat adds the value, a multiplying one multiplies by it.
    const float max_before = cs.find(kMaxHealth)->value;
    const int64_t max_guid = cs.find(kMaxHealth)->guid;
    ActiveEffect gm;
    gm.effect = {kRefTypeEffect, 0};
    gm.stacks = 1;
    gm.mods = {{max_guid, 500.0}};
    cs.set_gm_effects({gm});
    CHECK(cs.update(none, changed, e), "the stats are computed with the override");
    CHECK(cs.gm_effects().size() == 1, "the override is remembered");
    CHECK(cs.find(kMaxHealth)->value == max_before + 500.0f, "the override is added to the stat");

    // a multiplying stat is multiplied by the value: 2 doubles it, 0.5 halves it
    const int64_t mult_guid = guid_named(b, "HealingMult");
    if (mult_guid) {
        const float mult_before = cs.find(mult_guid)->value;
        ActiveEffect gm2;
        gm2.effect = {kRefTypeEffect, 0};
        gm2.stacks = 1;
        gm2.mods = {{mult_guid, 2.0}};
        cs.set_gm_effects({gm2});
        CHECK(cs.update(none, changed, e), "the stats are computed with the multiplying override");
        CHECK(cs.find(mult_guid)->value == mult_before * 2.0f, "the override doubles the stat");
    }

    // clearing the overrides brings the gear's numbers back
    cs.set_gm_effects({});
    CHECK(cs.update(none, changed, e), "the stats are computed after the clear");
    CHECK(cs.find(kMaxHealth)->value == max_before, "the clear restores the value");
    CHECK(cs.gm_effects().empty(), "the overrides are gone");
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target test_worldd_stats
```
Expected: the build fails with `no member named 'set_gm_effects' in 'aoc::worldd::stats::CharacterStats'`.

- [ ] **Step 3: Add the setter, the getter and the member**

In `server/worldd/stats/character_stats.h`, after `void set_ranks(std::vector<SkillRank> ranks) { ranks_ = std::move(ranks); }` add:

```cpp
    /// The game master's overrides: the effects applied on top of the computed
    /// stats (the gear's, the level's, the other effects'). They are part of the
    /// next prepare(), so the client sees them like any other stat change. A value
    /// follows the stat's own rule: an additive stat adds it, a multiplying one
    /// multiplies by it.
    void set_gm_effects(std::vector<ActiveEffect> effects) { gm_effects_ = std::move(effects); }
    const std::vector<ActiveEffect>& gm_effects() const { return gm_effects_; }
```

and next to `std::vector<ActiveEffect> passives_;` in the private members add:

```cpp
    std::vector<ActiveEffect>      gm_effects_;   // the game master's overrides
```

- [ ] **Step 4: Apply the overrides in `compute()` and in the evaluation's run**

In `server/worldd/stats/character_stats.cpp`, in `CharacterStats::compute()`, change:

```cpp
    in.effects = passives_;
    in.effects.insert(in.effects.end(), effects_.begin(), effects_.end());
```
to:
```cpp
    in.effects = passives_;
    in.effects.insert(in.effects.end(), effects_.begin(), effects_.end());
    // the game master's overrides are effects too: the engine applies them in each
    // stat's own form, so an additive stat adds them and a multiplying one
    // multiplies by them.
    in.effects.insert(in.effects.end(), gm_effects_.begin(), gm_effects_.end());
```

And in `CharacterStats::evaluation()` (the run the expression programs read), merge
`gm_effects_` into its `StatInputs` the same way, so the ability formulas and the
client agree on the numbers.

- [ ] **Step 5: Run the test to verify it passes**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target test_worldd_stats
D:\SandalCore-main\dist\Release\test_worldd_stats.exe
```
Expected: the build succeeds and the test prints no `[FAIL]` line about the overrides.

- [ ] **Step 6: Commit**

```bash
git add server/worldd/stats/character_stats.h server/worldd/stats/character_stats.cpp server/worldd/stats/tests/test_worldd_stats.cpp
git commit -m "Added the game master's stat overrides to the stat engine"
```

---

### Task 2: The session's entry point for the overrides

`PlayerSession::apply_gear_stats(const char* what)` already recomputes the stats and sends the change to the client. The override rides that path: set the mods, then call it.

**Files:**
- Modify: `server/worldd/session/player_session.h`
- Modify: `server/worldd/session/player_session.cpp`
- Test: `server/worldd/tests/test_worldd_gm_travel.cpp`

**Interfaces:**
- Consumes: `CharacterStats::set_gm_effects(std::vector<ActiveEffect>)` and `gm_effects()` (Task 1 — the override travels as an **effect**, so the stat's own rule applies: an additive stat adds the value, a multiplying one multiplies by it).
- Produces on `aoc::worldd::PlayerSession`:
  ```cpp
  /// The game master's stat overrides on this character: the effects that carry
  /// them. They are applied on top of the computed stats and the change goes to
  /// the client like a gear change does. False, with `err`, when the stats
  /// cannot be computed.
  bool set_gm_effects(const std::vector<stats::ActiveEffect>& effects, std::string& err);
  void clear_gm_effects();
  const std::vector<stats::ActiveEffect>& gm_effects() const;
  /// The character's stats as they are now.
  std::vector<stats::StatValue> player_stats() const;
  /// Sets the current value of a stat the character holds (Health, Mana).
  bool set_player_current_stat(int64_t guid, double value, std::string& err);
  ```

- [ ] **Step 1: Write the failing test**

In `server/worldd/tests/test_worldd_gm_travel.cpp`, add a function that uses the file's own `Harness` (it holds `std::unique_ptr<PlayerSession> s`):

```cpp
// The game master's stat overrides reach the character: the stats the session
// holds change, and the override is remembered.
static void test_gm_effects(Harness& h) {
    PlayerSession* session = h.s.get();
    if (!session || !session->character_stats()) { std::printf("  [SKIP] the session has no stats\n"); return; }

    const auto before = session->player_stats();
    if (before.empty()) { CHECK(false, "the session has stats"); return; }
    const int64_t guid = before.front().guid;
    const float value = before.front().value;

    // the override travels as an effect: one stack, one mod on the stat
    stats::ActiveEffect effect;
    effect.effect = {stats::kRefTypeEffect, 0};
    effect.stacks = 1;
    effect.mods = {{guid, 5.0}};

    std::string err;
    CHECK(session->set_gm_effects({effect}, err), "the override is accepted");
    CHECK(session->gm_effects().size() == 1, "the override is remembered");
    const auto after = session->player_stats();
    bool found = false;
    for (const stats::StatValue& v : after) {
        if (v.guid == guid) { found = true; CHECK(std::fabs(v.value - (value + 5.0f)) < 0.001f, "the value grew"); }
    }
    CHECK(found, "the overridden stat is still there");

    session->clear_gm_effects();
    CHECK(session->gm_effects().empty(), "the override is cleared");
}
```

Call it from the file's `main()` with a `Harness` built the way the file's other checks build one (read the file; it has a helper that makes a joined session with a character).

- [ ] **Step 2: Run the test to verify it fails**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target test_worldd_gm_travel
```
Expected: the build fails with `no member named 'set_gm_effects' in 'aoc::worldd::PlayerSession'`.

- [ ] **Step 3: Add the session's methods**

In `server/worldd/session/player_session.h`, next to `const stats::CharacterStats* character_stats() const`, add the five declarations of the Interfaces block.

In `server/worldd/session/player_session.cpp`, add the definitions:

```cpp
bool PlayerSession::set_gm_effects(const std::vector<stats::ActiveEffect>& effects, std::string& err) {
    if (!stats_ || !stats_->started()) { err = "the character has no stats"; return false; }
    stats_->set_gm_effects(effects);
    const size_t failed = stat_updates_failed_;
    apply_gear_stats("the game master's stat overrides");
    if (stat_updates_failed_ != failed) { err = "the engine refused the stat overrides"; return false; }
    return true;
}

void PlayerSession::clear_gm_effects() {
    if (!stats_) return;
    stats_->set_gm_effects({});
    apply_gear_stats("the game master's stat overrides were cleared");
}

const std::vector<stats::ActiveEffect>& PlayerSession::gm_effects() const {
    static const std::vector<stats::ActiveEffect> kEmpty;
    return stats_ ? stats_->gm_effects() : kEmpty;
}

std::vector<stats::StatValue> PlayerSession::player_stats() const {
    if (!stats_) return {};
    return stats_->values();
}

bool PlayerSession::set_player_current_stat(int64_t guid, double value, std::string& err) {
    if (!stats_) { err = "the character has no stats"; return false; }
    stats_->set_current(guid, static_cast<float>(value));
    return true;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target test_worldd_gm_travel
D:\SandalCore-main\dist\Release\test_worldd_gm_travel.exe
```
Expected: the build succeeds and the test prints no `[FAIL]` line about the overrides.

- [ ] **Step 5: Commit**

```bash
git add server/worldd/session/player_session.h server/worldd/session/player_session.cpp server/worldd/tests/test_worldd_gm_travel.cpp
git commit -m "Gave the session the game master's stat overrides"
```

---

### Task 3: The GM helper's methods

`GmTravel` is the game master's tool box on the network thread; `find_player(name)` returns a `PlayerHandle` whose `player` is the `PlayerSession*`, so the new methods reach the session directly.

**Files:**
- Modify: `server/worldd/session/gm_travel.h`
- Modify: `server/worldd/session/gm_travel.cpp`
- Test: `server/worldd/tests/test_worldd_gm_travel.cpp`

**Interfaces:**
- Consumes: the session's methods (Task 2).
- Produces on `aoc::worldd::GmTravel`:
  ```cpp
  struct GmStatInfo {
      int64_t     guid = 0;
      std::string name;
      uint32_t    flags = 0;
      double      value = 0, max = 0;
      bool        has_override = false;
      double      override = 0;
  };
  struct GmStatsResult {
      GmStatus    status = GmStatus::Ok;
      std::string message;
      std::string character;
      std::vector<GmStatInfo> stats;
  };
  struct GmStatModsResult {
      GmStatus    status = GmStatus::Ok;
      std::string message;
      std::string character;
      std::vector<GmStatInfo> stats;      // the stats after the change
  };
  struct GmCurrentStatResult {
      GmStatus    status = GmStatus::Ok;
      std::string message;
      std::string character;
      double      value = 0;
  };

  GmStatsResult      get_player_stats(const std::string& character);
  GmStatModsResult   set_stat_mods(const std::string& character, const std::vector<std::pair<int64_t, double>>& mods, const std::string& by);
  GmStatModsResult   clear_stat_mods(const std::string& character, const std::string& by);
  GmCurrentStatResult set_current_stat(const std::string& character, int64_t stat, double value, const std::string& by);
  ```

- [ ] **Step 1: Write the failing test**

In `server/worldd/tests/test_worldd_gm_travel.cpp` add:

```cpp
// The GM's stat tools: the catalog, the overrides and the current value.
static void test_stat_tools(Harness& h, GmTravel& gm, const std::string& name) {
    GmStatsResult stats = gm.get_player_stats(name);
    CHECK(stats.status == GmStatus::Ok, "the stats are read");
    CHECK(!stats.stats.empty(), "the character has stats");

    if (!stats.stats.empty()) {
        const int64_t guid = stats.stats.front().guid;
        GmStatModsResult set = gm.set_stat_mods(name, {{guid, 3.0}}, "the test");
        CHECK(set.status == GmStatus::Ok, "the override is accepted");
        bool found = false;
        for (const GmStatInfo& s : set.stats) {
            if (s.guid == guid) { found = true; CHECK(s.has_override, "the override shows"); }
        }
        CHECK(found, "the overridden stat is in the answer");

        GmStatModsResult cleared = gm.clear_stat_mods(name, "the test");
        CHECK(cleared.status == GmStatus::Ok, "the clear is accepted");
        for (const GmStatInfo& s : cleared.stats) {
            if (s.guid == guid) CHECK(!s.has_override, "the override is gone");
        }
    }

    GmStatsResult missing = gm.get_player_stats("nobody at all");
    CHECK(missing.status == GmStatus::NotFound, "an unknown character is not found");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target test_worldd_gm_travel
```
Expected: the build fails with `no member named 'get_player_stats' in 'aoc::worldd::GmTravel'`.

- [ ] **Step 3: Add the structures and the declarations**

In `server/worldd/session/gm_travel.h`, after `struct GmTutorialResult { ... };` add the four structures of the Interfaces block, and after the last public method declaration (before `private:`) add the four method declarations.

- [ ] **Step 4: Implement the methods**

In `server/worldd/session/gm_travel.cpp`, add the definitions. They follow `give_money`'s shape (find the player, act, fill the result):

```cpp
namespace {
/// The bundle's stat names, by guid: the world's stat context holds it.
std::map<int64_t, std::string> stat_names(const PlayerWorld& world) {
    std::map<int64_t, std::string> names;
    const stats::StatBundle* bundle = world.stat_context().bundle;
    if (!bundle) return names;
    for (size_t i = 0; i < bundle->stats.size(); ++i) names[bundle->stats[i].guid] = bundle->stat_name(i);
    return names;
}
}  // namespace

GmStatsResult GmTravel::get_player_stats(const std::string& character) {
    GmStatsResult out;
    PlayerHandle h = find_player(character);
    if (!h) { out.status = GmStatus::NotFound; out.message = "no player named " + character; return out; }
    out.character = h.player->name();
    const std::map<int64_t, std::string> names = stat_names(world_);
    for (const stats::StatValue& v : h.player->player_stats()) {
        GmStatInfo info;
        info.guid = v.guid;
        info.flags = v.flags;
        info.value = v.value;
        info.max = v.has_max ? v.max : 0.0;
        const auto it = names.find(v.guid);
        if (it != names.end()) info.name = it->second;
        for (const stats::ActiveEffect& e : h.player->gm_effects()) {
            for (const stats::EffectMod& m : e.mods) {
                if (m.stat == v.guid) { info.has_override = true; info.override = m.value; }
            }
        }
        out.stats.push_back(std::move(info));
    }
    return out;
}

GmStatModsResult GmTravel::set_stat_mods(const std::string& character,
                                          const std::vector<std::pair<int64_t, double>>& mods,
                                          const std::string& by) {
    GmStatModsResult out;
    PlayerHandle h = find_player(character);
    if (!h) { out.status = GmStatus::NotFound; out.message = "no player named " + character; return out; }
    std::vector<stats::ActiveEffect> list;
    list.reserve(mods.size());
    for (const auto& m : mods) {
        // one effect per stat: the game master's own (no Effect record, no tags), one
        // stack, one mod on the stat. The engine applies it in the stat's own form:
        // an additive stat adds the value, a multiplying one multiplies by it.
        stats::ActiveEffect effect;
        effect.effect = {stats::kRefTypeEffect, 0};
        effect.stacks = 1;
        effect.mods = {{m.first, m.second}};
        list.push_back(std::move(effect));
    }
    std::string err;
    if (!h.player->set_gm_effects(list, err)) {
        out.status = GmStatus::FailedPrecondition;
        out.message = err;
        return out;
    }
    out.character = h.player->name();
    out.stats = get_player_stats(character).stats;
    return out;
}

GmStatModsResult GmTravel::clear_stat_mods(const std::string& character, const std::string& by) {
    GmStatModsResult out;
    PlayerHandle h = find_player(character);
    if (!h) { out.status = GmStatus::NotFound; out.message = "no player named " + character; return out; }
    h.player->clear_gm_effects();
    out.character = h.player->name();
    out.stats = get_player_stats(character).stats;
    return out;
}

GmCurrentStatResult GmTravel::set_current_stat(const std::string& character, int64_t stat,
                                                double value, const std::string& by) {
    GmCurrentStatResult out;
    PlayerHandle h = find_player(character);
    if (!h) { out.status = GmStatus::NotFound; out.message = "no player named " + character; return out; }
    std::string err;
    if (!h.player->set_player_current_stat(stat, value, err)) {
        out.status = GmStatus::FailedPrecondition;
        out.message = err;
        return out;
    }
    out.character = h.player->name();
    out.value = value;
    return out;
}
```

`world.stat_context().bundle` is the world's stat context; read `server/worldd/world/player_world.h` (or wherever `stat_context()` lives) for the exact member name and use it.

- [ ] **Step 5: Run the test to verify it passes**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target test_worldd_gm_travel
D:\SandalCore-main\dist\Release\test_worldd_gm_travel.exe
```
Expected: the build succeeds, the test prints no `[FAIL]`, and the exit code is 0.

- [ ] **Step 6: Commit**

```bash
git add server/worldd/session/gm_travel.h server/worldd/session/gm_travel.cpp server/worldd/tests/test_worldd_gm_travel.cpp
git commit -m "Added the game master's stat tools to the GM travel box"
```

---

### Task 4: The four administrative calls

The proto gains four RPCs and their messages; the C++ service gains the four handlers. The C++ protobuf code is generated by a CMake rule, so a rebuild produces the new stubs.

**Files:**
- Modify: `proto/internal/aoc/worldadmin/v1/worldadmin.proto`
- Modify: `server/worldd/svc/world_admin.h`
- Modify: `server/worldd/svc/world_admin.cpp`
- Regenerate: `services/gen/aoc/worldadmin/v1/*` (the Go stubs, by `buf generate`)

**Interfaces:**
- Consumes: `GmTravel`'s four methods (Task 3).
- Produces on `aoc.worldadmin.v1.WorldAdminService`: `GetPlayerStats`, `SetStatMods`, `ClearStatMods`, `SetCurrentStat`.

- [ ] **Step 1: Add the messages and the RPCs to the proto**

In `proto/internal/aoc/worldadmin/v1/worldadmin.proto`, inside `service WorldAdminService`, after `rpc SetWeaponXp(SetWeaponXpRequest) returns (SetWeaponXpResponse);` add:

```proto
  // The bundle's stats with the player's current values and the game master's
  // override on each. The catalog is the whole bundle, so the dashboard shows
  // every stat the game has.
  rpc GetPlayerStats(GetPlayerStatsRequest) returns (GetPlayerStatsResponse);
  // The game master's stat mods on a player, replacing the ones already there.
  rpc SetStatMods(SetStatModsRequest) returns (SetStatModsResponse);
  // The game master's stat mods on a player are removed.
  rpc ClearStatMods(ClearStatModsRequest) returns (ClearStatModsResponse);
  // The current value of a stat the player holds (Health, Mana, Stamina).
  rpc SetCurrentStat(SetCurrentStatRequest) returns (SetCurrentStatResponse);
```

and the messages, next to the other messages of the service:

```proto
// One stat of the bundle, with the player's current value.
message StatInfo {
  int64  guid    = 1;   // the stat's guid in the bundle
  string name    = 2;   // the bundle's name (English)
  uint32 flags   = 3;   // the bundle's kStat* flags
  double value   = 4;   // the player's current value (0 when the player is offline)
  double max     = 5;   // its maximum (0 when the stat has none)
  bool   has_override = 6;
  double override = 7;  // the game master's override on it (0 when there is none)
}

// A stat mod: a stat's guid and the value the game master gives it. The stat's
// own rule decides what the value means: an additive stat adds it, a multiplying
// one multiplies by it.
message StatMod {
  int64  stat  = 1;
  double value = 2;
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

- [ ] **Step 2: Add the handlers**

In `server/worldd/svc/world_admin.h`, the `Service` class implements the proto's service: add the four method declarations in the same style as the existing ones (each is `grpc::Status Name(grpc::ServerContext*, const adminv1::XRequest*, adminv1::XResponse*) override;`).

In `server/worldd/svc/world_admin.cpp`, add the four handlers, following `GiveMoney` exactly:

```cpp
namespace {
void fill_stats(google::protobuf::RepeatedPtrField<adminv1::StatInfo>* out, const std::vector<GmStatInfo>& stats) {
    for (const GmStatInfo& st : stats) {
        adminv1::StatInfo* info = out->Add();
        info->set_guid(st.guid);
        info->set_name(st.name);
        info->set_flags(st.flags);
        info->set_value(st.value);
        info->set_max(st.max);
        info->set_has_override(st.has_override);
        info->set_override(st.override);
    }
}
}  // namespace

grpc::Status GetPlayerStats(grpc::ServerContext* ctx, const adminv1::GetPlayerStatsRequest* req,
                             adminv1::GetPlayerStatsResponse* resp) override {
    if (auto s = authorize(ctx, "GetPlayerStats"); !s.ok()) return s;
    GmStatsResult out;
    if (auto s = on_net_thread([&] { out = owner.gm_.get_player_stats(req->character()); }); !s.ok()) return s;
    if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
    fill_stats(resp->mutable_stats(), out.stats);
    return grpc::Status::OK;
}

grpc::Status SetStatMods(grpc::ServerContext* ctx, const adminv1::SetStatModsRequest* req,
                         adminv1::SetStatModsResponse* resp) override {
    if (auto s = authorize(ctx, "SetStatMods"); !s.ok()) return s;
    const std::string by = "the admin API (" + ctx->peer() + ")";
    std::vector<std::pair<int64_t, double>> mods;
    mods.reserve(req->mods_size());
    for (const adminv1::StatMod& m : req->mods()) mods.emplace_back(m.stat(), m.value());
    GmStatModsResult out;
    if (auto s = on_net_thread([&] { out = owner.gm_.set_stat_mods(req->character(), mods, by); }); !s.ok()) return s;
    if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
    fill_stats(resp->mutable_stats(), out.stats);
    return grpc::Status::OK;
}

grpc::Status ClearStatMods(grpc::ServerContext* ctx, const adminv1::ClearStatModsRequest* req,
                           adminv1::ClearStatModsResponse* resp) override {
    if (auto s = authorize(ctx, "ClearStatMods"); !s.ok()) return s;
    const std::string by = "the admin API (" + ctx->peer() + ")";
    GmStatModsResult out;
    if (auto s = on_net_thread([&] { out = owner.gm_.clear_stat_mods(req->character(), by); }); !s.ok()) return s;
    if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
    fill_stats(resp->mutable_stats(), out.stats);
    return grpc::Status::OK;
}

grpc::Status SetCurrentStat(grpc::ServerContext* ctx, const adminv1::SetCurrentStatRequest* req,
                            adminv1::SetCurrentStatResponse* resp) override {
    if (auto s = authorize(ctx, "SetCurrentStat"); !s.ok()) return s;
    const std::string by = "the admin API (" + ctx->peer() + ")";
    GmCurrentStatResult out;
    if (auto s = on_net_thread([&] { out = owner.gm_.set_current_stat(req->character(), req->stat(), req->value(), by); }); !s.ok()) return s;
    if (out.status != GmStatus::Ok) return grpc::Status(code_of(out.status), out.message);
    resp->set_value(out.value);
    return grpc::Status::OK;
}
```

The member that holds the GM helper: read `server/worldd/svc/world_admin.h` — the service holds it as `travel_` (a `GmTravel&`); use that name.

- [ ] **Step 3: Build worldd; the C++ stubs regenerate**

Run:
```powershell
pwsh -File D:\SandalCore-main\scripts\build.ps1 -Target worldd
```
Expected: the build succeeds, and `build\x64\server\worldd\generated\aoc\worldadmin\v1\*` now holds the new calls.

- [ ] **Step 4: Test the new handlers**

In `server/worldd/tests/test_worldd_services.cpp` (the file that already drives the
services), add a check for the new calls: a call without the token is refused, a
call with the token for a character that is not there answers `NotFound`, and a
call with the token for a character that is there answers `Ok`. Read the file
first: it has the rig that starts the world with a session.

```cpp
// The new administrative calls: the token is still the door, and an unknown
// character is refused.
static void test_admin_stat_calls() {
    // the file's own rig: the world, the session and the token
    CHECK(admin_call_without_the_token_is_refused(), "a call without the token is refused");
    CHECK(admin_call_for_an_unknown_character_is_not_found(), "an unknown character is not found");
    CHECK(admin_call_for_a_player_answers_ok(), "the call for a player answers");
}
```

- [ ] **Step 5: Regenerate the Go stubs**

Run (PowerShell):
```powershell
$env:PATH = "C:\Program Files\Go\bin;$env:USERPROFILE\go\bin;$env:PATH"
Set-Location D:\SandalCore-main\services
go install github.com/bufbuild/buf/cmd/buf@latest
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
buf generate
```
Expected: `services\gen\aoc\worldadmin\v1\worldadmin.pb.go` and `worldadmin_grpc.pb.go` hold the four new calls.

- [ ] **Step 6: Check the Go module still builds**

Run:
```powershell
Set-Location D:\SandalCore-main\services
go build ./...
```
Expected: no error.

- [ ] **Step 7: Commit**

```bash
git add proto/internal/aoc/worldadmin/v1/worldadmin.proto server/worldd/svc/world_admin.h server/worldd/svc/world_admin.cpp server/worldd/tests/test_worldd_services.cpp services/gen
git commit -m "Added the four administrative calls for the game master's stats"
```
---

### Task 5: The Go client of the application

The application needs one client that reaches the server with the service token. It reuses the generated code and `services/internal/svcauth`.

**Files:**
- Create: `services/cmd/aocadmin/client.go`
- Create: `services/cmd/aocadmin/client_test.go`

**Interfaces:**
- Consumes: `services/gen/aoc/worldadmin/v1`, `services/gen/aoc/character/v1`, `services/gen/aoc/citynode/v1`, `services/internal/svcauth`.
- Produces: `Client` with the methods the UI calls.

- [ ] **Step 1: Write the failing test**

Create `services/cmd/aocadmin/client_test.go`:

```go
// SPDX-License-Identifier: AGPL-3.3-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
package main

import (
	"context"
	"testing"
)

// A missing token file is an error: without the token the server refuses every
// call, so the dashboard says so instead of failing later.
func TestClientNeedsAToken(t *testing.T) {
	c := NewClient(Options{TokenFile: "no-such-token.txt"})
	if err := c.Dial(context.Background()); err == nil {
		t.Fatal("a missing token file must be an error")
	}
}

// The addresses have defaults, so the dashboard works with the stock stack.
func TestClientHasTheDefaultAddresses(t *testing.T) {
	c := NewClient(Options{})
	if c.AdminAddr() == "" {
		t.Fatal("the admin address must have a default")
	}
	if c.CharacterAddr() == "" {
		t.Fatal("the character address must have a default")
	}
	if c.WorldStreamAddr() == "" {
		t.Fatal("the world stream address must have a default")
	}
	if c.CityNodeAddr() == "" {
		t.Fatal("the city node address must have a default")
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```powershell
Set-Location D:\SandalCore-main\services
go test ./cmd/aocadmin/...
```
Expected: `undefined: NewClient`.

- [ ] **Step 3: Write the client**

Create `services/cmd/aocadmin/client.go`:

```go
// SPDX-License-Identifier: AGPL-3.3-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
//  client.go: the calls the dashboard makes to the server: the administrative API
//  of worldd, the live world stream, the character service and the city nodes.
//  Every call carries the service token, the way the other services authenticate
//  to each other.
package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"

	"aocrop/services/gen/aoc/character/v1"
	"aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/gen/aoc/worldadmin/v1"
)

// CallTimeout: how long a call waits.
const CallTimeout = 5 * time.Second

// Options: where the services are and where the token is.
type Options struct {
	AdminAddr      string
	StreamAddr     string
	CharacterAddr  string
	CityNodeAddr   string
	TokenFile      string
}

// Client: the connections to the server.
type Client struct {
	opts      Options
	conn      *grpc.ClientConn
	charConn  *grpc.ClientConn
	cityConn  *grpc.ClientConn
	admin     worldadmin.WorldAdminServiceClient
	stream    worldadmin.WorldStreamServiceClient
	character character.CharacterServiceClient
	citynode  citynode.CityNodeServiceClient
	token     string
}

// NewClient: the client with the defaults the services use.
func NewClient(opts Options) *Client {
	if opts.AdminAddr == "" {
		opts.AdminAddr = envOr("AOC_WORLD_ADMIN_ADDR", "127.0.0.1:50056")
	}
	if opts.StreamAddr == "" {
		opts.StreamAddr = envOr("AOC_WORLD_STREAM_ADDR", "127.0.0.1:50057")
	}
	if opts.CharacterAddr == "" {
		opts.CharacterAddr = envOr("AOC_CHARACTER_GRPC_ADDR", "127.0.0.1:50062")
	}
	if opts.CityNodeAddr == "" {
		opts.CityNodeAddr = envOr("AOC_CITYNODE_GRPC_ADDR", "127.0.0.1:50055")
	}
	if opts.TokenFile == "" {
		opts.TokenFile = envOr("AOC_SERVICE_TOKEN_FILE", defaultTokenFile())
	}
	return &Client{opts: opts}
}

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

// defaultTokenFile: the token services.ps1 writes, next to the services' exes.
func defaultTokenFile() string {
	if exe, err := os.Executable(); err == nil {
		return filepath.Join(filepath.Dir(exe), "service.token")
	}
	return "service.token"
}

// Dial: reads the token and opens the connections.
func (c *Client) Dial(ctx context.Context) error {
	raw, err := os.ReadFile(c.opts.TokenFile)
	if err != nil {
		return fmt.Errorf("the service token (%s) cannot be read: %w", c.opts.TokenFile, err)
	}
	c.token = strings.TrimSpace(string(raw))

	dial := func(addr string) (*grpc.ClientConn, error) {
		return grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	}
	if c.conn, err = dial(c.opts.AdminAddr); err != nil {
		return fmt.Errorf("the server at %s cannot be reached: %w", c.opts.AdminAddr, err)
	}
	if c.charConn, err = dial(c.opts.CharacterAddr); err != nil {
		return fmt.Errorf("the character service at %s cannot be reached: %w", c.opts.CharacterAddr, err)
	}
	if c.cityConn, err = dial(c.opts.CityNodeAddr); err != nil {
		return fmt.Errorf("the city nodes at %s cannot be reached: %w", c.opts.CityNodeAddr, err)
	}
	c.admin = worldadmin.NewWorldAdminServiceClient(c.conn)
	c.stream = worldadmin.NewWorldStreamServiceClient(c.conn)
	c.character = character.NewCharacterServiceClient(c.charConn)
	c.citynode = citynode.NewCityNodeServiceClient(c.cityConn)
	return nil
}

// Close: closes the connections.
func (c *Client) Close() {
	for _, conn := range []*grpc.ClientConn{c.conn, c.charConn, c.cityConn} {
		if conn != nil {
			conn.Close()
		}
	}
}

// The addresses, for the window's status line.
func (c *Client) AdminAddr() string       { return c.opts.AdminAddr }
func (c *Client) CharacterAddr() string   { return c.opts.CharacterAddr }
func (c *Client) WorldStreamAddr() string { return c.opts.StreamAddr }
func (c *Client) CityNodeAddr() string    { return c.opts.CityNodeAddr }

// WithToken: the token the calls carry.
func (c *Client) WithToken(ctx context.Context) context.Context {
	return metadata.AppendToOutgoingContext(ctx, "aoc-service-token", c.token)
}
```

The metadata key must be the one `services/internal/svcauth` uses: read that file and use its key and its token-file default.

- [ ] **Step 4: Run the test to verify it passes**

Run:
```powershell
Set-Location D:\SandalCore-main\services
go test ./cmd/aocadmin/...
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add services/cmd/aocadmin/client.go services/cmd/aocadmin/client_test.go
git commit -m "Added the dashboard's client to the server"
```

---

### Task 6: The application window

**Files:**
- Create: `services/cmd/aocadmin/main.go`
- Create: `services/cmd/aocadmin/app.go`
- Create: `services/cmd/aocadmin/frontend/index.html`
- Create: `services/cmd/aocadmin/frontend/app.js`
- Create: `services/cmd/aocadmin/frontend/style.css`
- Modify: `services/go.mod`, `services/go.sum`

**Interfaces:**
- Consumes: `Client` (Task 5), the generated clients.
- Produces: the Wails bindings the UI calls: `Status()`, `Players()`, `Places()`, `Stats(character)`, `SetStatMods(character, mods)`, `ClearStatMods(character)`, `SetCurrentStat(character, stat, value)`, `GiveMoney(character, copper)`, `GiveItem(character, item, count)`, `Teleport(character, place, x, y, z)`, `SetSpeed(character, multiplier)`, `NpcAction(action, id, x, y, z)`, `Nodes()`, `LogTail(name, lines)`.

- [ ] **Step 1: Add the Wails dependency**

Run:
```powershell
Set-Location D:\SandalCore-main\services
$env:PATH = "C:\Program Files\Go\bin;$env:USERPROFILE\go\bin;$env:PATH"
go get github.com/wailsapp/wails/v2@v2.10.2
go install github.com/wailsapp/wails/v2/cmd/wails@v2.10.2
```
Expected: the dependency lands in `go.mod`.

- [ ] **Step 2: Write the window**

Create `services/cmd/aocadmin/main.go`:

```go
// SPDX-License-Identifier: AGPL-3.3-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
//  main.go: the game master's dashboard: one window with the players, their
//  stats, the living world and the cities.
package main

import (
	"context"
	"embed"
	"log"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

//go:embed all:frontend
var assets embed.FS

func main() {
	app := NewApp()
	err := wails.Run(&options.App{
		Title:            "Sandal Core — لوحة تحكم الخادم",
		Width:            1360,
		Height:           900,
		AssetServer:      &assetserver.Options{Assets: assets},
		OnStartup:        app.startup,
		OnShutdown:       app.shutdown,
		BackgroundColour: &options.RGBA{R: 18, G: 20, B: 26, A: 1},
	})
	if err != nil {
		log.Fatal(err)
	}
	_ = context.Background()
}
```

- [ ] **Step 3: Write the bindings**

Create `services/cmd/aocadmin/app.go`:

```go
// SPDX-License-Identifier: AGPL-3.3-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
//  app.go: the methods the window calls. Each one is one call to the server.
package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"aocrop/services/gen/aoc/worldadmin/v1"
)

// App: the window's backend.
type App struct {
	ctx    context.Context
	client *Client
}

// NewApp: the application.
func NewApp() *App { return &App{ctx: context.Background()} }

// startup: the window is up: connect to the server.
func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	c := NewClient(Options{})
	if err := c.Dial(ctx); err != nil {
		log.Printf("[aocadmin] the server is not reachable: %v", err)
		return
	}
	a.client = c
}

// shutdown: the window is closed: close the connections.
func (a *App) shutdown(ctx context.Context) {
	if a.client != nil {
		a.client.Close()
	}
}

// Status: what the window shows in its status line.
type Status struct {
	AdminAddr     string `json:"adminAddr"`
	CharacterAddr string `json:"characterAddr"`
	StreamAddr    string `json:"streamAddr"`
	CityNodeAddr  string `json:"cityNodeAddr"`
	Connected     bool   `json:"connected"`
	Error         string `json:"error"`
}

// Status: the server's state.
func (a *App) Status() Status {
	if a.client == nil {
		return Status{Error: "الخادم مو شغّال"}
	}
	return Status{
		AdminAddr:     a.client.AdminAddr(),
		CharacterAddr: a.client.CharacterAddr(),
		StreamAddr:    a.client.WorldStreamAddr(),
		CityNodeAddr:  a.client.CityNodeAddr(),
		Connected:     true,
	}
}

// Players: the players in the world.
func (a *App) Players() ([]worldadmin.Player, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.ListPlayers(a.client.WithToken(ctx), &worldadmin.ListPlayersRequest{})
	if err != nil {
		return nil, err
	}
	return out.GetPlayers(), nil
}

// Places: the places a teleport can name.
func (a *App) Places() ([]worldadmin.Place, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.ListPlaces(a.client.WithToken(ctx), &worldadmin.ListPlacesRequest{})
	if err != nil {
		return nil, err
	}
	return out.GetPlaces(), nil
}

// Stats: the bundle's stats with the player's current values.
func (a *App) Stats(character string) ([]worldadmin.StatInfo, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.GetPlayerStats(a.client.WithToken(ctx), &worldadmin.GetPlayerStatsRequest{Character: character})
	if err != nil {
		return nil, err
	}
	return out.GetStats(), nil
}

// SetStatMods: the game master's stat mods on a player, replacing the ones there.
func (a *App) SetStatMods(character string, mods []worldadmin.StatMod) ([]worldadmin.StatInfo, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.SetStatMods(a.client.WithToken(ctx), &worldadmin.SetStatModsRequest{Character: character, Mods: mods})
	if err != nil {
		return nil, err
	}
	return out.GetStats(), nil
}

// ClearStatMods: the overrides are removed.
func (a *App) ClearStatMods(character string) ([]worldadmin.StatInfo, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.ClearStatMods(a.client.WithToken(ctx), &worldadmin.ClearStatModsRequest{Character: character})
	if err != nil {
		return nil, err
	}
	return out.GetStats(), nil
}

// SetCurrentStat: the current value of a stat the player holds.
func (a *App) SetCurrentStat(character string, stat int64, value float64) (float64, error) {
	if a.client == nil {
		return 0, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.SetCurrentStat(a.client.WithToken(ctx), &worldadmin.SetCurrentStatRequest{Character: character, Stat: stat, Value: value})
	if err != nil {
		return 0, err
	}
	return out.GetValue(), nil
}

// GiveMoney: copper into the player's purse.
func (a *App) GiveMoney(character string, copper int64) (int64, error) {
	if a.client == nil {
		return 0, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.GiveMoney(a.client.WithToken(ctx), &worldadmin.GiveMoneyRequest{Character: character, Copper: copper})
	if err != nil {
		return 0, err
	}
	return out.GetBalance(), nil
}

// GiveItem: an item into the player's bags.
func (a *App) GiveItem(character, item string, count int32) (string, error) {
	if a.client == nil {
		return "", fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.GiveItem(a.client.WithToken(ctx), &worldadmin.GiveItemRequest{Character: character, Item: item, Count: count})
	if err != nil {
		return "", err
	}
	return out.GetItem(), nil
}

// Teleport: the player to a place or to a point.
func (a *App) Teleport(character, place string, x, y, z float64, hasZ bool) (string, error) {
	if a.client == nil {
		return "", fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	req := &worldadmin.TeleportRequest{Character: character}
	if place != "" {
		req.Target = &worldadmin.TeleportRequest_Place{Place: place}
	} else {
		req.Target = &worldadmin.TeleportRequest_Point{Point: &worldadmin.Point{X: x, Y: y, Z: z, HasZ: hasZ}}
	}
	out, err := a.client.admin.Teleport(a.client.WithToken(ctx), req)
	if err != nil {
		return "", err
	}
	return out.GetCharacter(), nil
}

// SetSpeed: the player's run speed.
func (a *App) SetSpeed(character string, multiplier float64) (float64, error) {
	if a.client == nil {
		return 0, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.admin.SetMoveSpeed(a.client.WithToken(ctx), &worldadmin.SetMoveSpeedRequest{Character: character, Multiplier: multiplier})
	if err != nil {
		return 0, err
	}
	return out.GetMultiplier(), nil
}

// NpcAction: kill, respawn, reset, heal, despawn or move a creature.
func (a *App) NpcAction(action string, id uint64, x, y, z float64, hasZ bool) (string, error) {
	if a.client == nil {
		return "", fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	tok := a.client.WithToken(ctx)
	switch action {
	case "kill":
		out, err := a.client.admin.KillNpc(tok, &worldadmin.KillNpcRequest{Id: id})
		if err != nil {
			return "", err
		}
		return out.GetResult(), nil
	case "respawn":
		out, err := a.client.admin.RespawnNpc(tok, &worldadmin.RespawnNpcRequest{Id: id})
		if err != nil {
			return "", err
		}
		return out.GetResult(), nil
	case "reset":
		out, err := a.client.admin.ResetNpc(tok, &worldadmin.ResetNpcRequest{Id: id})
		if err != nil {
			return "", err
		}
		return out.GetResult(), nil
	case "heal":
		out, err := a.client.admin.HealNpc(tok, &worldadmin.HealNpcRequest{Id: id})
		if err != nil {
			return "", err
		}
		return out.GetResult(), nil
	case "despawn":
		out, err := a.client.admin.DespawnNpc(tok, &worldadmin.DespawnNpcRequest{Id: id})
		if err != nil {
			return "", err
		}
		return out.GetResult(), nil
	case "move":
		out, err := a.client.admin.MoveNpc(tok, &worldadmin.MoveNpcRequest{Id: id, Point: &worldadmin.Point{X: x, Y: y, Z: z, HasZ: hasZ}})
		if err != nil {
			return "", err
		}
		return out.GetResult(), nil
	}
	return "", fmt.Errorf("ما فيه أمر %q", action)
}

// StartStack: starts the server stack (scripts\start-stack.ps1), so the window
// can bring it up when it is down.
func (a *App) StartStack() (string, error) {
	script := filepath.Join("..", "..", "build", "start-stack.ps1")
	cmd := exec.Command("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("the stack did not start: %w: %s", err, string(out))
	}
	return "شغّلنا الستاك", nil
}

// LogTail: the last lines of a log file.
func (a *App) LogTail(name string, lines int) []string {
	path := filepath.Join("..", "..", "dist", "services", "logs", name)
	raw, err := os.ReadFile(path)
	if err != nil {
		return []string{}
	}
	all := strings.Split(strings.TrimRight(string(raw), "\n"), "\n")
	if len(all) > lines {
		all = all[len(all)-lines:]
	}
	return all
}
```

The bodies marked `...` follow the same shape as the ones written out above: the matching RPC, the token, the timeout. Read the proto's message names (`GiveMoneyRequest`, `GiveItemRequest`, `TeleportRequest`, `SetMoveSpeedRequest`, `KillNpcRequest`, ...) and use them.

- [ ] **Step 4: Write the window's shell**

Create `services/cmd/aocadmin/frontend/index.html`:

```html
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
  <meta charset="utf-8">
  <title>Sandal Core — لوحة تحكم الخادم</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <header>
    <h1>لوحة تحكم الخادم</h1>
    <div class="status-area">
      <div id="status" class="status">…</div>
      <button id="start-stack" hidden>شغّل الستاك</button>
    </div>
  </header>

  <nav class="tabs">
    <button class="tab active" data-tab="players">اللاعبون</button>
    <button class="tab" data-tab="stats">الإحصائيات</button>
    <button class="tab" data-tab="world">العالم</button>
    <button class="tab" data-tab="cities">المدن</button>
    <button class="tab" data-tab="logs">السجل</button>
  </nav>

  <main>
    <section id="players" class="panel active">
      <div class="row">
        <input id="player-filter" placeholder="ابحث عن لاعب…">
        <button id="refresh-players">تحديث</button>
      </div>
      <table id="players-table">
        <thead><tr><th>الاسم</th><th>المعرّف</th><th>الموقع</th><th>مركب</th><th>السرعة</th><th>إجراءات</th></tr></thead>
        <tbody></tbody>
      </table>
    </section>

    <section id="stats" class="panel">
      <div class="row">
        <span id="stats-player">اختر لاعباً من تبويب اللاعبين</span>
        <input id="stats-filter" placeholder="ابحث عن stat…">
        <button id="save-stats">احفظ</button>
        <button id="max-all-stats">املأ الكل بالحد الأقصى</button>
        <button id="clear-stats">امسح الكل</button>
        <button id="fill-health">املأ الدم</button>
      </div>
      <table id="stats-table">
        <thead><tr><th>Stat</th><th>النوع</th><th>الحالي</th><th>الأقصى</th><th>التعديل</th><th>قيمة جديدة</th></tr></thead>
        <tbody></tbody>
      </table>
    </section>

    <section id="world" class="panel">
      <div class="row">
        <input id="world-filter" placeholder="ابحث في العالم (حرفان على الأقل)…" value="a">
        <button id="refresh-world">تحديث</button>
      </div>
      <table id="world-table">
        <thead><tr><th>الاسم</th><th>النوع</th><th>المستوى</th><th>الصحة</th><th>الموقع</th><th>إجراءات</th></tr></thead>
        <tbody></tbody>
      </table>
    </section>

    <section id="cities" class="panel">
      <table id="cities-table">
        <thead><tr><th>المدينة</th><th>المستوى</th><th>الخبرة</th><th>الساعة</th><th>إجراءات</th></tr></thead>
        <tbody></tbody>
      </table>
    </section>

    <section id="logs" class="panel">
      <pre id="log-view"></pre>
    </section>
  </main>

  <div id="toast" class="toast"></div>
  <script src="app.js"></script>
</body>
</html>
```

- [ ] **Step 5: Write the window's script**

Create `services/cmd/aocadmin/frontend/app.js`:

```javascript
// app.js: the dashboard's calls and tables. The Go side is window.go.<method>.
const state = { players: [], selected: "", stats: [], places: [], tab: "players" };

function toast(message, isError) {
  const el = document.getElementById("toast");
  el.textContent = message;
  el.className = "toast" + (isError ? " error" : "");
  el.classList.add("show");
  clearTimeout(el._timer);
  el._timer = setTimeout(() => el.classList.remove("show"), 4000);
}

async function call(name, ...args) {
  try {
    return await window.go[name].apply(null, args);
  } catch (err) {
    toast(String(err), true);
    return null;
  }
}

// ── the tabs ────────────────────────────────────────────────────────────
document.querySelectorAll(".tab").forEach((button) => {
  button.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
    button.classList.add("active");
    document.getElementById(button.dataset.tab).classList.add("active");
    state.tab = button.dataset.tab;
    refresh();
  });
});

// ── the players tab ─────────────────────────────────────────────────────
async function loadPlayers() {
  const players = await call("Players");
  if (!players) return;
  state.players = players;
  const filter = document.getElementById("player-filter").value.toLowerCase();
  const body = document.querySelector("#players-table tbody");
  body.innerHTML = "";
  for (const p of players) {
    if (filter && !p.name.toLowerCase().includes(filter)) continue;
    const row = document.createElement("tr");
    if (p.name === state.selected) row.classList.add("selected");
    row.innerHTML = `
      <td>${p.name}</td>
      <td class="mono">${p.character_id.slice(0, 8)}</td>
      <td class="mono">${Math.round(p.position.x)}, ${Math.round(p.position.y)}, ${Math.round(p.position.z)}</td>
      <td>${p.mounted ? "نعم" : "لا"}</td>
      <td>×${p.move_speed_multiplier}</td>
      <td class="actions">
        <button data-act="select">إحصائيات</button>
        <button data-act="teleport">انقل</button>
        <button data-act="speed">السرعة</button>
        <button data-act="money">فلوس</button>
        <button data-act="item">item</button>
      </td>`;
    row.querySelector('[data-act="select"]').addEventListener("click", () => {
      state.selected = p.name;
      document.querySelector('.tab[data-tab="stats"]').click();
    });
    row.querySelector('[data-act="teleport"]').addEventListener("click", () => teleport(p));
    row.querySelector('[data-act="speed"]').addEventListener("click", () => setSpeed(p));
    row.querySelector('[data-act="money"]').addEventListener("click", () => giveMoney(p));
    row.querySelector('[data-act="item"]').addEventListener("click", () => giveItem(p));
    body.appendChild(row);
  }
}

async function teleport(p) {
  const places = state.places.length ? state.places : (state.places = (await call("Places")) || []);
  const names = places.map((x) => x.display_name || x.name);
  const where = prompt("انقل إلى:\n" + names.join("\n"));
  if (!where) return;
  const place = places.find((x) => (x.display_name || x.name) === where);
  if (!place) { toast("ما فيه مكان بهذا الاسم", true); return; }
  const out = await call("Teleport", p.name, place.name, 0, 0, 0, false);
  if (out) toast("انتقل " + p.name + " إلى " + (place.display_name || place.name));
}

async function setSpeed(p) {
  const value = prompt("السرعة (0.5 - 10، و1 يرجّعها):", "3");
  if (!value) return;
  const out = await call("SetSpeed", p.name, Number(value));
  if (out !== null) toast("سرعة " + p.name + " صارت ×" + out);
}

async function giveMoney(p) {
  const value = prompt("كم نحط من copper؟", "1000");
  if (!value) return;
  const out = await call("GiveMoney", p.name, Number(value));
  if (out !== null) toast("الرصيد صار " + out);
}

async function giveItem(p) {
  const name = prompt("اسم الـ item (أو الـ Guid):");
  if (!name) return;
  const out = await call("GiveItem", p.name, name, 1);
  if (out) toast("أُعطي " + p.name + " " + out);
}

// ── the stats tab ───────────────────────────────────────────────────────
// The bundle's own flags (server/worldd/stats/stat_program.h): a stat whose
// kStatMulMods bit is set multiplies by its mods instead of adding them, and a
// kStatMulCompl stat multiplies by one minus the value.
const kStatMulMods = 1 << 0;
const kStatMulCompl = 1 << 6;
const kStatResource = 1 << 7;

// The unit of an override: the stat's own rule decides what the number means.
function statUnit(stat) {
  if ((stat.flags & kStatMulMods) === 0) return "+";
  if ((stat.flags & kStatMulCompl) !== 0) return "×(1-v)";
  return "×";
}

function statKind(stat) {
  const name = stat.name || "";
  if (/resist/i.test(name)) return "مقاومة";
  if (/damage/i.test(name)) return "ضرر";
  if (/^max/i.test(name)) return "حد أقصى";
  if ((stat.flags & kStatResource) !== 0) return "مورد";
  return "خاصية";
}

async function loadStats() {
  if (!state.selected) return;
  const stats = await call("Stats", state.selected);
  if (!stats) return;
  state.stats = stats;
  document.getElementById("stats-player").textContent = "إحصائيات: " + state.selected;
  const filter = document.getElementById("stats-filter").value.toLowerCase();
  const body = document.querySelector("#stats-table tbody");
  body.innerHTML = "";
  for (const stat of stats) {
    if (filter && !(stat.name || "").toLowerCase().includes(filter)) continue;
    const unit = statUnit(stat);
    const row = document.createElement("tr");
    row.innerHTML = `
      <td class="mono">${stat.name || stat.guid}</td>
      <td>${statKind(stat)}</td>
      <td>${stat.value}</td>
      <td>${stat.max || "بلا حد"}</td>
      <td>${stat.has_override ? unit + stat.override : "-"}</td>
      <td><input class="stat-input" data-guid="${stat.guid}" data-multi="${(stat.flags & kStatMulMods) !== 0 ? 1 : 0}"
                 value="${stat.has_override ? stat.override : ""}" placeholder="${unit}"></td>`;
    body.appendChild(row);
  }
}

document.getElementById("save-stats").addEventListener("click", async () => {
  if (!state.selected) { toast("اختر لاعباً أول", true); return; }
  const mods = [];
  document.querySelectorAll("#stats-table tbody input.stat-input").forEach((input) => {
    if (input.value.trim() === "") return;
    mods.push({ stat: Number(input.dataset.guid), value: Number(input.value) });
  });
  const stats = await call("SetStatMods", state.selected, mods);
  if (stats) { toast("حُفظت التعديلات"); await loadStats(); }
});

document.getElementById("max-all-stats").addEventListener("click", async () => {
  if (!state.selected) { toast("اختر لاعباً أول", true); return; }
  const stats = await call("Stats", state.selected);
  if (!stats) return;
  // Every stat that declares a maximum goes to it. The override is what the stat's
  // own rule applies, so the number that is sent is the one the engine needs:
  // an additive stat takes the missing amount (plus what it already has), a
  // multiplying one takes the factor (times what it already has). A stat with no
  // declared maximum (max = 0) is left alone.
  const mods = [];
  for (const stat of stats) {
    const max = stat.max || 0;
    const value = stat.value || 0;
    const current = stat.has_override ? stat.override : 0;
    const multiplicative = (stat.flags & kStatMulMods) !== 0;
    if (max > 0 && max > value) {
      if (multiplicative) {
        if (value > 0) mods.push({ stat: stat.guid, value: (max * (current || 1)) / value });
      } else {
        mods.push({ stat: stat.guid, value: max - value + current });
      }
    } else if (stat.has_override) {
      mods.push({ stat: stat.guid, value: current });   // keep the override it has
    }
  }
  if (!mods.length) { toast("كل الإحصائيات عندها حدها الأقصى بالفعل"); return; }
  const after = await call("SetStatMods", state.selected, mods);
  if (after) { toast("ملّأت " + mods.length + " stat بالحد الأقصى"); await loadStats(); }
});

document.getElementById("clear-stats").addEventListener("click", async () => {
  if (!state.selected) { toast("اختر لاعباً أول", true); return; }
  const stats = await call("ClearStatMods", state.selected);
  if (stats) { toast("امسحنا كل التعديلات"); await loadStats(); }
});

document.getElementById("fill-health").addEventListener("click", async () => {
  const health = state.stats.find((s) => /health/i.test(s.name || "") && !/^max/i.test(s.name || ""));
  if (!health) { toast("ما لقيت stat الدم", true); return; }
  const value = await call("SetCurrentStat", state.selected, health.guid, health.max || health.value);
  if (value !== null) toast("الدم صار " + value);
});

document.getElementById("stats-filter").addEventListener("input", loadStats);
document.getElementById("player-filter").addEventListener("input", loadPlayers);

// ── the world tab ───────────────────────────────────────────────────────
// The world stream's Search: the entities whose name holds the query (at least
// two characters). The details of the one that is picked come from Inspect.
async function loadWorld() {
  const query = document.getElementById("world-filter").value;
  if (query.length < 2) { return; }
  const rows = await call("SearchEntities", query);
  if (!rows) return;
  const body = document.querySelector("#world-table tbody");
  body.innerHTML = "";
  for (const row of rows) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${row.name}</td>
      <td>${row.kind}</td>
      <td>${row.level || "-"}</td>
      <td>${row.health ? Math.round(row.health) + " / " + Math.round(row.max_health) : "-"}</td>
      <td class="mono">${Math.round(row.x)}, ${Math.round(row.y)}</td>
      <td class="actions">
        <button data-act="kill">اقتل</button>
        <button data-act="heal">عالج</button>
        <button data-act="reset">أعد</button>
        <button data-act="despawn">اختفِ</button>
      </td>`;
    tr.querySelector('[data-act="kill"]').addEventListener("click", async () => { await call("NpcAction", "kill", row.id, 0, 0, 0, false); loadWorld(); });
    tr.querySelector('[data-act="heal"]').addEventListener("click", async () => { await call("NpcAction", "heal", row.id, 0, 0, 0, false); loadWorld(); });
    tr.querySelector('[data-act="reset"]').addEventListener("click", async () => { await call("NpcAction", "reset", row.id, 0, 0, 0, false); loadWorld(); });
    tr.querySelector('[data-act="despawn"]').addEventListener("click", async () => { await call("NpcAction", "despawn", row.id, 0, 0, 0, false); loadWorld(); });
    body.appendChild(tr);
  }
}

document.getElementById("world-filter").addEventListener("input", loadWorld);

// ── the cities tab ───────────────────────────────────────────────────────
async function loadCities() {
  const nodes = await call("Nodes");
  if (!nodes) return;
  const body = document.querySelector("#cities-table tbody");
  body.innerHTML = "";
  for (const node of nodes) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${node.name}</td>
      <td>${node.level}</td>
      <td>${node.xp}</td>
      <td>${node.clock}</td>
      <td class="actions">
        <button data-act="xp">أضف خبرة</button>
        <button data-act="clock">زِد ساعة</button>
      </td>`;
    tr.querySelector('[data-act="xp"]').addEventListener("click", async () => {
      const value = prompt("كم خبرة؟", "100000");
      if (!value) return;
      await call("NodeXp", node.name, Number(value));
      loadCities();
    });
    tr.querySelector('[data-act="clock"]').addEventListener("click", async () => {
      await call("NodeClock", node.name, 3600);
      loadCities();
    });
    body.appendChild(tr);
  }
}

// ── the log tab ─────────────────────────────────────────────────────────
async function loadLogs() {
  const names = ["worldd.log", "gateway.log", "auth.log", "character.log", "mesh.log", "citynode.log", "quests.log"];
  const parts = [];
  for (const name of names) {
    const lines = await call("LogTail", name, 60);
    if (lines && lines.length) parts.push("=== " + name + " ===\n" + lines.join("\n"));
  }
  document.getElementById("log-view").textContent = parts.join("\n\n");
}

// ── the loop ────────────────────────────────────────────────────────────
async function refresh() {
  const status = await call("Status");
  if (status) {
    document.getElementById("status").textContent = status.connected
      ? "متصل — " + status.adminAddr + " · " + status.characterAddr + " · " + status.streamAddr
      : "الخادم مو شغّال";
    document.getElementById("status").className = "status" + (status.connected ? " ok" : " bad");
    document.getElementById("start-stack").hidden = status.connected;
  }
  if (state.tab === "players") { await loadPlayers(); }
  else if (state.tab === "stats") { await loadStats(); }
  else if (state.tab === "world") { await loadWorld(); }
  else if (state.tab === "cities") { await loadCities(); }
  else if (state.tab === "logs") { await loadLogs(); }
}

// The stack is down: the window can start it (scripts\start-stack.ps1).
document.getElementById("start-stack").addEventListener("click", async () => {
  const out = await call("StartStack");
  toast(out || "شغّلنا الستاك");
  setTimeout(refresh, 3000);
});

document.getElementById("refresh-players").addEventListener("click", loadPlayers);
document.getElementById("refresh-world").addEventListener("click", loadWorld);
setInterval(refresh, 2000);
refresh();
```

- [ ] **Step 6: Write the window's style**

Create `services/cmd/aocadmin/frontend/style.css`:

```css
/* style.css: the dashboard's dark theme. */
:root {
  --bg: #12141a;
  --panel: #191c24;
  --line: #262b36;
  --text: #e7e9ee;
  --muted: #98a0b0;
  --accent: #c9a227;
  --ok: #4caf7d;
  --bad: #e05a5a;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: "Segoe UI", Tahoma, sans-serif;
  font-size: 14px;
}
header {
  display: flex; align-items: center; justify-content: space-between;
  padding: 12px 18px; border-bottom: 1px solid var(--line); background: var(--panel);
}
h1 { font-size: 17px; margin: 0; font-weight: 600; }
.status { color: var(--muted); font-size: 12px; }
.status.ok { color: var(--ok); }
.status.bad { color: var(--bad); }
.tabs { display: flex; gap: 4px; padding: 10px 14px 0; border-bottom: 1px solid var(--line); }
.tab {
  background: transparent; color: var(--muted); border: 1px solid transparent;
  border-bottom: none; padding: 8px 16px; cursor: pointer; font-size: 14px;
  border-radius: 6px 6px 0 0;
}
.tab.active { background: var(--bg); color: var(--text); border-color: var(--line); }
main { padding: 14px; }
.panel { display: none; }
.panel.active { display: block; }
.row { display: flex; gap: 8px; align-items: center; margin-bottom: 10px; flex-wrap: wrap; }
input {
  background: #0e1015; color: var(--text); border: 1px solid var(--line);
  border-radius: 5px; padding: 6px 9px; min-width: 200px;
}
button {
  background: #232834; color: var(--text); border: 1px solid var(--line);
  border-radius: 5px; padding: 6px 12px; cursor: pointer;
}
button:hover { border-color: var(--accent); }
table { width: 100%; border-collapse: collapse; }
th, td { text-align: right; padding: 7px 9px; border-bottom: 1px solid var(--line); }
th { color: var(--muted); font-weight: 500; font-size: 12px; }
tr.selected td { background: #1f2531; }
td.mono { font-family: Consolas, monospace; font-size: 12px; color: var(--muted); }
td.actions { display: flex; gap: 5px; }
td.actions button { padding: 3px 8px; font-size: 12px; }
.stat-input { min-width: 90px; }
pre#log-view {
  background: #0e1015; border: 1px solid var(--line); border-radius: 6px;
  padding: 12px; max-height: 70vh; overflow: auto; direction: ltr; text-align: left;
  font-family: Consolas, monospace; font-size: 12px; color: #c7ccd6;
}
.toast {
  position: fixed; bottom: 18px; left: 50%; transform: translateX(-50%) translateY(30px);
  background: #222735; border: 1px solid var(--line); border-radius: 6px;
  padding: 10px 16px; opacity: 0; transition: opacity .2s, transform .2s; pointer-events: none;
}
.toast.show { opacity: 1; transform: translateX(-50%) translateY(0); }
.toast.error { border-color: var(--bad); color: #ffd7d7; }
```

- [ ] **Step 7: Build and run it**

Run:
```powershell
Set-Location D:\SandalCore-main\services
$env:PATH = "$env:USERPROFILE\go\bin;C:\Program Files\Go\bin;$env:PATH"
wails build -o aocadmin.exe
```
Expected: `services\build\bin\aocadmin.exe` exists.

Run it:
```
D:\SandalCore-main\services\build\bin\aocadmin.exe
```
Expected: the window opens, the status line says the server answers, and the players table lists the player in the world.

- [ ] **Step 8: Commit**

```bash
git add services/cmd/aocadmin services/go.mod services/go.sum
git commit -m "Added the game master's dashboard window"
```

---

### Task 7: The five tabs against the running server

The last task fills the tabs with the live server: the entities of the world (the world stream's `Inspect` and `Search`), the city nodes (the citynode service) and the logs.

**Files:**
- Modify: `services/cmd/aocadmin/app.go`
- Modify: `services/cmd/aocadmin/frontend/app.js`

**Interfaces:**
- Consumes: the bindings of Task 6 and the running stack.
- Produces: `WorldEntities()`, `Nodes()`, `NodeXp(name, xp)`, `NodeClock(name, seconds)`.

- [ ] **Step 1: The world's entities**

Add to `app.go` a method that asks the world stream's `Search` (the entities whose
name holds the query, at least two characters) and returns them as plain rows.
The details of one entity come from the stream's `Inspect`:

```go
// WorldEntity: one thing in the world, for the table.
type WorldEntity struct {
	ID        uint64  `json:"id"`
	Name      string  `json:"name"`
	Kind      string  `json:"kind"`
	Level     uint32  `json:"level"`
	Health    float64 `json:"health"`
	MaxHealth float64 `json:"max_health"`
	X         float64 `json:"x"`
	Y         float64 `json:"y"`
}

// SearchEntities: the things in the world whose name holds the query.
func (a *App) SearchEntities(query string) ([]WorldEntity, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	if len(query) < 2 {
		return nil, fmt.Errorf("اكتب حرفين على الأقل")
	}
	ctx, cancel := context.WithTimeout(a.ctx, 15*time.Second)
	defer cancel()
	out, err := a.client.stream.Search(a.client.WithToken(ctx), &worldadmin.SearchRequest{Query: query, Limit: 50})
	if err != nil {
		return nil, err
	}
	rows := make([]WorldEntity, 0, len(out.GetResults()))
	for _, r := range out.GetResults() {
		row := WorldEntity{ID: r.GetEntity().GetId(), Name: r.GetName()}
		detail, err := a.client.stream.Inspect(a.client.WithToken(ctx), &worldadmin.InspectRequest{Id: r.GetEntity().GetId()})
		if err == nil {
			if d := detail.GetActor(); d != nil {
				row.Kind = "npc"
				row.Level = d.GetLevel()
				row.Health = d.GetHealth()
				row.MaxHealth = d.GetMaxHealth()
			}
			if d := detail.GetPlayer(); d != nil {
				row.Kind = "player"
				row.Level = d.GetLevel()
			}
		}
		rows = append(rows, row)
	}
	return rows, nil
}
```

The entity's id and position come from the `Entity` message of the stream's
proto: read `proto/internal/aoc/worldadmin/v1/worldadmin.proto`'s `Entity`
message for the exact field names (`id`, `x`, `y`, `z`).

- [ ] **Step 2: The city nodes**

Add to `app.go`:

```go
// CityNode: one city node for the table.
type CityNode struct {
	Name        string `json:"name"`
	DisplayName string `json:"displayName"`
	Level       uint32 `json:"level"`
	LevelName   string `json:"levelName"`
	XP          int64  `json:"xp"`
	Percent     uint32 `json:"percent"`
}

// Nodes: the city nodes.
func (a *App) Nodes() ([]CityNode, error) {
	if a.client == nil {
		return nil, fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	out, err := a.client.citynode.ListNodes(a.client.WithToken(ctx), &citynode.ListNodesRequest{})
	if err != nil {
		return nil, err
	}
	rows := make([]CityNode, 0, len(out.GetNodes()))
	for _, n := range out.GetNodes() {
		rows = append(rows, CityNode{
			Name:        n.GetName(),
			DisplayName: n.GetDisplayName(),
			Level:       n.GetLevel(),
			LevelName:   n.GetLevelName(),
			XP:          n.GetExperience(),
			Percent:     n.GetPercent(),
		})
	}
	return rows, nil
}

// NodeXp: adds experience to a node.
func (a *App) NodeXp(name string, xp int64) error {
	if a.client == nil {
		return fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	_, err := a.client.citynode.AddNodeExperience(a.client.WithToken(ctx), &citynode.AddNodeExperienceRequest{
		Node: &citynode.NodeRef{Name: name},
		Xp:   xp,
	})
	return err
}

// NodeClock: moves a node's clock forward.
func (a *App) NodeClock(name string, seconds int64) error {
	if a.client == nil {
		return fmt.Errorf("الخادم مو شغّال")
	}
	ctx, cancel := context.WithTimeout(a.ctx, Client.CallTimeout)
	defer cancel()
	_, err := a.client.citynode.AdvanceClock(a.client.WithToken(ctx), &citynode.AdvanceClockRequest{Seconds: seconds})
	return err
}
```

The citynode proto's exact field names: read
`proto/internal/aoc/citynode/v1/citynode.proto` — `AddNodeExperienceRequest`
carries a `NodeRef` and the experience, `AdvanceClockRequest` carries the
seconds. The clock's own state comes from `GetClock` when the tab opens.

- [ ] **Step 3: Prove it against the running stack**

Run the stack:
```powershell
pwsh -File D:\SandalCore-main\build\start-stack.ps1
```
Then run the dashboard, sign in, press LAUNCH GAME and, from the dashboard:
1. the player appears in the players table ✅
2. the stats tab lists the bundle's stats ✅
3. an override on `MaxHealth` raises the player's health ✅
4. "املأ الدم" fills it ✅
5. "امسح الكل" brings the gear's numbers back ✅
6. the world tab shows the NPCs ✅
7. the money button adds copper and the client's purse grows ✅
8. the cities tab lists the nodes ✅
9. the log tab shows the services' logs ✅

- [ ] **Step 4: Commit**

```bash
git add services/cmd/aocadmin/app.go services/cmd/aocadmin/frontend/app.js
git commit -m "Filled the dashboard's five tabs"
```

