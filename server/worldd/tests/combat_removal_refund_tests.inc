// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_removal_refund_tests.inc: what an ability's hit takes off a creature and puts on it, and what a cast bar that ends gives back
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
//
//  A hit works out the effects it puts on a creature against the creature as the hit found it, then takes off what its removal list names, then puts the effects on: the Snipe's Hemorrhage counts the
//  bleed that the same hit takes off.  A cast bar that a Dodge or the client ends after its cost was paid gives back the stats, the charge and the effect stacks the cost took.
// ============================================================================

/// What one cast of `ability` by a Ranger left on a Wolf 12 m ahead of him; `bleeding` puts a bleed on the Wolf first.
struct BleedCast {
    bool ran = false;     // the arrow struck (the Wolf lost Health)
    int  bleed = -1;      // the stacks of the bleed left on the Wolf
    int  hemorrhage = -1; // the stacks of the hemorrhage on it
};

static BleedCast bleed_cast(Fixture& f, const char* ability, bool bleeding, uint64_t seed) {
    BleedCast out;
    Forge g(f, 1, {}, seed);
    if (!g.ok()) return out;
    const uint64_t wolf = g.wolf(1200, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named(ability);
    const abilities::EffectDef* bleed = f.catalog->effect_named("Status_Bleeding");
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    if (!wolf || !def || !bleed || !hemorrhage) return out;
    g.rig.know({ability});
    constexpr double t0 = 1.0;
    if (bleeding) {
        effects::Applied a;
        a.def = bleed;
        a.duration = 120.0;
        g.world.apply_effects(wolf, 77, {a}, t0);
    }
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    if (g.rig.pipe->trigger(def->row.guid, 50, t0, info) != Refusal::None) return out;
    g.rig.take();
    g.rig.run_to(t0 + 6.0);   // (the Snipe's arrow leaves about 3.8 s after the trigger)
    out.ran = g.lost({wolf}) > 0.0;
    out.bleed = g.world.effect_stacks(wolf, bleed->row.guid);
    out.hemorrhage = g.world.effect_stacks(wolf, hemorrhage->row.guid);
    return out;
}

void test_hit_effects_before_removal(Fixture& f) {
    std::printf("an ability's hit works out its effects before it takes its removal list off a creature: the Snipe turns a bleed into a Hemorrhage, the Power Shot only takes the bleed off\n");
    const abilities::HitDef* snipe = f.catalog->hit_named("Ranger_Snipe_V2");
    const abilities::HitDef* power = f.catalog->hit_named("Ranger_PowerShot");
    const abilities::EffectDef* bleed = f.catalog->effect_named("Status_Bleeding");
    CHECK(snipe && power && bleed && snipe->remove_effects.size() == 1 && snipe->remove_effects[0] == bleed->row.guid && power->remove_effects.size() == 1 && power->remove_effects[0] == bleed->row.guid,
          "the data: the Snipe's hit and the Power Shot's hit each take the bleed off");
    if (!snipe || !power || !bleed) return;
    // (a seed whose arrows strike in all three casts)
    bool done = false;
    for (uint64_t seed = 1; seed <= 12 && !done; ++seed) {
        const BleedCast clean = bleed_cast(f, "Ranger_Snipe", false, seed);
        const BleedCast bleeding = bleed_cast(f, "Ranger_Snipe", true, seed);
        const BleedCast shot = bleed_cast(f, "Ranger_PowerShot", true, seed);
        if (!clean.ran || !bleeding.ran || !shot.ran) continue;
        done = true;
        std::printf("    seed %llu: Snipe on a Wolf that does not bleed: bleed %d, hemorrhage %d; Snipe on a bleeding Wolf: bleed %d, hemorrhage %d; Power Shot on a bleeding Wolf: bleed %d, hemorrhage %d\n",
                    static_cast<unsigned long long>(seed), clean.bleed, clean.hemorrhage, bleeding.bleed, bleeding.hemorrhage, shot.bleed, shot.hemorrhage);
        CHECKF(clean.bleed == 0 && clean.hemorrhage == 0, "a Snipe on a Wolf that does not bleed puts no hemorrhage on it (its stack count works out to 0): bleed %d, hemorrhage %d", clean.bleed, clean.hemorrhage);
        CHECKF(bleeding.hemorrhage == 1, "a Snipe on a bleeding Wolf puts the hemorrhage on it, one stack as the data's formula gives (%d)", bleeding.hemorrhage);
        CHECKF(bleeding.bleed == 0, "...and takes the bleed off (%d)", bleeding.bleed);
        CHECKF(shot.bleed == 0 && shot.hemorrhage == 0, "a Power Shot on a bleeding Wolf takes the bleed off and puts no hemorrhage on it, as its data says (bleed %d, hemorrhage %d)", shot.bleed, shot.hemorrhage);
    }
    CHECK(done, "a seed was found in which all three arrows strike");
}
AOC_REGISTER_TEST(test_hit_effects_before_removal);

// ---- what a cast bar that ends gives back ---------------------------------------------------------------------------------------------------------------
// The Arcane Circle pays before its bar's end (its Mana and its cooldown).  No ability whose cost takes effect stacks pays before its bar's end in the data today (the Scatter Shot pays after its
// bar's montage jump), so the check gives the Circle a cost of stacks of one effect for the length of the check, and puts it back.
struct ForgedEffectCost {
    abilities::AbilityDef*             def = nullptr;
    std::vector<content::EffectCostRow> before;
    ForgedEffectCost(const abilities::AbilityDef* ability, int64_t effect_guid) : def(const_cast<abilities::AbilityDef*>(ability)) {
        if (!def) return;
        before = def->effect_costs;
        content::EffectCostRow row;
        row.cost_ord = 0;   // (the cost group of the Circle's Mana, always enabled)
        row.ord = 0;
        row.effect_guid = effect_guid;
        row.num_expr = 2;   // (the number 1)
        def->effect_costs.push_back(row);
    }
    ~ForgedEffectCost() {
        if (def) def->effect_costs = before;
    }
};

/// One Arcane Circle of a Mage who holds three stacks of an effect it also costs one of: how the character stands once the cost is paid and again after the cast bar ended (`how` 0: nothing ends it,
/// 1: a Dodge, 2: the client's own end of the cast bar).
struct CircleCast {
    bool   ok = false;
    double mana_before = 0, mana_paid = 0, mana_after = 0;
    int    stacks_paid = -1, stacks_after = -1;
    int    charges_paid = -2, charges_after = -2;
    size_t ended = 0;
};

static CircleCast circle_cast(Fixture& f, int how) {
    CircleCast out;
    Forge g(f, 8, {});
    if (!g.ok()) return out;
    const abilities::EffectDef* held = g.rig.catalog->effect_named("Ranger_LightningReload");   // (the rig's own catalog is the one the runtime reads)
    const abilities::AbilityDef* def = g.rig.catalog->ability_named("Mage_ArcaneCircle");
    if (!held || !def) return out;
    const ForgedEffectCost cost(def, held->row.guid);
    g.rig.know({"Mage_ArcaneCircle"});
    g.rig.run_to(0.5);
    if (!g.rig.pipe->grant_effect(held->row.guid, 3, 0.6)) return out;
    g.rig.take();
    constexpr double t0 = 1.0;
    out.mana_before = g.rig.mana();
    if (g.rig.trigger("Mage_ArcaneCircle", t0, false) != Refusal::None) return out;
    g.rig.run_to(t0 + 0.5);   // (the cost is paid 75 ms after the trigger)
    out.mana_paid = g.rig.mana();
    out.stacks_paid = g.rig.pipe->stacks_of(held->row.guid);
    out.charges_paid = g.rig.pipe->charges_left(def->row.guid);
    if (how == 1) {
        out.ended = g.rig.pipe->end_by_dodge(t0 + 0.6);
    } else if (how == 2) {
        int64_t instance = -1;
        for (const auto& [key, item] : g.rig.pawn.abilities) {
            if (item.ability_guid == def->row.guid) instance = key;
        }
        if (instance >= 0 && g.rig.pipe->end_request(static_cast<int32_t>(instance), t0 + 0.6)) out.ended = 1;
    }
    g.rig.run_to(t0 + 0.8);
    out.mana_after = g.rig.mana();
    out.stacks_after = g.rig.pipe->stacks_of(held->row.guid);
    out.charges_after = g.rig.pipe->charges_left(def->row.guid);
    out.ok = g.rig.pawn.broken.empty();
    return out;
}

void test_cast_bar_end_gives_back_the_cost(Fixture& f) {
    std::printf("a cast bar that a Dodge or the client ends after its cost was paid gives back the Mana, the charge and the effect stacks the cost took\n");
    const CircleCast control = circle_cast(f, 0);
    CHECK(control.ok, "(a Mage casts the Arcane Circle, holding three stacks of an effect it costs one of)");
    if (!control.ok) return;
    std::printf("    control: Mana %.1f -> %.1f, stacks 3 -> %d, charges %d\n", control.mana_before, control.mana_paid, control.stacks_paid, control.charges_paid);
    CHECK(control.mana_before - control.mana_paid > 40.0 && control.stacks_paid == 2 && control.charges_paid == 0, "control: the cost is paid before the bar's end: its Mana, one stack of the effect, and its charge");
    CHECKF(control.stacks_after == 2, "control, nothing ends it: the stacks stay paid (%d)", control.stacks_after);
    const CircleCast dodge = circle_cast(f, 1);
    std::printf("    Dodge: %zu ended, Mana %.1f -> %.1f -> %.1f, stacks %d -> %d, charges %d -> %d\n", dodge.ended, dodge.mana_before, dodge.mana_paid, dodge.mana_after, dodge.stacks_paid, dodge.stacks_after,
                dodge.charges_paid, dodge.charges_after);
    CHECK(dodge.ok && dodge.ended >= 1 && dodge.stacks_paid == 2, "a Dodge ends the cast bar after the cost was paid");
    CHECKF(dodge.mana_after >= dodge.mana_before - 0.5, "...the Mana comes back (%.1f of %.1f)", dodge.mana_after, dodge.mana_before);
    CHECKF(dodge.stacks_after == 3, "...and so does the stack of the effect the cost took (%d of 3)", dodge.stacks_after);
    CHECKF(dodge.charges_after == -1, "...and the cooldown is off (%d)", dodge.charges_after);
    const CircleCast client = circle_cast(f, 2);
    std::printf("    the client's own end: %zu ended, Mana %.1f -> %.1f -> %.1f, stacks %d -> %d, charges %d -> %d\n", client.ended, client.mana_before, client.mana_paid, client.mana_after, client.stacks_paid,
                client.stacks_after, client.charges_paid, client.charges_after);
    CHECK(client.ok && client.ended == 1 && client.stacks_paid == 2, "the client ends the cast bar of a cast that had paid (ServerEndAbility)");
    CHECKF(client.mana_after >= client.mana_before - 0.5, "...the Mana comes back, as after a Dodge (%.1f of %.1f)", client.mana_after, client.mana_before);
    CHECKF(client.stacks_after == 3, "...and so does the stack of the effect (%d of 3)", client.stacks_after);
    CHECKF(client.charges_after == -1, "...and the cooldown is off (%d)", client.charges_after);
}
AOC_REGISTER_TEST(test_cast_bar_end_gives_back_the_cost);

// The Scatter Shot's cost (a stack of Ranger_LightningReload when the Ranger holds one, else Mana) is paid after its bar's montage jump: a Dodge in the bar ends the cast with nothing paid, so nothing is
// taken and nothing is to give back.
void test_scatter_shot_dodge_takes_no_stack(Fixture& f) {
    std::printf("a Dodge in the Scatter Shot's bar ends it before its cost is paid: the stacks of Ranger_LightningReload stay, no Mana is spent\n");
    Forge g(f, 1, {});
    const abilities::EffectDef* held = f.catalog->effect_named("Ranger_LightningReload");
    CHECKF(g.ok() && held, "the Scatter Shot's world (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok() || !held) return;
    g.wolf(600, 0);
    g.rig.know({"Ranger_ScatterShot"});
    g.rig.run_to(0.5);
    CHECK(g.rig.pipe->grant_effect(held->row.guid, 3, 0.6), "the Ranger holds three stacks of Ranger_LightningReload");
    g.rig.take();
    constexpr double t0 = 1.0;
    const double mana = g.rig.mana();
    CHECK(g.rig.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the Scatter Shot is cast");
    g.rig.run_to(t0 + 0.15);
    const size_t ended = g.rig.pipe->end_by_dodge(t0 + 0.15);
    g.rig.run_to(t0 + 1.0);
    CHECKF(ended >= 1, "a Dodge 0.15 s into the cast ends it (%zu ended)", ended);
    CHECKF(g.rig.pipe->stacks_of(held->row.guid) == 3 && g.rig.mana() >= mana - 0.5 && g.rig.pipe->charges_left(g.rig.ability("Ranger_ScatterShot")) == -1,
           "...with all three stacks still on him, his Mana unspent (%.1f of %.1f) and no cooldown running (stacks %d)", g.rig.mana(), mana, g.rig.pipe->stacks_of(held->row.guid));
}
AOC_REGISTER_TEST(test_scatter_shot_dodge_takes_no_stack);
