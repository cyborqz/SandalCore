// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/content_db_fixture.h
//
//  A small content.db for worldd's tests (test_worldd_content_db, test_worldd_character_setup):
//  a SQLite file in a temp directory with the tables and columns the reader uses.  Their names are
//  services/internal/content/schema.go's (the one place contentc's DDL lives): meta(key, value),
//  class(guid, name, archetype_id), class_stat_base(class_guid, level, stat_guid, stat_name, value,
//  rounding_ambiguous).  Only those columns are created; the reader needs no others.  Plan A7 adds
//  ability_runtime, effect_runtime, effect_stat_runtime and stamina_rule with the real numbers contentc resolves
//  (DbOptions::ability_tables).
//
//  Contents: the Bard (the real class Guid, archetype 17747) at levels 1 (the capture's attributes but
//  Intelligence 39, as contentc computes them) and 2 (Strength 26.5, rounding-ambiguous), the Tank
//  (17754) at level 1 with made-up values, and the Cleric (17748) with five of its six attributes.
//  The stat Guids are the real ones (Strength 109183598614138 ... Mentality 109183598614143).
//
//  Plan B3 (contentc version 4) adds what the stat engine reads: stat_program holds the real stat program (stats_fixture.h takes it from
//  the shared corpus), and the eight classes, the equip slot names and the starting kits' items with their real stat blocks, quality curve and
//  tags (gear_fixture.h, generated from the real file) are there, so a PlayerWorld on the fixture computes the real stats of every class's
//  kit: they equal the corpus's `kit_<class>_l1` cases, and the Bard's kit gives the captured stats. The made-up items have no stat block.
// ============================================================================
#pragma once

#include "gear_fixture.h"
#include "stats/stat_program.h"
#include "stats_fixture.h"

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
// ~ Cyb - C | O | A ~

#ifndef TALENT_FIXTURE_PATH
#define TALENT_FIXTURE_PATH "server/worldd/tests/talent_fixture.sql"
#endif

