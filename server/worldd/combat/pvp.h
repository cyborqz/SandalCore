// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  combat/pvp.h  (PvP: the rules between two players)
//
//  Who may harm or support whom, and what an action does to the actor's flag, as the game works it out for itself
//  from the alignments of content.db (CharacterAlignmentRecordBase, PlayerAlignmentContext_Default).  The server runs
//  the same rule on every hit a player lands on another, so a hit the client would not have sent is refused, and a hit it
//  would have sent lands.
//
//  The alignments a player holds (UAlignmentComponent.Alignments, what every client reads to colour the nameplate):
//    Player, Instigator {own CharacterGuid}, PVPSuppressed_Perceiver, and one of
//    NonCombatant   white, the default.  Harming one flags you (FlaggingActions Attack)
//    Combatant      purple, for 60 s after the last flagging action (RemovalTimer 60; the timer InstanceTimers[timer_key] counts it down
//                   on the client).  Harming or supporting one flags you (Attack|Support); supporting one flags you even when a higher
//                   tier (a party) decides the relation (PriorityAgnosticFlaggingActions Support)
//    Corrupted      red, while the Corruption stat is above 0.  Supporting one flags you (Support); harming one does not
//
//  The relation of two players, the first tier of PlayerAlignmentContext_Default that decides it:
//    Instigator (the same player)          support only
//    PVPSuppressed (either side)           support only
//    Duel (the same active duel)           Hostile: harm only, no flagging (Duel FlaggingActions 0)
//    Party, Raid (one group)               Friendly: support only
//    GuildMember (one guild)               Friendly_Guild: support only (Side::guild is the guild the player is in; zero: none, and the tier never decides)
//    Corrupted / Combatant / NonCombatant  Neutral_Player (CombatPermissions 3): harm and support
//  Without the force flag (the client's PvP key: ServerSetPvpFlag, and each ServerTriggerAbility's bPvpForceFlag) every permission whose
//  use would flag the actor is taken away, then given back: harm on a corrupted target always, harm on a combatant unless the actor's
//  account setting bRequireForceFlagToHarmCombatants is on; support on a friendly target always, on a neutral one only on a combatant and
//  unless bRequireForceFlagToSupportUnaffiliatedCombatants is on.  A target with PvP opt-in protection (an Emberspring's) cannot be harmed
//  outside a duel with the actor.
//
//  Pure logic: no world, no session.  The session keeps each player's side (combat/combat_world.h PlayerPort::pvp_side).
// ============================================================================
#pragma once

#include "protocol/repl/value.h"

#include <array>
#include <cstdint>

