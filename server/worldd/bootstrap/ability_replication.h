// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/ability_replication.h
//
//  What worldd's ability runtime (session/abilities.h, plan A7) changes on the pawn, as plain data, and what
//  PlayerBootstrap::apply_ability_change() makes of it: the Stamina and regeneration stat deltas of the
//  StatsComponent (StatRepInt32Owner items), its CurrentEffects items, and the AbilityComponent's
//  ReplicatedActiveAbilities and Cooldowns items.  Pure data: no sockets, no Connection, no clock.
//
//  Evidence for each form (spec section 7; research 2026-10-01, `scratchpad/a7/evidence.md`):
//    * The original server answered one ability in `replay_data.bin`: General_Rest (pkt#8175-8320), 6 packets whose
//      items test_worldd_abilities compares bit for bit: an active-ability item, an effect item and two stat deltas,
//      then the instance replaced by a second one, the effect added again, the montage jumped to its end section and
//      the effect removed.  Those packets are the reference for every item below.
//    * Items go out as FFastArraySerializer deltas (libs/aocnet/protocol/repl/fast_array.h): an item is added with the
//      next ReplicationID, changed in place, or removed (the original moves the array key by two for a removal).
//    * The retail server's replies to Sprint and Dodge (PCAPRepo captures, research 2026-10-02: 100 Sprint and 31 Dodge replies
//      in 67 and 16 captures; the settled numbers in the worldd README) are the reference for the rest: a Sprint's reply is one
//      packet with the ability item (MontageGroup "Sprint"), the MoveSpeedMult stat of the EveryoneProxy array (1.0 + 0.2) and
//      the infinite Sprint effect (TickTime 1.0), a Dodge's has the Cooldowns item {AbilityGuid, bCooldownTriggered 1,
//      InitialCooldown 0.65, start and end on the synchronized clock, Charges 1}, its ability item (the montage by the
//      trigger's AnimationIndex, MeshYaw) and the 0.5 s Evasion effect; the montage's export rides in the first item's bunch.
//
//  Keys.  The runtime names every item it adds by a key of its own, unique within its array: an effect's EffectId (a
//  per-owner counter from 0: the capture's two effects are 0 and 1), an ability instance's ServerInstanceId (the server's
//  own counter of the instances it started, from 0: not the client's ClientInstanceId), a cooldown's ability Guid.
//  PlayerBootstrap maps keys to the arrays' ReplicationIDs.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/value.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::bootstrap {

/// A stat of the Owner, OwnerProxy or EveryoneProxy array set to a value (by record Guid; the item's other members stay, unless
/// `buckets` says the item carries base and equipment too: plan B3's gear changes them with the value).
struct StatChange {
    int64_t record = 0;
    float   value = 0;
    bool    buckets = false;
    float   base = 0;
    float   equipment = 0;
    /// Plan D1, stage 2: the audience mask of the stat's array (1 Owner, 5 OwnerProxy, 7 EveryoneProxy) when the stat has no item there yet and
    /// should get one (a class change brings the new class's own resource: the arrays were laid out for the old class); 0: an item must exist.
    uint8_t add_to_replication = 0;
};

/// A CurrentEffects item.
struct EffectItem {
    int64_t effect_id = 0;       // Items.EffectId and the runtime's key
    int64_t effect_guid = 0;     // Items.EffectGUID: the effect record's Guid
    double  start_time = 0;      // Items.StartTime: on the synchronized clock of ClientSetTime's NewSynchronizedTime
    float   duration = 0;        // Items.Duration; 3.40282347e+38 (FLT_MAX) for an infinite effect
    float   tick_time = 0;       // Items.TickTime: the effect's TickTimer (0 for none)
    int32_t stacks = 1;          // Items.CurrentStacks
};

