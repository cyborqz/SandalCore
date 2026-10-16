// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/content_db.h
//
//  A read-only reader of content.db, the SQLite file plan C1's contentc builds from
//  the client's design database (services/cmd/contentc, `go run ./cmd/contentc` ->
//  dist/content/content.db).  worldd reads from it what the native bootstrap needs
//  to value a character (plan A5/E1): the class of an archetype id and the six
//  primary attributes of a class at a level.
//
//  The schema is services/internal/content/schema.go (the one place the DDL lives);
//  this reader uses these of its tables and columns, and open() checks that they
//  are there:
//
//    meta             key, value        schema_version (must be 1), contentc.version,
//                                       source.dbc.sha256 (the design database's)
//    class            guid, name, archetype_id
//    class_stat_base  class_guid, level, stat_guid, stat_name, value, rounding_ambiguous
//    ability_runtime, effect_runtime, effect_stat_runtime, stamina_rule
//                     plan A7: what the ability runtime runs (below); contentc has run every formula
//    item, item_equip_slot
//                     plan B2 (contentc version 3): where each ItemRecord may be worn and whether a two-handed item takes all its
//                     slots (the equip rules and the visible gear; below)
//    skill_tree, skill_node, skill_node_require, skill_node_exclusive, skill_rank, skill_rank_cost, skill_rank_stat_mod, skill_point_type,
//    leveling_point, item_weapon
//                     plan D1 stage 2 (contentc version 5): the talents and the weapons' combos (talent_data, item_weapon)
//    stat_program, quality_curve, stat_block_stat, item_set, set_bonus, item_tag, equip_slot
//                     plan B3 (contentc version 4): the compiled stat formula graph, and what an item adds to the stats
//                     (its stat block at each rarity, the quality bands, the sets, the tags) and the names of the equip slots
//
//  The file is opened SQLITE_OPEN_READONLY: a path that does not exist is an error
//  and is never created (SQLite would otherwise make an empty file that the next run
//  opens as a database with no tables), and nothing here can write to the file.
//
//  One ContentDb may be shared by several threads: the connection is opened in
//  SQLite's serialized mode and every query prepares and finalizes its own
//  statement.
//
//  Errors are returned, not thrown: a failing call returns nullopt and sets `error`
//  to a message fit for a log line (and, from character_setup, for a login refusal).
// ============================================================================
#pragma once

#include "stats/gear.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace aoc::worldd::content {

/// contentc's meta rows that identify the file (logged by open()).
struct ContentMeta {
    int         schema_version = 0;     // meta schema_version; the only version this reader reads is kSchemaVersion
    std::string contentc_version;       // meta contentc.version
    std::string source_dbc_sha256;      // meta source.dbc.sha256: the design database the file was built from
    std::string built_at;               // meta built_at ("" when the file has none)
};
// Cyb / C-O-A

/// The schema version this reader reads (services/internal/content SchemaVersion).
inline constexpr int kSchemaVersion = 1;

/// The oldest contentc whose file this worldd reads (services/internal/content MinContentcVersion): version 4 has the stat program and
/// the item stat tables (plan B3), version 5 the item uses, reward tables, level curves and auto-granted ranks (plan D1), version 6 the ability tables and the stat bundle's format 2 (plan D2), version 7 the NPC population chain and
/// the vendors (plan E1), version 8 the routes' waypoints, 9 the combat tables and the spawn filter's records, 10 the reward tables, 11 the mounts (mount, mount_stat, mount_ability, mount_effect), 12 the city nodes (city_node, city_zoi and the other node tables: Nodes 1), 13 the gathering tables (gather_def, gather_tool, item_variation, profession, certification_level ...), the profession level-up curves and the host functions 180-184 (Gathering stage 2), 14 the items' inventory dimension and resource flag (item.inv_w, inv_h, resource) and the resource bags' grids (resource_bag, resource_bag_cell: Gathering stage 3, "Resource storage"), 15 the foliage defs and states and every gathering interaction (foliage_def, foliage_state, gather_interaction, gather_interaction_def: Gathering stage 3, "Foliage gathering"). An older file does not open, and the message says to rebuild it.
inline constexpr int kMinContentcVersion = 15;

/// One row of `class`: a player class.
struct ClassRecord {
    int64_t     guid = 0;               // the class's record Guid (CombatInfo.PrimaryClassId.Guid; the Bard's is 108640158570105)
    std::string name;                   // "Bard"
    int64_t     archetype_id = 0;       // the character-select archetype id (Bard 17747 ... Tank 17754)
};

/// One row of `class_stat_base`: a primary attribute of a class at a level.
struct PrimaryAttribute {
    int64_t     stat_guid = 0;          // the stat's record Guid, as the StatsComponent names it (Strength 0x634D499C567A)
    std::string stat_name;              // "Strength" ...
    double      value = 0;              // computed by contentc in float64, Round rounding halves up
    /// contentc's flag: evaluating the formula in float32 (the engine's likely arithmetic) rounds to another integer
    /// (7 of the 2400 rows; the real value is one of two neighbouring integers).
    bool        rounding_ambiguous = false;
};

/// The levels class_stat_base covers (services/internal/content MinLevel, MaxLevel).
inline constexpr int kMinLevel = 1;
inline constexpr int kMaxLevel = 50;

// ---- plan A7: the ability runtime's numbers ---------------------------------------------------------------------------------

/// The record Guids the ability runtime needs by name: the three abilities, and the stats it spends and regenerates
/// (services/internal/content known.go; the stats' Guids are the ones the captured Owner array carries).
inline constexpr int64_t kGuidSprint = 6064631641648332884ll;
inline constexpr int64_t kGuidDodge = 107258680465016ll;
inline constexpr int64_t kGuidRest = 109393746155128ll;
inline constexpr int64_t kGuidHealth = 108986352686712ll;                // the stats the basic regeneration tick reads and sets (plan D1, stage 2)
inline constexpr int64_t kGuidMaxHealth = 108986356618873ll;
inline constexpr int64_t kGuidHealthRegeneration = 109343399171806ll;
inline constexpr int64_t kGuidMana = 109183576135288ll;
inline constexpr int64_t kGuidMaxMana = 109183578756729ll;
inline constexpr int64_t kGuidManaRegeneration = 109343399171796ll;
inline constexpr int64_t kGuidGuard = 107887054050936ll;                // General_Guard: the weapon bank's first slot (plan D1, stage 2)
inline constexpr int64_t kTypeAbilityRecord = 1841048597116038327ll;       // the TypeId of ReplicatedRecordsToLoad's elements
inline constexpr int64_t kGuidStamina = 6064629575339934476ll;
inline constexpr int64_t kGuidMaxStamina = 6064631747701440948ll;
inline constexpr int64_t kGuidStaminaRegeneration = 6064631663405498419ll;
/// plan D2, stage 2: TempHealth (the float-array stat the shields fill), the record types a hit's source names in its confirmation (the record
/// type ids of the retail confirmations: 3,529 projectile hits, 5,455 ability hits, 1,525 effect hits, 432 reactions, 84 lingering zones), and
/// the targeting record of a hit aimed at the caster, whose hits the server never confirms.
inline constexpr int64_t kGuidTempHealth = 6064629069966082048ll;
/// The class resources whose current value is the character's own state between limits, with a regeneration program that reads it (stat_def rows with
/// a regeneration and no spawn at the maximum: the Tank's Courage, the Cleric's DivinePower and its Gen, the Fighter's Momentum, the Ranger's Focus,
/// the Summoner's Essence).
inline constexpr int64_t kResourceStats[] = {6064629429438907308ll, 6064629623747772676ll, 6064629623761404733ll, 6064629959589298751ll,
                                             6064633175093543338ll, 6064633668368219158ll};
