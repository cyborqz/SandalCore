# worldd/stats: the stat engine (plan B3)

A small C++20 library (`worldd_stats`, no dependency on the rest of worldd or the net code) that reads the
compiled stat formula graph that `services/internal/statprog` (Go) writes, and runs it for one character: the
stats of its class at its level with its equipment, each with the audience mask of its replication. The format, the
model and the compiler are documented in [`services/internal/statprog/README.md`](../../../services/internal/statprog/README.md);
this is the reader and the interpreter, the gear rules and the character's stats, which `worldd` runs (`server/worldd/README.md`, "Stats").

```cpp
#include "stats/stat_engine.h"
using namespace aoc::worldd::stats;

StatBundle bundle;  std::string err;
if (!parse_bundle(bytes, bundle, err)) { /* refused: err says why */ }   // verifies everything, once

StatInputs in;                       // what the character is
in.class_enum = 2;                   // ECharacterClass: Ranger 1, Bard 2, Cleric 3, Fighter 4, Rogue 5, Summoner 6, Tank 7, Mage 8
in.level = 1;                        // 1..255
in.mods = {{stat_guid, 20.0}, ...};  // the equipment: item stat blocks at their quality + set bonuses, summed per stat
in.equipped = {{"Secondary", "Item.Gear.Weapon.Shield"}};   // the gameplay tags of each worn item, by slot name

StatResult out;
if (run_stats(bundle, in, StatState{}, out, err)) {
    for (const StatOut& s : out.stats)     // the stats of the class, in stat order
        /* s.guid, s.replication (0 none, 1 Owner, 5 OwnerProxy, 7 EveryoneProxy, 6 ProxyOnly), s.value, s.base, s.equip, s.regen */;
}
```

- `parse_bundle` refuses anything that is not a well-formed bundle, whatever made the bytes: bad header, crc, counts that do not
  fit, tables out of range, a program that could read outside its tables, overflow its stack, jump backward or into an instruction,
  or read a stat before it is settled, an evaluation order that does not hold every stat once. After it returns true the
  interpreter needs no further checks (it keeps two anyway: one instruction per code byte at most, and a budget of
  `kMaxRunInstructions` = 4,194,304 per run; a run of the real graph takes about 3,000).
- `run_stats` returns false for a class the bundle does not have, a level outside 1..255, or a bundle that asks for more than the
  budget. A division by zero, a curve read at NaN or in a cubic segment, and a value that is not finite are *faults*: the value is 0
  and `StatResult::faults` counts them (none happens on the real graph).
- `StatState` is the state of part 2 (effects, skills, combat, professions, consumables): subclass it and override what exists.
  The defaults are the "none" values of a character with nothing yet (no effect, no skill, out of combat, not moving, killable,
  level 1 in every profession, nothing consumed). The equipped-item tags come from `StatInputs::equipped` first.
- The arithmetic is IEEE double, one operation per instruction, with the values the engine keeps rounded to float32
  (`Mode::Float32`, what the captured Bard needs); `Mode::Double` and `Mode::Single` are in the format too. It is built to give
  Go's bits: `go_min` / `go_max` are Go's `math.Min` / `math.Max`, `rnd32` is `float64(float32(x))` with an overflow an infinity,
  the curve read is written as Go's with each step in its own variable.

## The gear and the character's stats (`gear.h`, `character_stats.h`)

`gear_inputs(catalog, worn, rounding)` makes the engine's equipment input from the worn items (record, Quality, EEquipSlot): the rules of `statprog`'s `gear.go` over the tables of content.db (an item's stat block at its rarity band,
interpolated `Min + (Max - Min) * (q - X) / (Y - X)`, an item with no curve reads CommonMin; the sets' tiers up to the worn count at the lowest rarity of the worn pieces; the gameplay tags under the slot's name). The catalog is an interface
(`GearCatalog`): `content::ContentGearCatalog` reads content.db and keeps what it read, a test uses a table. `Rounding::HalfUp` is what worldd uses (the research's best fit on the retail Ranger: 376 of 442 against 358; not proven); the Go
corpus's kit cases are `None`, which is the same for them (the kits' stat values are whole). The C++ equals Go's on the real tables for the captured Bard, the retail Ranger and the sixteen kit cases: mods, tags and the digest of
every stat, bit for bit (`test_worldd_character_setup` with `AOC_CONTENT_DB`; the cases carry their items in the corpus: `rounding` and `item RECORD QUALITY SLOT` lines).

`CharacterStats` is one player's stats as the client holds them (float32): `start(worn)` at the join; `update(worn, changed)` after every gear change returns the stats whose wire value changed (the float bits, and base and equipment for a
stat that replicates them), in stat order. The engine returns the maximum for the stats that spawn at their maximum (Health, Mana, Stamina, a class's resource: `StatValue::held()`); the current value is the character's own: an update clamps it
to a new maximum (a change like any other) and leaves it alone otherwise, and `set_current` is how the game's own events (and the ability runtime's Stamina) set it. Every comparison the engine makes to decide that a stat changed is on the float bits
(+0.0 and -0.0 are two values on the wire, a NaN is the same value as itself; `clamp_current` replaces a NaN current value by the maximum). `prepare(worn, changed, next)` is `update` without the commit: the session commits `next` only once the pawn's
arrays took the change, and does not send a run whose `next.faults` is not 0 (as the join refuses such a character).