namespace content_fixture {

namespace fs = std::filesystem;

inline constexpr int64_t kBard = 108640158570105ll;
inline constexpr int64_t kTank = 108640201692798ll;
inline constexpr int64_t kStrength = 109183598614138ll;

struct DbOptions {
    const char* schema_version = "1";
    bool        contentc_version = true;
    bool        source_sha = true;
    bool        class_table = true;
    bool        stat_table = true;
    /// Plan A7: ability_runtime, effect_runtime, effect_stat_runtime and stamina_rule, with the numbers contentc resolves
    /// from the real design database (what test_worldd_content_db and the abilities tests expect of both this fixture and a
    /// real content.db).
    bool        ability_tables = true;
    /// When set, the first `ability_from` in the ability_runtime INSERT is replaced by `ability_to`: a content.db whose
    /// rows say something else than contentc's (a Sprint that is not held ...).
    const char* ability_from = nullptr;
    const char* ability_to = "";
    /// The stamina_rule's stat_regen_time (seconds between regeneration ticks): contentc's is 1.
    const char* stat_regen_time = "1";
    /// Plan B2: the item and item_equip_slot tables with the items below.
    bool        item_tables = true;
    /// Plan B3: the stat program (the real one, or `stat_program_bytes` when set) and the tables of what an item adds to the stats.
    bool        stat_tables = true;
    const std::vector<uint8_t>* stat_program_bytes = nullptr;
    /// XORed into the stat_program row's crc32 (the header's own otherwise): a row that does not belong to its blob.
    int64_t     stat_program_crc_xor = 0;
    /// meta contentc.version (contentc's: 11 has the mounts, 5 has the item uses, reward tables, level curves and auto grants of plan D1).
    const char* contentc_version_value = "12";
    /// Plan D1 (contentc version 5): item_use, item_action, action_response, action_reward, reward_table, reward_item, reward_xp, leveling_type,
    /// leveling_curve, skill_auto_grant and skill_rank_stat_mod, with the real rows of the GM items below, the Adventuring curve and the
    /// Universal tree's grants.
    bool        progress_tables = true;
    /// Plan D1, stage 2 (still contentc version 5): skill_point_type, leveling_point, skill_tree, skill_node, skill_node_require,
    /// skill_node_exclusive, skill_rank, skill_rank_cost and item_weapon, with the real rows of talent_fixture.sql (tools/worldd/gen_talent_fixture.py):
    /// the class, Universal, weapon and gathering trees, and the starting kits' weapons. Needs progress_tables (skill_rank_stat_mod is theirs).
    bool        talent_tables = true;
};

/// Plan D1: the GM items of the fixture (real record Guids; the actions and tables of the 10 and gear scrolls are the real ones).
namespace gm {
inline constexpr int64_t kLevel10 = 6064634615524884604ll;      // GM_Scroll_MakeMeLevel10
inline constexpr int64_t kLevel15 = 6064630843430862909ll;
inline constexpr int64_t kLevel20 = 6064632139774033920ll;
inline constexpr int64_t kLevel25 = 6064632956008729280ll;
inline constexpr int64_t kClassBard = 6064632988571664384ll;    // GM_Scroll_PrimaryArchetypeBard
inline constexpr int64_t kClassTank = 6064632988571926530ll;
inline constexpr int64_t kGear01 = 6064634082466267136ll;       // GM_Scroll_Gear 01 (Light ) Starter: the fixture's grants the chest and the legs
inline constexpr int64_t kConsumables = 6064633408453149160ll;  // GM_Scroll_Consumables 06: 20 potions (Test_Potion stacks to 20)
inline constexpr int64_t kOdd = 6064631187700000020ll;          // Test_Odd_Scroll: an action of type Callout (refused)
inline constexpr int64_t kRandom = 6064631187700000021ll;       // Test_Random_Scroll: a table that is not modelled (refused)
inline constexpr int64_t kKeep = 6064631187700000022ll;         // Test_Keep_Scroll: 20 potions, not consumed
inline constexpr int64_t kAdventuringCurve = 6064630643828785675ll;
inline constexpr int64_t kXPAddMult = 6064630809541214212ll;
inline constexpr int64_t kUniversal = 6064631673604472832ll;
inline constexpr int64_t kSprint1Rank = 6064631719298203663ll, kStamBoost1Rank = 6064631719222247427ll, kGuard1Rank = 6064631719319896088ll,
                         kSprint2Rank = 6064631719299645456ll;
inline constexpr int64_t kSprint1Skill = 6064631719292305420ll, kStamBoost1Skill = 6064631719212023808ll, kGuard1Skill = 6064631719321862171ll,
                         kSprint2Skill = 6064631719293419533ll;
inline constexpr int64_t kStamBoost1Effect = 6064631743701843973ll, kSprint1Effect = 6064631744011698196ll;
}  // namespace gm

inline constexpr const char* kProgressStatements[] = {
    "INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots) VALUES "
    "(6064634615524884604, 'GM_Scroll_MakeMeLevel10', 1, 0, 0), (6064630843430862909, 'GM_Scroll_MakeMeLevel15', 1, 0, 0), "
    "(6064632139774033920, 'GM_Scroll_MakeMeLevel20', 1, 0, 0), (6064632956008729280, 'GM_Scroll_MakeMeLevel25', 1, 0, 0), "
    "(6064632988571664384, 'GM_Scroll_PrimaryArchetypeBard', 1, 0, 0), (6064632988571926530, 'GM_Scroll_PrimaryArchetypeTank', 1, 0, 0), "
    "(6064634082466267136, 'GM_Scroll_Gear 01 (Light ) Starter', 5, 0, 0), (6064633408453149160, 'GM_Scroll_Consumables 06', 5, 0, 0), "
    "(6064631187700000020, 'Test_Odd_Scroll', 1, 0, 0), (6064631187700000021, 'Test_Random_Scroll', 1, 0, 0), (6064631187700000022, 'Test_Keep_Scroll', 1, 0, 0)",
    "UPDATE item SET max_stack_size = 20 WHERE guid = 6064631187700000007",   // Test_Potion stacks to 20
    "UPDATE item SET max_durability = 11 WHERE guid IN (6064630214426886486, 6064630214427083132)",   // the chest and the legs
    "INSERT INTO item_use VALUES (6064634615524884604, 'ActivateActionResponses', 1, 0, 0), (6064630843430862909, 'ActivateActionResponses', 1, 0, 0), "
    "(6064632139774033920, 'ActivateActionResponses', 1, 0, 0), (6064632956008729280, 'ActivateActionResponses', 1, 0, 0), "
    "(6064632988571664384, 'ActivateActionResponses', 1, 0, 0), (6064632988571926530, 'ActivateActionResponses', 1, 0, 0), "
    "(6064634082466267136, 'ActivateActionResponses', 1, 0, 0), (6064633408453149160, 'ActivateActionResponses', 1, 0, 0), "
    "(6064631187700000020, 'ActivateActionResponses', 1, 0, 0), (6064631187700000021, 'ActivateActionResponses', 1, 0, 0), "
    "(6064631187700000022, 'ActivateActionResponses', 0, 0, 0)",
    "INSERT INTO item_action VALUES (6064634615524884604, 0, 6064634615529013375), (6064630843430862909, 0, 108329676396190), "
    "(6064632139774033920, 0, 6064632139805425666), (6064632956008729280, 0, 6064632956017773249), (6064632988571664384, 0, 6064632988321120266), "
    "(6064632988571926530, 0, 6064632988317974535), (6064634082466267136, 0, 6064634082573549569), (6064633408453149160, 0, 6064633408454328809), "
    "(6064631187700000020, 0, 7100000000000000020), (6064631187700000021, 0, 7100000000000000021), (6064631187700000022, 0, 6064633408454328809)",
    "INSERT INTO action_response VALUES (6064634615529013375, 'GM_Scroll_Level10Reward', 'GiveItem', 0, '', 1), (108329676396190, 'GM_Scroll_Level15Reward', 'GiveItem', 0, '', 1), "
    "(6064632139805425666, 'GM_Scroll_Level20Reward', 'GiveItem', 0, '', 1), (6064632956017773249, 'GM_Scroll_Level25Reward', 'GiveItem', 0, '', 1), "
    "(6064632988321120266, 'SetPrimaryArchetypeBard', 'SetPrimaryArchetype', 108640158570105, 'Bard', 1), "
    "(6064632988317974535, 'SetPrimaryArchetypeTank', 'SetPrimaryArchetype', 108640201692798, 'Tank', 1), "
    "(6064634082573549569, 'GM_Scroll_Gear 01 (Light ) Starter', 'GiveItem', 0, '', 1), (6064633408454328809, 'GM_Scroll_Consumables 06', 'GiveItem', 0, '', 1), "
    "(7100000000000000020, 'Test_Callout', 'Callout', 0, '', 1), (7100000000000000021, 'Test_Random', 'GiveItem', 0, '', 1)",
    "INSERT INTO action_reward VALUES (6064634615529013375, 0, 6064634615529865344), (108329676396190, 0, 6064630843443445822), "
    "(6064632139805425666, 0, 6064632139788779521), (6064632956017773249, 0, 6064632956018035394), (6064634082573549569, 0, 6064634082574270466), "
    "(6064633408454328809, 0, 6064633408454984170), (7100000000000000021, 0, 7100000000000000031)",
    "INSERT INTO reward_table VALUES (6064634615529865344, 'GM_Scroll_Level10Reward', 'GetPlayerLevel() < 10', 10, 1), "
    "(6064630843443445822, 'GM_Scroll_Level15Reward', 'GetPlayerLevel() < 15', 15, 1), (6064632139788779521, 'GM_Scroll_Level20Reward', 'GetPlayerLevel() < 20', 20, 1), "
    "(6064632956018035394, 'GM_Scroll_Level25Reward', '', 0, 1), (6064634082574270466, 'GM_Scroll_Gear 01 (Light ) Starter', '', 0, 1), "
    "(6064633408454984170, 'GM_Scroll_Consumables 06', '', 0, 1), (7100000000000000031, 'Test_Random', 'IsNight()', -1, 0)",
    "INSERT INTO reward_item VALUES (6064634082574270466, 0, 6064630214426886486, 6064629086629396481, 1), (6064634082574270466, 1, 6064630214427083132, 6064629086629396481, 1), "
    "(6064633408454984170, 0, 6064631187700000007, 6064629086629396481, 20)",
    "INSERT INTO reward_xp VALUES "
    "(6064634615529865344, 0, 'Adventuring', 10, 10, 6064630643828785675, 6064630809541214212, '(formula)'), (6064634615529865344, 1, 'Weapon', 0, 0, 6064633413997300497, 0, 'xp - cur + 10'), "
    "(6064630843443445822, 0, 'Adventuring', 15, 10, 6064630643828785675, 6064630809541214212, '(formula)'), (6064630843443445822, 1, 'Weapon', 0, 0, 6064633413997300497, 0, 'xp - cur + 10'), "
    "(6064632139788779521, 0, 'Adventuring', 20, 10, 6064630643828785675, 6064630809541214212, '(formula)'), (6064632139788779521, 1, 'Weapon', 0, 0, 6064633413997300497, 0, 'xp - cur + 10'), "
    "(6064632956018035394, 0, 'Adventuring', 25, 10, 6064630643828785675, 6064630809541214212, '(formula)'), (6064632956018035394, 1, 'Weapon', 0, 0, 6064633413997300497, 0, 'xp - cur + 10')",
    "INSERT INTO leveling_type VALUES (108651085518458, 'Adventuring', 'Adventuring', 25, 1, 6064630809541214212), "
    "(6064632150851584017, 'Weapon_1H_Scepter', 'Weapon', 0, 0, 0)",
    "INSERT INTO leveling_curve VALUES (108651085518458, 1, 0), (108651085518458, 2, 300), (108651085518458, 3, 4000), (108651085518458, 4, 11500), "
    "(108651085518458, 5, 29900), (108651085518458, 6, 56500), (108651085518458, 7, 98400), (108651085518458, 8, 160200), (108651085518458, 9, 246600), "
    "(108651085518458, 10, 418400), (108651085518458, 11, 577200), (108651085518458, 12, 788400), (108651085518458, 13, 1063000), (108651085518458, 14, 1413400), "
    "(108651085518458, 15, 1853000), (108651085518458, 16, 2396700), (108651085518458, 17, 3060800), (108651085518458, 18, 3862900), (108651085518458, 19, 4822000), "
    "(108651085518458, 20, 6146400), (108651085518458, 21, 7439800), (108651085518458, 22, 8853900), (108651085518458, 23, 10389200), (108651085518458, 24, 12045300), "
    "(108651085518458, 25, 14295400), (108651085518458, 26, 16563400), (6064632150851584017, 1, 0), (6064632150851584017, 2, 400)",
    "INSERT INTO skill_auto_grant VALUES "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719303577618, 'General_Stamina_Passive_Sprint_1', 5, 6064631719292305420, 'General_Stamina_Passive_Sprint_1', 6064631719298203663, 6064631744011698196), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719278477321, 'General_Stamina_Passive_StamBoost_1', 10, 6064631719212023808, 'General_Stamina_Passive_StamBoost_1', 6064631719222247427, 6064631743701843973), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719324614686, 'General_Stamina_Passive_Guard_1', 15, 6064631719321862171, 'General_Stamina_Passive_Guard_1', 6064631719319896088, 6064631744006193170), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719304691731, 'General_Stamina_Passive_Sprint_2', 20, 6064631719293419533, 'General_Stamina_Passive_Sprint_2', 6064631719299645456, 6064631744013991957), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719281557514, 'General_Stamina_Passive_StamBoost_2', 30, 6064631719221002241, 'General_Stamina_Passive_StamBoost_2', 6064631719224803332, 6064631743728189446), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719305543700, 'General_Stamina_Passive_Sprint_3', 35, 6064631719294205966, 'General_Stamina_Passive_Sprint_3', 6064631719299973137, 6064631744014712854), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719325794335, 'General_Stamina_Passive_Guard_2', 45, 6064631719322779676, 'General_Stamina_Passive_Guard_2', 6064631719320485913, 6064631744008945683), "
    "(6064631673604472832, 'Universal', 108651085518458, 6064631719282933771, 'General_Stamina_Passive_StamBoost_3', 50, 6064631719221395458, 'General_Stamina_Passive_StamBoost_3', 6064631719225196549, 6064631743747325959), "
    "(6064630422842835238, 'Rogue', 108651085518458, 6064632989524433929, 'Rogue_Passive_Cunning', 10, 6064632989524000001, 'Rogue_Passive_Cunning', 6064632989524000002, 0)",
    "INSERT INTO skill_rank_stat_mod VALUES (6064631719222247427, 0, 6064631747701440948, 'MaxStamina', 10), (6064631719224803332, 0, 6064631747701440948, 'MaxStamina', 10)",
};
// -- C.Y.B & C-O-A --

