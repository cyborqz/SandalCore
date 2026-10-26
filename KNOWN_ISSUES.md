# SandalCore — Known Issues & Backlog

This document tracks known issues, behaviors under active investigation, workarounds, and the project backlog.

---

## Known Issues

| Area | What Happens | Workaround | Status |
|---|---|---|---|
| **Mounts / Aquatic** | Attempting to mount or remount an aquatic mount while swimming in water fails to complete possession. | Mount on land or shallow shoreline before entering deep water. | Investigating water volume entry state during mount possession. |
| **Mounts / Flight** | 44 mayor-specific flying mounts fail client-side item validation (`ValidateItemUse`) unless node citizenship and mayorship criteria are satisfied. | Use the PTR Eagle Gryphon mounts, which bypass node tenure validation. | Tied to City Node election milestone completion. |
| **Combat / No navmesh** | No navmesh = no combat: every mob has 0 HP and damage does nothing. Cause: `worldd` starts its combat system only after `dist\content\area_nav.bin` (with `area_geom.bin`) has loaded, and these 1.2 GB files are built on your PC, not shipped. | Run `extract_game_data.bat` (`build_all.bat` and the launchers do): it builds them from your Ashes of Creation install. It needs the game found (`GAME_ROOT` or `-GameRoot`), the .NET 8 SDK and a built `aoc_navbuild`. | Automatic: the `navmesh` step of `extract_game_data.bat` builds it. |
| **World / Biomes** | `GetNodeBiome()` returns undefined on certain boundary cells at the edge of the Riverlands, preventing regional reagent drops on edge spawns. | Stay within central Riverlands zone coordinates for node harvesting. | Adding boundary cell interpolation in region mapping. |
| **Gathering / Speed** | Gathering takes longer than on retail. | None. | Investigating how the gathering speed ratings (skills, talents, tools and gear) apply. |
| **Gathering / Full bag** | When the resource bag is full, the extra items drop in an ash pile, the look used for death piles. | Make room in the resource bag before gathering. | Open: dropped items need a look of their own. |
| **Inventory** | Dragging a stack onto another stack of the same item (to merge them) is refused. | Keep the stacks apart. | Open: merging stacks is not built yet. |
| **Gathering / Restart** | Cut trees and hunted animals reset when the server restarts. | None. | Open: their state is not saved between runs. |
| **Towns** | The floor of the five towns is solid, but their walls, stairs, upper floors and towers have no collision yet, town NPCs stand still, some guards and townspeople stand in the wrong place or have 0 HP, and trees inside towns can't be cut. Nearby town NPCs can hit you with no visible attack (no animation or projectile). A long teleport into a town falls about 140 m before the safety net puts the player back, 4.5 to 6 seconds later. | Walk into a town from outside, or teleport to a point close to its streets. | Planned: the towns' structures and a navmesh for their streets (the next City Nodes work). |
| **Combat / Falling** | A fall does no damage, however far. | None. | Planned: fall damage. |
| **Abilities** | Chilling Laminate shows a placeholder blue circle on the ground below the player. | None. | Investigating which visual effect is sent for it. |
| **Bard** | A weapon combo's finisher doesn't spend Resonance (only Fortissimo does). | None. | Open: the finisher's melody effect, which spends up to 3 stacks, is one of the game's reactions, and reactions are not built yet. |
| **Combat / Finishers** | The extra effects of a weapon combo's last hit (class resources, weapon-tree finisher procs) are missing; its damage lands. | None. | Open: they are the game's reactions, which are not built yet. |

---

## Codebase Markers Summary

An audit of internal code markers across active services and servers indicates:
- **`TODO` markers**: 18 instances across 6 files (focused on edge-case protocol fields, auxiliary telemetry, and stubbed RPC fallbacks).
- **`FIXME` markers**: 0 active instances.

---

## Backlog

### Gameplay Systems
- **Quest System**: Full quest state replication, quest item prerequisites, objective markers, and NPC turn-in dialogues.
- **Narrative & Dialogue**: Branching dialogue trees with NPC voice/animation synchronization.
- **Crafting & Recipes**: Material combining, crafting station proximity detection, and quality outcome rolls.
- **Player Economy**: Direct secure player trading, regional consignment markets, and currency sinks.
- **Caravans**: Player-driven caravan summoning, cargo health, escort buffs, and bandit ambush events.
- **Node Sieges**: Declaration timers, battlefield boundary fencing, siege weapon physics, and territory control transfers.
- **Guild Progression**: Guild leveling, guild bank replication, alliance flags, and guild crest customization.

### Infrastructure & Tooling
- **Distributed Cell Sharding**: Multi-process cell partitioning for high-concurrency world instances.