inline constexpr int64_t kSourceTypeAbility = 1841048597116038327ll;
inline constexpr int64_t kSourceTypeProjectile = 979581256175327151ll;
inline constexpr int64_t kSourceTypeEffect = 3377240744094973029ll;
inline constexpr int64_t kSourceTypeZone = 7793593157135267051ll;
inline constexpr int64_t kSourceTypeReaction = 6091248426050982904ll;
inline constexpr int64_t kTargetingDefaultSelf = 6064628968549646338ll;

/// HeldAbilityBehavior: Smart is held while the key is pressed (Sprint), Toggle starts on one press and ends on the next
/// (Rest), Naive is a plain press (Dodge).
enum class Held : uint8_t { Naive, Smart, Toggle };
// [CoA - C.Y.B]

/// One row of `ability_runtime` (contentc's resolved numbers; the table's comments in schema.go say what each is).
struct AbilityRule {
    int64_t     guid = 0;
    std::string name;
    Held        held = Held::Naive;
    bool        requires_out_of_combat = false;
    double      min_stamina = 0;            // the Stamina the ability needs to start (0: none)
    int64_t     cost_stat_guid = 0;         // 0: it spends nothing
    double      cost_amount = 0;            // once, at the trigger, or per charge when charge_interval > 0
    bool        cost_requires_moving = false;
    double      charge_interval = 0;        // seconds between charges of a held ability (0: paid once)
    double      cooldown = 0;               // seconds
    bool        triggers_cooldown = false;
    bool        disable_during_cooldown = false;
    bool        targets_self = false;
    int64_t     effect_guid = 0;            // the ability's own effect (0: none)
    std::string montage_path;
    std::string cancel_section;
};

/// A stat mod of an effect: `stat_guid` gets + (factor x the owner's `source_stat_guid` + addend); no source stat (0): a
/// constant.
struct StatMod {
    int64_t stat_guid = 0;
    int64_t source_stat_guid = 0;
    double  factor = 0;
    double  addend = 0;
};

/// One row of `effect_runtime` with its `effect_stat_runtime` mods.
struct EffectRule {
    int64_t              guid = 0;
    std::string          name;
    bool                 infinite = false;
    double               duration = 0;      // seconds; 0 with infinite
    double               tick_timer = 0;
    bool                 stops_regen_standing = false;   // the StaminaRegeneration formula gives 0 with this effect on the owner
    bool                 stops_regen_moving = false;
    int64_t              end_effect_guid = 0;            // the regeneration pause its End hit applies (0: none)
    double               end_effect_duration = 0;
    std::vector<StatMod> stat_mods;
};

/// Everything the ability runtime reads from content.db.
struct AbilityRules {
    AbilityRule                   sprint, dodge, rest;
    std::map<int64_t, EffectRule> effects;               // by effect Guid: the abilities' effects and their End hits' effects
    double                        stat_regen_time = 1;   // stamina_rule stat_regen_time: seconds between regeneration ticks
    double                        regen_rate = 5;        // regen.rate_enabled: Stamina per second when nothing stops it

    const AbilityRule* ability(int64_t guid) const {
        return guid == sprint.guid ? &sprint : guid == dodge.guid ? &dodge : guid == rest.guid ? &rest : nullptr;
    }
    const EffectRule* effect(int64_t guid) const {
        const auto it = effects.find(guid);
        return it == effects.end() ? nullptr : &it->second;
    }
};

// ---- plan B2: items ---------------------------------------------------------------------------------------------------------

/// The facts of one ItemRecord the equip rules need (rows of `item` and `item_equip_slot`).
struct ItemRecord {
    int64_t              guid = 0;              // the item's record Guid (an item instance's ItemRecordId)
    std::string          name;                  // "Gear_Armor_Light_Starter_Chest"
    /// The record's bCanEquip: NOT what the client wears. The three starter armor pieces of every class's kit have it 0 and are
    /// worn, and 374 gear records have a slot and 0 (services/internal/content schema.go); only equip_slots decides what is worn.
    bool                 can_equip = false;
    /// A two-handed item: worn, it takes every slot of equip_slots (a Sword2H: Primary and Secondary; a Book: Ranged and RangedOffHand).
    bool                 occupies_all_equip_slots = false;
    /// The EEquipSlot values the item may be worn in, in the record's order (empty: it is not worn). The first free one is used
    /// when the client does not name a slot.
    std::vector<uint8_t> equip_slots;
    /// Plan D1: what a fresh instance of the item is (a reward): its default quality, the most of it one stack holds and its
    /// durability at that quality (the kit's rule: MaxDurability rounded half up, 0 for an item with none).
    int32_t              default_quality = 1;
    int32_t              max_stack_size = 1;
    double               max_durability = 0;
    /// Gathering stage 3 (contentc version 14): the cells the item takes in a resource container's grid (inv_w x inv_h, never turned), and whether it is a resource
    /// (its gameplay tags hold a child of item.resource): a resource item is kept in a Resource container, any other item in the Backpack, the Equipment or the Quest storage.
    int32_t              inv_w = 1;
    int32_t              inv_h = 1;
    bool                 resource = false;
    /// Gathering stage 3, "the pick-up popup" (contentc version 16): the record's ItemName as the player reads it ("Coconut Wood"; item.display_name).  Empty where the record has
    /// no name and in a file older than version 16 (it opens all the same; the popup's text then uses the record's `name`).
    std::string          display_name;
};

/// Gathering stage 3 (contentc version 14): the grid a worn resource bag gives its container (rows of resource_bag and resource_bag_cell).
struct ResourceBagRecord {
    int64_t                       item = 0;       // the bag item's record Guid
    int64_t                       def = 0;        // its ResourceBagDef
    std::string                   def_name;
    int32_t                       columns = 0;    // BagDimension.X
    int32_t                       rows = 0;       // BagDimension.Y
    bool                          custom = false; // bCustomDimension: the matrix applies
    /// The def's BagMatrixOverride by column (matrix[x][y]): -1 usable, -2 blocked; empty when the def has none.  It is stored even when `custom` is false (the Sanctus def's).
    std::vector<std::vector<int32_t>> matrix;
};