/// The items of the fixture (plan B2): the captured Bard's 13 starter items by their real record Guids, and the others the
/// equip tests need. Slots are EEquipSlot values: Helmet 1, Chest 2, Legs 5, Feet 6, RingRight 10, RingLeft 11, EarRight 13, EarLeft 14,
/// Primary 15, Secondary 16, Ranged 17, RangedOffHand 18, ResourceBag_0..2 23-25, Pickaxe 26, Axe 29, Sickle 33, FishingRod 35, Bow 38.
namespace item {
inline constexpr int64_t kHelmet = 6064630736269279236ll;      // Gear_Armor_Light_Bard_Head
inline constexpr int64_t kChest = 6064630214426886486ll;       // Gear_Armor_Light_Starter_Chest (CanEquip is 0 in the design data)
inline constexpr int64_t kLegs = 6064630214427083132ll;
inline constexpr int64_t kFeet = 6064630214427279778ll;
inline constexpr int64_t kScepter = 6064631131688402948ll;     // Gear_Weapon_Scepter_1H_Starter: Primary
inline constexpr int64_t kFocus = 6064630695486029824ll;       // Gear_Weapon_Focus_OH_Starter: Secondary or RangedOffHand
inline constexpr int64_t kBook = 6064631670974054529ll;        // Gear_Weapon_Book_2H_Starter: Ranged and RangedOffHand, two-handed
inline constexpr int64_t kBag = 6064629382556287782ll;         // Bag_Resource_Sanctus
inline constexpr int64_t kPickaxe = 6064634744584208707ll;
inline constexpr int64_t kAxe = 6064634744581914946ll;
inline constexpr int64_t kSickle = 6064634744578441536ll;
inline constexpr int64_t kFishingRod = 6064634744576540991ll;
inline constexpr int64_t kBow = 6064634744579883329ll;
// made-up records for the equip tests
inline constexpr int64_t kSword2H = 6064630695616315423ll;     // Primary and Secondary, two-handed (the real Gear_Weapon_Sword_2H_Starter)
inline constexpr int64_t kMace1H = 6064631187700000001ll;      // Primary
inline constexpr int64_t kShield = 6064630695512702988ll;      // Secondary only
inline constexpr int64_t kHeavyChest = 6064631187700000002ll;  // Chest
inline constexpr int64_t kRingA = 6064631187700000003ll;       // RingRight, RingLeft
inline constexpr int64_t kRingB = 6064631187700000004ll;
inline constexpr int64_t kEarLeftFirst = 6064631187700000005ll;   // EarLeft, EarRight (the record's order decides the first free slot)
inline constexpr int64_t kEarRightFirst = 6064631187700000006ll;  // EarRight, EarLeft
inline constexpr int64_t kPotion = 6064631187700000007ll;      // not worn
inline constexpr int64_t kGatherCape = 6064631187700000008ll;  // Back
inline constexpr int64_t kCostume = 6064631187700000009ll;     // Costume 71
inline constexpr int64_t kCloak = 6064631187700000010ll;       // CloakCosmetic 79
inline constexpr int64_t kShirt = 6064631187700000011ll;       // Shirt 19 (worn, but not shown in AppearanceIDs)
inline constexpr int64_t kSizeRod = 6064631187700000012ll;     // FishingRod 35: Power +100 (replication 1) and Size_Small +7 (replication 0, held by no array)
inline constexpr int64_t kCursedEarring = 6064631187700000013ll;   // EarRight 13: Hook_Rate +1e39, which no float32 holds and the stat's clamp (100) hides from its value
}  // namespace item

