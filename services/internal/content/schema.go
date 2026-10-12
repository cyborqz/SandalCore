// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package content reads content.db: the game numbers the services need,
// extracted from the client's design database (CacheDB.dbc) by cmd/contentc
// (plan C1). The file is a SQLite database; this package defines its schema
// (the one place the DDL lives) and the typed queries over it.
//
// What is in it (spec docs/superpowers/specs/2026-09-29-native-world-state-design.md §6):
//
//	meta                 where the file came from and how it was made
//	record_table         every table of the .dbc and whether it decoded
//	record               every row of every decoded table, its fields as JSON
//	stat_def             StatTypeDef: what a stat is, its formulas and audience
//	stat_curve_def,
//	stat_curve           the StatCurve tables the formulas call (EvalCurve)
//	stat_init            StatInitializerList: a stat's starting formula per list
//	class                the eight player classes
//	class_stat_base      the six primary attributes of each class at levels 1-50,
//	                     computed from stat_init and stat_curve (package aocexpr)
//	ability, ability_*   Sprint and Dodge
//	effect, effect_*,
//	hit, hit_effect,
//	owner_stat_mod       the effects and hits they reach, and the stat mods of both
//	stamina_rule,
//	stamina_regen_disable
//	                     what plan A7 needs to spend and regenerate stamina
//	ability_runtime,
//	effect_runtime,
//	effect_stat_runtime  the numbers worldd's ability runtime runs (Sprint, Dodge and Rest, the
//	                     effects they apply), resolved from the rows above by contentc's
//	                     aocexpr: no formula is left for worldd to evaluate
//	equip_slot, item,
//	item_equip_slot,
//	starting_equipment   plan B2: the items of the design database (where each may be worn, its fresh
//	                     durability) and the kit each class starts with
//	quality_curve, stat_block_stat,
//	item_set, set_bonus, item_tag,
//	stat_program         plan B3 (contentc version 4): what an item adds to the stats (its stat block at each rarity,
//	                     the quality bands, the set bonuses, the gameplay tags) and the compiled stat formula graph
//	                     (package statprog) that worldd runs
//	item_use, item_action, action_response, action_reward,
//	reward_table, reward_item, reward_xp,
//	leveling_type, leveling_curve,
//	skill_auto_grant, skill_rank_stat_mod
//	                     plan D1 (contentc version 5): what using an item does (the GM scrolls' action responses and
//	                     reward tables, the level scrolls' XP formula resolved to its parameters), the level curves and
//	                     the ranks a level grants
//	skill_point_type, leveling_point, skill_tree, skill_node, skill_node_require, skill_node_exclusive,
//	skill_rank, skill_rank_cost, item_weapon
//	                     plan D1 stage 2 (still version 5): the talents (the trees, nodes and ranks with their gates, costs,
//	                     abilities and effects, the points each level gives, the weapons' combos)
//	ab_*                 plan D2 (contentc version 6): every ability of every class and what it reaches, as plain data and as
//	                     expressions compiled into the stat program (the tables are in abilities.go)
//	npc_*, vendor_*      plan E1 (contentc version 7): the NPC population chain (assets, asset sets, instances, sets, world spawns,
//	                     areas, patrol routes) and the vendors with their inventories (the tables are in population.go)
//
// Guids are the design database's int64 record ids; type ids are its uint64
// table ids, stored as int64 (all of them fit).
package content

// built by Cyb with C | O | A

import (
	"database/sql"
	"fmt"
)

// SchemaVersion is stored in meta as schema_version; Open refuses another.
const SchemaVersion = 1

// Meta keys the reader relies on (contentc writes more; see its package doc).
const (
	MetaSchemaVersion  = "schema_version"
	MetaRecordEncoding = "record.encoding"
)

// Record encodings: how record.data holds a row's fields.
const (
	EncodingJSON = "json"      // data is the JSON text
	EncodingGzip = "gzip+json" // data is the gzip-compressed JSON (a BLOB)
)