// ---- plan D1: using an item, the level curves and the ranks a level grants (contentc version 5) ---------------------------------------

/// The record Guids and TypeIds of plan D1 (services/internal/content known.go).
inline constexpr int64_t kGuidAdventuring = 108651085518458ll;              // the LevelingTypeDef of the character's own level
inline constexpr int64_t kGuidUniversalTree = 6064631673604472832ll;        // the SkillTree whose nodes a level grants by itself
inline constexpr int64_t kGuidAdventuringXPAddMult = 6064630809541214212ll; // the stat the level scrolls' formula adds to 1
/// Gathering stage 3, "Hunting": the Hunting profession (CharacterProfession; its leveling type is LevelingTypeDef_Hunter 109460722964090).  A gathering interaction of this profession is
/// the hunt of a creature's corpse with a Bow (the 53 Hunting_<kind> interactions); every other gathering interaction is a node's or a tree's.
inline constexpr int64_t kProfessionHunting = 108752770061965ll;
inline constexpr int64_t kTypeLevelingTypeDef = 5949882024684912047ll;      // the TypeId of the XPTracker's map keys
inline constexpr int64_t kTypeSkillTree = 297887639323616934ll;             // the TypeId of SkillTreePurchasesFastMap's keys
inline constexpr int64_t kTypeSkillRank = 4847957796014348780ll;            // the TypeId of a tree list's ranks
inline constexpr int64_t kTypeItemRecord = 2616523457551378613ll;

/// A row of `item_use` with its `item_action` rows: what using the item does.
struct ItemUse {
    int64_t              item = 0;
    std::string          activation;        // EItemActivationAction without its prefix (ActivateActionResponses runs `actions`)
    bool                 consume_on_use = false;
    double               cooldown = 0;
    bool                 disabled_in_combat = false;
    std::vector<int64_t> actions;           // ActionResponse guids, in the record's order
};

/// A row of `action_response` with its `action_reward` rows.
struct ActionResponse {
    int64_t              guid = 0;
    std::string          name;
    std::string          response_type;     // EActionResponse without its prefix: GiveItem, SetPrimaryArchetype ...
    int64_t              class_guid = 0;    // SetPrimaryArchetype's class (a `class` row's guid), 0 for the others
    std::string          class_name;
    bool                 remove_up_to_quantity = false;
    std::vector<int64_t> reward_tables;     // GiveItem's, in the record's order
};

struct RewardItem {
    int64_t item = 0;
    int64_t variation = 0;
    int32_t quantity = 1;
};

/// An ExperienceRewards entry resolved to the level scrolls' formula (schema.go reward_xp): the XP it grants is
/// (curve(target) - curve(level) + debt_ratio * xp_of_level) / max(0.001, 1 + mult) + margin. target_level 0: not that formula.
struct RewardXp {
    std::string experience_type;   // EExperienceContextType without its prefix: Adventuring, Weapon ...
    int         target_level = 0;
    double      margin = 0;
    int64_t     curve_guid = 0;
    int64_t     mult_stat_guid = 0;
    /// Gathering stage 2 (contentc version 13): a Profession reward's own expression, as an ab_expr id (0: none; the other kinds have none), and its text (the
    /// profession is named in it: professions::profession_named_in).
    int64_t     expr = 0;
    std::string expression;
};
// == C-O-A | C:Y:B ==

/// A row of `reward_table` with its items and experience rewards.
struct RewardTable {
    int64_t                 guid = 0;
    std::string             name;
    std::string             predicate;
    int                     level_below = 0;   // the predicate GetPlayerLevel() < N as N; 0 none; -1 one contentc does not model
    bool                    modelled = false;  // every container grants always and selects all (the GM items' shape)
    std::vector<RewardItem> items;
    std::vector<RewardXp>   experience;
};

/// A row of `leveling_type` with its curve: the XP total at which each level is reached, ascending by level.
struct LevelingType {
    int64_t                                   guid = 0;
    std::string                               name;
    std::string                               experience_type;
    int                                       level_cap = 0;     // LevelCapOverride; 0: the curve's last level
    bool                                      uses_debt = false;
    int64_t                                   xp_mult_stat_guid = 0;
    std::vector<std::pair<int, int64_t>>      curve;             // (level, xp)
};

/// A constant stat mod of an auto-granted rank's passive effect (skill_rank_stat_mod).
struct RankStatMod {
    int64_t     stat_guid = 0;
    std::string stat_name;
    double      value = 0;
};

/// A row of `skill_auto_grant`: a SkillTreeNode a level grants by itself, with its rank's constant stat mods.
struct AutoGrant {
    int64_t                  tree_guid = 0;
    std::string              tree_name;
    int64_t                  leveling_type_guid = 0;
    int64_t                  node_guid = 0;
    std::string              node_name;
    int                      level_requirement = 0;
    int64_t                  skill_guid = 0;
    std::string              skill_name;
    int64_t                  rank_guid = 0;
    int64_t                  effect_guid = 0;
    std::vector<RankStatMod> mods;
};

// ---- Gathering stage 2, part "Professions and GM items" (contentc version 13): the professions and their certification levels -----------------------------------

/// A row of `certification_level`: the five tiers (Novice 0 ... GrandMaster 4), the profession levels each spans and how many professions a character may hold at it.
struct CertificationRow {
    int64_t     guid = 0;
    std::string name;
    int         tier = 0;
    int         min_level = 1;
    int         max_level = 10;
    bool        limited = false;       // has_max_allowable
    int         max_allowable = 0;
};

/// A row of `profession` with its leveling type and curve (the XP total at which each of the 50 levels is reached).
struct ProfessionRow {
    int64_t       guid = 0;
    std::string   name;
    std::string   category;            // Gathering, Processing or Crafting
    int64_t       leveling_type_guid = 0;
    int64_t       skill_tree_guid = 0;
    LevelingType  leveling;
};

/// A row of `action_certification`: a SetCertificationLevel ActionResponse, one tier up (Promote) or down (Demote) for a profession.
struct CertificationActionRow {
    int64_t     action_guid = 0;
    std::string name;
    int64_t     profession_guid = 0;
    bool        promote = true;
};

struct ProfessionData {
    std::vector<ProfessionRow>                   professions;      // guid order
    std::vector<CertificationRow>                certifications;   // tier order
    std::map<int64_t, CertificationActionRow>    actions;          // by action guid
};

// ---- Gathering stage 2, part "Harvesting" (contentc version 13): what a harvest reads (gather_def, gather_tool, item_variation) ------------------------------------

