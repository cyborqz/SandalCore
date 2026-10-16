# Sandal Core — Known Issues & Backlog

This document tracks known issues, behaviors under active investigation, workarounds, and the project backlog.

---

## Known Issues

| Area | What Happens | Workaround | Status |
|---|---|---|---|
| **Mounts / Aquatic** | Attempting to mount or remount an aquatic mount while swimming in water fails to complete possession. | Mount on land or shallow shoreline before entering deep water. | Investigating water volume entry state during mount possession. |
| **Mounts / Flight** | 44 mayor-specific flying mounts fail client-side item validation (`ValidateItemUse`) unless node citizenship and mayorship criteria are satisfied. | Use the PTR Eagle Gryphon mounts, which bypass node tenure validation. | Tied to City Node election milestone completion. |
| **Combat / No navmesh** | No navmesh = no combat: every mob has 0 HP and damage does nothing. Cause: `worldd` starts its combat system only after `dist\content\area_nav.bin` (with `area_geom.bin`) has loaded, and these 1.2 GB files are not in the repo. | Run `scripts\install_content.ps1` (the launchers do): it downloads `sandal-core-navmesh-v1.zip` from the Releases page. If the download is not possible, get the zip from Releases by hand and put it in `content\`, or point `SANDAL_NAVMESH_ZIP` at it, then run the script again. | Automatic: the installer downloads it from the `navmesh-v1` Release. |
| **World / Biomes** | `GetNodeBiome()` returns undefined on certain boundary cells at the edge of the Riverlands, preventing regional reagent drops on edge spawns. | Stay within central Riverlands zone coordinates for node harvesting. | Adding boundary cell interpolation in region mapping. |
| **Gathering / Rarity** | Every gathered item comes out Common. | None. | Planned fix: the rarity roll of each harvest. |
| **Gathering / Speed** | Gathering takes longer than on retail. | None. | Investigating how the gathering speed ratings (skills, talents, tools and gear) apply. |
| **Gathering / Full bag** | When the resource bag is full, the extra items drop in an ash pile, the look used for death piles. | Make room in the resource bag before gathering. | Open: dropped items need a look of their own. |
| **Inventory** | Items can't be destroyed by dragging them out of the inventory; the request is refused. | None. | Open: destroying an item (with the confirmation the client shows) is not built yet. |
| **Gathering / Restart** | Cut trees and hunted animals reset when the server restarts. | None. | Open: their state is not saved between runs. |
| **Towns** | Inside the five town bowls the ground isn't solid yet. Mount summons are refused, logging in inside a town can hang, town NPCs stand still, and trees inside towns can't be cut. | Summon mounts and log out outside the towns. | Planned: solid ground in the five towns (the next City Nodes work). |
| **Combat** | Level 1 goblins at the starter area hit too hard. | None. | Investigating their damage against their level and the game data. |
| **Abilities** | Chilling Laminate shows a placeholder blue circle on the ground below the player. | None. | Investigating which visual effect is sent for it. |
| **Mounts** | A summoned mount's nameplate shows the player's name instead of the mount's. | None. | Investigating the mount's name fields. |
| **Bard** | Resonance isn't consumed by songs. | None. | Investigating the consume rule of the songs. |

---

## Codebase Markers Summary

An audit of internal code markers across active services and servers indicates:
- **`TODO` markers**: 27 instances across 13 files (focused on edge-case protocol fields, auxiliary telemetry, and stubbed RPC fallbacks).
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
- **Automated Replay Regression Runner**: CI integration for continuous replay packet validation.