inline constexpr const char* kItemInsert =
    "INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots) VALUES "
    "(6064630736269279236, 'Gear_Armor_Light_Bard_Head', 1, 1, 0), (6064630214426886486, 'Gear_Armor_Light_Starter_Chest', 1, 0, 0), "
    "(6064630214427083132, 'Gear_Armor_Light_Starter_Legs', 1, 0, 0), (6064630214427279778, 'Gear_Armor_Light_Starter_Feet', 1, 0, 0), "
    "(6064631131688402948, 'Gear_Weapon_Scepter_1H_Starter', 1, 1, 0), (6064630695486029824, 'Gear_Weapon_Focus_OH_Starter', 1, 1, 0), "
    "(6064631670974054529, 'Gear_Weapon_Book_2H_Starter', 1, 1, 1), (6064629382556287782, 'Bag_Resource_Sanctus', 1, 1, 0), "
    "(6064634744584208707, 'Gear_Artisan_Mining_Tool_Pickaxe_T1_0_FTUE', 5, 1, 0), (6064634744581914946, 'Gear_Artisan_Lumberjacking_Tool_Axe_T1_0_FTUE', 5, 1, 0), "
    "(6064634744578441536, 'Gear_Artisan_Herbalism_Tool_Sickle_T1_0_FTUE', 5, 1, 0), (6064634744576540991, 'Gear_Artisan_Fishing_Tool_Pole_T1_0_FTUE', 5, 1, 0), "
    "(6064634744579883329, 'Gear_Artisan_Hunting_Tool_Bow_T1_0_FTUE', 5, 1, 0), "
    "(6064630695616315423, 'Gear_Weapon_Sword_2H_Starter', 1, 1, 1), (6064631187700000001, 'Test_Mace_1H', 1, 1, 0), "
    "(6064630695512702988, 'Gear_Weapon_Shield_OH_Starter', 1, 1, 0), (6064631187700000002, 'Test_Heavy_Chest', 10, 1, 0), "
    "(6064631187700000003, 'Test_Ring_A', 5, 1, 0), (6064631187700000004, 'Test_Ring_B', 5, 1, 0), "
    "(6064631187700000005, 'Test_Earring_LeftFirst', 5, 1, 0), (6064631187700000006, 'Test_Earring_RightFirst', 5, 1, 0), "
    "(6064631187700000007, 'Test_Potion', 1, 0, 0), (6064631187700000008, 'Test_Cape', 5, 1, 0), "
    "(6064631187700000009, 'Test_Costume', 1, 1, 0), (6064631187700000010, 'Test_Cloak', 1, 1, 0), (6064631187700000011, 'Test_Shirt', 1, 1, 0), "
    "(6064631187700000012, 'Test_Size_Rod', 5, 1, 0), (6064631187700000013, 'Test_Cursed_Earring', 5, 1, 0)";
