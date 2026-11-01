# Changes of creature-effects-burn: the Burning debuff lands on mobs (a line that starts with # is a comment)

@known-issues set Combat / Finishers
| **Combat / Finishers** | The game's reactions run for the events of your own hits and of paying an ability's cost, and only the Bard's have been played through. The other classes' reactions (Fighter Momentum, Mage essences and Tank Courage among them) run from the game's data where their shape is served and have not been checked against the game yet, so a number, a buff or a resource may differ from the game's. The weapon talents that react at a finisher (the wand's Burning, Chilled and Volatile, the axes' Bleed, Snare and Wound, and the like) fire at the deadly finishing hit of the weapon in hand; the wand's Burning puts the Burning debuff on the mob, and the other debuffs they put on a mob do not show on the target yet (see Combat / Mob debuffs). Reactions that wait for you being hit (reflect, vengeance and the like) or for other events (an effect, a projectile, a lingering zone), that are cast by the hit's instigator, or that fire further hits at the allies around do nothing yet. | `--no-reactions` (or `AOC_NO_REACTIONS=1`) turns all reactions off. | Open: checking the other classes against the game, then the debuffs on mobs. |

@known-issues add
| **Combat / Mob debuffs** | A mob takes the Burning debuff of a hit that sets it on fire (the Mage's Fireball with the Fire Empowerment, the wand's Burning finisher): a fire number over it every second for 8 seconds (up to 24 with more burning hits), with the fire and the Burning icon on it. The Burning debuff goes on through a reset, ends with the mob's death, and ends when the player who lit it leaves or dies. While a mob walks home after a reset the debuff still hurts it but starts no fight; once it is home a Burning debuff that is still on it starts the fight again, so a burner it cannot see makes it leash and heal again every second until the debuff ends. The other debuffs a hit lists are only started on mobs: the ones that change the mob's own numbers (Shaken, Weakened and the like) do not change them yet, and slows, snares and CC (roots, stuns) are left off, so a mob does not slow down or stop for one. | None. | Open: the other debuffs, then slows, snares and CC (roots, stuns). |

@known-issues set Ranger / Air Strike
| **Ranger / Air Strike** | Air Strike hurts the enemies where its arrows land, but does not root (CC) one that a slow debuff is on (mobs do not slow down or stop for a slow, snare or CC yet). | None. | Open. |

@roadmap-achieved add
- Burning mobs: the Burning debuff takes a fire number from a mob every second for 8 seconds (up to 24), shows the fire and the Burning icon, follows it home after a reset and ends with its death or the burner's leaving.

@roadmap-in-progress set Reactions (first iteration)
- Reactions (first iteration): the game's reactions run for your own hits and for paying an ability's cost. The Bard's Resonance follows the game's rule (the Resonant Weapon passive from level 5) and its Menacing and Cathartic melodies add their damage at a combo finisher and spend up to 3 Resonance. The weapon talents that react at a finisher (the wand's Burning, Chilled and Volatile, the axes' Bleed, Snare and Wound, and the like) fire at the deadly finishing hit of the weapon in hand, the wand's Burning puts the Burning debuff on the mob, and the damage talents of the weapon trees count for its hits. The other classes' reactions run from the game's data and are not checked against the game yet.

@roadmap-left set Reactions: the Bard's Cheerful
- Reactions: the Bard's Cheerful, Pensive and Epic melodies at a finisher, the debuffs of its damage melodies, the other debuffs the weapon finishers put on a mob (the slows, bleeds and wounds), checking the other classes' finisher effects against the game, and the reactions that wait for you being hit or for other events.

@roadmap-milestones set 7
7. **7- Combat and abilities** — [In progress] Most classes' abilities, weapon combos and finishers, buffs, death and respawn. Mobs take the Burning debuff of a hit. Still to come: the other debuffs and CC on mobs (the weapon finisher talents' slows, bleeds and wounds among them) and Summoner pets.

@readme-roadmap set 7
| | 🛠️ | **7 · Combat and abilities** | Most classes' abilities, weapon combos and finishers, buffs, death and respawn. *Mobs take the Burning debuff of a hit. Still to come: the other debuffs and CC on mobs (the weapon finisher talents' slows, bleeds and wounds among them) and Summoner pets.* |
