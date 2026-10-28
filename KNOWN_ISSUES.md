# SandalCore — Known Issues

This document lists the known issues of the current SandalCore main in one place: what happens, a workaround when there is one, and the status. What is still to be built is in [ROADMAP.md](ROADMAP.md), under "Achieved and left".

**Last updated:** 28 October 2026

---

## Known Issues

| Area | What Happens | Workaround | Status |
|---|---|---|---|
| **Quests** | A quest can be taken, tracked and abandoned, but its objectives do not count yet: it cannot be finished by play, pays no reward, and has no hand-in or map markers. | A game master can finish a quest with `questctl`. | Next: the second part of quests. |
| **Quests / Givers** | Most quest givers are not in the world yet: of the 309 in the game's data only about 50 stand in the open world and the towns; the others are not placed there (quests and events spawn them, or they belong to other regions), so their quests are not offered. Quests that start from items are not offered either. | None. | Open: the givers that quests and events spawn; item quests later. |
| **Glint / Shops** | Shops take only the Glint kind a price names, so paying with a bigger kind (for example a Dim for a Dull price) is refused. | Keep some Dull Glint. | Planned: paying with change. |
| **Embersprings / Death window** | The death window does not list the first Emberspring a character discovered (for a new character, the Aelan Divine Gate). | Revive brings you back at the starting area. | Open. |
| **Weapons / Talents** | Weapon lines earn no experience, so weapon-tree talents cannot be picked. | None. | Open: weapon line experience. |
| **Combat / Finishers** | The extra effects of a weapon combo's last hit (class resources, weapon-tree finisher procs) are missing; its damage lands. | None. | Open: they are the game's reactions, which are not built yet. |
| **Bard** | A weapon combo's finisher doesn't spend Resonance (only Fortissimo does). | None. | Open: the finisher's melody effect, which spends up to 3 stacks, is one of the game's reactions. |
| **Combat / Dice** | The hit, block and critical rolls repeat the same sequence after every server start. | None. | Open. |
| **Creatures / Packs** | Creatures next to one that noticed you do not join the fight until they notice you themselves. | None. | Open: pack aggro. |
| **Creatures / Damage** | Some creatures hit harder or softer than in retail (deer and rams about three times as hard, bears almost twice). | None. | Open: their stats are compared with retail first. |
| **Creatures / Fights** | About half of the hostile creatures (undead, Minotaurs, constructs, spiders and others) use a generic attack list instead of their own. Bandits and cultists do not spawn yet. | None. | Open. |
| **World / Day and night** | Day and night creatures follow the clock of the server's PC at start, not the sky. | None. | Open: one world clock. |
| **World / Walls and water** | Creatures can walk through fences and walls and along lake beds, and lava does no damage. | None. | Open. |
| **World / Biomes** | `GetNodeBiome()` returns undefined on certain boundary cells at the edge of the Riverlands, preventing regional reagent drops on edge spawns. | Stay within central Riverlands zone coordinates for node harvesting. | Adding boundary cell interpolation in region mapping. |
| **Inventory** | Dragging a stack onto another stack of the same item (to merge them) is refused, and so is splitting a stack. | Keep the stacks apart. | Planned: joining and splitting stacks. |
| **Towns** | The floor of the five towns is solid, but their walls, stairs, upper floors and towers have no collision yet, town NPCs stand still, some guards and townspeople stand in the wrong place or have 0 HP, and trees inside towns can't be cut. Nearby town NPCs can hit you with no visible attack (no animation or projectile). A long teleport into a town falls about 140 m before the safety net puts the player back, 4.5 to 6 seconds later. | Walk into a town from outside, or teleport to a point close to its streets. | Planned: the towns' structures and a navmesh for their streets (the next City Nodes work). |
| **Combat / Falling** | A fall does no damage, however far. | None. | Planned: fall damage. |
| **Abilities** | Chilling Laminate shows a placeholder blue circle on the ground below the player. | None. | Investigating which visual effect is sent for it. |
| **Gathering / Speed** | Gathering takes longer than on retail. | None. | Investigating how the gathering speed ratings (skills, talents, tools and gear) apply. |
| **Gathering / Full bag** | When the resource bag is full, the extra items drop in an ash pile, the look used for death piles. | Make room in the resource bag before gathering. | Open: dropped items need a look of their own. |
| **Gathering / Restart** | Cut trees and hunted animals reset when the server restarts. | None. | Open: their state is not saved between runs. |
| **Mounts / Aquatic** | Attempting to mount or remount an aquatic mount while swimming in water fails to complete possession. | Mount on land or shallow shoreline before entering deep water. | Investigating water volume entry state during mount possession. |
| **Mounts / Flight** | 44 mayor-specific flying mounts fail client-side item validation (`ValidateItemUse`) unless node citizenship and mayorship criteria are satisfied. | Use the PTR Eagle Gryphon mounts, which bypass node tenure validation. | Tied to City Node election milestone completion. |
| **Combat / No navmesh** | No navmesh = no combat: every mob has 0 HP and damage does nothing. Cause: `worldd` starts its combat system only after `dist\content\area_nav.bin` (with `area_geom.bin`) has loaded, and these 1.2 GB files are built on your PC, not shipped. | Run `extract_game_data.bat` (`build_all.bat` and the launchers do): it builds them from your Ashes of Creation install. It needs the game found (`GAME_ROOT` or `-GameRoot`), the .NET 8 SDK and a built `aoc_navbuild`. | Automatic: the `navmesh` step of `extract_game_data.bat` builds it. |

---

## Codebase Markers Summary

An audit of internal code markers across active services and servers (recounted 28 October 2026) indicates:
- **`TODO` markers**: 18 instances across 6 files (focused on edge-case protocol fields, auxiliary telemetry, and stubbed RPC fallbacks).
- **`FIXME` markers**: 0 active instances.