/// A row of `gather_def`: a GatherableActorDef joined to its foliage data (the respawn), its PlayerInteractionDef (the range) and its GatheringInteractionData (the rest).
struct GatherDefRow {
    int64_t     guid = 0;               // the GatherableActorDef (what AGatherableActor.GatherableActorDef.Guid carries)
    std::string name;                   // Plant_Daffodil_Medium_1
    double      empty_duration = 0;     // GatheringFoliageData.EmptyDuration, seconds
    double      override_respawn = 0;   // the actor def's OverrideRespawnDuration, seconds (0: none)
    int64_t     interaction_guid = 0;   // the PlayerInteractionDef
    double      interact_radius = 0;    // cm
    double      interact_tolerance = 0; // cm
    bool        allow_in_combat = true;
    int64_t     profession_guid = 0;    // the CharacterProfession (Mining, Lumberjacking, Herbalism ...)
    int         tool_slot = 0;          // the EEquipSlot of the tool: Pickaxe 26, Axe 29, Sickle 33, FishingRod 35, Bow 38
    int64_t     cert_guid = 0;          // the CertificationLevel the interaction needs
    int         cert_tier = 0;          // ... its tier (Novice 0 ... GrandMaster 4)
    double      base_duration = 0;      // BaseGatheringDuration, seconds
    int64_t     duration_expr = 0;      // ab_expr ids (0: none): the channel's length and the tool's wear
    int64_t     durability_expr = 0;
    int64_t     reward_guid = 0;        // loot_table.guid
};

/// A row of `gather_tool`: a tool item and what it is for.
struct GatherToolRow {
    int64_t     item_guid = 0;
    std::string item_name;
    int64_t     profession_guid = 0;    // GatheringToolData.Profession
    int64_t     cert_guid = 0;
    int         cert_tier = 0;          // the tier of nodes the tool reaches
    int         tool_slot = 0;          // the slot the item is worn in
    int64_t     required_profession = 0;   // the item's own requirement to be used (0: none)
    int64_t     required_cert = 0;
};

struct GatherData {
    std::map<int64_t, GatherDefRow>  defs;         // by GatherableActorDef guid
    std::map<int64_t, GatherToolRow> tools;        // by item guid
    std::map<int64_t, int64_t>       quality_expr; // ItemVariation guid -> the ab_expr id of its Quality formula (0: none)
    std::map<int64_t, int64_t>       durability_expr; // ... and of its Durability formula
};

// ---- Gathering stage 3, part "Foliage gathering" (contentc version 15): the painted trees, plants and stones, and every gathering interaction ------------------------

/// EFoliageState as the client's IndexedStates and FoliageData carry it (a harvested instance is Empty; regrowth passes Infancy and Juvenile up to the state it is painted in).
inline constexpr int kFoliageInfancy = 0, kFoliageJuvenile = 1, kFoliageAdult = 2, kFoliageEmpty = 3;

/// A row of `foliage_def`: a GatheringFoliageData.
struct FoliageDefRow {
    int64_t     guid = 0;
    std::string name;                   // Tree_Oak_Large_1
    double      empty_duration = 0;     // seconds an instance stays Empty after a harvest
    int64_t     dynamic_guid = 0;       // the seasonal factor's def (not applied)
    int         paint_state = kFoliageAdult;   // the state the map paints the instances in: what a regrown instance returns to
    int         gatherable_state = kFoliageAdult, min_state = kFoliageInfancy, max_state = kFoliageAdult;
    int64_t     state_guids[3] = {0, 0, 0};    // the GatheringFoliageState of Infancy, Juvenile, Adult (0: none)
};

/// A row of `foliage_state`: a GatheringFoliageState.
struct FoliageStateRow {
    int64_t     guid = 0;
    std::string name;
    double      lifecycle_duration = 0;   // seconds in the state before the next one (0: permanent)
    int64_t     interaction_guid = 0;     // the gather_interaction that harvests the instance in this state (0: none)
    double      override_distance = -1;   // cm (-1: the interaction's own)
    double      override_angle = -1;      // degrees (-1: the interaction's own)
    double      collision_radius = 0;     // cm
};
// (Cyb : C O A)

/// A row of `gather_interaction`: a GatheringInteractionData with the range of the PlayerInteractionDef named like it.
struct GatherInteractionRow {
    int64_t     guid = 0;
    std::string name;                   // Tree_Oak_Large, Hunting_Wolf
    int         target_state = kFoliageEmpty;
    int64_t     profession_guid = 0;
    int64_t     cert_guid = 0;
    int         cert_tier = 0;
    int         tool_slot = -1;         // -1: the data names no tool
    double      base_duration = 0;
    int64_t     duration_expr = 0;
    int64_t     durability_expr = 0;
    int64_t     reward_guid = 0;
    int64_t     interaction_guid = 0;   // the PlayerInteractionDef that carries the range (0: none)
    double      interact_radius = 0, interact_tolerance = 0;
    bool        allow_in_combat = true;
};

/// A row of `gather_interaction_def`: a PlayerInteractionDef that names a gathering interaction (the Hunting_<X> defs of the huntable creatures among them).
struct GatherInteractionDefRow {
    int64_t     guid = 0;
    std::string name;
    int64_t     gather_guid = 0;        // the GatheringInteractionData it names (one is no record)
    double      interact_radius = 0, interact_tolerance = 0, discovery_distance = 0;
    bool        allow_in_combat = true;
};

struct FoliageData {
    std::map<int64_t, FoliageDefRow>           defs;               // by guid
    std::multimap<std::string, int64_t>        defs_by_name;       // a name -> its guids (Stone_Limestone_Large_2 has two records)
    std::map<int64_t, FoliageStateRow>         states;             // by guid
    std::map<int64_t, GatherInteractionRow>    interactions;       // by GatheringInteractionData guid
    std::map<int64_t, GatherInteractionDefRow> interaction_defs;   // by PlayerInteractionDef guid
};

// ---- plan D1, stage 2: the talents (contentc's skill_tree, skill_node, skill_node_require, skill_node_exclusive, skill_rank,
//      skill_rank_cost, skill_rank_stat_mod, skill_point_type, leveling_point and item_weapon) ---------------------------------------

/// The record Guids of the stage's rules by name: the point types a tree's ranks cost, which content.db names.
inline constexpr const char* kPointArchetype = "Combat_Archetype";   // the class trees' points: 1 per level from 2
inline constexpr const char* kPointStamina = "Combat_Stamina";       // the Universal tree's: 1 per level from 2
inline constexpr const char* kPointWeapon = "Combat_Weapon";         // a weapon tree's: 1 per level of the line from 2 (24 by level 25)

/// What a rank costs in points of one type.
struct SkillCost {
    int64_t point_type = 0;
    int64_t quantity = 0;
};

