# src/protocol/ — UE5 Wire Protocol Module

This module owns the generation of AoC server→client (S>C) packets from first
principles — eventually replacing the replay file dependency entirely.

## Goal

Get the game client into a playable world **without** `replay_data.bin` on disk.
The packet content is compiled into the server binary.

## Evolution (3 phases)

### Phase 1 — Extract (current focus)

The 400-packet bootstrap that successfully spawns the client into the world is
extracted from `replay_data.bin` once, converted into a C++ header
(`bootstrap/bootstrap_data.h`) with the raw bytes, and served by the server
directly from memory. The `.bin` file is no longer needed at runtime.

### Phase 2 — Modularize

The big monolithic `bootstrap_data.h` is split by logical actor:
- `actors/player_controller.*` — the ch=3 ActorOpen (PlayerController spawn)
- `actors/character.*` — the ch=14,24,... ActorOpens (BP_Character instances)
- `actors/npc.*` — the 5350-bit NPC ActorOpens
- `actors/game_state.*` — 2278-bit game state actor
- etc.

Each file carries raw bytes + known metadata (channel, bits, decoded fields).

### Phase 3 — Synthesize

For each actor, we decode its internal format (`SerializeNewActor` + property
values) and replace the raw-byte blob with a `build_from_profile(profile)`
function that generates the bytes dynamically.

When ALL actors have builders, the module produces the entire bootstrap from
pure code + a `CharacterProfile`. That's true "no replay."

## Layout

```
protocol/
├── bunch_builder.h/cpp       # Writes S>C bunch headers from parameters
├── packet_builder.h/cpp      # Assembles full packets (outer + notify + bunches)
├── bootstrap/
│   ├── bootstrap_sequence.h/cpp   # Orchestrates delivery of bootstrap packets
│   ├── bootstrap_data.h           # [Phase 1] embedded raw bytes
│   └── README.md
├── actors/                   # [Phase 2+] per-actor builders
│   ├── actor_base.h          # Common interface
│   ├── player_controller.h/cpp
│   ├── character.h/cpp
│   ├── game_state.h/cpp
│   └── npc.h/cpp
├── character_profile.h/cpp   # Player's chosen data (shared input)
├── tools/
│   └── extract_bootstrap.py  # Reads replay_data.bin → emits bootstrap_data.h
└── README.md                 # This file
```

## Non-goals (for now)

- Replacing the live NMT handshake (already live-synthesized by `GameServer`)
- Per-player personalization (deferred; will come for free in Phase 3)
- World actors beyond the captured bootstrap set (far future)

## Relationship to the replay system

The protocol module does NOT replace `ReplayData` immediately. It runs
alongside it as an alternative source of packet data. A CLI flag
(`--use-embedded-bootstrap`) switches to the new path. Once verified bit-
equivalent with the replay path, the embedded path becomes the default.
