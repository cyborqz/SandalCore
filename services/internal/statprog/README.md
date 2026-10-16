# statprog: the stat engine's compiler, format and Go interpreter (plan B3 groundwork)

The stats of a player (466 of them: the six attributes, the ratings, mitigation, regeneration, speeds, the
artisan stats) are not stored anywhere in the design data: they are a graph of formulas. `StatTypeDef` rows
have a `Base`, `ValueMin`, `ValueMax` and `RegenRate` expression, four `StatInitializerList` rows give the
starting values, equations (`EvalE`) and formulas (`EvalFormula`) are shared sub-expressions. This package
compiles that graph into a small versioned program format (`Bundle`), checks it, and runs it. The C++ interpreter
in [`server/worldd/stats`](../../../server/worldd/stats/README.md) reads the same bytes and gives the same bits.

```
content.db --LoadContentDB--> Graph --Compile--> Bundle --Encode--> bytes --Decode (verify)--> Bundle --Run--> stats
                                                                       |                                         ^
                                                    worldd (C++): parse_bundle -------- run_stats ---------------+
```

Wired in by plan B3: contentc version 4 writes the bundle into `content.db` (`stat_program`, with the item stat tables the gear reads: `services/README.md`, "Stats (plan B3)"), and worldd
parses and runs it (`server/worldd/README.md`, "Stats"). The code is `internal/statprog`, additions to `internal/aocexpr`, `server/worldd/stats`.

## The model

For a character of a class and level with its equipment, in the order `Bundle.Order` gives (every stat's
initial value, then its value, after the stats they read):