inline constexpr const char* kItemSlotInsert =
    "INSERT INTO item_equip_slot VALUES "
    "(6064630736269279236, 0, 1), (6064630214426886486, 0, 2), (6064630214427083132, 0, 5), (6064630214427279778, 0, 6), "
    "(6064631131688402948, 0, 15), (6064630695486029824, 0, 16), (6064630695486029824, 1, 18), (6064631670974054529, 0, 17), "
    "(6064631670974054529, 1, 18), (6064629382556287782, 0, 23), (6064629382556287782, 1, 24), (6064629382556287782, 2, 25), "
    "(6064634744584208707, 0, 26), (6064634744581914946, 0, 29), (6064634744578441536, 0, 33), (6064634744576540991, 0, 35), "
    "(6064634744579883329, 0, 38), (6064630695616315423, 0, 15), (6064630695616315423, 1, 16), (6064631187700000001, 0, 15), "
    "(6064630695512702988, 0, 16), (6064631187700000002, 0, 2), (6064631187700000003, 0, 10), (6064631187700000003, 1, 11), "
    "(6064631187700000004, 0, 10), (6064631187700000004, 1, 11), (6064631187700000005, 0, 14), (6064631187700000005, 1, 13), "
    "(6064631187700000006, 0, 13), (6064631187700000006, 1, 14), (6064631187700000008, 0, 8), (6064631187700000009, 0, 71), "
    "(6064631187700000010, 0, 79), (6064631187700000011, 0, 19), (6064631187700000012, 0, 35), (6064631187700000013, 0, 13)";

/// Plan B3: stat blocks for three made-up items, so that the equip tests see stats move: the heavy chest gives Constitution +100 and Mentality +100
/// (MaxHealth and MaxMana, and so what a clamp of the current Health and Mana needs), ring A MaxStamina +50 and the Mace Strength +10 at
/// every rarity; all on the real default quality curve. Three more serve the review's tests: the two earrings give Hook_Rate (a stat that
/// replicates base and equipment and is clamped to 100) +150 and +200, so that wearing the second changes the equipment contribution and
/// not the value (350 against 150, both clamped to 100), and the shirt gives Constitution +1e39, which no float32 holds: the stat program
/// faults for a character who wears it. The other made-up items have no stat data.
inline constexpr int64_t kDefaultCurve = 109166871729784ll;
inline constexpr int64_t kConstitution = 109183598614140ll;
inline constexpr int64_t kMentality = 109183598614143ll;
inline constexpr int64_t kHookRate = 6064634285519601665ll;   // Artisan_Gathering_Fishing_Sport_Hook_Rate: bucketed, ValueMax 100
inline constexpr int64_t kMaxStamina = 6064631747701440948ll;
inline constexpr int64_t kSizeSmall = 6064634297636421659ll;   // Artisan_Gathering_Fishing_Sport_Size_Small: replication 0 (no array holds it), ValueMax 100
inline constexpr int64_t kFishingPower = 6064634285492994048ll;   // Artisan_Gathering_Fishing_Sport_Power: Owner array, bucketed
inline constexpr const char* kTestGearStatements[] = {
    "INSERT INTO stat_block_stat VALUES (7000000000000000001, 109183598614140, 100,100, 100,100, 100,100, 100,100, 100,100, 100,100, 100,100)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000001, 109183598614143, 100,100, 100,100, 100,100, 100,100, 100,100, 100,100, 100,100)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000002, 6064631747701440948, 50,50, 50,50, 50,50, 50,50, 50,50, 50,50, 50,50)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000003, 109183598614138, 10,10, 10,10, 10,10, 10,10, 10,10, 10,10, 10,10)",
    "UPDATE item SET stat_block_guid = 7000000000000000001, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000002",
    "UPDATE item SET stat_block_guid = 7000000000000000002, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000003",
    "UPDATE item SET stat_block_guid = 7000000000000000003, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000001",
    "INSERT INTO stat_block_stat VALUES (7000000000000000004, 6064634285519601665, 150,150, 150,150, 150,150, 150,150, 150,150, 150,150, 150,150)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000005, 6064634285519601665, 200,200, 200,200, 200,200, 200,200, 200,200, 200,200, 200,200)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000006, 109183598614140, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39)",
    "UPDATE item SET stat_block_guid = 7000000000000000004, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000005",
    "UPDATE item SET stat_block_guid = 7000000000000000005, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000006",
    "UPDATE item SET stat_block_guid = 7000000000000000006, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000011",
    // review fix (S03): stats whose replication is 0 move with gear and no array holds them (the real rods give Size_Small, Type_Fish01 ...).
    // The size rod gives one of them next to Power, ring B only one of them: wearing it changes a stat that is nobody's to send.
    "INSERT INTO stat_block_stat VALUES (7000000000000000007, 6064634285492994048, 100,100, 100,100, 100,100, 100,100, 100,100, 100,100, 100,100)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000007, 6064634297636421659, 7,7, 7,7, 7,7, 7,7, 7,7, 7,7, 7,7)",
    "INSERT INTO stat_block_stat VALUES (7000000000000000008, 6064634297636421659, 7,7, 7,7, 7,7, 7,7, 7,7, 7,7, 7,7)",
    "UPDATE item SET stat_block_guid = 7000000000000000007, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000012",
    "UPDATE item SET stat_block_guid = 7000000000000000008, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000004",
    // review fix (I1): an equipment sum beyond a float32 behind a clamp: Hook_Rate is held to 100, so its VALUE is finite and only the equipment is not.
    "INSERT INTO stat_block_stat VALUES (7000000000000000009, 6064634285519601665, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39, 1e39,1e39)",
    "UPDATE item SET stat_block_guid = 7000000000000000009, quality_curve_guid = 109166871729784 WHERE guid = 6064631187700000013",
};