**Plan E1, stage 3 (format version 3).** The engine runs NPCs and other actors: `Evaluator::SetActor(role, view)` / `ExprEvaluator::set_actor` give a role (target, instigator, owner) its actor, `OpActor` brackets a read of it, and the seven native damage functions are in `damage.h` / `damage.cpp` (`DamageProfile`, `calculate`, float32 in the exe's order; the Go twin is `services/internal/statprog/damage.go`). `NpcShape` carries an NPC's elite rank and the eight distribution shares; the NPC classes (32 and up) have no common lists. The shared corpus (`testdata/corpus.txt`, regenerated by `go run ./internal/statprog/cmd/statprog corpus --out <file>`) holds the NPC, role and `damage` cases to the same bits in both languages.

## D2 engine layer (`stat_engine.h`, `content/ability_content.h`; plan D2)

The same additions as Go's (`services/internal/statprog/README.md`, "D2 engine layer"), bit for bit. Plan D2's ability runtime (`session/ability_pipeline.h`, `server/worldd/README.md` "Abilities of every class") runs on them: `CharacterStats` holds the character's effect state (`set_passives`: the ranks' passives, `set_effects`: the runtime's effects, `set_ranks`: what `GetSkillPurchased` reads, `set_moving` and `set_in_combat`) and `CharacterStats::evaluation` runs the expression programs against it; the passives plan D1 fed as extra mods (Endurance Training's `MaxStamina +10`) are effects now, the one path for every stat mod.

```cpp
in.effects = {{effect_ref, /*stacks*/ 2, /*tags*/ {"Debuff.Staggered"}, /*mods*/ {{stat_guid, /*one stack's value*/ 0.03}}}};
in.ranks = {{{kTypeSkillTree, tree_guid}, {kTypeSkill, skill_guid}}};     // purchased and auto-granted: GetSkillPurchased reads them
run_stats(bundle, in, state, out, err);       // effects apply after the equipment, in the stat's own form; out.values is every stat of the bundle

ExprEvaluator ev(bundle, in, state, out);     // the expression programs against that run (bundle, in, state, out must outlive it)
SplitMix rng; rng.state = seed;               // or your own ExprHost: progress, effects by instigator, random numbers, the hit in progress, XP debt
double v; ev.eval(program, host, v, err);
```

- **Effect mods** by the stat's flags: additive `value += mod * stacks`; `kStatMulMods` multiplies once per stack by the mod; with `kStatMulCompl` by `1 - mod` (the mitigations). The engine refuses stacks outside 0..`kMaxEffectStacks`,
  more than `kMaxActiveEffects` effects, and more than 2^20 applications; a mod for a stat the bundle has not is ignored and counted (`ignored_effect_mods`). `GetEffectCount`, `GetEffectCountByTag` and `GetSkillPurchased` read
  `in.effects` and `in.ranks` first and add what the `StatState` says.
- **`ExprEvaluator`** refuses a program that is not in `StatBundle::exprs` (`is_expr`); `parse_bundle` refuses a bundle in which a stat formula uses a host operation, or whose `exprs` are out of range or not increasing (format version 2,
  `kBundleVersion`: a version 1 file is refused).
