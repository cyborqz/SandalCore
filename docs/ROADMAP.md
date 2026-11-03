# SandalCore — Project Roadmap

> [!NOTE]
> This roadmap reflects the ongoing development progress of SandalCore. SandalCore is a work in progress, not a final product. Features and milestones evolve continuously as the protocol is worked out and the server emulation advances.

**Last updated:** 4 November 2026

---

## Where We Are Now

**5 of 16 phases are done** (1-5), **7 are in progress** (6-12) and 4 are planned (13-16).

**Current phases:** `6- Progression and items`, `7- Combat and abilities`, `8- Gathering and professions`, `9- Living world`, `10- City nodes`, `11- Quests` and `12- Economy` (all in progress)

Phases 1-5 (the network protocol, login, entering the world, stats and mounts) are complete and validated against retail captures and test suites in `server/worldd/tests`. Phases 6-12 work in their first parts, and what is left of each is named in its line below and under "Achieved and left". Since the first phases the server gained NPC conversations and the first part of quests, vendors and currency, crafting at the towns' stations, item rarities, weapon combo finishers, magic weapon scaling, mobs that notice and chase by their own settings, storage, NPC looks and names, and the Embersprings.

---

## Achieved and left (no specific order)

### Achieved
- Connection, login and character creation; the world's start and player state.
- Stats, talents, items and bags (the item bag and the material bags).
- Combat and the class abilities; weapon combo finishers and weapon swaps; magic weapon scaling (caster weapons scale with Magical Power, and melee combos hit for their full share).
- The walking map and NPC spawning; mobs that notice by hearing and sight and give up 140 m from where the fight began; loot rolls that change on every server start.
- Mounts: ground, aquatic and flying.
- Gathering, hunting and professions.
- City nodes: node levels and experience, the towns' scenery, people and floor, and the towns' crafting stations.
- NPC conversations (the talking window).
- Vendors: currency, buying, selling, repairing and destroying items; talking shopkeepers.
- Crafting: processing jobs, quick make and collecting finished jobs; the artisan gear a crafter wears (shirt, pants and tool belt of a profession) changes the result: a crafted piece's quality and experience, and a job's time, extra units, units that come out a rarity higher, and experience.
- Consumables (first part): scrolls and elixirs are used from the item bag, take one off the stack, run their cooldown and give their buff for its time: the stats and artisan ratings the item holds at its quality (an Uncommon elixir of Herbalism, the Constitution scrolls, the experience scrolls), one scroll and one elixir at a time.
- Item rarities for resources, weapons and armor, from mobs and from crafting.
- Glint: all six kinds, ten of a kind into one of the next with a right-click, and kills paying Glint in kinds.
- Tutorials that stay done, and quiet confirmations (targets, the PvP flag, loot windows, map markers).
- The first part of quests: quest givers offer their quests; accepting, the journal and the tracker, abandoning; quests are kept across logins; Welcome to Verra starts in the starting area.
- The second part of quests: objectives that count (talking to people, kills, Embersprings, places to reach, items carried, crates delivered, quest items that mobs drop), the tracker counting, the hand-in with its rewards (items, experience and town experience), and Welcome to Verra (both ways) with the quest after it.
- Quest objects: the bodies, crates, roots, building sites and other objects of the beginner quests stand in the world, take the time the game gives their use, give what they give, count for the quests that name them, and come back after their own time.
- The item hand-in to a person: a conversation that asks for items opens the hand-in window at its end; the items the player picks leave the bag, count for the quest's hand-in objective and the talk goes on (Hungry For More: the Suspicious Bread to Cleric Beyam). A hand-in objective that a conversation completes counts too (the crest of The Root of the Problem), and an active hand-in objective names the node its person's talk starts at.
- Quest map markers: a pin or an area on the map for every active objective that has a place (the people a talk objective names, the places of a "go to" objective, what a collect, use or hand-in objective tracks, the camps of mobs to kill), and the quest-available mark on the NPCs that have a quest to take, over the NPC and on the map.
- Storage: the Storage Clerk's item and materials storage, kept across logins.
- NPC looks and names: every human NPC dressed, named and posed from the game's records, and the missing peaceful NPCs.
- Bandit raiders and captains: dressed, named and armed in the Riverlands' camps, and fighting with their own attacks (strikes, cleaves, heavy slashes and whirlwinds).
- Bandit arsonists: dressed, named and in the Riverlands' camps, throwing their fire bolts (and the fire bolt combo for the few that have it) every two seconds or so from up to 30 m, and closing in to 25 m between casts.
- Mob projectiles: a bolt or a bomb leaves the mob when its animation says, flies to its target at the game's speed and hurts when it arrives (the arsonists' fire bolts and the goblin shamans' oozing bomb).
- Goblins and other mobs that were built from their Blueprint alone play their own attack animations, with the lunges and rolls that carry them (the Languid Goblins, the Flayers, the Warlord, the Diggers and the Skinwalkers among them).
- Goblins and bandits chase at the game's own speed (faster with the mob's rank) and turn a fifth faster, walk straight up to a player instead of waiting where a special attack would reach, and the Goblin Flayers spin and the Goblin Assassins backstab and kickflip.
- Embersprings: discovering them, attuning to one, and choosing where to respawn.
- Weapon line experience: kills and the game master level scrolls pay every weapon you wear, each of the 21 weapon lines levels up to 25 on its own, the Skill Tree shows a line's level, bar and talent points, the weapon talents are bought with them, and the free Accuracy passives arrive at levels 5, 10, 15, 20 and 25.
- Burning mobs: the Burning debuff takes a fire number from a mob every second for 8 seconds (up to 24), shows the fire and the Burning icon, follows it home after a reset and ends with its death or the burner's leaving.
- Debuffs on mobs: the bleeds (Bleeding, Hemorrhage), the poisons and the stat debuffs of a hit take effect on a mob. Bleeds and poisons take Health at their ticks (one Bleeding debuff for each attacker, a poison's stacks make its ticks bigger), Shaken and Volatile make a mob take more physical and magical damage, Demoralized makes it hit softer and Blinded makes it miss more.
- Server speed with many players: a fixed-rate server tick, the players' views of the world taking turns, cheaper updates, one update built for all the players who see the same mob in the same state, the players' views worked out on several processor cores, and move answers at the original server's pace. In a load test with simulated players that walk, the server held its 30 updates a second with about twice as many of them as before.

### In progress
- Node stages: building sites below Village, and the Wilderness camp.
- Reactions (first iteration): the game's reactions run for your own hits and for paying an ability's cost. The Bard's Resonance follows the game's rule (the Resonant Weapon passive from level 5) and its Menacing and Cathartic melodies add their damage at a combo finisher and spend up to 3 Resonance. The weapon talents that react at a finisher (the wand's Burning, Chilled and Volatile, the axes' Bleed, Snare and Wound, and the like) fire at the deadly finishing hit of the weapon in hand, the wand's Burning puts the Burning debuff on the mob, and the damage talents of the weapon trees count for its hits. The other classes' reactions run from the game's data and are not checked against the game yet.
- Crates: construction crates and trade crates are made at the crate stations and carried in the cargo slot, which slows the carrier; a trade crate's rarity follows its materials, a quest can give a crate, and the Drop Crate ability takes the crate off the back. Next: the crate lying on the ground for others to pick up, then putting crates into building sites and selling them. The other crate types stay under Left.
- Summoner pets: the Guardian, Hunter and Mystic come with the summon, follow, fight, take commands and stances, play their ten abilities and come back when more than 40 m away.

### Left
- Quests: the speech bubbles over NPCs who have an objective for you; the choose-one reward window and the Quest bag; hand-ins that ask for items by tag or rarity, and the pendant of The Great Mirage; objects that stand in the map itself (the Harbinger quests); the Destiny window, group quests, events and boards.
- Reactions: the Bard's Cheerful, Pensive and Epic melodies at a finisher, the debuffs of its damage melodies, the slows the weapon finishers put on a mob, checking the other classes' finisher effects against the game, and the reactions that wait for you being hit or for other events.
- Consumables: food and drinks (sitting down to eat, the regeneration meals), potions and salves (the heals, the shared cooldown), and buffs that stay through a logout.
- Crafting ratings from guild perks (there are no guilds yet).
- Stacks: joining, splitting and dragging stacks; the gathering bags' bigger stacks; paying with change.
- Storage: buying more room, tab jobs, crates and guild storage.
- Embersprings: the map markers, the respawn effect, and the siege, war and corruption respawns.
- NPCs: face variety, names for rare mobs, and the other hostile humanoids (the bandit archers and healers, and the cultists).
- Mobs: the remaining mob families' own fights and abilities (the Goblin Flame Shamans' fire bombs and the arsonists' fire spray, stream and ring among them), smooth movement for goblins and bandits while their attack animations play, pack aggro, damage matched to retail, and flying mobs.
- Abilities: the bars of Deliverance (a held cast) and Disengage follow the game's recorded timeline.
- The walking map: walls, fences, water and holes.
- The world: spawns that follow node levels, a world clock with day and night spawns, lava, world events and weather, treasure hunts, fishing, random gatherables, and camp props (tents, totems, caches, doors).
- Nodes: the Camp's stations, harvesting a node's own trees, node experience and vassals, citizenship and mayor, and node taxes.
- Economy: player trading, marketplaces, node currency and crates.
- Social: seeing other players, guilds, parties and raids, chat and mail.
- Node sieges and territory wars.
- Caravans and trade routes.
- Conversations: NPC voice and animation while they talk.
- Infrastructure: multi-process cell sharding for crowded worlds.

---

## Milestones

1. **1- Network protocol** — [Done] How the client and the server talk: the handshake, packet framing and replication.
2. **2- Login** — [Done] The launcher, accounts, character creation and character select.
3. **3- Into the world** — [Done] Entering the world as your character, with your progress kept across logins.
4. **4- Stats** — [Done] The game's own stat formulas, updated as your gear changes.
5. **5- Mounts** — [Done] Ground, flying and swimming mounts, sprinting and roads.
6. **6- Progression and items** — [In progress] XP and levels, talents, the action bar, bags, item rarities, weapon XP, scrolls and elixirs. Still to come: splitting and joining stacks, food and potions.
7. **7- Combat and abilities** — [In progress] Most classes' abilities, weapon combos and finishers, buffs, death and respawn. Mobs take the Burning debuff, the bleeds, poisons and stat debuffs of a hit, and the Summoner's Guardian, Hunter and Mystic fight at their owner's side. Still to come: slows, snares and CC on mobs (roots, stuns; the weapon finisher talents' slows among them).
8. **8- Gathering and professions** — [In progress] Mining, logging, herbalism and hunting, and processing and crafting at the towns' stations. Still to come: fishing and the rest of the 22 professions.
9. **9- Living world** — [In progress] Navigation, and mobs that see, hear, chase and fight with their own attacks, with loot and respawns. Still to come: the other mob families, flying mobs, and day and night spawns.
10. **10- City nodes** — [In progress] Node levels and XP, and the five towns with their people, buildings and crafting stations. Still to come: citizenship, mayors, taxes and vassal nodes.
11. **11- Quests** — [In progress] NPC conversations, quest offers, objectives, rewards and map markers, for the first quests of the starting area. Still to come: more quests, group quests and events.
12. **12- Economy** — [In progress] Gold, silver, copper and Glint, the NPC vendors, crates and storage. Still to come: node currencies and more crates.
13. **13- Social** — [Planned] Seeing other players, guilds, parties and raids, chat and mail, player trading and the marketplace.
14. **14- Node sieges** — [Planned] Node sieges and territory wars.
15. **15- Caravans** — [Planned] Caravans and trade routes.
16. **16- World events** — [Planned] Corruption events and other world events.
