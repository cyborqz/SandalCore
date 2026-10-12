# Sandal Core — Known Issues & Backlog

This document tracks known issues, behaviors under active investigation, workarounds, and the project backlog.

---

## Known Issues

| Area | What Happens | Workaround | Status |
|---|---|---|---|
| **Gathering / Trees** | Hunting works and the gathered item's pop-up icon displays, but gathering a tree never finishes: the interaction channel bar fills while standing still, the tree is never harvested/removed, and subsequent gatherable interactions report that you cannot interact while gathering. | Relog or restart the session to clear the stuck interaction state; hunt creatures for resources in the interim. | Investigating state machine transition on harvest channel completion. |
| **Mounts / Aquatic** | Attempting to mount or remount an aquatic mount while swimming in water fails to complete possession. | Mount on land or shallow shoreline before entering deep water. | Investigating water volume entry state during mount possession. |
| **Mounts / Flight** | 44 mayor-specific flying mounts fail client-side item validation (`ValidateItemUse`) unless node citizenship and mayorship criteria are satisfied. | Use the PTR Eagle Gryphon mounts, which bypass node tenure validation. | Tied to City Node election milestone completion. |
| **Combat / No navmesh** | No navmesh = no combat: every mob has 0 HP and damage does nothing. Cause: `worldd` starts its combat system only after `dist\content\area_nav.bin` (with `area_geom.bin`) has loaded, and these 1.2 GB files are not in the repo. | Run `scripts\install_content.ps1` (the launchers do): it downloads `sandal-core-navmesh-v1.zip` from the Releases page. If the download is not possible, get the zip from Releases by hand and put it in `content\`, or point `SANDAL_NAVMESH_ZIP` at it, then run the script again. | Automatic since the installer step; the owner must still publish the asset and fill in the url in `content\navmesh.json`. |
| **World / Biomes** | `GetNodeBiome()` returns undefined on certain boundary cells at the edge of the Riverlands, preventing regional reagent drops on edge spawns. | Stay within central Riverlands zone coordinates for node harvesting. | Adding boundary cell interpolation in region mapping. |

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
