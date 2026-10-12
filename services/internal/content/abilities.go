// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

// The ability tables (plan D2, contentc version 6; the groundwork was written beside version 5, which D1 owns).
//
// Where the older ability, effect and hit tables hold what plan A7 needs for Sprint, Dodge and Rest, these hold every
// ability of every class and what it reaches, as plain data and as expressions:
//
//	(the ranks are plan D1's: skill_tree, skill_node and skill_rank hold every tree's ranks with the ability or effect each grants, the
//	level it needs and whether the game grants it by itself; a class's abilities are the ranks of its tree. There is no table of ranks
//	here: the ab_* tables point at those ranks' abilities and effects)
//	ab_ability           the Ability rows reached from the ranks: targeting, held behaviour, usability, cooldown group
//	ab_targeting         ... its targeting records (TargetingRecordsIds), ab_cost and ab_effect_cost what it spends,
//	ab_montage           its montages by AnimationIndex (the position in AnimMontages) with the soft path, the direction, the
//	                     expression that picks it and, where a retail capture shows it, the montage's NetGUID
//	ab_ability_effect,
//	ab_ability_hit       the effects it applies by itself and the hits it fires, directly (source 'hit'), through a
//	                     projectile or through a lingering zone
//	ab_hit, ab_hit_effect,
//	ab_hit_projectile,
//	ab_hit_zone          a hit: who it reaches, what it applies, fires and spawns
//	ab_projectile, ab_projectile_hit,
//	ab_zone, ab_zone_hit projectiles and lingering zones as plain data
//	ab_effect,
//	ab_effect_hit        an effect: duration, ticks, stacking, persistence, its hits per phase
//	ab_mod               the stat mods of effects and hits: the stat and the value as an expression
//	ab_expr              every expression of the tables above once (the same text and terms share a row): the text and, after
//	                     contentc has compiled the stat program, the program (an index in the bundle of stat_program, listed in
//	                     its Exprs) or the reason it does not compile
//
// An expression column holds an ab_expr id, 0 for none (an empty text). Guids are the design database's. A montage NetGUID is a
// uint64 stored as its int64 bits.
const AbilityDDL = `
CREATE TABLE ab_expr (
    id          INTEGER PRIMARY KEY,
    text        TEXT NOT NULL,        -- AoCExpression text
    terms       TEXT NOT NULL,        -- JSON object: a StatMod's ValueInputTerms, {"<term guid>": "<text>"}; '{}' for none
    program     INTEGER NOT NULL,     -- the program in stat_program's bundle, -1 when there is none (not compiled, or it does not compile)
    uses_target INTEGER NOT NULL,     -- the text calls GetTarget(): compiled as the caster (see statprog.ExprResult)
    error       TEXT NOT NULL         -- why it does not compile; '' when it does
);
CREATE TABLE ab_ability (
    guid                   INTEGER PRIMARY KEY,
    name                   TEXT NOT NULL,
    display_name           TEXT NOT NULL,
    tags                   TEXT NOT NULL,   -- JSON array
    held_behavior          TEXT NOT NULL,
    activation_input       TEXT NOT NULL,
    target_requirement     TEXT NOT NULL,   -- EAbilityActivationTargetRequirement: Always (a target is needed), Never ...
    fallback_target        TEXT NOT NULL,
    applicable_targets     INTEGER NOT NULL, -- mask of EAffectedTarget: Self 1, Enemy 2, Ally 4, Party 8, Raid 16
    applicable_types       INTEGER NOT NULL,
    weapon_requirement     TEXT NOT NULL,
    shared_cooldown_tag    TEXT NOT NULL,
    disable_during_cooldown INTEGER NOT NULL,
    max_hold_time          REAL NOT NULL,
    hold_animation         INTEGER NOT NULL,
    usable_in_air          INTEGER NOT NULL,
    usable_while_swimming  INTEGER NOT NULL,
    usable_while_walking   INTEGER NOT NULL,
    usable_while_mounted   INTEGER NOT NULL,
    usable_from_hotbar     INTEGER NOT NULL,
    area_range             REAL NOT NULL,
    area_radius            REAL NOT NULL,
    networking_max_speed   REAL NOT NULL,
    cooldown_expr          INTEGER NOT NULL, -- seconds
    charges_expr           INTEGER NOT NULL, -- CooldownCharges: the charges (empty: one)
    cast_expr              INTEGER NOT NULL, -- DynamicCastDuration: seconds (empty: the montage's)
    requirement_expr       INTEGER NOT NULL, -- ActivationRequirements
    can_break_expr         INTEGER NOT NULL,
    has_proc_expr          INTEGER NOT NULL,
    extra                  TEXT NOT NULL     -- JSON: the guids of ModifyCooldownsIds, ReactionsIds and TeleportRecordsIds, stored and not followed
);
CREATE INDEX ab_ability_by_name ON ab_ability (name);
CREATE TABLE ab_targeting (
    ability_guid   INTEGER NOT NULL,
    key            INTEGER NOT NULL,      -- the TargetingRecordsIds map's key
    targeting_guid INTEGER NOT NULL,
    targeting_name TEXT NOT NULL,
    PRIMARY KEY (ability_guid, key)
) WITHOUT ROWID;
CREATE TABLE ab_cost (
    ability_guid      INTEGER NOT NULL,
    cost_ord          INTEGER NOT NULL,   -- position in the ability's Costs
    stat_ord          INTEGER NOT NULL,   -- position in the group's StatCosts, -1 for a group without one
    enabled_expr      INTEGER NOT NULL,
    triggers_cooldown INTEGER NOT NULL,
    charge_cost_expr  INTEGER NOT NULL,
    stat_guid         INTEGER NOT NULL,
    stat_name         TEXT NOT NULL,
    amount_expr       INTEGER NOT NULL,   -- the amount of the stat
    PRIMARY KEY (ability_guid, cost_ord, stat_ord)
) WITHOUT ROWID;
CREATE TABLE ab_effect_cost (
    ability_guid INTEGER NOT NULL,
    cost_ord     INTEGER NOT NULL,
    ord          INTEGER NOT NULL,
    effect_guid  INTEGER NOT NULL,        -- the effect the filter names ("GetEffect() == <ref>"), 0 for a filter of another shape
    filter       TEXT NOT NULL,
    num_expr     INTEGER NOT NULL,        -- how many stacks it consumes
    PRIMARY KEY (ability_guid, cost_ord, ord)
) WITHOUT ROWID;
CREATE TABLE ab_montage (
    ability_guid  INTEGER NOT NULL,
    anim_index    INTEGER NOT NULL,       -- the position in AnimMontages: the AnimationIndex of a ServerTriggerAbility
    soft_path     TEXT NOT NULL,          -- the first AbilityAnimationSoftPaths entry
    paths         TEXT NOT NULL,          -- JSON array of all of them
    direction     TEXT NOT NULL,
    select_expr   INTEGER NOT NULL,       -- AnimExpression: which montage a target gets
    start_section TEXT NOT NULL,
    hit_keys      TEXT NOT NULL,          -- JSON array
    targeting_key INTEGER NOT NULL,
    net_guid      INTEGER,                -- NULL: no retail capture names the montage; else a uint64 as its int64 bits (montage_ids.tsv, contentc)
    PRIMARY KEY (ability_guid, anim_index)
) WITHOUT ROWID;
CREATE TABLE ab_ability_effect (
    ability_guid INTEGER NOT NULL,
    key          INTEGER NOT NULL,
    effect_guid  INTEGER NOT NULL,
    PRIMARY KEY (ability_guid, key)
) WITHOUT ROWID;
-- source 'hit': HitsIds (owner_guid 0); 'projectile': a hit of one of the ability's ProjectilesIds; 'zone': a hit of one of its
-- LingeringEffectsIds (owner_guid is the projectile or zone, key the ability's map key of it, role the zone's list the hit is in).
CREATE TABLE ab_ability_hit (
    ability_guid INTEGER NOT NULL,
    source       TEXT NOT NULL,
    key          INTEGER NOT NULL,
    owner_guid   INTEGER NOT NULL,
    role         TEXT NOT NULL,
    ord          INTEGER NOT NULL,
    hit_guid     INTEGER NOT NULL,
    PRIMARY KEY (ability_guid, source, key, owner_guid, role, ord)
) WITHOUT ROWID;
CREATE TABLE ab_hit (
    guid               INTEGER PRIMARY KEY,
    name               TEXT NOT NULL,
    net_type           TEXT NOT NULL,   -- EAttackNetworkType: NET_ServerVerify (the client's RequestHitVerification) ...
    applicable_targets INTEGER NOT NULL,
    applicable_types   INTEGER NOT NULL,
    fallback_target    TEXT NOT NULL,
    requirement_expr   INTEGER NOT NULL,
    delay_expr         INTEGER NOT NULL,
    threat_expr        INTEGER NOT NULL,
    max_targets_expr   INTEGER NOT NULL,
    extra              TEXT NOT NULL    -- JSON: ModifyCooldownsIds, ModifyEffectsIds, EffectsToRemoveIds, ReactionsIds, TeleportId, ResurrectId, EventTags
);
-- kind 'apply' (ApplyEffects, to the targets) or 'self' (EffectsAppliedToSelf).
CREATE TABLE ab_hit_effect (
    hit_guid         INTEGER NOT NULL,
    kind             TEXT NOT NULL,
    ord              INTEGER NOT NULL,
    effect_guid      INTEGER NOT NULL,
    requirement_expr INTEGER NOT NULL,
    stack_count_expr INTEGER NOT NULL,
    duration_expr    INTEGER NOT NULL,
    on_hit_behavior  TEXT NOT NULL,     -- 'self' only
    max_applications INTEGER NOT NULL,  -- 'self' only
    PRIMARY KEY (hit_guid, kind, ord)
) WITHOUT ROWID;
-- kind 'fire' (ProjectilesToFire) or 'spawn' (ProjectilesToSpawn).
CREATE TABLE ab_hit_projectile (
    hit_guid        INTEGER NOT NULL,
    kind            TEXT NOT NULL,
    ord             INTEGER NOT NULL,
    projectile_guid INTEGER NOT NULL,
    PRIMARY KEY (hit_guid, kind, ord)
) WITHOUT ROWID;
CREATE TABLE ab_hit_zone (
    hit_guid  INTEGER NOT NULL,
    ord       INTEGER NOT NULL,
    zone_guid INTEGER NOT NULL,
    PRIMARY KEY (hit_guid, ord)
) WITHOUT ROWID;
CREATE TABLE ab_projectile (
    guid            INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    projectile_type TEXT NOT NULL,
    speed           REAL NOT NULL,
    range           REAL NOT NULL,
    life_time       REAL NOT NULL,
    max_hits        INTEGER NOT NULL,
    chain_hit_count INTEGER NOT NULL,
    chain_radius    REAL NOT NULL,
    sphere_radius   REAL NOT NULL
);
CREATE TABLE ab_projectile_hit (
    projectile_guid INTEGER NOT NULL,
    ord             INTEGER NOT NULL,
    hit_guid        INTEGER NOT NULL,
    PRIMARY KEY (projectile_guid, ord)
) WITHOUT ROWID;
CREATE TABLE ab_zone (
    guid        INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    life_time   REAL NOT NULL,
    tick_rate   REAL NOT NULL,
    use_global_tick_rate INTEGER NOT NULL,
    charges     INTEGER NOT NULL,
    delay_time  REAL NOT NULL,
    delay_type  TEXT NOT NULL,
    speed       REAL NOT NULL,
    expire_after_hits_expr INTEGER NOT NULL,
    expire_if_expr         INTEGER NOT NULL,
    targeting_guid         INTEGER NOT NULL   -- LingeringHitTargetingInfoId
);
-- role: the zone's list the hit is in: InitialHitsIds, LingeringHitsIds (every tick), OnBeginOverlapHitsIds, OnEndOverlapHitsIds, EndHitsIds.
CREATE TABLE ab_zone_hit (
    zone_guid INTEGER NOT NULL,
    role      TEXT NOT NULL,
    ord       INTEGER NOT NULL,
    hit_guid  INTEGER NOT NULL,
    PRIMARY KEY (zone_guid, role, ord)
) WITHOUT ROWID;
CREATE TABLE ab_effect (
    guid                   INTEGER PRIMARY KEY,
    name                   TEXT NOT NULL,
    display_name           TEXT NOT NULL,
    category               TEXT NOT NULL,
    tags                   TEXT NOT NULL,   -- JSON array (Tags)
    effect_tags            TEXT NOT NULL,   -- JSON array (EffectTags)
    infinite_duration      INTEGER NOT NULL,
    duration_expr          INTEGER NOT NULL, -- seconds; 0 with infinite_duration
    tick_timer             REAL NOT NULL,    -- seconds between ticks, 0 none
    ignore_initial_tick    INTEGER NOT NULL,
    stack_type             TEXT NOT NULL,
    global_stack_limit     INTEGER NOT NULL,
    in_place_stack_limit_expr INTEGER NOT NULL,
    stack_duration_refresh TEXT NOT NULL,
    persists_through_death INTEGER NOT NULL,
    persists_relog         INTEGER NOT NULL,
    dispellable            INTEGER NOT NULL,
    significant            INTEGER NOT NULL,
    requirement_expr       INTEGER NOT NULL, -- ApplicationRequirements
    crowd_control_guid     INTEGER NOT NULL,
    extra                  TEXT NOT NULL     -- JSON: ReactionsIds, HitModsIds, FragilityDefinitionsIds: stored, not followed
);
CREATE INDEX ab_effect_by_name ON ab_effect (name);
-- phase: PreApply, Apply, Tick, Consume, Remove, Expire or End.
CREATE TABLE ab_effect_hit (
    effect_guid INTEGER NOT NULL,
    phase       TEXT NOT NULL,
    ord         INTEGER NOT NULL,
    hit_guid    INTEGER NOT NULL,
    PRIMARY KEY (effect_guid, phase, ord)
) WITHOUT ROWID;
-- owner_kind 'effect' or 'hit': the StatMods of the owner. value_expr is the StatMod's Value with its ValueInputTerms.
CREATE TABLE ab_mod (
    owner_kind    TEXT NOT NULL,
    owner_guid    INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    stat_mod_guid INTEGER NOT NULL,
    stat_guid     INTEGER NOT NULL,
    stat_name     TEXT NOT NULL,
    value_expr    INTEGER NOT NULL,
    shield_absorb INTEGER NOT NULL,   -- bUseShieldAbsorb
    PRIMARY KEY (owner_kind, owner_guid, ord)
) WITHOUT ROWID;
`
