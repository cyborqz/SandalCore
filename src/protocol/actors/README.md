# src/protocol/actors/

Per-actor knowledge files for the AoC world-bootstrap protocol.

## Purpose

Each file in this folder describes ONE logical actor type (or family of actors)
that the server needs to spawn into the client's world during the bootstrap
sequence. In the lifecycle of the protocol module:

- **Phase 2 (current)** — Files hold METADATA only: channel, bit count, known
  ChSequence, hypothesized class, evidence. The actual bytes still live in
  `../bootstrap/bootstrap_data.h`.

- **Phase 3 (next)** — Each file gains a `.cpp` with a `build(profile)` function
  that SYNTHESIZES the actor's bunch bytes from a `CharacterProfile`. Once
  every actor has a builder, `bootstrap_data.h` can be deleted.

## File naming

Names reflect our **best hypothesis** of what each actor represents, based on:
- The bunch's bit size (classes have stable sizes)
- Which channels they appear on
- Their position in the bootstrap sequence
- Stock UE5 conventions

Confidence levels per file (as of 2026-04-21):

| File | Confidence | Why |
|------|-----------|-----|
| `player_controller.h` | 🟢 Very high | Unique, first actor opened, ch=3, 3302 bits |
| `game_state.h`        | 🟢 High      | Unique bit size, early in bootstrap, UE5 always has one |
| `characters.h`        | 🟡 Medium    | 4326-bit, 7 instances — plausible characters, could be PlayerStates |
| `npcs.h`              | 🟡 Medium    | 5350-bit, 19 instances — mobs/enemies pattern |
| `interactables.h`     | 🟠 Low-med   | 1254-bit, 8 instances — best guess |

**Names can be renamed in Phase 3** once `ArchetypeGUID` decoding reveals the
actual class blueprints. Filename changes are trivial (find-replace).

## Reading these files

Each actor header declares a `constexpr ActorSpec` inside its own namespace.
The spec has:
- `channel` — the UE5 channel index used
- `bunch_data_bits` — the size of the ActorOpen bunch's payload
- `ch_sequence` — the first reliable ChSequence (typically 1978)
- `instance_count` — how many times this spec appears in the bootstrap
- `hypothesis` — one-line description of what we think it is

The spec is purely informational in Phase 2. In Phase 3 the namespace will
also declare a `build(BunchBuffer&, const CharacterProfile&)` function.

## Aggregate registry

`actor_manifest.h` includes all actor headers and exposes an iterable list
of all known specs. This lets the bootstrap sender walk every actor type
without hardcoding channel numbers.
