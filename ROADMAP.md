# SandalCore — Project Roadmap

> [!NOTE]
> This roadmap reflects the ongoing development progress of SandalCore. SandalCore is a work in progress, not a final product. Features and milestones evolve continuously as the protocol is worked out and the server emulation advances.

**Last updated:** 28 October 2026

---

## Where We Are Now

**Current milestones:** `9- City Nodes and Civilization`, `11- Quests and Narrative` and `13- Economy and Player Trading` (all in progress)

Milestones 1-8 and 10 (login, world bootstrap, stats, talents, combat and abilities, navigation, mounts, and gathering and professions) are complete and validated against retail captures and test suites in `server/worldd/tests`. Since then the server gained NPC conversations and the first part of quests, merchants and currency, crafting at the towns' stations, item rarities, weapon combo finishers, magic weapon scaling, and creatures that notice and chase by their own settings.

---

## Achieved and left (no specific order)

### Achieved
- Connection, login and character creation; the world's start and player state.
- Stats, talents, items and bags (the item bag and the material bags).
- Combat and the class abilities; weapon combo finishers and weapon swaps; magic weapon scaling (caster weapons scale with Magical Power, and melee combos hit for their full share).
- The walking map and NPC spawning; creatures that notice by hearing and sight and give up 140 m from where the fight began; loot rolls that change on every server start.
- Mounts: ground, aquatic and flying.
- Gathering, hunting and professions.
- City nodes: node levels and experience, the towns' scenery, people and floor, and the towns' crafting stations.
- NPC conversations (the talking window).
- Merchants: currency, buying, selling, repairing and destroying items; talking shopkeepers.
- Crafting: processing jobs, quick make and collecting finished jobs.
- Item rarities for resources, weapons and armor, from creatures and from crafting.
- Tutorials that stay done, and quiet confirmations (targets, the PvP flag, loot windows, map markers).
- The first part of quests: quest givers offer their quests; accepting, the journal and the tracker, abandoning; quests are kept across logins; Welcome to Verra starts in the starting area.

### In progress
- Storage clerks: putting items in and taking them out.
- NPC looks and names: every human NPC dressed and named from the game's records, and the missing peaceful NPCs.
- Glint tiers: ten Glint of a kind into one of the next kind, and kills paying Glint in kinds.
- Embersprings: discovering them, attuning to one, and choosing where to respawn.
- Node stages: building sites below Village, and the Wilderness camp.

### Left
- Quests: objectives, hand-in and rewards; the Destiny window, group quests, events and boards.
- Reactions: the extra effects of combo finishers (class resources, weapon-tree finisher procs, the Bard's melody).
- Weapon line experience and the weapon trees.
- Stacks: joining, splitting and dragging stacks; the gathering bags' bigger stacks; paying with change.
- Storage: buying more room, tab jobs, crates and guild storage.
- NPCs: face variety, names for rare creatures, and hostile humanoids such as bandits and cultists.
- Creatures: the remaining creature families' own fights and abilities, pack aggro, damage matched to retail, and flying creatures.
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

1. **1- Connection and Protocol Foundations** — [Done] Handshake protocol, stateless connect, packet framing, NetGUID allocation, and bunch replication codecs.
2. **2- Authentication and Character Management** — [Done] Auth service, account profiles, character creation with visual customization, and lobby transitions.
3. **3- World Bootstrap and Replication** — [Done] Map loading into Verra World Master, actor open sequences, possession handshake, and player state synchronization.
4. **4- Stats Engine and Calculations** — [Done] Core attribute formulas, expression evaluator, compiled stat programs, and equipment/gear stat recalculation.
5. **5- Progression, Talents, and Items** — [Done] Adventuring XP curves, level caps, skill tree purchases, action bar assignments, item usage, and inventory storage.
6. **6- Combat and Class Abilities** — [Done] Ability pipeline across all classes, cast verification, cooldown ledger, projectiles, hit confirmations, floating text, and status effects.
7. **7- Living World and Navigation** — [Done] Chunked navmesh windows, Riverlands region partitioning, NPC actor spawning, AI agro/leash behavior, and respawn cycles.
8. **8- Mounts and Traversal** — [Done] Ground, aquatic, and flying mount summoning, riding possession, mount combat on mount health, dash, and road traversal.
9. **9- City Nodes and Civilization** — [In progress] CityNodeService, node experience and level state, town asset replication, and citizenship/mayorship structures; the five towns have their real floor and their crafting stations; next: the Wilderness and Camp stages, the towns' buildings and walkable streets.
10. **10- Gathering and Professions** — [Done] Gathering professions, node harvesting, the Riverlands' foliage, hunting, resource bags and the pick-up popup.
11. **11- Quests and Narrative** — [In progress] NPC conversations, quest offers, acceptance, the journal and abandoning are done; next: objective tracking, hand-in and reward fulfillment.
12. **12- Guilds and Social Systems** — [Planned] Guild creation, ranks, member permissions, party/raid formations, chat channels, and mail delivery.
13. **13- Economy and Player Trading** — [In progress] Currency, merchants, selling and repairing, crafting at stations and item rarities are done; next: secure player trading, marketplaces and node currency.
14. **14- Node Sieges and Territory Wars** — [Planned] Siege declarations, siege weaponry, battlefield boundaries, and node destruction/de-leveling mechanics.
15. **15- Caravans and Trade Routes** — [Planned] Caravan creation, trade route navigation, open-world PvP escort/plunder, and regional trade goods.