/// A ReplicatedActiveAbilities item.  Instigator is the pawn; Targets is the pawn's CollisionCylinder when targets_self.
struct ActiveAbilityItem {
    int64_t     ability_guid = 0;
    int64_t     server_instance_id = 0;   // also the key
    bool        targets_self = false;
    /// plan E1, stage 3: the other actors' components the ability is aimed at (an NPC's collision cylinder): the item's Targets
    std::vector<protocol::emit::FIntrepidNetworkGUID> target_objects;
    /// E1 stage 3 fix 2: the actor each target object belongs to, in the same order (the update exports the component as that actor's
    /// "CollisionCylinder" so the client can resolve the item's Targets: its homing projectile flies at it and its hit reaction plays)
    std::vector<protocol::emit::FIntrepidNetworkGUID> target_actors;
    std::string montage_path;             // the soft path of the montage ("" or one worldd has no export for: Montage unset)
    uint64_t    montage_guid = 0;         // plan D2: the montage's static NetGUID where a capture names it and montage_for(path) does not know it (0: none)
    std::string montage_group = "DefaultGroup";   // MontageGroup: the Rest's and the Dodge's are "DefaultGroup", the Sprint's "Sprint"
    float       montage_position = 0;
    bool        may_jump_to_position = false;
    int32_t     animation_counter = 0;
    uint16_t    mesh_yaw = 0;             // MeshYaw: a 16-bit angle (65536 = 360 degrees); the Dodge's direction when it is not the montage's
};
// C | O | A with Cyb

/// A Cooldowns item (keyed by the ability's Guid).
struct CooldownItem {
    int64_t ability_guid = 0;
    bool    triggered = false;       // bCooldownTriggered
    float   initial_cooldown = 0;    // seconds
    double  start_time = 0;          // CooldownSynchronizedStartTime
    double  end_time = 0;            // CooldownSynchronizedEndTime
    int32_t charges = 0;
};

/// One shield of the TempHealth float array (plan D2, stage 2): the item of StatRepFloatArrayEveryoneProxy whose record is TempHealth lists them in
/// `Values`, one element per shield effect on the pawn: an FGuid of its own, the amount it was given (Base) and what is left of it (Value).
/// [R: retail captures 38, 44, 50, 249, 516: Base = Value = the amount when it is given, EquipmentValue 0, the array emptied when the effect ends.]
struct ShieldElement {
    uint32_t guid[4] = {0, 0, 0, 0};
    float    base = 0;
    float    value = 0;
    bool operator==(const ShieldElement&) const = default;
};

/// One stat mod of a hit's confirmation: the StatMod record, the value its expression gave (the amount: a heal's, a shield's, a damage's) and the
/// part that took effect on the target (a heal's amount clamped by the Health that was missing: 0 at full Health). The client draws its floating
/// combat numbers from these two [R: ClientHitConfirmationsReliable, 11,362 targets; research/allclasses].
struct HitStatModOut {
    int64_t stat_mod_guid = 0;
    float   value = 0;
    float   effective = 0;
    bool operator==(const HitStatModOut&) const = default;
};

/// What the server confirms to the client about a hit on the pawn (one FHitInst with one target, the pawn): ClientHitConfirmationsReliable's
/// content. `source_is_actor`: the hit's Source is the pawn (an ability's or an effect's hit); a projectile's hit has none [R: source (0,0,0,0)].
/// A target of a hit that is not the pawn (plan E1, stage 3): the component that was hit (an NPC's collision cylinder), what the hit did to its stats and the flags
/// the server rolled for it (Crit 1, Lethal 2, Block 4, Miss 8 ...).
struct HitTargetOut {
    /// The HitComp: the target ACTOR's NetGUID [R: research/hitfx 1: 7,450 retail targets, every one the actor (the NPC pawn, a player pawn), never a
    /// component]. E1 stage 3 fix 2: the draft named the cylinder (the actor's id + 24), a GUID the client never got an export for and resolved to nothing:
    /// no impact, no hit reaction, no floating number (the user's run of 2026-10-06).
    protocol::emit::FIntrepidNetworkGUID component;
    /// The target's collision cylinder, exported with the confirmation (as that actor's "CollisionCylinder": flags 7, checksum 0xb876932f) unless the
    /// client acknowledged it already [R: 1402 packet 239 exports each of its four targets' cylinders in the confirmation's bunch]; unset: nothing to export.
    protocol::emit::FIntrepidNetworkGUID cylinder;
    /// ImpactLocation and ImpactNormal [R: the target's replicated position, median 0 cm off, z up to +46; a unit vector]. An unset location means the
    /// caller's (the pawn's position: a hit on the pawn).
    protocol::repl::Vec3d                impact_location;
    protocol::repl::Vec3d                impact_normal;
    bool                                 has_impact = false;
    std::vector<HitStatModOut>           mods;
    uint8_t                              flags = 0;
    bool operator==(const HitTargetOut&) const = default;
};