// DDL creates every table. Expressions are AoCExpression text, as stored in the
// design database (CRLF line ends included).
const DDL = `
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;

-- One row per table of the .dbc (369), whether or not its rows decoded.
CREATE TABLE record_table (
    table_index INTEGER PRIMARY KEY,  -- position in the .dbc's table directory
    type_id     INTEGER NOT NULL,     -- the table's TypeId, as Id structs name it
    schema      TEXT NOT NULL,        -- the <Type>RecordBase struct; '' when not identified
    rows        INTEGER NOT NULL,     -- the directory's row count
    decoded     INTEGER NOT NULL      -- 1: every row is in record
);

-- Every row of every decoded table. data is the row's fields (the
-- <Type>RecordBase properties, not Guid/ParentGuid/Name/bDevOnly) as JSON:
-- enums as "EType::Value" strings, Id structs as {"Guid","TypeId","Name"},
-- maps as [{"Key","Value"}], text as {"History","Namespace","Key","Source","Table"}.
-- A float that is NaN or infinite, which JSON cannot hold, is the string "NaN",
-- "+Inf" or "-Inf" (meta decode.nonfinite_floats counts them).
-- record.encoding in meta says whether data is that text or gzip of it.
-- A (type_id, guid) names one row: the UNIQUE constraint is also the lookup index.
CREATE TABLE record (
    table_index INTEGER NOT NULL,
    type_id     INTEGER NOT NULL,
    schema      TEXT NOT NULL,
    guid        INTEGER NOT NULL,
    parent_guid INTEGER NOT NULL,
    name        TEXT NOT NULL,
    dev_only    INTEGER NOT NULL,
    data        BLOB NOT NULL,
    UNIQUE (type_id, guid)
);
CREATE INDEX record_by_table ON record (table_index);

-- StatTypeDef. replication is the row's Replication byte, a mask of
-- EReplicationRules (1 autonomous = the owner, 2 simulated, 4 proxy); audience
-- spells it out and rep_field is the StatsComponent array that carries the
-- stat, for the three values the capture shows (1, 5, 7).
CREATE TABLE stat_def (
    guid                   INTEGER PRIMARY KEY,
    name                   TEXT NOT NULL,
    gameplay_type          TEXT NOT NULL,
    stat_type              TEXT NOT NULL,
    replication            INTEGER NOT NULL,
    audience               TEXT NOT NULL,
    rep_field              TEXT,
    base_expr              TEXT NOT NULL,
    min_expr               TEXT NOT NULL,
    max_expr               TEXT NOT NULL,
    regen_expr             TEXT NOT NULL,
    on_spawn               TEXT NOT NULL,
    on_death               TEXT NOT NULL,
    on_revive              TEXT NOT NULL,
    persist_value          INTEGER NOT NULL,
    hidden                 INTEGER NOT NULL,
    clamp_final_value      INTEGER NOT NULL,
    multiply_mods          INTEGER NOT NULL,
    regen_while_dead       INTEGER NOT NULL,
    recalc_on_combat_state INTEGER NOT NULL
);
CREATE INDEX stat_def_by_name ON stat_def (name);

-- StatCurve: one row per curve, and its keys. level is the key's Time (the
-- character level for the player curves).
CREATE TABLE stat_curve_def (
    guid           INTEGER PRIMARY KEY,
    name           TEXT NOT NULL,
    key_count      INTEGER NOT NULL,
    default_value  REAL NOT NULL,
    external_curve TEXT NOT NULL      -- an asset path when the curve is also given by an asset, else ''
);
CREATE TABLE stat_curve (
    guid   INTEGER NOT NULL,
    name   TEXT NOT NULL,
    ord    INTEGER NOT NULL,
    level  REAL NOT NULL,
    value  REAL NOT NULL,
    interp TEXT NOT NULL,             -- Linear | Constant | Cubic
    arrive_tangent REAL NOT NULL,     -- (contentc version 9) the key's ArriveTangent and LeaveTangent: a Cubic segment is a Bezier through the control
    leave_tangent  REAL NOT NULL,     -- points the first key's leave and the next key's arrive tangents make
    PRIMARY KEY (guid, ord)
) WITHOUT ROWID;

-- StatInitializerList: the entries of each list, in file order. stat_guid 0 is
-- a null reference in the database (stat_name '').
CREATE TABLE stat_init (
    list_guid  INTEGER NOT NULL,
    list_name  TEXT NOT NULL,
    ord        INTEGER NOT NULL,
    stat_guid  INTEGER NOT NULL,
    stat_name  TEXT NOT NULL,
    expression TEXT NOT NULL,         -- '' leaves the stat as the lists before it left it
    PRIMARY KEY (list_guid, ord)
) WITHOUT ROWID;
CREATE INDEX stat_init_by_list_stat ON stat_init (list_guid, stat_guid);

-- The eight player classes (CharacterClasses rows named like the class enum
-- without the "Stage" duplicates). archetype_id is the wire archetype record id
-- of services/internal/aocchar (not in the design database); stat_init_list_*
-- is the list "Player_Base_<name>" (a naming convention: no row refers to it).
CREATE TABLE class (
    guid                INTEGER PRIMARY KEY,
    name                TEXT NOT NULL UNIQUE,
    class_enum          TEXT NOT NULL,
    archetype_id        INTEGER NOT NULL UNIQUE,
    level_type_guid     INTEGER NOT NULL,
    level_type_name     TEXT NOT NULL,
    skill_tree_guid     INTEGER NOT NULL,
    skill_tree_name     TEXT NOT NULL,
    prefers_melee       INTEGER NOT NULL,
    stat_init_list_guid INTEGER NOT NULL,
    stat_init_list_name TEXT NOT NULL
);

-- The six primary attributes of each class at each level, computed from the
-- class's stat_init expressions and the stat_curve keys, in float64 with Round
-- rounding halves up. rounding_ambiguous is 1 where evaluating in float32 (the
-- engine's likely arithmetic) rounds to another integer: 7 of the 2400 rows.
CREATE TABLE class_stat_base (
    class_guid         INTEGER NOT NULL,
    class_name         TEXT NOT NULL,
    level              INTEGER NOT NULL,
    stat_guid          INTEGER NOT NULL,
    stat_name          TEXT NOT NULL,
    value              REAL NOT NULL,
    rounding_ambiguous INTEGER NOT NULL,
    PRIMARY KEY (class_guid, level, stat_guid)
) WITHOUT ROWID;

-- Abilities (Sprint and Dodge): the row, its cost groups (one row per stat
-- cost) and the effects and hits it lists.
CREATE TABLE ability (
    guid                    INTEGER PRIMARY KEY,
    name                    TEXT NOT NULL,
    display_name            TEXT NOT NULL,
    tags                    TEXT NOT NULL,   -- JSON array of tag names
    activation_requirements TEXT NOT NULL,
    cooldown_expr           TEXT NOT NULL,
    disable_during_cooldown INTEGER NOT NULL,
    shared_cooldown_tag     TEXT NOT NULL,
    held_behavior           TEXT NOT NULL,
    override_held_behavior  INTEGER NOT NULL,
    hold_animation          INTEGER NOT NULL,
    max_hold_time           REAL NOT NULL,
    combo_inputs            TEXT NOT NULL,   -- JSON array
    activation_input        TEXT NOT NULL,
    usable_in_air           INTEGER NOT NULL,
    usable_while_swimming   INTEGER NOT NULL,
    usable_while_walking    INTEGER NOT NULL,
    usable_while_mounted    INTEGER NOT NULL,
    networking_max_speed    REAL NOT NULL
);
CREATE INDEX ability_by_name ON ability (name);
CREATE TABLE ability_cost (
    ability_guid      INTEGER NOT NULL,
    cost_ord          INTEGER NOT NULL,      -- position in the ability's Costs array
    stat_ord          INTEGER NOT NULL,      -- position in the group's StatCosts
    enabled_expr      TEXT NOT NULL,         -- the group applies while this is true ('' always)
    triggers_cooldown INTEGER NOT NULL,
    charge_cost_expr  TEXT NOT NULL,
    stat_guid         INTEGER NOT NULL,
    stat_name         TEXT NOT NULL,
    expression        TEXT NOT NULL,         -- the amount of the stat
    PRIMARY KEY (ability_guid, cost_ord, stat_ord)
) WITHOUT ROWID;
CREATE TABLE ability_effect (
    ability_guid INTEGER NOT NULL,
    ord          INTEGER NOT NULL,           -- the EffectsIds map's key
    effect_guid  INTEGER NOT NULL,
    effect_name  TEXT NOT NULL,
    PRIMARY KEY (ability_guid, ord)
) WITHOUT ROWID;
CREATE TABLE ability_hit (
    ability_guid INTEGER NOT NULL,
    ord          INTEGER NOT NULL,           -- the HitsIds map's key
    hit_guid     INTEGER NOT NULL,
    hit_name     TEXT NOT NULL,
    PRIMARY KEY (ability_guid, ord)
) WITHOUT ROWID;

-- Effects reached from the abilities (their EffectsIds, and what their hits
-- apply) and the effects the StaminaRegeneration formula names; hit is each
-- effect's hits. phase is the Effect's list: PreApply, Apply, Tick, Consume,
-- Remove, Expire or End.
CREATE TABLE effect (
    guid                     INTEGER PRIMARY KEY,
    name                     TEXT NOT NULL,
    display_name             TEXT NOT NULL,
    category                 TEXT NOT NULL,
    tags                     TEXT NOT NULL,  -- JSON array (the Tags container)
    effect_tags              TEXT NOT NULL,  -- JSON array (EffectTags)
    application_requirements TEXT NOT NULL,
    infinite_duration        INTEGER NOT NULL,
    duration_expr            TEXT NOT NULL,  -- seconds; '' with infinite_duration
    tick_timer               REAL NOT NULL,  -- seconds between ticks; 0 none
    ignore_initial_tick      INTEGER NOT NULL,
    stack_type               TEXT NOT NULL,
    global_stack_limit       INTEGER NOT NULL,
    stack_duration_refresh   TEXT NOT NULL,
    persists_through_death   INTEGER NOT NULL,
    persists_relog           INTEGER NOT NULL,
    dispellable              INTEGER NOT NULL,
    significant              INTEGER NOT NULL
);
CREATE INDEX effect_by_name ON effect (name);
CREATE TABLE effect_hit (
    effect_guid INTEGER NOT NULL,
    phase       TEXT NOT NULL,
    ord         INTEGER NOT NULL,
    hit_guid    INTEGER NOT NULL,
    hit_name    TEXT NOT NULL,
    PRIMARY KEY (effect_guid, phase, ord)
) WITHOUT ROWID;
CREATE TABLE hit (
    guid                     INTEGER PRIMARY KEY,
    name                     TEXT NOT NULL,
    application_requirements TEXT NOT NULL
);
-- What a hit applies: kind 'apply' (ApplyEffects, to the targets) or 'self'
-- (EffectsAppliedToSelf).
CREATE TABLE hit_effect (
    hit_guid         INTEGER NOT NULL,
    kind             TEXT NOT NULL,
    ord              INTEGER NOT NULL,
    effect_guid      INTEGER NOT NULL,
    effect_name      TEXT NOT NULL,
    requirement_expr TEXT NOT NULL,
    stack_count_expr TEXT NOT NULL,
    duration_expr    TEXT NOT NULL,
    PRIMARY KEY (hit_guid, kind, ord)
) WITHOUT ROWID;
-- StatMod rows of effects (owner_kind 'effect') and hits ('hit'): the stat a
-- mod changes and by how much.
CREATE TABLE owner_stat_mod (
    owner_kind    TEXT NOT NULL,
    owner_guid    INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    stat_mod_guid INTEGER NOT NULL,
    stat_mod_name TEXT NOT NULL,
    stat_guid     INTEGER NOT NULL,
    stat_name     TEXT NOT NULL,
    value_expr    TEXT NOT NULL,
    PRIMARY KEY (owner_kind, owner_guid, ord)
) WITHOUT ROWID;

-- What plan A7 needs to spend and regenerate stamina, key by key; source says
-- where each value comes from. See contentc's package doc for the keys.
CREATE TABLE stamina_rule (
    key    TEXT PRIMARY KEY,
    value  TEXT NOT NULL,
    source TEXT NOT NULL
) WITHOUT ROWID;
-- The effects that stop stamina regenerating, from the StaminaRegeneration
-- formula, and whether each does while the character stands still and while it
-- moves (found by running the formula with that effect present).
CREATE TABLE stamina_regen_disable (
    ord               INTEGER PRIMARY KEY,
    effect_guid       INTEGER NOT NULL,
    effect_name       TEXT NOT NULL,
    disables_standing INTEGER NOT NULL,
    disables_moving   INTEGER NOT NULL
);

-- Plan A7: what worldd's ability runtime runs, as numbers. One row per ability worldd
-- handles (Sprint, Dodge, Rest), from its Ability row and the rows it reaches; every
-- formula was run by contentc (package aocexpr) with a character out of combat and, for
-- the Sprint cost, moving. source says where each number comes from.
CREATE TABLE ability_runtime (
    ability_guid            INTEGER PRIMARY KEY,
    name                    TEXT NOT NULL,
    held_behavior           TEXT NOT NULL,   -- HeldAbilityBehavior without its enum prefix: Smart (held while pressed), Toggle, Naive (a plain press)
    requires_out_of_combat  INTEGER NOT NULL, -- ActivationRequirements fails while the caster is in combat
    min_stamina             REAL NOT NULL,   -- ActivationRequirements holds from this much Stamina (0: no such requirement)
    cost_stat_guid          INTEGER NOT NULL, -- the stat the ability spends (0: it spends none)
    cost_amount             REAL NOT NULL,   -- the cost formula run out of combat: per charge when charge_interval > 0, else once, at the trigger
    cost_requires_moving    INTEGER NOT NULL, -- the cost group's Enabled is IsMoving: charged only while the character moves
    charge_interval         REAL NOT NULL,   -- seconds between the charges of a held ability (0: the cost is paid once)
    cooldown                REAL NOT NULL,   -- the cooldown formula run with no skill bought, seconds
    triggers_cooldown       INTEGER NOT NULL, -- the cost group's bTriggersCooldown: using the ability starts the cooldown
    disable_during_cooldown INTEGER NOT NULL,
    targets_self            INTEGER NOT NULL, -- ApplicableTargets is the caster: the replicated instance lists the caster as its target
    effect_guid             INTEGER NOT NULL, -- the ability's own effect, EffectsIds key 1 (0: none)
    montage_path            TEXT NOT NULL,   -- AnimMontages[0]'s first soft path ('' when none)
    cancel_section          TEXT NOT NULL,   -- CancelSectionName ('' when None): the montage section a cancel jumps to
    source                  TEXT NOT NULL
);
-- The effects those abilities apply and the effects that stop stamina regenerating after
-- them. duration is the Effect row's EffectDuration run (0 with infinite). end_effect_guid
-- is the regeneration-disabling effect the effect's End hit applies to its owner (0:
-- none) and end_effect_duration how long that effect lasts when it is applied.
CREATE TABLE effect_runtime (
    effect_guid          INTEGER PRIMARY KEY,
    name                 TEXT NOT NULL,
    infinite             INTEGER NOT NULL,
    duration             REAL NOT NULL,
    tick_timer           REAL NOT NULL,
    stops_regen_standing INTEGER NOT NULL,   -- from stamina_regen_disable: the StaminaRegeneration formula gives 0 with this effect on the owner
    stops_regen_moving   INTEGER NOT NULL,
    end_effect_guid      INTEGER NOT NULL,
    end_effect_duration  REAL NOT NULL,
    source               TEXT NOT NULL
);
-- What those effects add to a stat: the value is factor * (the owner's source stat) +
-- addend, found by running the StatMod's formula at three values of the source stat and
-- requiring the three results on one line (source_stat_guid 0: a constant, factor 0).
CREATE TABLE effect_stat_runtime (
    effect_guid      INTEGER NOT NULL,
    ord              INTEGER NOT NULL,
    stat_guid        INTEGER NOT NULL,
    stat_name        TEXT NOT NULL,
    source_stat_guid INTEGER NOT NULL,
    source_stat_name TEXT NOT NULL,
    factor           REAL NOT NULL,
    addend           REAL NOT NULL,
    PRIMARY KEY (effect_guid, ord)
) WITHOUT ROWID;

-- Plan B2: items and the starting kit of each class. contentc version 3 (version 4 adds item.quality_curve_guid).
--
-- EEquipSlot, as the client's mappings have it: the slot a worn item is in (Equipment storage's slot index is this value),
-- by the enumerator's name. The pseudo enumerators (_MAIN_EQUIPMENT_CHECK_START ...) and the ends of the enum are left out.
CREATE TABLE equip_slot (
    slot INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
-- One row per ItemRecord of the design database: the facts the character service and worldd need of an item.
-- can_equip is the record's bCanEquip, which is NOT what the client wears: the three starter armor pieces of every
-- class's kit have it 0 and are worn; 374 gear records have it 0 and a slot. occupies_all_equip_slots is a
-- two-handed item: worn, it takes every slot of item_equip_slot (a sword 2H: Primary and Secondary). max_durability is
-- the MaxDurability of the item's ItemStatBlock at default_quality (the rarity band of the quality curve, interpolated,
-- round half up as the retail server's first items show), 0 for an item with no stat block or no MaxDurability.
CREATE TABLE item (
    guid                     INTEGER PRIMARY KEY,
    name                     TEXT NOT NULL,
    level                    INTEGER NOT NULL,
    can_equip                INTEGER NOT NULL,
    occupies_all_equip_slots INTEGER NOT NULL,
    default_quality          INTEGER NOT NULL,
    max_stack_size           INTEGER NOT NULL,
    stat_block_guid          INTEGER NOT NULL,   -- 0: none
    appearance_guid          INTEGER NOT NULL,   -- the ItemAppearance record, 0: none
    max_durability           REAL NOT NULL,
    quality_curve_guid       INTEGER NOT NULL    -- plan B3: the QualityCurve whose bands pick the rarity of a quality, 0: none
);
CREATE INDEX item_by_name ON item (name);
-- The slots an item may be worn in, in the order of the record's EquipSlots (the first free one is used when the client
-- does not ask for a slot). An item that is not worn has none.
CREATE TABLE item_equip_slot (
    item_guid INTEGER NOT NULL,
    ord       INTEGER NOT NULL,
    slot      INTEGER NOT NULL,
    PRIMARY KEY (item_guid, ord)
) WITHOUT ROWID;
-- StartingEquipmentRecord, one set per player class. container is 'Equipment' (EquippedItems: slot is the EEquipSlot) or
-- 'Backpack' (StoredItems: slot is the position in the list). ord is the position in the record.
CREATE TABLE starting_equipment (
    class_guid     INTEGER NOT NULL,
    class_name     TEXT NOT NULL,
    container      TEXT NOT NULL,
    ord            INTEGER NOT NULL,
    slot           INTEGER NOT NULL,
    item_guid      INTEGER NOT NULL,
    variation_guid INTEGER NOT NULL,
    quantity       INTEGER NOT NULL,
    PRIMARY KEY (class_guid, container, ord)
) WITHOUT ROWID;

-- Plan B3: what an item adds to a character's stats, as the stat engine's gear input (contentc version 4; the rules are in
-- services/internal/statprog gear.go and server/worldd/stats gear.h, which read the same rows).
--
-- A QualityCurve: the range of Quality of each rarity, rarity 0 Common, 1 Uncommon, 2 Rare, 3 Heroic, 4 Epic, 5 Legendary,
-- 6 Artifact (Quality x..y is that rarity).
CREATE TABLE quality_curve (
    guid INTEGER NOT NULL,
    ord  INTEGER NOT NULL,   -- the rarity
    x    REAL NOT NULL,
    y    REAL NOT NULL,
    PRIMARY KEY (guid, ord)
) WITHOUT ROWID;
-- An ItemStatBlock's Stats (item.stat_block_guid): per stat, the Min and Max at each rarity. An item's value of a stat is
-- Min + (Max - Min) * (Quality - X) / (Y - X) in the rarity band its Quality falls in; MaxDurability (the item's durability)
-- is among them and is no stat of the player's.
CREATE TABLE stat_block_stat (
    block_guid    INTEGER NOT NULL,
    stat_guid     INTEGER NOT NULL,
    common_min    REAL NOT NULL, common_max    REAL NOT NULL,
    uncommon_min  REAL NOT NULL, uncommon_max  REAL NOT NULL,
    rare_min      REAL NOT NULL, rare_max      REAL NOT NULL,
    heroic_min    REAL NOT NULL, heroic_max    REAL NOT NULL,
    epic_min      REAL NOT NULL, epic_max      REAL NOT NULL,
    legendary_min REAL NOT NULL, legendary_max REAL NOT NULL,
    artifact_min  REAL NOT NULL, artifact_max  REAL NOT NULL,
    PRIMARY KEY (block_guid, stat_guid)
) WITHOUT ROWID;
-- The SetBonus records an item belongs to (ItemRecord.SetBonusIds), in the record's order.
CREATE TABLE item_set (
    item_guid INTEGER NOT NULL,
    ord       INTEGER NOT NULL,
    set_guid  INTEGER NOT NULL,
    PRIMARY KEY (item_guid, ord)
) WITHOUT ROWID;
-- A SetBonus's SetStatBonuses: per tier (the pieces of the set that must be worn), per bonus (tier_ord, bonus_ord: the
-- record's order), the stat it raises and the value at each rarity (NULL: the record has none for that rarity, which adds 0).
-- Every tier up to the worn count applies, at the lowest rarity among the worn pieces of the set.
CREATE TABLE set_bonus (
    set_guid  INTEGER NOT NULL,
    tier_ord  INTEGER NOT NULL,
    pieces    INTEGER NOT NULL,
    bonus_ord INTEGER NOT NULL,
    stat_guid INTEGER NOT NULL,
    common    REAL, uncommon REAL, rare REAL, heroic REAL, epic REAL, legendary REAL, artifact REAL,
    PRIMARY KEY (set_guid, tier_ord, bonus_ord)
) WITHOUT ROWID;
-- The GameplayTags of an item, in the record's order (the stat formulas ask EquippedItemInSlotHasTag).
CREATE TABLE item_tag (
    item_guid INTEGER NOT NULL,
    ord       INTEGER NOT NULL,
    tag       TEXT NOT NULL,
    PRIMARY KEY (item_guid, ord)
) WITHOUT ROWID;
-- The compiled stat formula graph (package statprog): one row, written after every table above by contentc, which compiles
-- the graph from this file's own rows, encodes it and decodes the bytes again as a check. worldd parses and verifies data
-- before it serves a login. version is the bundle format's version, mode its arithmetic (float32, double or single),
-- stats and classes what it holds, crc32 the body's checksum as the bundle's header has it.
CREATE TABLE stat_program (
    id      INTEGER PRIMARY KEY CHECK (id = 1),
    version INTEGER NOT NULL,
    mode    TEXT NOT NULL,
    stats   INTEGER NOT NULL,
    classes INTEGER NOT NULL,
    crc32   INTEGER NOT NULL,
    data    BLOB NOT NULL
);

-- Plan D1 (contentc version 5): using an item, what it gives, the level curves and the ranks a level grants.
--
-- An item that can be used: its ItemActivationArchetype (activation is EItemActivationAction without its prefix;
-- ActivateActionResponses runs item_action's ActionResponses in order) and whether a use consumes one of the stack.
CREATE TABLE item_use (
    item_guid          INTEGER PRIMARY KEY,
    activation         TEXT NOT NULL,
    consume_on_use     INTEGER NOT NULL,
    cooldown           REAL NOT NULL,
    disabled_in_combat INTEGER NOT NULL
);
-- The item's ActivationActionsIds, in the record's order.
CREATE TABLE item_action (
    item_guid   INTEGER NOT NULL,
    ord         INTEGER NOT NULL,
    action_guid INTEGER NOT NULL,
    PRIMARY KEY (item_guid, ord)
) WITHOUT ROWID;
-- Every ActionResponse record an item names: its ResponseType (EActionResponse without its prefix), the class of a
-- SetPrimaryArchetype (0 for the others) and its reward tables (action_reward, in the record's order).
CREATE TABLE action_response (
    guid                  INTEGER PRIMARY KEY,
    name                  TEXT NOT NULL,
    response_type         TEXT NOT NULL,
    class_guid            INTEGER NOT NULL,
    class_name            TEXT NOT NULL,
    remove_up_to_quantity INTEGER NOT NULL
);
CREATE TABLE action_reward (
    action_guid       INTEGER NOT NULL,
    ord               INTEGER NOT NULL,
    reward_table_guid INTEGER NOT NULL,
    PRIMARY KEY (action_guid, ord)
) WITHOUT ROWID;
-- A RewardTable an action names. level_below is its Predicate 'GetPlayerLevel() < N' as N (0: no predicate; -1: a predicate
-- contentc does not model, which refuses the table). modelled is 1 when every container grants always and selects all of its
-- rewards (what the GM items use); 0 when it does not, and the rows below are not written for it.
CREATE TABLE reward_table (
    guid        INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    predicate   TEXT NOT NULL,
    level_below INTEGER NOT NULL,
    modelled    INTEGER NOT NULL
);
-- The ItemRewards of a modelled table, in the record's order (containers, then rewards, then items).
CREATE TABLE reward_item (
    table_guid     INTEGER NOT NULL,
    ord            INTEGER NOT NULL,
    item_guid      INTEGER NOT NULL,
    variation_guid INTEGER NOT NULL,
    quantity       INTEGER NOT NULL,
    PRIMARY KEY (table_guid, ord)
) WITHOUT ROWID;
-- The ExperienceRewards of a modelled table. experience_type is EExperienceContextType without its prefix. target_level
-- is the level the reward's ExperienceValue raises the player to (the GM level scrolls: (curve(target) - curve(level) +
-- debt_ratio * xp_of_level) / max(0.001, 1 + xp_add_mult) + margin, which contentc has checked the expression against
-- with package aocexpr at every level and several debts and multipliers; 0 for an expression that is not of that shape,
-- which refuses the reward). curve_guid is the StatCurve the expression reads, mult_stat_guid the stat it adds to 1.
CREATE TABLE reward_xp (
    table_guid      INTEGER NOT NULL,
    ord             INTEGER NOT NULL,
    experience_type TEXT NOT NULL,
    target_level    INTEGER NOT NULL,
    margin          REAL NOT NULL,
    curve_guid      INTEGER NOT NULL,
    mult_stat_guid  INTEGER NOT NULL,
    expression      TEXT NOT NULL,
    PRIMARY KEY (table_guid, ord)
) WITHOUT ROWID;
-- LevelingTypeDef: a leveling type (Adventuring, the weapon lines, the professions), its cap (LevelCapOverride, 0 for
-- none), whether it carries an experience debt, the stat its XPMultiplier adds to 1 (0: none) and its LevelUpCurve
-- (leveling_curve: the XP total at which each level is reached, from the record's keys).
CREATE TABLE leveling_type (
    guid              INTEGER PRIMARY KEY,
    name              TEXT NOT NULL UNIQUE,
    experience_type   TEXT NOT NULL,
    level_cap         INTEGER NOT NULL,
    uses_debt         INTEGER NOT NULL,
    xp_mult_stat_guid INTEGER NOT NULL
);
CREATE TABLE leveling_curve (
    type_guid INTEGER NOT NULL,
    level     INTEGER NOT NULL,
    xp        INTEGER NOT NULL,
    PRIMARY KEY (type_guid, level)
) WITHOUT ROWID;
-- The SkillTreeNodes a level grants by itself (bAutoGranted with a LevelRequirement), by tree: the node, its skill, the
-- skill's first rank and the rank's passive effect (0: none). skill_rank_stat_mod holds the constant stat mods of that
-- effect when it is infinite (a mod whose formula is a number; Endurance Training's MaxStamina +10), which the stat
-- engine takes as an input the way it takes gear; a finite effect's mods (Sprint Training's, 1 s on the sprint) are not.
CREATE TABLE skill_auto_grant (
    tree_guid          INTEGER NOT NULL,
    tree_name          TEXT NOT NULL,
    leveling_type_guid INTEGER NOT NULL,
    node_guid          INTEGER NOT NULL,
    node_name          TEXT NOT NULL,
    level_requirement  INTEGER NOT NULL,
    skill_guid         INTEGER NOT NULL,
    skill_name         TEXT NOT NULL,
    rank_guid          INTEGER NOT NULL,
    effect_guid        INTEGER NOT NULL,
    PRIMARY KEY (tree_guid, node_guid)
) WITHOUT ROWID;
CREATE TABLE skill_rank_stat_mod (
    rank_guid INTEGER NOT NULL,
    ord       INTEGER NOT NULL,
    stat_guid INTEGER NOT NULL,
    stat_name TEXT NOT NULL,
    value     REAL NOT NULL,
    PRIMARY KEY (rank_guid, ord)
) WITHOUT ROWID;
-- Plan D1, stage 2: the talents. skill_tree is every SkillTree with its kind: 'class' (a class's ActiveSkillTreeId: class_guid is
-- the class), 'universal', 'weapon' (a WeaponSkillLines record's tree: weapon_line_guid), 'profession' (a gathering, crafting or
-- processing tree) or 'other' (the staging and test trees). skill_node is each tree's nodes in the tree's order (a node that
-- names no skill and no slot is left out; a node can be in several trees, so the key is the pair). A node names its skill
-- directly (slot_guid 0, opt 0) or, when it has none, names a SkillTreeSlot: a talent choice whose SkillsIds are the skills that can
-- be picked for it. A slot node is written once per option (the same node_guid and ord, opt 0, 1, ... in the slot's order, slot_guid
-- the slot): the character may own one option of a node's slot at a time. The row also holds: the level and the points spent in the
-- tree it needs, whether a level grants it by itself (auto_granted: its first rank) and whether the client puts it on the action
-- bar (auto_hotbar), and whether all its requirements are needed or one (all_required). skill_node_require is a node's
-- SkillsRequired as the SKILLS that must be owned (a requirement that names no node and no skill is written with 0 and is met; one
-- that names a slot node and no option of it is written with slot_node_guid, the node, and is met by any option of that node the
-- character owns);
-- skill_node_exclusive its MutuallyExclusiveSkillsIds. skill_rank is each skill's ranks in order with the rank's ability (0 none)
-- and passive effect (0 none; effect_infinite 1 when the effect has no duration or a duration that is a formula, else
-- effect_duration seconds). skill_rank_cost is what a rank costs in points of a type (skill_point_type: Combat_Archetype for a
-- class tree, Combat_Stamina for the Universal tree, Combat_Weapon for a weapon tree); an auto-granted rank costs nothing.
-- leveling_point is what reaching a level of a leveling type gives (the Adventuring entry of level n gives a point of Archetype and
-- one of Stamina; a weapon line's one of Weapon). item_weapon is the combo ability (AttackAbilityId) and the weapon line of
-- every weapon item (an ItemRecord with a WeaponSkillLineId or an AttackAbilityId).
CREATE TABLE skill_point_type (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE leveling_point (
    type_guid       INTEGER NOT NULL,
    level           INTEGER NOT NULL,
    point_type_guid INTEGER NOT NULL,
    quantity        INTEGER NOT NULL,
    PRIMARY KEY (type_guid, level, point_type_guid)
) WITHOUT ROWID;
CREATE TABLE skill_tree (
    guid               INTEGER PRIMARY KEY,
    name               TEXT NOT NULL,
    kind               TEXT NOT NULL,
    leveling_type_guid INTEGER NOT NULL,
    class_guid         INTEGER NOT NULL,
    weapon_line_guid   INTEGER NOT NULL
);
CREATE TABLE skill_node (
    tree_guid         INTEGER NOT NULL,
    node_guid         INTEGER NOT NULL,
    ord               INTEGER NOT NULL,
    name              TEXT NOT NULL,
    skill_guid        INTEGER NOT NULL,
    skill_name        TEXT NOT NULL,
    level_requirement INTEGER NOT NULL,
    point_requirement INTEGER NOT NULL,
    auto_granted      INTEGER NOT NULL,
    auto_hotbar       INTEGER NOT NULL,
    all_required      INTEGER NOT NULL,
    opt               INTEGER NOT NULL,
    slot_guid         INTEGER NOT NULL,
    PRIMARY KEY (tree_guid, node_guid, ord, opt)
) WITHOUT ROWID;
CREATE TABLE skill_node_require (
    tree_guid       INTEGER NOT NULL,
    node_guid       INTEGER NOT NULL,
    ord             INTEGER NOT NULL,
    skill_guid      INTEGER NOT NULL,
    slot_node_guid  INTEGER NOT NULL,
    PRIMARY KEY (tree_guid, node_guid, ord)
) WITHOUT ROWID;
CREATE TABLE skill_node_exclusive (
    tree_guid  INTEGER NOT NULL,
    node_guid  INTEGER NOT NULL,
    ord        INTEGER NOT NULL,
    skill_guid INTEGER NOT NULL,
    PRIMARY KEY (tree_guid, node_guid, ord)
) WITHOUT ROWID;
CREATE TABLE skill_rank (
    skill_guid      INTEGER NOT NULL,
    ord             INTEGER NOT NULL,
    rank_guid       INTEGER NOT NULL,
    name            TEXT NOT NULL,
    ability_guid    INTEGER NOT NULL,
    ability_name    TEXT NOT NULL,
    effect_guid     INTEGER NOT NULL,
    effect_name     TEXT NOT NULL,
    effect_infinite INTEGER NOT NULL,
    effect_duration REAL NOT NULL,
    PRIMARY KEY (skill_guid, ord)
) WITHOUT ROWID;
CREATE INDEX skill_rank_by_rank ON skill_rank (rank_guid);
CREATE TABLE skill_rank_cost (
    rank_guid       INTEGER NOT NULL,
    ord             INTEGER NOT NULL,
    point_type_guid INTEGER NOT NULL,
    quantity        INTEGER NOT NULL,
    PRIMARY KEY (rank_guid, ord)
) WITHOUT ROWID;
CREATE TABLE item_weapon (
    item_guid           INTEGER PRIMARY KEY,
    attack_ability_guid INTEGER NOT NULL,
    skill_line_guid     INTEGER NOT NULL,
    skill_line_name     TEXT NOT NULL
);
`

// Create makes the empty schema in db.
func Create(db *sql.DB) error {
	if _, err := db.Exec(DDL); err != nil {
		return fmt.Errorf("content: create schema: %w", err)
	}
	if _, err := db.Exec(AbilityDDL); err != nil {
		return fmt.Errorf("content: create the ability tables: %w", err)
	}
	if err := createPopulation(db); err != nil {
		return err
	}
	if err := createCombat(db); err != nil {
		return err
	}
	if err := createLoot(db); err != nil {
		return err
	}
	if err := createMounts(db); err != nil {
		return err
	}
	return createNodes(db)
}
