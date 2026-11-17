// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  combat/pvp.cpp  (PvP)  -- see pvp.h
// ============================================================================
#include "combat/pvp.h"

#include <bit>
#include <cstring>

namespace aoc::worldd::combat::pvp {

int64_t alignment_of(Flag f) {
    switch (f) {
        case Flag::Combatant: return kCombatant;
        case Flag::Corrupted: return kCorrupted;
        default: return kNonCombatant;
    }
}

// the records' numbers (content.db): NonCombatant FlaggingActions 1 / PriorityAgnostic 0, Combatant 3 / 2, Corrupted 2 / 2
uint8_t flagging_actions(Flag f) {
    switch (f) {
        case Flag::Combatant: return kAttack | kSupport;
        case Flag::Corrupted: return kSupport;
        default: return kAttack;
    }
}

uint8_t priority_agnostic_actions(Flag f) {
    switch (f) {
        case Flag::Combatant:
        case Flag::Corrupted: return kSupport;
        default: return 0;
    }
}

Tier deciding_tier(const Side& self, const Side& target, const Pair& pair) {
    if (self.actor == target.actor) return Tier::Self;
    if (self.suppressed || target.suppressed) return Tier::Suppressed;
    if (self.duel != 0 && self.duel == target.duel) return Tier::Duel;
    if (pair.group >= 1) return Tier::Group;
    const bool guilded = self.guild != std::array<uint32_t, 4>{};
    if (guilded && self.guild == target.guild) return Tier::Guild;
    return Tier::Players;
}

uint8_t base_permissions(Tier t) {
    switch (t) {
        case Tier::Duel: return kHarm;                // Duel -> Duel: Hostile
        case Tier::Players: return kHarm | kHelp;     // Neutral_Player
        default: return kHelp;                        // the same player, a suppression area, Party/Raid (Friendly), GuildMember (Friendly_Guild)
    }
}

uint8_t flagging_bits(const Side& self, const Side& target, const Pair& pair) {
    const Tier t = deciding_tier(self, target, pair);
    if (t == Tier::Self || t == Tier::Suppressed) return 0;
    uint8_t bits = priority_agnostic_actions(target.flag);
    if (t == Tier::Players) bits |= flagging_actions(target.flag);
    return bits;
}

uint8_t permissions(const Side& self, const Side& target, const Pair& pair, bool force_flag) {
    const Tier t = deciding_tier(self, target, pair);
    const uint8_t base = base_permissions(t);
    if (t == Tier::Self || t == Tier::Suppressed) return base;   // (APlayerCharacter::GetCombatPermissions: support only, before the base rule)
    uint8_t out = base;
    if (!force_flag) {
        // every permission whose use would flag is taken away ...
        const uint8_t flagging = flagging_bits(self, target, pair);
        if (flagging & kAttack) out &= static_cast<uint8_t>(~kHarm);
        if (flagging & kSupport) out &= static_cast<uint8_t>(~kHelp);
        // ... and given back as the account's settings say
        if (base & kHarm) {
            if (target.flag == Flag::Corrupted || (target.flag == Flag::Combatant && !self.require_force_to_harm_combatants)) out |= kHarm;
        }
        if (base & kHelp) {
            const bool neutral = (base & kHarm) != 0;
            if (!neutral || (target.flag == Flag::Combatant && !self.require_force_to_support_unaffiliated)) out |= kHelp;
        }
    }
    // a target under PvP opt-in protection cannot be harmed but in a duel with the actor
    if (target.protection && t != Tier::Duel) out &= static_cast<uint8_t>(~kHarm);
    return out;
}

// ---- the timer's key: UE's GetTypeHash(int64), CityHash64 of the instance's 16 bytes, and the old Jenkins HashCombine ------------------
namespace {
constexpr uint64_t kK2 = 0x9ae16a3b2f90404full;

uint64_t rotr(uint64_t x, int r) { return (x >> r) | (x << (64 - r)); }

uint64_t hash16(uint64_t u, uint64_t v, uint64_t mul) {
    uint64_t a = (u ^ v) * mul;
    a ^= a >> 47;
    uint64_t b = (v ^ a) * mul;
    b ^= b >> 47;
    return b * mul;
}

/// CityHash64 of 16 bytes (its 8-to-16-byte branch).
uint64_t city64_16(const unsigned char* s) {
    constexpr uint64_t len = 16;
    const uint64_t mul = kK2 + len * 2;
    uint64_t a, b;
    std::memcpy(&a, s, 8);
    std::memcpy(&b, s + len - 8, 8);
    a += kK2;
    const uint64_t c = rotr(b, 37) * mul + a;
    const uint64_t d = (rotr(a, 25) + b) * mul;
    return hash16(c, d, mul);
}

uint32_t hash_combine(uint32_t a, uint32_t c) {
    uint32_t b = 0x9e3779b9u;
    a += b;
    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);
    return c;
}
}  // namespace