namespace aoc::worldd::combat::pvp {

// ---- the records (content.db, CharacterAlignmentRecordBase) ----------------------------------------------------------------------
inline constexpr int64_t kAlignmentTypeId = 6221802054776738159ll;
inline constexpr int64_t kPlayer = 6064629444695359488ll;
inline constexpr int64_t kNonCombatant = 6064629443712188425ll;
inline constexpr int64_t kCombatant = 6064629443707731976ll;
inline constexpr int64_t kCorrupted = 6064629443752951818ll;
inline constexpr int64_t kDuel = 6064629494068806525ll;
inline constexpr int64_t kInstigator = 6064630243140894720ll;
inline constexpr int64_t kPvpSuppressed = 6064631957177696256ll;
inline constexpr int64_t kPvpSuppressedPerceiver = 6064631980646400000ll;
/// Combatant's RemovalTimer: the seconds a flag lasts after the last action that flagged.
inline constexpr double kCombatantSeconds = 60.0;
/// The Corruption stat (GameStatProfile.CorruptionId: a float every client is told) and its tier (Floor of the Corruption->CorruptionTier curve).
inline constexpr int64_t kStatCorruption = 109228544644730ll;
inline constexpr int64_t kStatCorruptionTier = 6064630485796716545ll;
/// The PvP opt-in protections (step 4): Emberspring_PVPOptInProtection (60 s after a respawn at a respawn point; its requirement !IsCombatant && !IsCorrupted is the
/// server's to check, aocexpr does not run it) and General_PVPOptInProtection (GamePvpSettings.PvpOptInProtectionEffectId).  While one holds, another player cannot harm
/// him outside a duel (Side::protection); a flagging action of his own takes it off.
inline constexpr int64_t kEffectEmberspringProtection = 6064632587050614784ll;
inline constexpr int64_t kEffectGeneralProtection = 6064632581294587904ll;

/// EFlaggableActions: what an action is.
inline constexpr uint8_t kAttack = 1;
inline constexpr uint8_t kSupport = 2;
/// ETargetingIntention (NotifyFlaggingReason's first parameter).
inline constexpr uint8_t kIntentionHarm = 1;
inline constexpr uint8_t kIntentionSupport = 2;
/// The combat permissions a rule gives (CharacterRelationship.CombatPermissions: bit 0 harm, bit 1 support).
inline constexpr uint8_t kHarm = 1;
inline constexpr uint8_t kHelp = 2;

enum class Flag : uint8_t { NonCombatant, Combatant, Corrupted };

/// The record of a flag's alignment, and what its record says.
int64_t alignment_of(Flag f);
/// FlaggingActions: the actions on a holder that flag the actor when this tier decides the relation.
uint8_t flagging_actions(Flag f);
/// PriorityAgnosticFlaggingActions: the actions on a holder that flag the actor whichever tier decides.
uint8_t priority_agnostic_actions(Flag f);

/// What the rules read of one player.
struct Side {
    uint64_t actor = 0;               // the pawn's actor id
    Flag     flag = Flag::NonCombatant;
    bool     suppressed = false;      // in a PvP suppression area (CombatInfo.InPvpSuppressionArea)
    bool     protection = false;      // PvP opt-in protection (an Emberspring's, for 60 s after a respawn)
    uint64_t duel = 0;                // the active duel the player fights (0: none)
    std::array<uint32_t, 4> guild{};  // the guild's guid (zero: none)
    // the account's PvP settings (the PlayerController's CharacterInGameSettings.CombatSettings.PVP, echoed from ServerUpdateAccountCombatSettings)
    bool     require_force_to_harm_combatants = false;
    bool     require_force_to_support_unaffiliated = false;
};

/// How the two are grouped: 0 not, 1 in one raid or party, 2 in one squad (CombatWorld's group query).
struct Pair {
    int group = 0;
};

enum class Tier : uint8_t { Self, Suppressed, Duel, Group, Guild, Players };

/// The tier that decides how `self` stands to `target`, and the permissions it gives before the force-flag rules.
Tier    deciding_tier(const Side& self, const Side& target, const Pair& pair);
uint8_t base_permissions(Tier t);

/// The actions on `target` that flag `self` (EFlaggableActions bits): the deciding tier's own (only the players' tier flags) and the target's priority-agnostic ones.
/// A corrupted actor is never flagged (InterServerHandleFlaggingOnHitInstigated: it stays corrupted).
uint8_t flagging_bits(const Side& self, const Side& target, const Pair& pair);

/// What `self` may do to `target` (kHarm | kHelp) with or without the force flag.
uint8_t permissions(const Side& self, const Side& target, const Pair& pair, bool force_flag);

/// Whether an action (kAttack or kSupport) on `target` flags `self`: what the server does after the hit lands.
inline bool flags(const Side& self, const Side& target, const Pair& pair, uint8_t action) {
    return self.flag != Flag::Corrupted && (flagging_bits(self, target, pair) & action) != 0;
}

/// The key of an alignment's removal timer in FAlignmentContainer.InstanceTimers: HashCombine(GetTypeHash(int64 guid), (uint32)CityHash64(instance, 16)) as the
/// game finds it.  Combatant with a zero instance is 1905362248.
uint32_t timer_key(int64_t alignment, const std::array<uint32_t, 4>& instance = {});

/// The Alignments a player's pawn replicates: Player, the flag's alignment, Instigator {own guid}, PVPSuppressed_Perceiver, and a duel's when it fights one;
/// a combatant's removal timer starts at `combatant_since` (the synchronized clock of ClientSetTime: bootstrap::effect_clock).
struct Alignments {
    Flag     flag = Flag::NonCombatant;
    double   combatant_since = 0;         // the synchronized clock of the last flagging action (a combatant's)
    std::array<uint32_t, 4> self{};       // the character's guid
    std::array<uint32_t, 4> duel{};       // the duel's instance guid (zero: none)
    double   duel_since = 0;
};
protocol::repl::AlignmentContainer container(const Alignments& a);
/// `now` with the flag's instance, the Combatant timer and the Duel instance set as `want` says: every other instance and timer `now` holds (Player,
/// Instigator, PVPSuppressed_Perceiver, a party's, a node's) stays, so a later feature's alignment is never dropped by a flag change.
protocol::repl::AlignmentContainer merge_container(const protocol::repl::AlignmentContainer& now, const protocol::repl::AlignmentContainer& want);

/// The corruption tier of an amount (Corruption->CorruptionTier: levels 1 above 0, 2 at 3, 3 at 6, 4 at 12, 5 at 24).
int corruption_tier(double corruption);

}  // namespace aoc::worldd::combat::pvp