/// A rank of a skill (skill_rank with its costs and, for an infinite effect, the constant stat mods).
struct SkillRankRow {
    int64_t                  guid = 0;
    int                      ord = 0;             // the rank's place in its skill
    std::string              name;
    int64_t                  ability = 0;         // the ability it grants (0: none: a passive)
    std::string              ability_name;
    int64_t                  effect = 0;          // the passive effect it applies (0: none)
    std::string              effect_name;
    bool                     effect_infinite = false;
    double                   effect_duration = 0;   // seconds; 0 with an infinite effect
    std::vector<SkillCost>   costs;
    std::vector<RankStatMod> mods;
};

/// A node of a tree with its skill's ranks.
struct SkillNodeRow {
    int64_t                  node = 0;
    int                      ord = 0;             // the place in the tree's list
    std::string              name;
    int64_t                  skill = 0;
    std::string              skill_name;
    int                      level_requirement = 0;
    int                      point_requirement = 0;   // points spent in the tree
    bool                     auto_granted = false;    // a level grants its first rank by itself
    bool                     auto_hotbar = false;     // the client puts it on the action bar
    bool                     all_required = false;
    int                      opt = 0;                 // a slot node is listed once per option (the same node and ord): the option's place in its slot
    int64_t                  slot = 0;                // the SkillTreeSlot a slot node offers its skills from (a talent choice); 0: a plain node
    std::vector<int64_t>     requires_skills;         // skills that must be owned (0: a requirement that is met)
    std::vector<int64_t>     requires_nodes;          // slot nodes of the tree, each a requirement that any of its options owned meets
    std::vector<int64_t>     exclusive_skills;
    std::vector<SkillRankRow> ranks;
};
// [C | O | A :: C-Y-B]

struct SkillTreeRow {
    int64_t                   guid = 0;
    std::string               name;
    std::string               kind;                   // class, universal, weapon, profession, other
    int64_t                   leveling_type_guid = 0;
    int64_t                   class_guid = 0;
    int64_t                   weapon_line_guid = 0;
    std::vector<SkillNodeRow> nodes;                  // in the tree's order
};

/// Everything the talents catalog reads, in one go at startup.
struct TalentData {
    std::vector<SkillTreeRow>                                     trees;
    std::map<std::string, int64_t>                                point_types;   // by name
    /// What reaching a level of a leveling type gives: type guid -> level -> point type guid -> quantity.
    std::map<int64_t, std::map<int, std::map<int64_t, int64_t>>>  points;
};

/// A weapon item's combo ability and weapon line (item_weapon).
struct ItemWeaponRow {
    int64_t     attack_ability = 0;
    int64_t     skill_line = 0;
    std::string skill_line_name;
};

// ---- plan E1: the NPC population chain and the vendors (contentc version 7) ----------------------------------------------------

/// A PopulationFilter (npc_asset.filter, npc_filter.filter, contentc version 9): the conditions under which an asset may spawn.  Enum values are
/// their names without the type ("Village", "Kaelar", "Riverlands"); `node_level_*` are ENodeLevel names (Wilderness, Crossroads, Encampment, Village,
/// Town, City, Metropolis; cCount never matches); a time of day of -1 on both ends is no condition.
struct PopFilter {
    std::vector<std::string> biomes, cultures, node_types, required_tags, seasons, weather;
    std::vector<int64_t>     node_ids, zones;
    std::string              node_level_min = "Wilderness", node_level_max = "Metropolis";
    double                   time_of_day_start = -1, time_of_day_end = -1;
};

/// A PopulationAsset (npc_asset): the NPC's blueprint and archetype path, faction, radii, scale.
struct NpcAssetRow {
    int64_t     guid = 0;
    std::string name;
    std::string asset_ref;
    std::string archetype_path;     // "" when the asset names no Blueprint
    int64_t     faction = 0;
    double      aggro_radius = 0, leash_radius = 0;
    bool        override_aggro = false, override_leash = false;
    double      scale_min = 1, scale_max = 1;
    int64_t     dialogue_root = 0, interact_component = 0, interaction_def = 0, appearance = 0;
    std::vector<int64_t> loot_tables;
    // contentc version 9 (the spawn filter)
    bool        dev_only = false;
    PopFilter   filter;
    std::vector<std::string> tags;       // the asset's GameplayTags
};

/// A PopulationInstance (npc_instance): the AssetSets it draws from and how its NPC lives.
struct NpcInstanceRow {
    int64_t     guid = 0;
    std::string name;
    int         level_min = 0, level_max = 0;
    bool        dynamic_leveling = false;
    double      respawn_time = 0, respawn_variance = 0;
    std::string respawn_rule;
    int64_t     patrol_route = 0, vendor = 0, interact_component = 0, interact_response = 0, dialogue = 0;
    std::string ambient_behavior;
    double      min_radius = 0, max_radius = 0, arrival_distance = 0, wander_speed_mult = 0, wander_delay = 0, wander_delay_deviation = 0;
    std::string speed_expr, snapping_rule, orientation_type, spawn_location_type;
    double      aggro_radius = 0, area_weight = 0, activation_range = 0;
    bool        override_aggro = false, blacklisted = false;
    int64_t     group_id = 0;
    // contentc version 8 (stage 2): how a patrol walks its route
    int         num_waypoints = 0;       // how many of the route's waypoints a patrol visits (0: all)
    double      max_angle = 0, follow_distance = 0;
    std::string formation, idle_anim;
    bool        align_to_slope = false;
    // contentc version 9 (the spawn filter)
    bool        dev_only = false;
    int64_t     filter_override = 0;     // FilteringOverrideId (PopulationData::filters), 0 none
    std::string spawner_priority;
    std::vector<std::string> tags;
    std::vector<int64_t> asset_sets;
    std::vector<int64_t> loot_tables;
};

/// A PathingRoute (npc_route, contentc version 8): the patrol path.  `waypoints` are the places a patrol walks to, in order (every route but 14 has
/// them), `pauses` the seconds it waits at each (shorter than waypoints: the rest wait 0), `points` the spline's points of the same path (924 routes
/// carry both).  Positions are relative to the actor's transform (translation, rotation as a quaternion, scale) unless a flag says the route is placed
/// by something else (a city node, an encounter): those are not walked in the open world.
struct NpcRouteRow {
    int64_t     guid = 0;
    std::string name;
    bool        looping = false, start_closest = false, relative_city_node = false, relative_encounter = false;
    double      tx = 0, ty = 0, tz = 0, qx = 0, qy = 0, qz = 0, qw = 1, sx = 1, sy = 1, sz = 1;
    std::vector<std::array<double, 3>> points;
    std::vector<std::array<double, 3>> waypoints;
    std::vector<double>                pauses;
    std::string pathing_set;
};

/// One instance of a PopulationSet at a local offset (cm, yaw in degrees).
struct NpcSetMember {
    int64_t key = 0;
    int64_t instance = 0;
    double  x = 0, y = 0, z = 0, yaw = 0;
    bool    random_rotation = false;
};