/// The A7 tables' contents, one INSERT each; the ids are the real record Guids (known.go).
inline constexpr const char* kAbilityRuntimeInsert =
    "INSERT INTO ability_runtime VALUES "
    // Sprint: held, needs 1 Stamina, 1.25 per 0.25 s while moving, no cooldown
    "(6064631641648332884, 'General_Sprint', 'Smart', 0, 1.0, 6064629575339934476, 1.25, 1, 0.25, 0.0, 0, 0, 0, 6064631641826394197, "
    "'/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage.CM_M_ABLT_Sprint_Montage', 'End'), "
    // Dodge: 50 once, a 0.65 s cooldown the paid cost starts
    "(107258680465016, 'General_Dodge', 'Naive', 0, 0.0, 6064629575339934476, 50.0, 0, 0.0, 0.65, 1, 1, 0, 106223481149050, "
    "'/Game/Characters/Animations/00_ABLT/Rogue/CM_M_ABLT_RogueDash_Fwd_Montage.CM_M_ABLT_RogueDash_Fwd_Montage', ''), "
    // Rest: a free toggle, out of combat, on the caster
    "(109393746155128, 'General_Rest', 'Toggle', 1, 0.0, 0, 0.0, 0, 0.0, 0.0, 0, 0, 1, 109394092054139, "
    "'/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage.CM_M_ABLT_General_Rest_Montage', 'SitEnd')";
inline constexpr const char* kEffectRuntimeInsert =
    "INSERT INTO effect_runtime VALUES "
    "(6064631641826394197, 'General_Sprint', 1, 0.0, 1.0, 0, 1, 6064631668308443138, 1.0), "
    "(106223481149050, 'General_Dodge_Evasion', 0, 0.5, 0.0, 1, 1, 6064631668308443138, 1.0), "
    "(109394092054139, 'General_Rest', 1, 0.0, 0.0, 0, 0, 0, 0.0), "
    "(6064631668308443138, 'General_Stamina_DisableRegen', 0, 1.0, 0.0, 1, 1, 0, 0.0)";
inline constexpr const char* kEffectStatInsert =
    "INSERT INTO effect_stat_runtime VALUES "
    "(106223481149050, 0, 109343399171812, 0, 0.0, 1.0), (106223481149050, 1, 109343399171802, 0, 0.0, 1.0), "
    "(109394092054139, 0, 109343399171806, 108986356618873, 0.01, 1.0), (109394092054139, 1, 109343399171796, 109183578756729, 0.01, 1.0), "
    "(6064631641826394197, 0, 109228544644729, 0, 0.0, 0.2)";

/// Runs `sql`; false (after printing why) when SQLite refuses it.
inline bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::printf("  [FAIL] test setup: %s (%s)\n", err ? err : "?", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}
// == C_Y_B | C O A ==

