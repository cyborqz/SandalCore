# SandalCore — Known Issues

This document lists the known issues of the current SandalCore main in one place: what happens, a workaround when there is one, and the status. What is still to be built is in [ROADMAP.md](ROADMAP.md), under "Achieved and left".

**Last updated:** 29 October 2026

---

## Known Issues

| Area | What Happens | Workaround | Status |
|---|---|---|---|
| **Quests** | Only part of the beginner quests can be finished: Welcome to Verra (both ways) and the quest after it play to the end. The others wait for people that quests or events spawn, props that are placed in the level (carts, crates, vessels, construction sites), and item hand-ins to a person. Quest items arrive in the item bag, not the quest bag. The reward boxes some quests give arrive as items, but opening one needs the choose-one reward window, which is not built. | A game master can finish a quest with `questctl`. | Open: people without a body, the props, item hand-ins, the quest bag and the reward window. |
| **Quests / Givers** | Most quest givers are not in the world yet: of the 309 in the game's data only about 50 stand in the open world and the towns; the others are not placed there (quests and events spawn them, or they belong to other regions), so their quests are not offered. Quests that start from items are not offered either. | None. | Open: the givers that quests and events spawn; item quests later. |
| **Quests / Map markers** | Every active objective with a place gets a pin or an area when it starts: the people a talk objective names, the places of a "go to" objective, what a collect, use or hand-in objective tracks, the camps of creatures to kill. Not drawn yet: kill objectives named only by tags or whose creatures spawn at random in an area, the spawn-socket places of a "go to" objective, and the speech bubbles over NPCs. A pin is made once, when its objective starts, so it does not follow a walking NPC, and an NPC the world does not spawn has no pin. The quest-available mark shows on the NPCs within about 360 m that have a quest you can take; when an NPC offers several, the first one in the game's order decides which mark it shows. | The tracker's text says where to go. | Open: the speech bubbles, tag-named and random-area kills. |
| **Crates** | A construction crate is made at a Crate Packing station (13 of the 14 kinds: the Animal Husbandry crate needs a trained animal, which is not a material yet) and a trade crate at the commodity tables, and carried on the back in the cargo slot, which halves the carrier's speed. A trade crate's rarity follows the rarity of its materials (a construction crate is always Common), and a crate a quest gives is carried the same way. The Drop Crate ability is put on the action bar and takes the crate off the back, but into the item bag: the crate does not lie on the ground for others to pick up yet. A crate cannot be put into a building site or sold yet. The citizen table is paid in node currency, which a character cannot earn yet. | A game master gives a crate with `worldctl give-item` and money with `worldctl give-money`. | Next: the crate on the ground, then the building sites. |
| **Glint / Shops** | Shops and the trade crate tables take only the Glint kind a price names, so paying with a bigger kind (for example a Dim for a Dull price) is refused. | Keep some Dull Glint. | Planned: paying with change. |
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
| **Towns** | The floor and the buildings of the five towns are in the walking map, but doors, stairs with high steps and thin awnings are not, a mount summoned indoors is not checked for room, and a town changed to another level keeps the buildings of the level it started at until the walking map is made again. Some guards and townspeople may still stand in the wrong place, and trees inside towns can't be cut. Nearby town NPCs of a kind with no attack animation yet can hit you with no visible attack (no animation or projectile); the town's wolves, Grems and ravens animate theirs. A long teleport into a town falls about 140 m before the safety net puts the player back, 4.5 to 6 seconds later. | Walk into a town from outside, or teleport to a point close to its streets. | Open: doors and stairs; swapping the walking map when a town changes level. |
| **Combat / Falling** | A fall does no damage, however far. | None. | Planned: fall damage. |
| **Abilities** | Chilling Laminate shows a placeholder blue circle on the ground below the player. | None. | Investigating which visual effect is sent for it. |
| **Summoner / Pets** | Summoner pets are not implemented yet: summoning does not bring out a pet. | None. | Planned: Summoner pets. |
| **Ranger / Salvo** | Salvo gets stuck in its animation and does no damage. | None. | Open. |
| **Mage / Arcane Volley** | The mage's first ability, Arcane Volley, plays its animation and its missiles reach the target, but it does no damage and does not draw the target's attention. Cast it at a creature to see it. | None. | Open: under investigation. |
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