struct NpcSetRow {
    int64_t                   guid = 0;
    std::string               name;
    int64_t                   area = 0;      // PopulationArea (0: none)
    double                    activation_range = 0;
    bool                      blacklisted = false;
    bool                      dev_only = false;           // contentc version 9
    int64_t                   filter_override = 0;
    std::vector<std::string>  tags;
    std::vector<NpcSetMember> members;
};

/// A WorldSpawn (npc_world_spawn): the map its placements belong to ("Verra_World_Master" for the open world; a quest's or an event's spawns, a node's
/// layout and the test maps name another or none) and its filter override.
struct NpcWorldSpawn {
    int64_t     guid = 0;
    std::string name;
    std::string culture;
    std::string map_name;
    bool        dev_only = false;
    int64_t     filter_override = 0;
};
// -- Cyb & C-O-A --

/// A WorldSpawn's placement of a set: where, and its level overrides (npc_placement).
struct NpcPlacement {
    int64_t spawn = 0;
    int64_t key = 0;
    int64_t set = 0;
    double  x = 0, y = 0, z = 0, yaw = 0;
    int     min_level = 0, max_level = 0;
};

struct NpcAreaCircle {
    double  x = 0, y = 0, z = 0, extent = 0, weight = 1;
    int64_t max_spawns = 0;
};

struct NpcAreaRow {
    int64_t                    guid = 0;
    std::string                name;
    int                        min_spawns = 0, max_spawns = 0;
    std::vector<NpcAreaCircle> circles;
};

/// Plan E1, stage 3 (contentc version 9): the combat configuration. The DamageProfile's stats (DefaultCombat.ini; combat_profile: the key is
/// "PowerMult", "Physical.Damage", ...; combat_element: one row per element tag), the combat rules' numbers (combat_config) and who fights whom
/// (CharacterAlignment.RelationshipsIds and the relationship records' CombatPermissions: 1 hostile, 2 friendly, 3 neutral).
// The keys of combat_config (services/internal/content/combat.go).
inline constexpr const char* kCombatInCombatTimer = "in_combat_timer";
inline constexpr const char* kCombatGlobalDamageMult = "global_damage_multiplier";
inline constexpr const char* kCombatGlobalHealingMult = "global_healing_multiplier";
inline constexpr const char* kCombatOutOfRange = "out_of_range_distance";
inline constexpr const char* kCombatActivationLeeway = "server_activation_leeway";
inline constexpr const char* kCombatThirdPartyHit = "third_party_hit_distance";
inline constexpr const char* kCombatMaxHitsPerRpc = "max_hits_per_rpc";
inline constexpr const char* kCombatStatRegenTime = "stat_regen_time";
// The CharacterRelationship CombatPermissions values, and the alignment records a player carries (Player) and that make a creature the players' enemy.
inline constexpr int kPermissionHostile = 1, kPermissionFriendly = 2, kPermissionNeutral = 3;
inline constexpr int64_t kAlignmentPlayer = 6064629444695359488ll;
inline constexpr int64_t kAlignmentNpcPlayerEnemy = 6064629454762803230ll;

struct CombatRows {
    struct Element {
        std::string tag;
        int64_t     damage = 0, damage_mult = 0, damage_add_mult = 0, mitigation = 0;
    };
    std::map<std::string, int64_t>          profile;
    std::vector<Element>                    elements;
    std::map<std::string, double>           config;
    std::map<int64_t, int>                  permissions;                     // relationship record -> CombatPermissions
    std::map<int64_t, std::map<int64_t, int64_t>> relation;                  // alignment -> other alignment -> relationship record
};

// ---- plan E1, stage 3, part 2 (contentc version 10): the reward tables ------------------------------------------------------------
/// One reward table (loot_table: services/internal/content/loot.go has the document).  An expression is an ab_expr id (0: none); the programs are the
/// ability catalog's.
struct LootXp {
    std::string type;      // Adventuring, Weapon, NodeActivity ...
    int64_t     expr = 0;
};
struct LootItem {
    int64_t item = 0, variation = 0, qty = 0;
};
struct LootCurrency {
    int64_t currency = 0, amount = 0;
};
struct LootReward {
    std::vector<LootXp>       xp;
    std::vector<LootItem>     items;
    std::vector<LootCurrency> currency;
};
struct LootContainer {
    std::string grant;      // Auto (straight to the players) or Always (into the loot)
    std::string algo;       // All, PureRandom, PerElementWeightedRandom, PerElementWeightedExpressionRandom, RollEachByPercent
    int64_t     pred = 0;   // GrantPredicate
    int64_t     n = 0;      // NumberToSelect
    std::vector<double>     w;
    std::vector<int64_t>    pct, xw;
    std::vector<LootReward> rewards;
};
struct LootTable {
    int64_t     guid = 0;
    std::string name;
    int64_t     pred = 0;
    std::string algo;       // the sub tables' selection ("" none)
    int64_t     n = 0;
    std::vector<int64_t>       subs;
    std::vector<double>        w;
    std::vector<int64_t>       pct, xw;
    std::vector<LootContainer> containers;
};

/// The chain, whole: 4,576 assets, 16,902 instances, 4,728 sets, 15,179 placements, 608 areas.
struct PopulationData {
    std::map<int64_t, NpcAssetRow>                         assets;
    std::map<int64_t, std::vector<std::pair<int64_t, double>>> asset_sets;   // set guid -> (asset guid, weight) in order
    std::map<int64_t, NpcInstanceRow>                      instances;
    std::map<int64_t, NpcSetRow>                           sets;
    std::vector<NpcPlacement>                              placements;
    std::map<int64_t, NpcAreaRow>                          areas;
    std::map<int64_t, NpcRouteRow>                         routes;           // contentc version 8
    // contentc version 9: what decides whether a point spawns now (the spawn filter, spawns/spawn_filter.h)
    std::map<int64_t, NpcWorldSpawn>                       world_spawns;
    std::map<int64_t, PopFilter>                           filters;          // FilteringOverride guid -> its PopulationFilter
    std::map<int64_t, std::vector<std::string>>            gates;            // set guid -> what spawns it when its turn comes (a quest, an event, ...)
    std::map<int64_t, std::vector<std::string>>            asset_set_tags;
};

/// A listing of a vendor's inventory: an item and the guids of the offers that price it (none: free).
struct VendorListing {
    int64_t              item = 0;
    std::vector<int64_t> offers;
    bool                 hidden = false;
};

struct VendorInventoryRow {
    int64_t                    guid = 0;
    std::string                name;
    std::vector<VendorListing> listings;
};

struct VendorDefRow {
    int64_t                         guid = 0;
    std::string                     name;
    std::vector<VendorInventoryRow> inventories;   // the vendor window's tabs, in order
};