- **initial value**: the entries of `CombatStats`, `Base_Artisan_Stats`, `Player_Base` and `Player_Base_<Class>` that
  name the stat, in that order. Each entry's expression sees the value so far as `GetStatInitRunningTotal()`
  (an empty expression leaves it). A stat starts at 0, or at 1 when it multiplies (`bMultiplyStatMods`).
  **The running total an entry reads is the stat's value so far with the `Base` folded in** (initial + Base, or initial x Base
  for a multiplying stat), and such an entry's result is the stat's new value: the initial value stored is `result - Base`
  (`result / Base`), what the value pass adds the Base to again; an entry that does not read the total sets the initial
  value as written. Found by the E1 review (2026-10-07) on the captured NPCs: the elite scalar's `GetStatInitRunningTotal() * f`
  over MaxHealth, whose Base is Constitution's part, gives every captured NPC's MaxHealth and MaxMana within 1 % (279 of 279 pairs,
  26 of 26 opens; before, the factor scaled only the small NPC_Base part: rank 0 was 5-19 % off, elites 25-40 %, guards
  several-fold) and every NPC's MoveSpeedMult (1.05 + 0.1 x rank over the Base's 1: 1.0 before). A player's entries that read the
  total are `GetStatInitRunningTotal() + v` (the same either way: the Bard's 442 of 442 hold), but for the Tank's
  `ThreatGeneration = GetStatInitRunningTotal() * 2`, now `2 + 2 x rating / 1000` (uncaptured). The compiler orders such a stat's
  initial value after the stats its Base reads, and the verifier (Go and C++) refuses a bundle where it is not: **a `content.db`
  built by a contentc before this rule is refused** (rebuild it). Both engines take the same steps in the same order (the corpus).
- **value**: `initial + Base` (`initial * Base` for a multiplying stat; no `Base` leaves the initial value), plus
  the equipment's contribution (item stat blocks at their quality, set bonuses: the caller sums them), then raised
  to `ValueMin` and lowered to `ValueMax`. A stat that spawns at its maximum (`Reset_SetMax`: Health, Mana, Stamina,
  Ranger_Focus) is its maximum; one that spawns at its minimum (`Reset_SetMin`) is its minimum.
  `bClampStatFinalValue` is read and not used: the capture clamps every stat.
- **regeneration**: `RegenRate` is evaluated after the values, reading any of them (`Fighter_Momentum`'s decay, the
  Cleric's Divine Power); the result carries it as `Regen`.
- Effects (talents, food, guild, node) would add after the equipment; the state they and the consumables, skills,
  professions and combat read is the explicit `aocexpr.State` (below). Nothing applies effects yet.

This is the model of the research (`cc/evidence.md` section B), checked here against the captured Bard to the bit.

### Arithmetic: why float32 values

`Bundle.Mode` (the header flags):

| mode | arithmetic | captured Bard (442 stats, equal as float32) |
|---|---|---|
| `ModeFloat32` (default) | float64, but every value the engine stores or returns is a float32: a formula's result, a stat's initial value, a stat's value | **442 of 442** |
| `ModeDouble` | float64 throughout | 440 (`PhysicalSpeed` and `HealingCriticalPower` are one float32 ulp off) |
| `ModeSingle` | also every operation, constant and curve value in float32 (what `aocexpr.Env.Single` does) | 440 (`HealthRegeneration`, `ManaRegeneration` one ulp off) |

`PhysicalSpeed` = `-0.3 (initial) + (1.0 + 48/(48+8000))`: only the order "the formula returns a float32, then the
initial value is added" gives the captured bits. So the formulas return floats and the stats are stored as floats,
while the arithmetic inside a formula is wider. The Python port of the research had 440 of 442 exact; this has 442.

## Version 3 (plan E1, stage 3)

**Roles.** `OpActor u8 role` brackets a read of another actor's stats, effects, skills, level or flags in an expression program (`aocexpr.Role...`: the caster, the target `GetTarget()`, the instigator, the owner); the compiler wraps one read with it and `OpActor 0`, and an unset role reads as the caster, so the programs of D2 are unchanged. The Go `Evaluator.SetActor` and the C++ `ExprEvaluator::set_actor` give each role its actor. **Host functions 20 to 29** are what the design data's `DamageMod` equation asks of the game: `CalculateDamage`, `...Multiplier`, `...AdditiveMultiplier`, `...Mitigation`, `...Critical`, `...Penetration`, `...Dampening` (`damage.go`, over a `DamageProfile`; float32 arithmetic in the exe's order), the distance and identity of two actors, the hit's source type and the relations. **NPC profile classes** (enum 32 and up, `NpcProfiles`): "NPC" (the lists CombatStats, NPC_Base, NPC_Elite_Scalar) and "NPC_Guard" (the Guard overtuned scalar), no common lists; the NPC state is the eight `StatDistribution` weights and the `EliteLevelRanks`. The corpus has `dprofile`, `dview` and `damage` parts and NPC cases; the C++ engine (`server/worldd/stats`, `stats/damage.cpp`) must give the same bits. A bundle of another version is refused by both readers.

## Rewards (plan E1, stage 3, part 2: contentc version 10)

The kill rewards' expressions (the loot tables' predicates, weights and quantities, the experience's decay) are programs of this format with four more host functions: **30** `GetPlayerLootMaxLevel`, **31** `GetPlayerLootAvgLuck`, **160** `DoesNPCContainTag(tag)` (a bool: it answers 1 or 0 to an `if`) and **161** `GetPlayerLevel`; the compiler takes `Int` (compiled as a floor), a zero-argument `GetNPCLevel()` (the creature the kill is of) and `$F:Name$` references to the named formulas (`StatFormulaType`). Gameplay tags are matched without regard to case in `aocexpr.TagMatches` and in the C++ engine (`tag_matches`): they are FNames, and the data writes `"pet.summon"` where the effect's tag is `Pet.Summon`. The shared corpus was regenerated for both.

## The city node reads (Nodes 1, stage 1: contentc version 12; no format change)

`GetNodeBiome`, `GetCurrentNode`, `GetNodeRecord`, `Node_GetNodeType`, `Node_GetLevel`, `Node_InLevelRangeInclusive`, `Node_IsPlayerCitizen`, `Node_IsPlayerCitizenOfNode` and `Node_IsPlayerMayor` compile to host functions **170 to 177** (`aocexpr.HostNodeBiome` ... `HostPlayerMayor`; `OpHost0..2`, so the bundle format is unchanged). `GetPlayer()` / `GetPlayerGuid()` (a player that only these reads take) and `EnumInt` come with them. In a program a node is its **rank** (the index of its `CityNodeRecordBase` in the list sorted by guid: `Graph.Nodes`, from content.db's `city_node`; -1 for none) and an enum answer is the value's ordinal (`aocexpr/enums.go`): a `$#<CityNodeRecord type>:<guid>$` or `$CityNode:<name>$` reference compared with a node, and an enum literal compared with an answer, become the same numbers at compile time. `Node_InLevelRangeInclusive(node, lo, hi)` is the level the host gives, compared with the two bounds. The host answers (`statprog.NodeWorld`, and worldd's `world::node_host_fn`, which the corpus's `nodehost` blocks hold to the same answers): `GetNodeBiome()` the biome of the zone of influence the expression runs in (Default for none), `GetCurrentNode(actor)` the node of the player (`aocexpr.ActorPlayer`) or of the expression's own actor, `GetNodeRecord()` the node of the context, `Node_GetNodeType` / `Node_GetLevel` of a rank (Default / Wilderness for no node or a rank past the registry), the citizen and mayor reads 0 until the citizenship milestone.

Checked against the client (`exe.md` section 8, the host functions' registrations): `GetNodeBiome() -> EBiomeType` needs a node in the NodeGuid context and is design data only; `GetNodeRecord() -> record` and `Node_GetNodeType` the same; `Node_GetLevel` and `Node_InLevelRangeInclusive` need the node service (server-only, as here); `GetCurrentNode` has three overloads (an actor, a proxy, a guid) and gives the record of the node whose zone holds the actor; `Node_IsPlayerCitizen(player)`, `Node_IsPlayerCitizenOfNode(player, record)` and `Node_IsPlayerMayor(player)` take the player and a record and give a bool. What the client does with no node in the context is not in the data; Default / -1 / Wilderness is the choice here.

## The gathering reads (Gathering stage 2: contentc version 13; no format change)

`GetArtisanshipModifiedStat(actor, stat)`, `GetRolledItemQuality()`, `GetItemRarity()`, `GetSourceCertificationTier()` and `GetGatheringInteractionBaseDuration()` compile to host functions **180 to 184** (`aocexpr.HostArtisanStat` ... `HostGatherBaseSecs`; `OpHost0..2`: the bundle format is unchanged). They are what the Artisan equations of the data ask: the speed, rarity, quantity and experience of a harvest, an `ItemVariation`'s `Quality`, a `Profession` reward's experience. The quality, the rarity and the tier are whole numbers (the equations compare them with `==` and `>=`), the rating and the duration floats; a rating's actor is a role (the player is the caster, the instigator). The host answers from a plain context, `statprog.GatherContext` (the reference of the C++ `stats::GatherContext` and `gather_host_fn`, `server/worldd/stats/gather_host.h`; the corpus's `gatherhost` blocks hold the two to the same answers): `Quality`, `ItemRarity`, `SourceTier`, `BaseDuration` and `Ratings` by the guid of the rating stat (a stat with none reads 0). `TableHost.Gather` answers them in tests; the corpus's real cases (`real_gather_*`) answer them with `Fns`. Two compiler rules came with them: an input term with no value (`"Value": ""`) is the term's default, 0, and `GetPlayerProfessionLevel` takes `GetPlayer()` as well as `GetActorGuid(...)`. `RandIntRange(lo, hi)` answers `lo` when `hi < lo` in `TableHost` and in every C++ host. The real-data tests (`gather_real_test.go`) run the equations on the engine: the rarity tables against the research's analytic odds (200,000 harvests per case, both variants, ratings 0 and 8409), +80/+90/+100/+120 experience, the speed factor 0.927 at a rating of about 215, the quantity.

## The format (version 2)

Little-endian. A reader (`Decode`, the C++ `parse_bundle`) checks all of the following before anything runs.

```
header  16 bytes   "AOSP" | u16 version = 2 | u16 flags (1 ModeSingle, 2 ModeDouble, not both) | u32 body length | u32 crc32 (IEEE) of the body
body               u32 n, f64 consts[n]              (finite)
                   u32 n, (u8 len, bytes)[n]         strings: stat and class names, tags, slot and profession names
                   u32 n, (u64 type id, i64 guid)[n] references: effects, skills, classes, a consumable's stat
                   u32 n, curves[n]                  u32 keys (>= 1), (f64 time, f64 value, u8 interp)[keys]; interp 0 linear, 1 constant,
                                                     2 cubic (not evaluated: a read inside is a fault); times strictly increasing, finite
                   u32 n, programs[n]                u16 locals, u32 code length, code
                   u32 n, stats[n]                   i64 guid, u32 name, u8 replication, u8 flags, i32 base, min, max, regen (program or -1)
                   u32 n, (u32 stat, u32 program)[n]   the common lists' entries, in order
                   u32 n, u32[n]                       the stats those lists name
                   u32 n, classes[n]                   u8 ECharacterClass, u32 name, steps (as above), members (as above)
                   u32 n, u32[n]                       the order: stat << 1 | kind (0 initial value, 1 value)
                   u32 n, u32[n]                       (version 2) the expression programs, by index, strictly increasing (D2 engine layer, below)
```

Opcodes (a stack of float64; a boolean is 1.0 or 0.0; operands little-endian, jump targets absolute and forward):

| | |
|---|---|
| `01 CONST u16` `02 LOAD u8` `03 STORE u8` `04 STAT u16` `05 TOTAL` `06 LEVEL` `07 POP` | push a constant, a local, a settled stat, the running total, the level; store; drop |
| `10 ADD 11 SUB 12 MUL 13 DIV 14 NEG 15 NOT` | `DIV` by zero pushes 0 and counts a fault |
| `20 LT 21 LE 22 GT 23 GE 24 EQ 25 NE` | push 1 or 0 |
| `30 JMP u16` `31 JZ u16` `32 JNZ u16` | forward jumps (`&&`, `\|\|`, `if`) |
| `40 ROUND 41 FLOOR 42 CEIL 43 MIN 44 MAX 45 SEL 46 CURVE u16` | `ROUND` = floor(x + 0.5); `MIN`/`MAX` are Go's; `SEL` is `c a b -> c != 0 ? a : b` |
| `50 EFFECT u16 51 EFFECT_TAG u16 52 SKILL u16 u16 53 COMBAT 54 MOVING 55 UNKILLABLE 56 EQUIP_TAG u16 u16 57 PROFESSION u16 58 CONSUMED u16 59 NPC_RANK` | the state of part 2 (`aocexpr.State`) |
| `47 LERP 48 ABS 5A..60` | (version 2) `Lerp`, `Abs` and the host operations: see "D2 engine layer" |
| `FF RET` | the value is the only one on the stack |

Stat flags: 1 multiplies, 2 clamp-final (read, unused), 4 spawns at max, 8 spawns at min, 16 replicates base and equipment, 64 multiplies by the complement (version 2: `bMultiplyCompliment`), 32 is a float-array stat (plan B3: the stat's `StatType` is
`Type_FloatArray`, which is TempHealth alone: it goes to `StatRepFloatArrayEveryoneProxy`, not to an Int32 array; it is the one audience stat the captured Bard's 442 lack).

**What the verifier guarantees**: every opcode and operand is known and inside its table (constants, stats, curves,
references, strings, locals); jumps go forward and land on an instruction; every path to an instruction arrives with
the same stack height; the stack stays within 0..64; the code ends in its only `RET` with one value; so a program
executes at most one instruction per code byte (at most 8,192) and cannot loop. Stat guids are unique, replication
is 0..7, flags are known, program indexes are in range, the order holds every stat's two nodes once with the initial
value first, and **a program reads only stats settled before the node that runs it** (so no read of an unsettled stat,
and the order is acyclic). Limits: bundle 4 MiB; 65,535 constants, strings, references, programs, steps; 4,096 curves,
curve keys and stats; 16 classes; code 8,192 bytes; 64 locals; strings 255 bytes. A run executes at most
`MaxRunInstructions` = 4,194,304 instructions in all (a valid bundle can still ask for more; a run of the real graph takes
about 3,000) and fails with an error past it. `go test` has 73 hostile bundles (52 broken tables, orders and programs, damaged headers and bodies, truncations), every
truncation of a bundle, a 30,000-mutation fuzz and `FuzzDecode`; the same blobs, and a differential fuzz of 28,000 more, are in the shared corpus the C++ reader refuses
and agrees on.

Faults, not errors, at run time: a division by zero (the result is 0), a curve read at NaN or inside a cubic segment (the
first key of the segment), a stat that is not finite after a formula (0) each count in `Result.Faults`; the reference engine
(below) stops with an error where the compiled one counts. None happens on the real graph.

## The compiler (`Compile`)

`aocexpr` parses each text; the compiler folds it into a small tree (constants computed, `const` bound at compile time,
variables resolved to locals, `EvalE` / `EvalFormula` / `GetTerm` replaced by the equation's own code, stat and curve references
resolved to indexes) and emits instructions. 473 stats, 166 programs (identical programs are shared), 50 constants, 19 curves,
13 references: **50,205 bytes**, deterministic (two compiles give the same bytes). The stats are those of the four lists of
every class plus what their formulas read.

- **Typing is static**: numbers and booleans live on the stack; objects (`GetOwner()`), references, strings, enum values and actor ids
  are compile-time and appear only as the operands of the operations that take them. A `var` that holds anything but a
  number or boolean, a `set` that changes a type, a non-boolean condition, an unknown function: an error naming the formula.
- **Division** follows `aocexpr`: it needs an operand proven to be a float (a literal with a decimal point, `Float`, `SelectFloat`,
  `EvalCurve`, `GetStat` and the other stat functions, a variable only ever given such a value). Every formula of the player graph passes it.
- A text that is not in the subset, an equation with constraints, a cycle between formulas, a number that does not fit (in
  `ModeSingle`), more than 64 locals or 8,192 bytes of code: an error, never a skipped formula.
- `GetTerm` is resolved in the scope of the formula's caller; equations may nest 16 deep and never call themselves.
- Not hoisted but not lazy either: `SelectFloat` and `SelectInt` evaluate all three arguments, as `aocexpr` does.

## What was added to `aocexpr`

Syntax: string literals (`"Item.Gear.Weapon.Shield"`), enum values (`EEquipSlot::Secondary`), named references
(`$Stat:MaxStamina$`), and a nesting limit of 200 levels (the parser and every walk recurse once per level).
Built-ins: `Floor`, `Ceil`. `Env.FloatFuncs` names the host functions that return floats (for the division rule). `Tree()`
exports the syntax tree. `StatEnv(ctx, curves)` supplies the stat functions over an explicit `StatContext`:

| function | answered by |
|---|---|
| `GetStat` `GetStatInitRunningTotal` `EvalE` `EvalFormula` `GetTerm` `GetCharacterLevel` | the stat engine |
| `GetOwner` `GetInstigator` `GetSource` `GetSelf` | one character: the same `Object` kinds apart |
| `GetEffectCount` `GetEffectCountByTag` `GetSkillPurchased` `IsInCombat` `IsMoving` `IsUnkillable` `EquippedItemInSlotHasTag` `GetActorGuid` `GetPlayerProfessionLevel` `GetConsumedItemStat` `GetNpcEliteRank` | `aocexpr.State`: the part-2 state |

`NoState` is the character with nothing yet: no effect (0), no skill bought, out of combat, not moving, killable, actor 0,
**level 1 in every profession** (it gives the Bard's `Artisan_Gathering_Fishing_Sport_Power` 10 + 100 from the rod),
nothing consumed, NPC rank 0. `EquippedItemInSlotHasTag` is answered from the equipped items' tags (`Inputs.Equipped`), then by
the state. Still unsupported (NPC lists only): `EvalF`, `EvalEquation`, `GetCreatureOwner`, `GetNPCLevel`, `GetNpcStatDistribution`.
`NPC_PowerMultiplier` (reached from `PowerMult`, behind `CharacterType < 0`) compiles: its curves have cubic segments, which
are a fault when read; no player reads them.

## D2 engine layer (plan D2: expressions, effects, ranks)

Groundwork for the ability pipeline (spec `2026-10-02-complete-character-part2-design.md`, section 3): the engine runs an ability's own expressions and applies
effects. It is the engine only: no worldd session or runtime uses it yet. **Format version 2** (`Version`); contentc version 6 writes it.

**Expression programs.** `CompileExprs(graph, opts, []Expr)` compiles the stat graph as `Compile` does and then each expression (an ability's cost, cooldown, charge count, cast
time, requirement, an effect's duration, a hit's requirement, the value of a stat mod with its `ValueInputTerms`) into the same bundle: a program like any other, whose index
the `ExprResult` gives, listed in `Bundle.Exprs` (the new table at the end of the body: u32 n, u32[n] program indexes, strictly increasing). An expression that does not compile
(a function the engine does not have) is a result with the error and no program, and leaves the bundle as it was (`snapshot`/`restore`); one that does reads the stats the bundle
already has (`TestExprsCompileAsTheyShould`: the failed ones change nothing, a second compile gives the same bytes). `UsesTarget` says it called `GetTarget()`.
`Evaluator` (`Bundle.NewEvaluator(run, inputs, state)`, then `Eval(program, host)`) runs an expression program against the stats of a finished run (`Result.Values`: every stat of the
bundle by index, members of the class's lists or not), the level, the effects and ranks, and a host. It refuses a program that is no expression program (`Bundle.IsExpr`).

**The verifier** (Go `Decode`, C++ `parse_bundle`, the same rules): the `exprs` are in range and strictly increasing; **no program of the stat graph** (initializer step, Base, Min, Max,
RegenRate) uses a host operation, since there is no host to answer it. The new opcodes (operands little-endian):

| | |
|---|---|
| `47 LERP` `48 ABS` | `a b t -> a + t * (b - a)`, the subtraction, the product and the sum each rounded as a stat formula's are; `\|x\|` |
| `5A PROGRESS u16 5B EFFECT_INST u16 5C RAND` | host: the progress ratio of the running ability of a montage group (string); the stacks of an effect the instigator applied (ref); the next random number in [0, 1) |
| `5D HIT_TAG u16 5E HIT_FLAG u16 5F XP_DEBT 60 HIT_TARGETS` | host: the hit in progress has a gameplay tag (string); the hit on the target has an `EHitFlag` ("Crit": string); the instigator's experience debt ratio; the hit's target count |

Stat flag `64` is `bMultiplyCompliment` (below). A hostile blob per new rule is in the corpus (a host operation in a Base, a limit, a regeneration, an initializer step and a class step; operands outside the tables;
an expression outside the programs, repeated or out of order; `Lerp` and `Abs` that underflow the stack; an unknown opcode).

**Functions** (compiled only in an expression, `unitExpr`, except where it says), in the order the work was done:

1. `GetTarget()`: **the caster**. Until there are other actors the target of a self or party hit is the caster, so `GetStat(GetTarget(), s)`, `GetEffectCount(GetTarget(), e)` and the objects' equality
   (`GetTarget() != GetInstigator()` is false: the objects of a formula are one character, in `aocexpr` too) read the caster. The 139 stored expressions that call it say so (`uses_target`); a host with
   other actors must not run those for a hit on one of them.
2. `Lerp(a, b, t)` and `Abs(x)` (everywhere; also `aocexpr` built-ins), `Clamp(x, lo, hi)` = `Min(Max(x, lo), hi)` (no opcode).
3. `GetAbilityProgressRatio("SongGroup")` and `GetEffectCountByRecordAndInstigator(target, effect, instigator)` read the host (`ExprHost`; the instigator is the caster).
4. `RandDouble()` reads the host's source. `SplitMix` (SplitMix64; `RandDouble` is the top 53 bits over 2^53) is the seeded source of the tests and the corpus, bit-identical in C++; the draws are in program order
   (`&&` and `||` are lazy: a draw behind a false skill test is not made, and the corpus keeps the order).
5. **Equation constraints**: a `StatEquationType` with one constraint (a clamp, `Rounding_None`) compiles: its value is raised to Min and lowered to Max (13 equations have constraints; the HealingMod's is a Min of 0).
   Another kind (several constraints, a rounding rule) is still refused.
6. The hit-event functions a heal or a shield formula reads (done last, after everything above): `GetHitInProgress`, `CheckForDamageTag`/`HitHasTag`, `GetHitFlags` + `HitFlagsContains(.., EHitFlag::Crit)`, `GetNumTargets`,
   `GetXPDebtRatio`: host inputs (the damage functions, `CalculateDamage*`, are not: no NPC).

Three things the real data needed: **terms** (`GetTerm`) resolve in the innermost formula's own terms, then the callers' (a StatMod's `ValueInputTerms` feed the formula its value calls), then the term record's default value
(`TermType.DefaultValue`, "" is 0), and the term's own limits bind the result (`Graph.Terms`; the merge rule for a term two sources give is not modelled: the innermost wins); function names are case-insensitive for the
functions the package knows (`selectInt`, `Evalcurve`); a boolean may stand where a number is wanted (`EvalFormula(..) > 0.0`: `aocexpr.Env.LooseBool`, set for an ability expression only; the stat formulas stay strict).

**Effect state** (`effects.go`; C++ `StatInputs::effects`, `ranks`). `Inputs.Effects` are the active effects: an effect record reference, its stacks (0 to `MaxEffectStacks` = 4096; at most 4,096 effects and 2^20 applications
a run), tags, and its stat mods as the value of **one stack** (the host ran the mod's expression program with `Evaluator.Eval` when it applied the effect: the engine does not resample). A mod changes its stat after the
equipment and before the limits:

| stat | the effect's mod | stat_def flags |
|---|---|---|
| additive (MoveSpeedMult, Evasion, MaxStamina) | `value += mod * stacks` | `bMultiplyStatMods` 0 |
| multiplying (HealingMult, DamageMult) | `value *= mod`, once per stack | `bMultiplyStatMods` 1, `bMultiplyCompliment` 0 |
| multiplying "compliment" (the mitigations and penetrations: 24 stats) | `value *= (1 - mod)`, once per stack | both 1 (`StatMulMods` + `StatMulCompl`) |

So **the stat decides the form** (the two flags of its `StatTypeDef`, bundle flags 1 and 64); the caller never says. Retail values from the research (`TestRetailEffectValues`, `TestSprintAndTheRetailBard`):
Mitigation 0.9652 under Bard_Glee 0.03 is 0.9362 (x (1 - 0.03)), HealingMult 1.1025 under Bard_Note_Comedy 1.05 is 1.1576 (x 1.05). `Inputs.Ranks` are the skill ranks the character holds (purchased and auto-granted:
`GetSkillPurchased(tree, skill)`); effects answer `GetEffectCount` (stacks by record) and `GetEffectCountByTag` (a tag matches itself and its descendants), added to whatever the `aocexpr.State` says. `RefEngine` (aocexpr over the
text) does all of it too, and the compiled engine equals it bit for bit in all three arithmetic modes on random effects (`TestEffectsAgreeWithTheReference`) and on every expression of the tiny graph
(`TestExprProgramsAgreeWithTheReference`).

**Proofs on the real data** (`abilities_real_test.go`, needs a contentc 6 `content.db`):

- *Sprint.* General_Sprint's one stat mod, `0.2 + 0.05` for each of `GetSkillPurchased(Universal, Sprint_1/2/3)`, run for a Bard with the ranks the game grants by itself (plan D1's `skill_node.auto_granted` with its level: Sprint_1 at level 5, Sprint_2 at 20,
  Sprint_3 at 35) and applied to `MoveSpeedMult` (base 1.0, additive): **1.3 at level 25** (the float32 1.3 of capture 44), 1.2 with no sprint skill, 1.25 at level 5, 1.35 from level 35. (The task text says "the three Universal sprint skills"
  for 1.3; the three give 1.35, the retail 1.3 is the two a level-25 character holds.)
- *Mana costs.* `k * ManaCostMult(25)` with `ManaCostMult(25) = 5.6875`: Jest, Flourish 56.875; Mesmerizing, Story_Joy 85.3125; Nimble, Misdirection 113.75; Saga 199.0625; Flourish's cooldown 20 s and its 2 charges (1 with Dance_Energy).
- *Glee* is 0.02, and 0.03 with the talent Merriment (the retail Bard held it), Note_Comedy 1.05.
- *The Bard's abilities*: 27 in the tree, 22 castable without an NPC (a target is not required, or Self is allowed); **all 22 compile in full** (every expression of what each reaches without an NPC; 12 of them go through `GetTarget` as the caster).
  The research of 2026-10-03 counted 7 that the B3 compiler had, 13 with `GetTarget`, 18 with `Lerp` and the progress ratio, and four that needed the hit-event functions (it counted Music_Cheerful, whose heal needs them, among the 18):
  with the hit-event functions the last five (QuickWit, Jest, Shielding, Saga, Cheerful) compile too. 560 of the file's 782 distinct ability expressions compile (507 programs); the 222 that do not need what an NPC fight has: `CalculateDamage` (68), `GetActiveTarget` (21),
  `GetEffect` (16), `GetCreatureOwner` (15), `GetDistanceBetween` (12), `IsComboButtonPressed`, `HasAlignment`, `IsApplicableTarget`, `GetAbilityRecordByName`, `GetAngleBetween`, `GetHitEvent`, `GetHealthPercentage`, `Now`, ... and a `%`.
  The heal amounts themselves (QuickWit 651.5, Serenade 699.6, TempHP 82.5 to 297) are not reproduced: they need the retail character's gear and the hit's own numbers.

The corpus (`testdata/corpus.txt`) has, besides the 38 cases of B3, **14 `fxcase`s**: seven on the tiny graph (every form, stacks, tags, ranks, the state, the limits, single arithmetic) and seven on the real bundle (the retail Bard sprinting, in double
arithmetic too, with Glee and Note_Comedy on a Bard whose equipment puts Mitigation at 0.9652 and HealingMult at 1.1025, a Mage, a Cleric in combat, a Fighter, a level-1 Ranger, each with the Sprint effect and the ranks of its level):
**3,640 expression results** (every one of the 507 expression programs of the real bundle in each of the seven real cases, and the tiny graph's 13 in the others), each with a seeded host (progress, effects by instigator, a hit in
progress), checked bit for bit in Go and C++ (`test_worldd_effects`), **12 retail numbers** (`retail` lines, checked when the corpus is written and replayed in Go and C++: the nine Mana costs of the evidence, the sprinting Bard's MoveSpeedMult 1.3, Mitigation 0.9362 under Glee and HealingMult 1.1576 under Note_Comedy), and 98 hostile blobs (26 more than B3's: the corpus line `hostile NAME 2 0 0 HEX` carries a whole blob, up to 16 KB, for the breaks that change the bundle in many places, such as a host operation in a stat's base, limit or regeneration or in an initializer step, which needs a program appended and a stat that names it; host function 0 and 256, host operands outside the tables, a host function that pops more than the stack has, and the unknown opcode 0x64). `corpus --check` replays it all.

Not in this layer: the runtime (casting, cooldowns as time, effect application and expiry, ticks, regeneration, hits); the merge rule of two sources of a term; `CalculateDamage`; a `TempHealth` shield (it is an additive stat here; the
float-array replication and the absorb rule are the runtime's); reactions, cooldown modifiers and teleports (stored as guid lists, not followed).

## Golden results

`go test ./internal/statprog/...` (needs `AOC_CONTENT_DB`; the rest of the tests need nothing):

- **Captured Bard, level 1, its 13 starter items: 442 of 442 stats equal as float32** (`ModeFloat32`); the audience
  mask of each equals the array it was in (335 Owner, 96 OwnerProxy, 11 EveryoneProxy); the four stats that carry base and
  equipment (`Artisan_Gathering_Fishing_Sport_*`) match both. The 466 stats of the Bard's lists include 443 with an audience:
  the one the capture does not carry is `TempHealth`.
- The compiled engine equals the reference engine (aocexpr over the text, no compiled program) on every stat of every class at levels
  1, 13, 20 and 50, bare and in its starting kit, in all three modes, bit for bit.
- The six attributes of every class at every level 1 to 50 equal `class_stat_base` (2,400 values).
- **Retail Ranger, level 20, 29 items, effects not applied**: 358 of 442 equal as float32 (81.0%) with the item stat blocks as
  interpolated, **376 (85.1%)** with the item values rounded half up (the best fit of the research, not proven). The 66 misses
  have 18 origins (the rest follow from them): the effects that add to the attributes (Constitution +220 and Mentality +180 from
  the armor-type effect, Dexterity +26 from a consumable, which the capture does not name), the class and weapon-line passives
  that add to Accuracy, `CriticalPower`, `DisableDuration`, `CriticalAvoidance`, `MaxStamina`, and that make Penetration 0.975, the
  artisan stats of the lure, the crowbar and the guild (`Artisan_Gathering_*_Rating`, `Crowbar_CrateLootPercent`, `Artisan_Crafting_XP_Rating`), and
  `Artisan_Gathering_Lumberjacking/Mining_Add_MaxStack` (the tools' stat blocks give 42 and 46; the capture says 0: unexplained). Health and
  Mana are max-based here, the capture has the current ones. With the 34 effects the research reached 417 (94.3%).

## Tools

```powershell
cd services
go run ./internal/statprog/cmd/statprog compile --db ..\dist\content\content.db --out stats.bin      # the bundle (--mode float32|double|single)
go run ./internal/statprog/cmd/statprog dump --in stats.bin
go run ./internal/statprog/cmd/statprog corpus --out ..\server\worldd\stats\testdata\corpus.txt          # write the shared corpus
go run ./internal/statprog/cmd/statprog corpus --out ..\server\worldd\stats\testdata\corpus.txt --check  # fail if it is stale or does not replay
```

`--db` defaults to `AOC_CONTENT_DB`, then `..\dist\content\content.db`. The corpus (`corpus.go` has the line formats): five
bundles (the real graph, its double-mode variant, the `TinyGraph` that uses every instruction and every state function, its
single-mode twin, an abusive one), 38 characters with their stats (the Bard and the Ranger with their capture comparison, every
class's starting kit at 1 and 20, bare characters, a Fighter in combat with extra mods), 71 hostile blobs, one run that must be
refused, and three differential fuzz runs. `TestCorpusUpToDate` fails when the file differs from what the engine produces;
`TestCorpusReplay` replays it with no database.

## What plan B3 did with it

1. **contentc** (`cmd/contentc/statprogram.go`): after the other tables are committed it runs `LoadContentDB` on the file it has just written, `Compile`, `Encode` and `Decode` (the bytes must verify and encode back to themselves) and stores the
   bundle in the one-row table `stat_program` with its format version, mode, counts and crc32; `meta` has `statprog.*`. The item data the gear reads (stat blocks, quality bands, sets, tags) are tables too (`services/README.md`).
   `TestGearTablesEqualTheRecords` and `TestStatProgramInTheContentDB` prove them.
2. **worldd** (`server/worldd/stats`, `session`): the bundle is parsed and verified at start and every class is run bare as a check (a damaged or foreign program makes the world not ready: no login); a session computes the stats of its class,
   level and worn items at the join, fills the StatsComponent's arrays by `replication` (1 Owner, 5 OwnerProxy, 7 EveryoneProxy; 6 is other players' views, 0 not replicated), and on every equip or unequip recomputes and sends only the stats whose float
   bits changed (the four `16` stats also base and equipment). The engine returns the maximum of Health, Mana and Stamina (spawn at max); the session holds the current values and clamps them to a new maximum. The mods are `Gear.Mods`
   rewritten in C++ over the tables (`stats/gear.h`), half-up rounded (the research's best fit; Go's own corpus cases use `RoundNone` for kits, which are whole numbers) and equal to Go's on the real file for the captured Bard, the retail Ranger and the sixteen kit cases.
3. Effects were still to do: done in the D2 engine layer below (effects with their stat mods in the stat's own form, ranks, expression programs); the runtime that applies them in a session is part 2's.

## Retail equip deltas (plan B3)

Capture 1219 (retail: a chest piece, Quality 6, put on a character wearing 18 other items) changes 30 Owner, 14 OwnerProxy and 2 EveryoneProxy stats (46); taking it off (1253) changes the same 46. The engine, run for each of the eight classes at level 50 with the
18 other items (their qualities are not in the capture: 1) and the chest at Quality 6, changes 22 Owner, 9 OwnerProxy and 2 EveryoneProxy stats, the same set for every class; of the engine's 33, 32 are in retail's 46 and the 33rd is Health, the current
value, which the session holds and does not send (so the session sends 22 + 9 + 1). The 14 retail stats the engine does not move are all on the mana side: Mentality (and what follows from it: MaxMana, ManaRegeneration and its rating, the five magical
mitigation, avoidance and disable ratings and their five OwnerProxy percentages), which an effect adds (the research found Mentality +180 from the armor-type effect). That is part 2. The values are not compared: the capture does not name the character's level or class or the other items' qualities.

## Unverified

- The float32 rule is fitted to one character (the Bard) and does not contradict the Ranger; the item-value rounding (half up) is the
  research's best fit on the Ranger and is not proven; set bonuses and quality interpolation come from two characters.
- Effects, consumables, guild, professions, current health and mana: no evidence beyond the research's 417 of 442 for the Ranger.
- `bClampStatFinalValue` is not modelled (the multiplying-compliment flag is, from the D2 engine layer, in the effects' mods); `Reset_SetMin` is applied as written (every
  such stat has a minimum of 0 in the design data, so nothing depends on it).
- Only the player graph is compiled (the NPC lists need `EvalF`, `GetNPCLevel` and others).
