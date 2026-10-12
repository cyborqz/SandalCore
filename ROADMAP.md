# Sandal Core — Project Roadmap

> [!NOTE]
> This roadmap reflects the ongoing development progress of Sandal Core. Sandal Core is a work in progress, not a final product. Features and milestones evolve continuously as the protocol is worked out and the server emulation advances.

**Last updated:** October 2026

---

## Where We Are Now

**Current Milestone:** `10- Gathering and Professions` (In progress)

Milestones through October 2026 (login, world bootstrap, stats, talents, combat/abilities, navigation, mounts, and city node services) were completed and validated against retail captures and test suites in `server/worldd/tests`. The current focus is completing resource harvesting and gathering mechanics across the Riverlands area. Hunting encounters and drop notifications are operational, while tree/vegetation harvest completion states are currently being resolved.

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
9. **9- City Nodes and Civilization** — [Done] CityNodeService, node experience and level state, town asset replication, and citizenship/mayorship structures.
10. **10- Gathering and Professions** — [In progress] Resource node actors, hunting harvesting, drop tables, tool requirements, and harvest interaction channels.
11. **11- Quests and Narrative** — [Next] Quest acceptance, objective condition tracking, NPC dialogue interactions, reward fulfillment, and journal state.
12. **12- Guilds and Social Systems** — [Planned] Guild creation, ranks, member permissions, party/raid formations, chat channels, and mail delivery.
13. **13- Economy and Player Trading** — [Planned] Secure player trading, vendor marketplaces, crafting recipe execution, and currency handling.
14. **14- Node Sieges and Territory Wars** — [Planned] Siege declarations, siege weaponry, battlefield boundaries, and node destruction/de-leveling mechanics.
15. **15- Caravans and Trade Routes** — [Planned] Caravan creation, trade route navigation, open-world PvP escort/plunder, and regional trade goods.