/// An item record whose name begins "GM" or holds "PTR": the items a GM or the public test realm hands out.
struct GmItemRow {
    int64_t     guid = 0;
    std::string name;
    int         max_stack = 1;
};

// ---- the mounts (contentc version 11; services/internal/content mounts.go) -------------------------------------------------

/// One row of `mount`: an item that summons a mount.
struct MountRow {
    int64_t     item = 0;                // the ItemRecord (Mount_Grounded_TerrorBird_0)
    std::string item_name;
    std::string kind;                    // "ground", "flying" or "aquatic"
    std::string blueprint_path;          // SummonNotifyClassPath: /Game/GameBlueprints/Mounts/Mount_Grounded_TerrorBird_0.Mount_Grounded_TerrorBird_0_C
    std::string blueprint;               // the package name: Mount_Grounded_TerrorBird_0 (several items share one)
    std::string activation;              // "SummonMount", or "CastAbility" (the flying ones)
    int64_t     cast_ability = 0;        // ActivationCastId: Summon_Mount
    int         level = 0;               // the item text's "Level N" (0: none)
    int         ground_pct = 0;          // "165% Ground Speed" (0: the text names none: 22 of the 51 ground items)
    int         flying_pct = 0;
    int         water_pct = 0;
    int         health_pct = 100;        // "500% Maximum Health"
    bool        mule = false;
    int         crate_capacity = 0;
    bool        tradeable = false;
    std::string description;
// [C O A x C | Y | B]

    double health_factor() const { return health_pct / 100.0; }
};

/// One row of `mount_stat`: a Mount_Base stat at an owner's level.
struct MountStatRow {
    int         level = 0;
    int64_t     stat = 0;
    std::string stat_name;
    double      value = 0;
};

/// One row of `mount_ability`.
struct MountAbilityRow {
    int64_t     ability = 0;
    std::string name;
    std::string role;                    // "summon", "dash", "dismount_elevated", "dismount_grounded" or ""
    bool        usable_while_mounted = false;
    bool        auto_dismount = false;
};

/// One row of `mount_effect`: a road effect (Road_T0..T4_Speed_Mount), the dash, an effect with bForceDismount, the crate debuff.
struct MountEffectRow {
    int64_t     effect = 0;
    std::string name;
    std::string kind;                    // "road", "dash", "force_dismount" or "crate"
    int         tier = -1;               // a road's T0..T4
    int64_t     stat = 0;                // MoveSpeedMult; 0 for none
    double      value = 0;               // added to the stat; the dash (relative) adds this share of the stat
    bool        relative = false;
    bool        infinite = false;
    double      duration = 0;            // seconds; 0 for an infinite effect
    bool        force_dismount = false;
};

/// The four tables, read whole (101 mounts, 250 stat rows, 34 abilities, 9 effects).
struct MountData {
    std::vector<MountRow>         mounts;               // by item name
    std::vector<MountStatRow>     stats;                // by level, then stat
    std::vector<MountAbilityRow>  abilities;
    std::vector<MountEffectRow>   effects;

    const MountRow*        find(int64_t item) const;
    /// The Mount_Base value of a stat at an owner's level (1 to 50); `fallback` when the file has none.
    double                 stat_at(int level, int64_t stat, double fallback = 0) const;
    const MountAbilityRow* ability(int64_t guid) const;
    const MountAbilityRow* ability_with_role(const std::string& role) const;
    const MountEffectRow*  road(int tier) const;
    const MountEffectRow*  dash() const;
};

// ---- plan B3: the stat program and the items' stat data --------------------------------------------------------------------

/// The one row of `stat_program`: the compiled stat formula graph (services/internal/statprog) and what contentc says it holds.
struct StatProgramRow {
    int                  version = 0;       // the bundle format's version
    std::string          mode;              // float32, double or single
    int                  stats = 0;
    int                  classes = 0;
    uint32_t             crc32 = 0;         // the body's checksum, as the bundle's header has it
    std::vector<uint8_t> data;              // the bundle: stats::parse_bundle reads and verifies it
};

class ContentDb {
public:
    /// Opens `path` read-only and checks meta and the tables above; logs contentc's version and the source's sha256.
    /// nullopt, with `error` set, when the file is missing, is not a SQLite database, is not a content.db of this
    /// schema version, or lacks a table or column this reader uses.
    static std::optional<ContentDb> open(const std::string& path, std::string& error);

    ContentDb(const ContentDb&) = delete;
    ContentDb& operator=(const ContentDb&) = delete;
    ContentDb(ContentDb&& other) noexcept;
    ContentDb& operator=(ContentDb&& other) noexcept;
    ~ContentDb();

    const ContentMeta& meta() const { return meta_; }
    const std::string& path() const { return path_; }

    /// The player class whose archetype id is `archetype_id`; nullopt (error says so) when there is none.
    std::optional<ClassRecord> class_for_archetype(int64_t archetype_id, std::string& error) const;

    /// The player class named `name` ("Bard": the name of an ECharacterClass); nullopt (error says so) when there is none.
    std::optional<ClassRecord> class_for_name(const std::string& name, std::string& error) const;

    /// The six primary attributes of a class at a level, ordered by stat Guid; nullopt (error says so) when the
    /// class has none at that level (levels kMinLevel..kMaxLevel are there).
    std::optional<std::vector<PrimaryAttribute>> primary_attributes(int64_t class_guid, int level, std::string& error) const;

    /// Plan A7: the three abilities worldd runs (Sprint, Dodge, Rest), the effects they apply and end with, and the
    /// regeneration numbers. nullopt (error says so) when a row is missing or says something the runtime cannot run (a Sprint
    /// that is not held, a Dodge with no cost ...): the file was built by another contentc than this worldd's.
    std::optional<AbilityRules> ability_rules(std::string& error) const;

    /// Plan B2: the item with the record Guid. nullopt with `error` empty when there is no such item (an instance of a record the
    /// file does not know); nullopt with `error` set when the query failed or the row holds an EEquipSlot that does not fit a byte.
    std::optional<ItemRecord> item(int64_t guid, std::string& error) const;

    /// Gathering stage 3 (contentc version 14): the grid of a resource bag item.  nullopt with `error` empty for an item that is no resource bag; with `error` set when a query
    /// failed or the rows are not what contentc writes (a dimension below 1 or above the 255 cells a container holds, a cell outside the grid).
    std::optional<ResourceBagRecord> resource_bag(int64_t item_guid, std::string& error) const;

    /// Plan B3: the stat program. nullopt, with `error`, when the table has no row or the query failed.
    std::optional<StatProgramRow> stat_program(std::string& error) const;