struct HitConfirm {
    int64_t record_guid = 0;       // the hit's record
    int64_t source_type = 0;       // the record type of the source: ability, projectile, effect, lingering zone, reaction (kHitSource... in constants.h)
    int64_t source_guid = 0;       // the ability, projectile, effect or zone the hit belongs to
    bool    source_is_actor = true;
    std::vector<HitStatModOut> mods;
    uint8_t flags = 0;             // the target's Flags: bit 0 a critical hit
    bool    predicted_autonomous = false, predicted_simulated = false;
    bool    response = false;      // it answers a client's RequestHitVerification_Server (for the log and the tests)
    bool    no_target = false;     // the hit found nobody: the confirmation lists no target (retail: Ranger_AirStrike_AoE, 10 hits)
    // plan E1, stage 3: a hit between the pawn and another actor
    std::optional<protocol::emit::FIntrepidNetworkGUID> instigator;   // who made the hit when it is not the pawn (an NPC's attack on the player)
    std::vector<HitTargetOut> targets;                                // the targets that are not the pawn: when set they are the confirmation's targets (the pawn is none of them)
    /// E1 stage 3 fix 3: the confirmation's Source when it is neither the instigator nor empty: the target actor a projectile landed on, for the hit
    /// that fires another projectile from there (the book's Book_Projectile_Split_Hit_n: the client spawns the split projectile at
    /// EAbilitySystemOrigin::Source and logs "Failed to find origin location" when the Source is empty) [R: Source = the target actor in 1,289 of
    /// 1,289 retail confirmations of those hits; 0 in the 2,177 of the hits a projectile carries].
    std::optional<protocol::emit::FIntrepidNetworkGUID> source;
    bool operator==(const HitConfirm&) const = default;
};

/// What the runtime asks of the session for a lingering zone's actor (plan D2, review: the dances' visual effects): the client draws a dance's aura
/// from the AAoCAbilityLingeringEffect actor the server replicates [R: retail capture 44 opens one 300 to 460 ms after each dance's trigger and
/// destroys it when the dance ends]. The session opens an actor channel for it, sends the bFadingOut update and closes the channel with Destroyed.
struct ZoneOp {
    enum class Kind : uint8_t { Open, Fade, Close };
    Kind    kind = Kind::Open;
    int64_t id = 0;        // the runtime's number of the zone instance: one actor through its life
    int64_t ability = 0;   // SourceRecordGuid: the ability that spawned the zone
    int64_t zone = 0;      // LingeringEffectId: the zone record
    bool operator==(const ZoneOp&) const = default;
};

/// The replicated items as the arrays hold them (the mounts milestone, stage 3: a possessed mount's CurrentEffects and Cooldowns are built the same way):
/// a CurrentEffects item with `character_guid` (four words) as its Instigator and Source, and a Cooldowns item.
::aoc::protocol::repl::ElementState effect_item_state(const std::array<uint32_t, 4>& character_guid, const EffectItem& e);
::aoc::protocol::repl::ElementState cooldown_item_state(const CooldownItem& c);

/// One step of the runtime: the changes to the pawn's replicated state, applied in the order of the members (stats, effects
/// removed then added, abilities removed, added then changed, cooldowns removed then set).
struct AbilityChange {
    std::vector<StatChange>        stats;
    std::vector<int64_t>           effects_removed;      // EffectId keys
    std::vector<EffectItem>        effects_added;
    std::vector<EffectItem>        effects_changed;      // plan D2: by EffectId, in place (a stack added, the duration refreshed); the item's members are replaced
    std::vector<int64_t>           abilities_removed;    // ServerInstanceId keys
    std::vector<ActiveAbilityItem> abilities_added;
    std::vector<ActiveAbilityItem> abilities_changed;    // by ServerInstanceId; the item's members are replaced
    std::vector<int64_t>           cooldowns_removed;    // ability Guids
    std::vector<CooldownItem>      cooldowns_set;        // added, or changed when the ability Guid has one
    /// plan D2, stage 2: the whole content of the TempHealth float array's item (every shield on the pawn, empty for none); unset: it stays
    std::optional<std::vector<ShieldElement>> shields;

    bool empty() const {
        return !shields && stats.empty() && effects_removed.empty() && effects_added.empty() && effects_changed.empty() && abilities_removed.empty() &&
               abilities_added.empty() && abilities_changed.empty() && cooldowns_removed.empty() && cooldowns_set.empty();
    }
};

}  // namespace aoc::worldd::bootstrap