- **`content/ability_content.h`** (`AbilityContent`, the library `worldd_ability_content`) reads the ability tables of a contentc 6 `content.db` on a connection of its own: ranks (`auto_ranks_at(level)` gives the engine's `ranks`), abilities,
  costs, effect costs, montages with the NetGUID where a capture names one, hits, effects, mods, projectiles, zones and the expressions (`expr(id)`: text, terms, program, `uses_target`, error). It refuses a file built by an older contentc
  (the message says to rebuild it) and one without the tables. `test_worldd_ability_content` runs it on the working tree's `dist/content/content.db` (or `AOC_CONTENT_DB`; skipped without one) together with this engine and the
  stored bundle: the Mana costs of the research (56.875 to 199.0625), Flourish's 20 s and 2 charges, the level-25 Bard sprinting at 1.3, Glee and Note_Comedy.

### Host functions and resources (plan D2, stage 2)

Expression functions the stat engine does not own go through a generic operation, bit for bit as in Go (`services/internal/statprog/README.md`): `OpHost0/1/2` (0x61 to 0x63, operand `u16 function + u16 string`) call `ExprHost::host_fn`. Function ids: 1 RemainingCharges, 2 ApplicableTarget, 3 MovementKey, 4 StatDelta, 5 HitStatMod, 6 HitStatEffective,
7 IsNeutral, 8 IsEnemy, 9 AngleBetween, 10 Now, 11 ProgressDuration. `RandDoubleRange` compiles to a `Lerp` with `Rand`; `GetAbilityProgressRatio()` with no argument asks about the running ability; an expression's own StatMod terms may read their siblings. The verifier knows the operations; `testdata/corpus.txt` has `host fn FN NAME F64` lines and `host progress -`
(the empty group), run by both engines (`go run . corpus --db ... --out ...` rewrites it, `corpus --check` compares).
A stat with `kStatResource` is a held class resource: `StatOut` carries its limits and regeneration (`max`, `min`, `has_*`), `StatInputs::current` feeds the held value to `GetStat`, `CharacterStats::set_resources` names the guids (`content::kResourceStats`).

### The gathering reads (Gathering stage 2; `gather_host.h`)

Host functions **180 to 184** (`kHostArtisanStat` .. `kHostGatherBaseSecs`) are what the Artisan equations ask of a harvest: `GetArtisanshipModifiedStat(actor, stat)` (name = the stat's guid, args[0] = the actor's role; the player is the caster), `GetRolledItemQuality()`, `GetItemRarity()`, `GetSourceCertificationTier()` and `GetGatheringInteractionBaseDuration()`. `stats::GatherContext` (`quality`, `item_rarity`, `source_tier`, `base_duration`, `ratings` by stat guid) and `gather_host_fn(ctx, fn, name, args, n, out)` answer them (false for any other function; a stat with no rating, another actor and a name that is no guid read 0): the harvest host of the Harvesting part fills the context and calls it from its `ExprHost::host_fn`. `RandIntRange` (14) answers `lo` when `hi < lo`, in every host. `test_worldd_stats` holds `gather_host_fn` to the corpus's `gatherhost` blocks (the Go `GatherContext`'s answers), `test_worldd_effects` runs the Artisan programs of the real bundle in the `real_gather_*` fxcases, with the same `fn` answers and SplitMix seeds as the Go engine.

## Tests

`test_worldd_stats` (`stats/tests/test_worldd_stats.cpp`, target in a separate block at the end of `server/worldd/CMakeLists.txt`)
runs the shared corpus [`stats/testdata/corpus.txt`](testdata/corpus.txt) that the Go side writes
(`go run ./internal/statprog/cmd/statprog corpus --out ../server/worldd/stats/testdata/corpus.txt`; `--check` fails when it is stale;
the file is `-text` so git never changes its line ends):

- 5 bundles parse and verify (the real graph, its double-mode variant, the `TinyGraph` that uses every instruction and state
  function, its single-mode twin, an abusive one);
- 38 characters give the Go engine's stats **bit for bit**: every value where the corpus lists them, and a digest of every output
  (value, base, equipment, regeneration) for all; the **captured level-1 Bard: 442 of 442 stats equal as float32**; the retail
  level-20 Ranger (effects not applied) compares with the capture exactly as in Go (376 of 442); every class's starting kit at levels
  1 and 20; a Fighter in combat with a table state;
- 71 hostile blobs (edits of the tiny bundle: broken tables, programs, orders, headers, truncations) are refused;
- a bundle that is valid but needs more than the budget is refused at run time;
- a differential fuzz: 28,000 random bit and byte flips and deletions of three bundles are made by the same generator in Go and C++;
  the count accepted and a hash of every verdict and result must match;
- on its own: every truncation of the real and the tiny bundle is refused, 12,000 more damaged bundles never crash the reader or the engine,
  a bad class or level is refused, repeated mods are summed, an unknown stat in the mods is ignored and counted, tags match their descendants;
- gear (plan B3): the interpolation in every band and at the edges (quality 0 and 250 extrapolate as Go's do), half-up rounding, no curve, sums across items, the mods in stat order, unknown records and sets counted, the sets' tiers and the lowest rarity,
  the tags under the slot names; the character's stats: start, an update that changes nothing returns nothing, the heavy chest raises MaxHealth and not Health, the clamp of a current value above a new maximum, the fishing stat's base and equipment,
  refusals, and the wire comparison.

```powershell
scripts\build.ps1 -Target test_worldd_stats
dist\Release\test_worldd_stats.exe          # from the repo root
```

`test_worldd_effects` (plan D2) reads the `fxcase` blocks of the same corpus: 14 characters with effects, ranks and a host, whose stats, 3,640 expression results and 12 retail numbers (the Mana costs, the Sprint's 1.3, Glee's and Note_Comedy's effect) equal the Go engine's to the bit
(the tiny graph's every form of effect mod, and the real bundle's 507 expression programs in each of seven cases), and checks by hand the three forms of an effect mod, the retail Mitigation and HealingMult, what the effects
answer the state functions, the inputs it refuses, the evaluator's refusals and SplitMix64's first outputs. `test_worldd_stats` skips the `fxcase` blocks and refuses the 98 hostile blobs (the host-operation-in-a-formula rule is proven here, in the reader that opens the files: those blobs are whole-blob corpus lines, FIX 2).
`test_worldd_ability_content` is described above. Both new binaries are in `fixtures/test-baseline.txt` (`scripts\run_tests.ps1 -SaveBaseline`). The tests that need the real tables or the real session are in
`test_worldd_character_setup`, `test_worldd_content_db`, `test_worldd_items_session` and `test_worldd_abilities`.