/// Writes the small content.db at `path`; false when it could not be written.
inline bool make_db(const fs::path& path, const DbOptions& o = {}) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::printf("  [FAIL] test setup: cannot create %s\n", path.string().c_str());
        if (db) sqlite3_close(db);
        return false;
    }
    bool ok = exec(db, "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
    ok = exec(db, (std::string("INSERT INTO meta VALUES ('schema_version', '") + o.schema_version + "')").c_str()) && ok;
    if (o.contentc_version) ok = exec(db, (std::string("INSERT INTO meta VALUES ('contentc.version', '") + o.contentc_version_value + "')").c_str()) && ok;
    if (o.source_sha) {
        ok = exec(db, "INSERT INTO meta VALUES ('source.dbc.sha256', 'a2a3b82715732603e37a5cffcd422691f4402133cfdd0f231c7eb31025114a23')") && ok;
    }
    ok = exec(db, "INSERT INTO meta VALUES ('built_at', '2026-10-01T05:28:49Z')") && ok;
    if (o.class_table) {
        ok = exec(db, "CREATE TABLE class (guid INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, archetype_id INTEGER NOT NULL UNIQUE)") && ok;
        ok = exec(db, "INSERT INTO class VALUES (108640158570105, 'Bard', 17747), (108640161388154, 'Cleric', 17748), (108640201692798, 'Tank', 17754)") && ok;
    }
    if (o.stat_table) {
        ok = exec(db, "CREATE TABLE class_stat_base (class_guid INTEGER NOT NULL, level INTEGER NOT NULL, stat_guid INTEGER NOT NULL, "
                      "stat_name TEXT NOT NULL, value REAL NOT NULL, rounding_ambiguous INTEGER NOT NULL, "
                      "PRIMARY KEY (class_guid, level, stat_guid)) WITHOUT ROWID") && ok;
        // Strength 109183598614138, Dexterity ..139, Constitution ..140, Intelligence ..141, Wisdom ..142, Mentality ..143
        ok = exec(db, "INSERT INTO class_stat_base VALUES "
                      "(108640158570105, 1, 109183598614143, 'Mentality', 30, 0), (108640158570105, 1, 109183598614138, 'Strength', 24, 0), "
                      "(108640158570105, 1, 109183598614139, 'Dexterity', 24, 0), (108640158570105, 1, 109183598614140, 'Constitution', 30, 0), "
                      "(108640158570105, 1, 109183598614141, 'Intelligence', 39, 0), (108640158570105, 1, 109183598614142, 'Wisdom', 36, 0), "
                      "(108640158570105, 2, 109183598614138, 'Strength', 26.5, 1), (108640158570105, 2, 109183598614139, 'Dexterity', 25, 0), "
                      "(108640158570105, 2, 109183598614140, 'Constitution', 31, 0), (108640158570105, 2, 109183598614141, 'Intelligence', 41, 0), "
                      "(108640158570105, 2, 109183598614142, 'Wisdom', 37, 0), (108640158570105, 2, 109183598614143, 'Mentality', 31, 0), "
                      "(108640201692798, 1, 109183598614138, 'Strength', 40, 0), (108640201692798, 1, 109183598614139, 'Dexterity', 20, 0), "
                      "(108640201692798, 1, 109183598614140, 'Constitution', 45, 0), (108640201692798, 1, 109183598614141, 'Intelligence', 10, 0), "
                      "(108640201692798, 1, 109183598614142, 'Wisdom', 15, 0), (108640201692798, 1, 109183598614143, 'Mentality', 12, 0), "
                      "(108640161388154, 1, 109183598614138, 'Strength', 18, 0), (108640161388154, 1, 109183598614139, 'Dexterity', 19, 0), "
                      "(108640161388154, 1, 109183598614140, 'Constitution', 28, 0), (108640161388154, 1, 109183598614141, 'Intelligence', 30, 0), "
                      "(108640161388154, 1, 109183598614142, 'Wisdom', 44, 0)") && ok;
    }
    if (o.ability_tables) {
        ok = exec(db, "CREATE TABLE ability_runtime (ability_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, held_behavior TEXT NOT NULL, "
                      "requires_out_of_combat INTEGER NOT NULL, min_stamina REAL NOT NULL, cost_stat_guid INTEGER NOT NULL, "
                      "cost_amount REAL NOT NULL, cost_requires_moving INTEGER NOT NULL, charge_interval REAL NOT NULL, "
                      "cooldown REAL NOT NULL, triggers_cooldown INTEGER NOT NULL, disable_during_cooldown INTEGER NOT NULL, "
                      "targets_self INTEGER NOT NULL, effect_guid INTEGER NOT NULL, montage_path TEXT NOT NULL, cancel_section TEXT NOT NULL)") && ok;
        std::string ability_insert = kAbilityRuntimeInsert;
        if (o.ability_from) {
            const size_t at = ability_insert.find(o.ability_from);
            if (at == std::string::npos) ok = false;   // the test names a row the fixture does not have
            else ability_insert.replace(at, std::string(o.ability_from).size(), o.ability_to);
        }
        ok = exec(db, ability_insert.c_str()) && ok;
        ok = exec(db, "CREATE TABLE effect_runtime (effect_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, infinite INTEGER NOT NULL, "
                      "duration REAL NOT NULL, tick_timer REAL NOT NULL, stops_regen_standing INTEGER NOT NULL, "
                      "stops_regen_moving INTEGER NOT NULL, end_effect_guid INTEGER NOT NULL, end_effect_duration REAL NOT NULL)") && ok;
        ok = exec(db, kEffectRuntimeInsert) && ok;
        ok = exec(db, "CREATE TABLE effect_stat_runtime (effect_guid INTEGER NOT NULL, ord INTEGER NOT NULL, stat_guid INTEGER NOT NULL, "
                      "source_stat_guid INTEGER NOT NULL, factor REAL NOT NULL, addend REAL NOT NULL, "
                      "PRIMARY KEY (effect_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, kEffectStatInsert) && ok;
        ok = exec(db, "CREATE TABLE stamina_rule (key TEXT PRIMARY KEY, value TEXT NOT NULL, source TEXT NOT NULL) WITHOUT ROWID") && ok;
        ok = exec(db, (std::string("INSERT INTO stamina_rule VALUES ('stat_regen_time', '") + o.stat_regen_time +
                       "', 'DefaultCombat.ini'), ('regen.rate_enabled', '5', 'the formula run')").c_str()) && ok;
    }
    if (o.item_tables) {
        ok = exec(db, "CREATE TABLE item (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, level INTEGER NOT NULL, can_equip INTEGER NOT NULL, "
                      "occupies_all_equip_slots INTEGER NOT NULL, default_quality INTEGER NOT NULL DEFAULT 1, max_stack_size INTEGER NOT NULL DEFAULT 1, "
                      "max_durability REAL NOT NULL DEFAULT 0, stat_block_guid INTEGER NOT NULL DEFAULT 0, quality_curve_guid INTEGER NOT NULL DEFAULT 0)") && ok;
        ok = exec(db, kItemInsert) && ok;
        ok = exec(db, "CREATE TABLE item_equip_slot (item_guid INTEGER NOT NULL, ord INTEGER NOT NULL, slot INTEGER NOT NULL, "
                      "PRIMARY KEY (item_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, kItemSlotInsert) && ok;
    }
    if (o.stat_tables) {
        ok = exec(db, "CREATE TABLE equip_slot (slot INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)") && ok;
        ok = exec(db, "CREATE TABLE quality_curve (guid INTEGER NOT NULL, ord INTEGER NOT NULL, x REAL NOT NULL, y REAL NOT NULL, "
                      "PRIMARY KEY (guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE stat_block_stat (block_guid INTEGER NOT NULL, stat_guid INTEGER NOT NULL, "
                      "common_min REAL NOT NULL, common_max REAL NOT NULL, uncommon_min REAL NOT NULL, uncommon_max REAL NOT NULL, "
                      "rare_min REAL NOT NULL, rare_max REAL NOT NULL, heroic_min REAL NOT NULL, heroic_max REAL NOT NULL, "
                      "epic_min REAL NOT NULL, epic_max REAL NOT NULL, legendary_min REAL NOT NULL, legendary_max REAL NOT NULL, "
                      "artifact_min REAL NOT NULL, artifact_max REAL NOT NULL, PRIMARY KEY (block_guid, stat_guid)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE item_set (item_guid INTEGER NOT NULL, ord INTEGER NOT NULL, set_guid INTEGER NOT NULL, "
                      "PRIMARY KEY (item_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE set_bonus (set_guid INTEGER NOT NULL, tier_ord INTEGER NOT NULL, pieces INTEGER NOT NULL, "
                      "bonus_ord INTEGER NOT NULL, stat_guid INTEGER NOT NULL, common REAL, uncommon REAL, rare REAL, heroic REAL, "
                      "epic REAL, legendary REAL, artifact REAL, PRIMARY KEY (set_guid, tier_ord, bonus_ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE item_tag (item_guid INTEGER NOT NULL, ord INTEGER NOT NULL, tag TEXT NOT NULL, "
                      "PRIMARY KEY (item_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE stat_program (id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER NOT NULL, mode TEXT NOT NULL, "
                      "stats INTEGER NOT NULL, classes INTEGER NOT NULL, crc32 INTEGER NOT NULL, data BLOB NOT NULL)") && ok;
        if (o.item_tables && o.class_table) {
            for (const char* sql : gear_fixture::kStatements) ok = exec(db, sql) && ok;
            for (const char* sql : kTestGearStatements) ok = exec(db, sql) && ok;
        }
        const std::vector<uint8_t>& bytes = o.stat_program_bytes ? *o.stat_program_bytes : stats_fixture::main_bundle();
        if (!bytes.empty()) {
            sqlite3_stmt* st = nullptr;
            // the row's counts are the bundle's own (a bundle that does not parse, a test's hostile one, gets 0)
            aoc::worldd::stats::StatBundle parsed;
            std::string parse_error;
            const bool parses = aoc::worldd::stats::parse_bundle(bytes, parsed, parse_error);
            const std::string row_sql = "INSERT INTO stat_program VALUES (1, " + std::to_string(aoc::worldd::stats::kBundleVersion) + ", 'float32', " +
                                        std::to_string(parses ? parsed.stats.size() : 0) + ", " + std::to_string(parses ? parsed.classes.size() : 0) + ", ?1, ?2)";
            ok = sqlite3_prepare_v2(db, row_sql.c_str(), -1, &st, nullptr) == SQLITE_OK && ok;
            if (st) {
                const int64_t crc = bytes.size() >= 16 ? static_cast<int64_t>(bytes[12]) | (static_cast<int64_t>(bytes[13]) << 8) |
                                                              (static_cast<int64_t>(bytes[14]) << 16) | (static_cast<int64_t>(bytes[15]) << 24)
                                                        : 0;
                sqlite3_bind_int64(st, 1, crc ^ o.stat_program_crc_xor);
                sqlite3_bind_blob(st, 2, bytes.data(), static_cast<int>(bytes.size()), SQLITE_STATIC);
                ok = sqlite3_step(st) == SQLITE_DONE && ok;
                sqlite3_finalize(st);
            }
        }
    }
    if (o.progress_tables) {
        ok = exec(db, "CREATE TABLE item_use (item_guid INTEGER PRIMARY KEY, activation TEXT NOT NULL, consume_on_use INTEGER NOT NULL, cooldown REAL NOT NULL, "
                      "disabled_in_combat INTEGER NOT NULL)") && ok;
        ok = exec(db, "CREATE TABLE item_action (item_guid INTEGER NOT NULL, ord INTEGER NOT NULL, action_guid INTEGER NOT NULL, PRIMARY KEY (item_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE action_response (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, response_type TEXT NOT NULL, class_guid INTEGER NOT NULL, "
                      "class_name TEXT NOT NULL, remove_up_to_quantity INTEGER NOT NULL)") && ok;
        ok = exec(db, "CREATE TABLE action_reward (action_guid INTEGER NOT NULL, ord INTEGER NOT NULL, reward_table_guid INTEGER NOT NULL, PRIMARY KEY (action_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE reward_table (guid INTEGER PRIMARY KEY, name TEXT NOT NULL, predicate TEXT NOT NULL, level_below INTEGER NOT NULL, modelled INTEGER NOT NULL)") && ok;
        ok = exec(db, "CREATE TABLE reward_item (table_guid INTEGER NOT NULL, ord INTEGER NOT NULL, item_guid INTEGER NOT NULL, variation_guid INTEGER NOT NULL, "
                      "quantity INTEGER NOT NULL, PRIMARY KEY (table_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE reward_xp (table_guid INTEGER NOT NULL, ord INTEGER NOT NULL, experience_type TEXT NOT NULL, target_level INTEGER NOT NULL, "
                      "margin REAL NOT NULL, curve_guid INTEGER NOT NULL, mult_stat_guid INTEGER NOT NULL, expression TEXT NOT NULL, PRIMARY KEY (table_guid, ord)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE leveling_type (guid INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, experience_type TEXT NOT NULL, level_cap INTEGER NOT NULL, "
                      "uses_debt INTEGER NOT NULL, xp_mult_stat_guid INTEGER NOT NULL)") && ok;
        ok = exec(db, "CREATE TABLE leveling_curve (type_guid INTEGER NOT NULL, level INTEGER NOT NULL, xp INTEGER NOT NULL, PRIMARY KEY (type_guid, level)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE skill_auto_grant (tree_guid INTEGER NOT NULL, tree_name TEXT NOT NULL, leveling_type_guid INTEGER NOT NULL, node_guid INTEGER NOT NULL, "
                      "node_name TEXT NOT NULL, level_requirement INTEGER NOT NULL, skill_guid INTEGER NOT NULL, skill_name TEXT NOT NULL, rank_guid INTEGER NOT NULL, "
                      "effect_guid INTEGER NOT NULL, PRIMARY KEY (tree_guid, node_guid)) WITHOUT ROWID") && ok;
        ok = exec(db, "CREATE TABLE skill_rank_stat_mod (rank_guid INTEGER NOT NULL, ord INTEGER NOT NULL, stat_guid INTEGER NOT NULL, stat_name TEXT NOT NULL, "
                      "value REAL NOT NULL, PRIMARY KEY (rank_guid, ord)) WITHOUT ROWID") && ok;
        if (o.item_tables) {
            for (const char* sql : kProgressStatements) ok = exec(db, sql) && ok;
        }
        if (o.talent_tables) {
            std::ifstream in(TALENT_FIXTURE_PATH, std::ios::binary);
            if (!in) {
                std::printf("  [FAIL] test setup: cannot read %s\n", TALENT_FIXTURE_PATH);
                ok = false;
            } else {
                std::ostringstream text;
                text << "BEGIN;\n" << in.rdbuf() << "COMMIT;\n";
                ok = exec(db, text.str().c_str()) && ok;
            }
        }
    }
    sqlite3_close(db);
    return ok;
}
// CoA with C.Y.B

/// A fresh directory under the system temp directory.
inline fs::path make_temp_dir(const char* prefix) {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()));
    fs::create_directories(dir);
    return dir;
}

}  // namespace content_fixture
