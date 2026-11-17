// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/abilities_stealth_tests.inc: the pawn's StealthInfo.bInStealth (included by test_worldd_abilities.cpp)
//
//  The retail server sends the pawn's StealthInfo.bInStealth in the same packet as the effect of an ability that stealths its caster (Ranger_Camouflage,
//  Rogue_Stealth, Summoner_PhaseOut: the three effect records with bAppliesStealth), 1 when the effect is put on and 0 when the last one is taken off
//  (Camouflage on and off, Rogue's Stealth and the Summoner's Phase Out all show it).  The client's own stealth look of a character reads it.
// ============================================================================

#include "protocol/repl/block_reader.h"

namespace {

constexpr int64_t kCamouflageEffect = 6064630841868877827ll;    // EffectRecord Ranger_Camouflage (bAppliesStealth)
constexpr int64_t kRogueStealthEffect = 6064632711256473912ll;  // EffectRecord Rogue_Stealth (bAppliesStealth)
constexpr int64_t kPhaseOutEffect = 6064633866989608593ll;      // EffectRecord Summoner_PhaseOut (bAppliesStealth)
constexpr int64_t kStealthLevel = 6064633186496741774ll;        // StatTypeDef StealthLevel (the character's level, less Camouflage's 5)

bs::EffectItem infinite_effect(int64_t id, int64_t guid, double start) {
    bs::EffectItem e = rest_effect(id, start);
    e.effect_guid = guid;
    return e;
}

/// What the update says of the pawn's StealthInfo.bInStealth: true, false, or nullopt when its pawn block does not name it (or there is no update).
std::optional<bool> stealth_in(const std::optional<bs::Send>& s) {
    if (!s) return std::nullopt;
    const auto views = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
    if (!views) return std::nullopt;
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    for (const BlockView& v : *views) {
        if (!v.header.is_actor) continue;
        BunchWriter w;
        w.write_bit_range(s->emission.payload.data(), v.payload_pos, v.payload_bits);
        auto st = load_object_state(cls, w.data(), v.payload_bits, v.header.has_rep_layout);
        if (!st) return std::nullopt;
        const Value* val = st->state.get(cls, "StealthInfo.bInStealth");
        const bool* b = val ? val->get_if<bool>() : nullptr;
        if (b) return *b;
    }
    return std::nullopt;
}

void test_stealth_flag() {
    std::printf("the pawn's StealthInfo.bInStealth follows the effects that stealth it\n");
    Pawn p;
    bs::AbilityChange c;
    c.effects_added.push_back(infinite_effect(0, kCamouflageEffect, 100.0));
    auto s = p.update(c);
    CHECK(s && stealth_in(s) == std::optional<bool>(true), "Camouflage's effect put on: the pawn's update says bInStealth 1 (as retail's, in the packet that adds the effect)");
    c = {};
    c.effects_changed.push_back(infinite_effect(0, kCamouflageEffect, 101.0));
    s = p.update(c);
    CHECK(s && !stealth_in(s), "the effect refreshed: the flag is not sent again");
    c = {};
    c.effects_added.push_back(rest_effect(1, 102.0));
    s = p.update(c);
    CHECK(s && !stealth_in(s), "an effect that stealths nobody (a Rest) leaves the flag alone");
    c = {};
    c.effects_added.push_back(infinite_effect(2, kRogueStealthEffect, 103.0));
    s = p.update(c);
    CHECK(s && !stealth_in(s), "a second stealth effect: the flag is already 1, nothing more is sent");
    c = {};
    c.effects_removed.push_back(0);
    s = p.update(c);
    CHECK(s && !stealth_in(s), "Camouflage taken off while Rogue's Stealth is on: the character is still stealthed");
    c = {};
    c.effects_removed.push_back(2);
    s = p.update(c);
    CHECK(s && stealth_in(s) == std::optional<bool>(false), "the last stealth effect taken off: the pawn's update says bInStealth 0");
    c = {};
    c.effects_removed.push_back(1);
    s = p.update(c);
    CHECK(!stealth_in(s), "(and the Rest going has nothing to do with it)");
    // the Summoner's Phase Out, on and off in one pawn that went through it before
    c = {};
    c.effects_added.push_back(infinite_effect(3, kPhaseOutEffect, 110.0));
    s = p.update(c);
    CHECK(stealth_in(s) == std::optional<bool>(true), "the Summoner's Phase Out stealths too");
    c = {};
    c.effects_removed.push_back(3);
    c.effects_added.push_back(infinite_effect(4, kCamouflageEffect, 111.0));
    s = p.update(c);
    CHECK(!stealth_in(s), "one stealth effect swapped for another in one step: still 1, nothing sent");
    c = {};
    c.effects_removed.push_back(4);
    s = p.update(c);
    CHECK(stealth_in(s) == std::optional<bool>(false), "...and gone: 0");
    // a pawn that has never been stealthed names no StealthInfo at all in its first update
    Pawn fresh;
    c = {};
    c.effects_added.push_back(rest_effect(0, 100.0));
    CHECK(!stealth_in(fresh.update(c)), "a pawn that was never stealthed says nothing of it");
}

/// The stealth level of the character in the update's EveryoneProxy array (the number every client is sent): the last changed item for the stat, if the
/// stats component block names it.
std::optional<float> stealth_level_in(const std::optional<bs::Send>& s, const FIntrepidNetworkGUID& stats_object) {
    if (!s) return std::nullopt;
    const auto views = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
    if (!views) return std::nullopt;
    for (const BlockView& v : *views) {
        if (v.header.is_actor || !(v.header.object == stats_object)) continue;
        size_t p = v.payload_pos;
        const auto fields = read_fast_array_fields(class_named("UAoCStatsComponent"), s->emission.payload.data(), v.payload_pos + v.payload_bits, p, stat_options());
        if (!fields) continue;
        const FastArrayField* arr = field_named(*fields, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
        if (!arr) continue;
        for (const FastArrayDelta::Changed& ch : arr->delta.changed) {
            const Value* val = item_value(ch.values, 1);
            const StatInt32Inst* it = val ? val->get_if<StatInt32Inst>() : nullptr;
            if (it && it->record == kStealthLevel) return std::bit_cast<float>(it->value);
        }
    }
    return std::nullopt;
}

/// Camouflage's update is retail's: the effect, the pawn's StealthInfo.bInStealth 1 and the StealthLevel item (level - 5) in the EveryoneProxy array, in the
/// same chain.  The client's own character is drawn from these (its stealth material and the state it computes); the still-or-moving difference is the client's.
void test_camouflage_stealth_on_the_wire() {
    std::printf("Camouflage: the effect, the stealth flag and the stealth level in the update the client is sent\n");
    Pawn p;
    bs::AbilityChange c;
    c.effects_added.push_back(infinite_effect(0, kCamouflageEffect, 100.0));
    c.stats.push_back({kStealthLevel, 20.0f});
    const auto s = p.update(c);
    CHECK(s && stealth_in(s) == std::optional<bool>(true), "bInStealth 1 in the packet that adds the effect");
    CHECK(s && stealth_level_in(s, p.pb.setup().guids.stats) == std::optional<float>(20.0f), "StealthLevel 20 (the character's 25 less the effect's 5) in StatRepInt32EveryoneProxy, in the same chain");
    c = {};
    c.effects_removed.push_back(0);
    c.stats.push_back({kStealthLevel, 25.0f});
    const auto off = p.update(c);
    CHECK(off && stealth_in(off) == std::optional<bool>(false), "the effect taken off: bInStealth 0");
    CHECK(off && stealth_level_in(off, p.pb.setup().guids.stats) == std::optional<float>(25.0f), "and the stealth level back to 25");
}

}  // namespace