    /// Plan B3: what the item adds to the stats (its quality bands, stat block, sets and tags). nullopt with `error` empty when there is
    /// no such item; nullopt with `error` set when a query failed or the rows are not what contentc writes (a curve without its seven
    /// rarities).
    std::optional<stats::ItemGear> item_gear(int64_t guid, std::string& error) const;

    /// Plan B3: a set bonus's tiers; nullopt with `error` empty for a set that is none, with `error` set when the query failed.
    std::optional<stats::SetBonus> set_bonus(int64_t guid, std::string& error) const;

    /// Plan B3: the EEquipSlot enumerator names by value ("Chest" for 2).
    std::optional<std::map<int, std::string>> equip_slot_names(std::string& error) const;

    /// Plan D1: how an item is used. nullopt with `error` empty for an item that is not used; with `error` set when a query failed.
    std::optional<ItemUse> item_use(int64_t item_guid, std::string& error) const;
    /// Plan D1: an ActionResponse; nullopt with `error` empty when there is none.
    std::optional<ActionResponse> action_response(int64_t guid, std::string& error) const;
    /// Plan D1: a RewardTable; nullopt with `error` empty when there is none. A row of another storage class than its column's is an error.
    std::optional<RewardTable> reward_table(int64_t guid, std::string& error) const;
    /// Plan D1: the leveling type named ("Adventuring") with its curve; nullopt with `error` set when there is none or a key is not a
    /// whole level at a whole XP total ascending.
    std::optional<LevelingType> leveling_type_named(const std::string& name, std::string& error) const;
    /// Gathering stage 2, "Professions and GM items": the 22 professions with their curves, the five certification levels and the certification actions. nullopt with
    /// `error` empty when the file has none of these tables (an older fixture); nullopt with `error` set when a query failed or a row is not what contentc writes.
    std::optional<ProfessionData> profession_data(std::string& error) const;
    /// Gathering stage 2, "Harvesting": the gather tables, whole (339 defs, 87 tools, 47 item variations in contentc version 13). nullopt with `error` empty when the file has none of the tables
    /// (an older fixture); nullopt with `error` set when a query failed or a row is not what contentc writes.
    std::optional<GatherData> gather_data(std::string& error) const;
    /// Gathering stage 3, "Foliage gathering": the foliage defs and states and every gathering interaction (402, 743, 236 and 194 rows in contentc version 15).  nullopt with `error` empty
    /// when the file has none of the tables (an older fixture); nullopt with `error` set when a query failed or a row is not what contentc writes.
    std::optional<FoliageData> foliage_data(std::string& error) const;
    /// Plan D1: the nodes a level grants in the trees of a leveling type, by level requirement then node guid, with their rank mods.
    std::optional<std::vector<AutoGrant>> auto_grants(int64_t leveling_type_guid, std::string& error) const;

    /// Plan D1, stage 2: every skill tree with its nodes, ranks, gates and costs, the point types and the points each level gives.
    /// nullopt with `error` set when a query failed or a row is of another storage class than its column's.
    std::optional<TalentData> talent_data(std::string& error) const;
    /// Plan D1, stage 2: a weapon item's combo ability and weapon line; nullopt with `error` empty for an item that names none.
    std::optional<ItemWeaponRow> item_weapon(int64_t item_guid, std::string& error) const;
// C_Y_B + C | O | A

    /// Plan E1, stage 3: the combat configuration (combat_profile, combat_element, combat_config, relationship, alignment_relation). nullopt with `error`
    /// set when a table is missing (a file built by an older contentc, the tests' fixtures).
    std::optional<CombatRows> combat_rows(std::string& error) const;

    /// Plan E1, stage 3, part 2: the reward tables with the guids `roots` and every sub table they reach (loot_table, contentc version 10).
    /// A guid that is no table is skipped (a Blueprint may name a record the file lacks).  nullopt with `error` set when the table is missing or a
    /// document does not read.
    std::optional<std::map<int64_t, LootTable>> loot_tables(const std::vector<int64_t>& roots, std::string& error) const;

    /// Plan E1, stage 3, part 2: an experience rule (xp_rule): the ab_expr id of the rule named `key` ("adventuring_decay"); 0 with `error` empty when the
    /// file has none, nullopt with `error` set when the query failed.
    std::optional<int64_t> xp_rule(const std::string& key, std::string& error) const;

    /// Plan E1: the NPC population chain (npc_*), read whole. nullopt with `error` set when a table is missing (a file of another shape: the
    /// fixtures of the tests have none) or a column is of another storage class than the column's.
    std::optional<PopulationData> population_data(std::string& error) const;
    /// Plan E1: a vendor (vendor_def with its inventories and their listings) by its VendorDef guid; nullopt with `error` empty when there is none.
    std::optional<VendorDefRow> vendor_def(int64_t guid, std::string& error) const;
    /// Plan E1: a vendor by name ("PTR_ONLY_Vendor_Gear_0-9"); nullopt with `error` empty when there is none.
    std::optional<VendorDefRow> vendor_def_named(const std::string& name, std::string& error) const;
    /// Plan E1: every vendor whose name begins with `prefix` ("PTR_ONLY_Vendor_"), by name, each read whole (inventories and listings);
    /// nullopt with `error` set when a query failed. Empty for a file with none.
    std::optional<std::vector<VendorDefRow>> vendor_defs_with_prefix(const std::string& prefix, std::string& error) const;
    /// Plan E1: every item record named GM% or %PTR%, by guid; nullopt with `error` set when the query failed.
    std::optional<std::vector<GmItemRow>> gm_items(std::string& error) const;
    /// The mounts milestone (contentc version 11): the mount tables, read whole. nullopt with `error` set when a table is missing (a file of another shape)
    /// or a column is of another storage class than the column's.
    std::optional<MountData> mount_data(std::string& error) const;

private:
    ContentDb() = default;

    sqlite3*    db_ = nullptr;
    std::string path_;
    ContentMeta meta_;
};

/// The stat engine's view of content.db's items (stats::GearCatalog), read when first asked and kept: an item's data is a handful of rows,
/// and a login or an equip asks for the few dozen the character wears. Safe to share between threads.
class ContentGearCatalog final : public stats::GearCatalog {
public:
    /// Reads the equip slot names; ok() is false, with error(), when it cannot. `db` must outlive the catalog.
    explicit ContentGearCatalog(const ContentDb& db);

    bool               ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    const stats::ItemGear*  item(int64_t record) const override;
    const stats::SetBonus*  set(int64_t guid) const override;
    std::string             slot_name(int slot) const override;

private:
    const ContentDb&                                                 db_;
    std::map<int, std::string>                                       slots_;
    std::string                                                      error_;
    mutable std::mutex                                               mutex_;
    mutable std::unordered_map<int64_t, std::unique_ptr<stats::ItemGear>> items_;
    mutable std::unordered_map<int64_t, std::unique_ptr<stats::SetBonus>> sets_;
};

}  // namespace aoc::worldd::content