uint32_t timer_key(int64_t alignment, const std::array<uint32_t, 4>& instance) {
    const uint64_t v = static_cast<uint64_t>(alignment);
    const uint32_t type_hash = static_cast<uint32_t>(v) + static_cast<uint32_t>(v >> 32) * 23u;
    unsigned char bytes[16];
    for (int i = 0; i < 4; ++i) std::memcpy(bytes + 4 * i, &instance[static_cast<size_t>(i)], 4);   // (FGuid A, B, C, D, little-endian)
    return hash_combine(type_hash, static_cast<uint32_t>(city64_16(bytes)));
}

protocol::repl::AlignmentContainer container(const Alignments& a) {
    using protocol::repl::GuidValue;
    protocol::repl::AlignmentContainer c;
    const GuidValue none{0, 0, 0, 0};
    const GuidValue self{a.self[0], a.self[1], a.self[2], a.self[3]};
    // the order of the game's alignment container: Player, the flag's, Instigator, PVPSuppressed_Perceiver
    c.instances = {{static_cast<uint64_t>(kPlayer), {none}},
                   {static_cast<uint64_t>(alignment_of(a.flag)), {none}},
                   {static_cast<uint64_t>(kInstigator), {self}},
                   {static_cast<uint64_t>(kPvpSuppressedPerceiver), {none}}};
    if (a.flag == Flag::Combatant) {
        protocol::repl::AlignmentContainer::Timer t;
        t.key = timer_key(kCombatant);
        t.set_time_stamp = std::bit_cast<uint64_t>(a.combatant_since);   // (a double on the wire: SetTimeStamp)
        t.alignment_id_a = static_cast<uint64_t>(kCombatant);
        t.alignment_id_b = static_cast<uint64_t>(kAlignmentTypeId);
        c.timers.push_back(t);
    }
    if (a.duel != std::array<uint32_t, 4>{}) {
        const GuidValue duel{a.duel[0], a.duel[1], a.duel[2], a.duel[3]};
        c.instances.push_back({static_cast<uint64_t>(kDuel), {duel}});
        protocol::repl::AlignmentContainer::Timer t;
        t.key = timer_key(kDuel, a.duel);
        t.set_time_stamp = std::bit_cast<uint64_t>(a.duel_since);
        t.alignment_id_a = static_cast<uint64_t>(kDuel);
        t.alignment_id_b = static_cast<uint64_t>(kAlignmentTypeId);
        c.timers.push_back(t);
    }
    return c;
}

protocol::repl::AlignmentContainer merge_container(const protocol::repl::AlignmentContainer& now, const protocol::repl::AlignmentContainer& want) {
    const auto is_flag = [](uint64_t k) {
        return k == static_cast<uint64_t>(kNonCombatant) || k == static_cast<uint64_t>(kCombatant) || k == static_cast<uint64_t>(kCorrupted);
    };
    const auto is_duel = [](uint64_t k) { return k == static_cast<uint64_t>(kDuel); };
    // the wanted rows: the flag's instance (the second of container()) and the duel's
    protocol::repl::AlignmentContainer flag;
    protocol::repl::AlignmentContainer duel;
    for (const auto& i : want.instances) {
        if (is_flag(i.key)) flag.instances.push_back(i);
        else if (is_duel(i.key)) duel.instances.push_back(i);
    }
    protocol::repl::AlignmentContainer next;
    bool placed = false;
    const auto insert_flag = [&] {
        if (placed || flag.instances.empty()) return;
        next.instances.push_back(flag.instances.front());
        placed = true;
    };
    for (const auto& i : now.instances) {
        if (is_duel(i.key)) continue;
        if (is_flag(i.key)) {
            insert_flag();   // where the old flag's instance sat (the retail order: after Player)
            continue;
        }
        next.instances.push_back(i);
        if (i.key == static_cast<uint64_t>(kPlayer)) insert_flag();
    }
    insert_flag();
    for (const auto& i : duel.instances) next.instances.push_back(i);   // (after the four the join sets, as retail's)
    // the other alignments' timers stay; the flag's and the duel's are the wanted ones
    for (const auto& t : now.timers) {
        if (t.alignment_id_a == static_cast<uint64_t>(kCombatant) || t.alignment_id_a == static_cast<uint64_t>(kDuel)) continue;
        next.timers.push_back(t);
    }
    for (const auto& t : want.timers) {
        if (t.alignment_id_a == static_cast<uint64_t>(kCombatant) || t.alignment_id_a == static_cast<uint64_t>(kDuel)) next.timers.push_back(t);
    }
    return next;
}

int corruption_tier(double corruption) {
    // Corruption->CorruptionTier, Floor of the linear curve (0,0) (0.001,1) (1,1) (3,2) (6,3) (12,4) (24,5)
    if (!(corruption >= 0.001)) return 0;
    if (corruption < 3) return 1;
    if (corruption < 6) return 2;
    if (corruption < 12) return 3;
    if (corruption < 24) return 4;
    return 5;
}

}  // namespace aoc::worldd::combat::pvp
