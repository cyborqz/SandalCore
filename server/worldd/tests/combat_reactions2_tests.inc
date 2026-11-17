// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_reactions2_tests.inc: reactions part 2
//
//    the Bard's melodies  a weapon combo's finishing hit fires Cheerful, Pensive or Epic when its Self effect is on and Resonance is above 0: the
//                         reaction's hit picks its allies by the targeting record (an injured one, one short of Mana, one under crowd control) and the
//                         projectile it fires heals, restores Mana, or takes up to three crowd-control effects off and gives Pep; the Resonance the data
//                         spends goes with it
//    the finishers        every other class's finishing reaction: the ones whose hit fires a further hit (the Fighter's Momentum), the two whose caster
//                         or target is the hit's instigator or source, and the two a creature carries (the Rogue's bomb, the Summoner's Hex)
//    being hit            the reactions a player's own effects carry fire when a creature or another player hits him: the Tank's Reflect, Tripped and
//                         the General Block FX, on every attacker
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

// This file's checks print their passing line too, so the proof of the eight angles can quote the [ok ] line as well as the [FAIL] one.
// (The program's own macros come back at the end of the file: what an .inc included after this one checks is counted the way it always was.)
#pragma push_macro("CHECK")
#pragma push_macro("CHECKF")
#undef CHECK
#undef CHECKF
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define CHECKF(cond, ...) do { \
    char rx_line[1024]; \
    std::snprintf(rx_line, sizeof rx_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", rx_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", rx_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace reactions2 {

constexpr double kPi = 3.14159265358979323846;
constexpr double kManaMult25 = 5.6875;            // ManaCostMult(25): EvalCurve(Player_Base_ManaCostMult, 25) / 32
constexpr int64_t kMagicalPower = 109343399106254ll;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

/// The effects the melodies read and write.
struct Melodies {
    int64_t resonance = 0, cheerful_self = 0, pensive_self = 0, epic_self = 0, glee = 0, pep = 0;
    int64_t cheerful_hit = 0, pensive_hit = 0, epic_hit = 0, cheerful_aoe = 0, pensive_aoe = 0, epic_aoe = 0;
    int64_t rooted = 0, snared = 0, stunned = 0, chilled = 0;
    void look_up(Fixture& f) {
        const abilities::Catalog& c = *f.catalog;
        const auto e = [&](const char* n) { const abilities::EffectDef* d = c.effect_named(n); return d ? d->row.guid : 0; };
        const auto h = [&](const char* n) { const abilities::HitDef* d = c.hit_named(n); return d ? d->row.guid : 0; };
        resonance = e("Bard_Resonance");
        cheerful_self = e("Bard_Music_Cheerful_Self");
        pensive_self = e("Bard_Music_Pensive_Self");
        epic_self = e("Bard_Music_Epic_Self");
        glee = e("Bard_Glee");
        pep = e("Bard_Pep");
        cheerful_hit = h("Bard_ResonantWeapon_End_Cheerful_Projectile");
        pensive_hit = h("Bard_ResonantWeapon_End_Pensive_Projectile");
        epic_hit = h("Bard_ResonantWeapon_End_Epic_Projectile");
        cheerful_aoe = h("Bard_ResonantWeapon_End_Cheerful_AOE");
        pensive_aoe = h("Bard_ResonantWeapon_End_Pensive_AoE");
        epic_aoe = h("Bard_ResonantWeapon_End_Epic_AoE");
        rooted = e("Status_Rooted");
        snared = e("Status_Snared");
        stunned = e("Status_Stunned");
        chilled = e("Status_Chilled");
    }
    bool ok() const {
        return resonance && cheerful_self && pensive_self && epic_self && glee && pep && cheerful_hit && pensive_hit && epic_hit && cheerful_aoe && pensive_aoe && epic_aoe &&
               rooted && snared && stunned && chilled;
    }
};

/// The confirmations of a hit record in the runtime's events, from `from` on.
std::vector<bootstrap::HitConfirm> confirms_of(const Rig& rig, int64_t record, size_t from = 0) {
    std::vector<bootstrap::HitConfirm> out;
    for (size_t i = from; i < rig.log.size(); ++i) {
        for (const bootstrap::HitConfirm& c : rig.log[i].confirmations) {
            if (c.record_guid == record) out.push_back(c);
        }
    }
    return out;
}

/// When a hit record's confirmation came, from `from` on (none: nullopt).
std::optional<double> at_of(const Rig& rig, int64_t record, size_t from = 0) {
    for (size_t i = from; i < rig.log.size(); ++i) {
        for (const bootstrap::HitConfirm& c : rig.log[i].confirmations) {
            if (c.record_guid == record) return rig.log[i].at;
        }
    }
    return std::nullopt;
}

/// The effect a rig's pipeline holds, with its stacks.
int stacks_of(const Rig& rig, int64_t effect) { return rig.pipe ? rig.pipe->stacks_of(effect) : 0; }

/// Makes the world know a creature someone spawned into it: the world makes its Npc when it first deals with it, and a test that reads its Health before
/// any hit asks here (a test rig spawns the creatures after its world is made; in production the world's tick takes them all).
void wake(combat::CombatWorld& world, uint64_t actor, uint64_t by, double at) {
    if (actor == 0 || world.health_of(actor) >= 0) return;
    combat::Resolved none;
    world.apply_hit_to_npc(actor, by, none, at);
}

/// Gives the rig an effect at `at` (just before the finisher that reads it: Resonance lasts 31 s).
bool grant(Rig& rig, int64_t effect, int stacks, double at) {
    if (!rig.pipe || effect == 0) return false;
    rig.clock = std::max(rig.clock, at);
    const bool ok = rig.pipe->grant_effect(effect, stacks, at);
    rig.take();
    return ok;
}

/// One finishing hit of the 1H axe combo at a Wolf: the cast, the client's request, and what landed.
struct Finish {
    bool accepted = false;
    int  landed = 0;   // the finishing hit's own confirmations
};
Finish finish_axe(Rig& rig, int32_t& id, double t, uint64_t wolf) {
    Finish out;
    const abilities::AbilityDef* combo = rig.catalog ? rig.catalog->ability_named("WeaponCombo_Melee_1H_Axe") : nullptr;
    const abilities::HitDef* finisher = rig.catalog ? rig.catalog->hit_named("WeaponCombo_Axe_1H_Hit4+") : nullptr;
    if (!combo || !finisher || wolf == 0) return out;
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : combo->hits) {
        if (h.hit_guid != finisher->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : combo->targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    rig.clock = t;
    TriggerInfo info;
    info.animation_index = 0;
    info.target_count = 1;
    info.target_actors = {wolf};
    out.accepted = rig.pipe->trigger(combo->row.guid, id++, t, info) == Refusal::None;
    rig.run_to(t + 0.4);
    rig.pipe->hit_request(combo->row.guid, targeting, finisher->row.guid, 1, 0, t + 0.8, {wolf});
    for (const Seen& s : rig.run_to(t + 1.8)) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            out.landed += c.record_guid == finisher->row.guid && c.source_type == content::kSourceTypeAbility ? 1 : 0;
        }
    }
    return out;
}

/// Two players of one world, each with his own pipeline and hit port (the way a session runs one for each).
struct Duo {
    combat_rig::World      w;
    combat::NpcStatBook    book;
    combat::CombatWorld    world;
    Rig                    a, b;
    combat_rig::FakePlayer pa, pb;
    combat::WorldHitPort   porta, portb;
    std::string            wolf_path;

    Duo(Fixture& f, int class_a = 2, int class_b = 2, std::vector<std::string> skills_a = {}, std::vector<std::string> skills_b = {})
        : book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          a(class_a, 25, skills_a, true, 101),
          b(class_b, 25, skills_b, true, 102),
          pa(a, 77, {0, 0, 1098}),
          pb(b, 78, {300, 0, 1098}),
          porta(world, w.registry, pa.fighter()),
          portb(world, w.registry, pb.fighter()),
          wolf_path(wolf_archetype(f)) {
        if (!a.ok() || !b.ok()) return;
        world.add_player(&pa);
        world.add_player(&pb);
        a.pipe->set_combat(&porta);
        b.pipe->set_combat(&portb);
    }
    bool ok() const { return a.ok() && b.ok() && w.problem.empty(); }
    /// A Wolf of level 25 at (x, y) (a small aggro radius: it does not come for the players).
    uint64_t wolf(double x, double y) { return w.spawn(wolf_path, x, y, 25, 100); }
};

// ---- the Bard's melodies -------------------------------------------------------------------------------------------------------------------

// Cheerful at a finishing hit: the Self effect is on, Resonance 1, and the ally is at half Health.  The reaction's own hit fires a targeting hit and that
// a projectile: one heal lands on the injured ally, its confirmation the reaction's, and the single Resonance stack is spent with its landing.
void test_reactions2_cheerful_heals_the_injured_ally(Fixture& f) {
    std::printf("Cheerful at a finisher heals the injured ally and spends the Resonance it read\n");
    Melodies m;
    m.look_up(f);
    CHECKF(m.ok(), "the melodies' effects and hits are in the data");
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the two players and the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    CHECK(wolf != 0, "a Wolf 12 m ahead of the Bard");
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 0.5), "(Cheerful is up)");
    CHECK(grant(d.a, m.resonance, 1, t0 - 0.5), "(one Resonance)");
    d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.5);
    const double hurt = d.b.health();
    CHECKF(near(hurt, d.b.max_health() * 0.5, 1.0), "(the ally is at half Health: %.1f of %.1f)", hurt, d.b.max_health());

    int32_t id = 500;
    const size_t from = d.a.log.size();
    const size_t fired = d.a.pipe->reactions_fired();
    const Finish fin = finish_axe(d.a, id, t0, wolf);
    CHECKF(fin.accepted && fin.landed == 1, "(the finishing hit landed: %d)", fin.landed);
    const auto heals = confirms_of(d.a, m.cheerful_hit, from);
    CHECKF(heals.size() == 1 && heals[0].targets.size() == 1, "one heal hit of the reaction landed (%zu)", heals.size());
    if (heals.size() == 1 && heals[0].targets.size() == 1) {
        const bootstrap::HitTargetOut& t = heals[0].targets[0];
        CHECKF(t.component.ObjectId == d.pb.actor_id(), "the heal landed on the injured ally (%llu)", static_cast<unsigned long long>(t.component.ObjectId));
        CHECKF(!t.mods.empty() && t.mods[0].value > 0 && t.mods[0].effective > 0, "its number is a heal: +%.2f (%.2f took effect)", t.mods.empty() ? 0.0 : t.mods[0].value,
               t.mods.empty() ? 0.0 : t.mods[0].effective);
    }
    CHECKF(heals.size() == 1 && heals[0].source_type == content::kSourceTypeProjectile && heals[0].source_guid != 0,
           "the heal's source is the homing projectile the targeting hit fired");
    // the projectile flies from the Bard to the ally at its record's 4,000 cm/s: 3 m is 0.075 s after the finishing hit landed
    const abilities::HitDef* finisher = d.a.catalog ? d.a.catalog->hit_named("WeaponCombo_Axe_1H_Hit4+") : nullptr;
    const std::optional<double> hit_at = finisher ? at_of(d.a, finisher->row.guid, from) : std::nullopt;
    const std::optional<double> heal_at = at_of(d.a, m.cheerful_hit, from);
    CHECKF(hit_at && heal_at && near(*heal_at - *hit_at, 300.0 / 4000.0, 0.02), "the heal landed %.3f s after the hit: the ally 3 m away over the projectile's 4,000 cm/s",
           hit_at && heal_at ? *heal_at - *hit_at : -1.0);
    CHECKF(d.b.health() > hurt + 1.0, "the ally's Health rose (%.1f over %.1f)", d.b.health(), hurt);
    CHECKF(stacks_of(d.a, m.resonance) == 0, "the Resonance the heal read was spent (1 -> %d)", stacks_of(d.a, m.resonance));
    CHECKF(d.a.pipe->reactions_fired() == fired + 1, "the reaction fired once (%zu over %zu)", d.a.pipe->reactions_fired() - fired, fired);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// The same finisher with an ally at full Health and one hurt: the targeting record's filter wants an injured one, so the heal goes to the hurt ally only.
void test_reactions2_cheerful_picks_the_hurt_ally(Fixture& f) {
    std::printf("Cheerful picks the hurt ally among a full and a hurt one\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    // a third player a little farther away, at full Health
    Rig c(2, 25, {}, true, 103);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    if (!c.ok()) return;
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 1, t0 - 0.5), "(Cheerful with one Resonance)");
    d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.5);
    const double hurt = d.b.health();
    const double full = c.health();
    int32_t id = 500;
    const size_t from = d.a.log.size();
    finish_axe(d.a, id, t0, wolf);
    const auto heals = confirms_of(d.a, m.cheerful_hit, from);
    CHECKF(heals.size() == 1 && heals[0].targets.size() == 1 && heals[0].targets[0].component.ObjectId == d.pb.actor_id(),
           "the one heal went to the hurt ally (%zu hits)", heals.size());
    CHECKF(d.b.health() > hurt + 1.0 && near(c.health(), full, 1e-3), "the hurt ally's Health rose and the full one's did not (%.1f -> %.1f, %.1f)", hurt, d.b.health(), c.health());
    CHECK(stacks_of(d.a, m.resonance) == 0, "(the Resonance was spent)");
}

// A solo Bard: the caster is an ally of himself, so an injured one is healed; at full Health the targeting record finds nobody and the Resonance stays.
void test_reactions2_cheerful_solo(Fixture& f) {
    std::printf("Cheerful on a solo Bard: an injured one heals himself, a full one heals nobody and spends nothing\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Forge g(f, 2, {});
    CHECKF(g.ok(), "the Bard and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(1200, 0);
    if (wolf == 0) return;
    g.rig.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(g.rig, m.cheerful_self, 1, t0 - 0.5) && grant(g.rig, m.resonance, 1, t0 - 0.5), "(Cheerful with one Resonance)");
    g.rig.set_current(content::kGuidHealth, g.rig.max_health() * 0.5);
    const double hurt = g.rig.health();
    int32_t id = 500;
    size_t from = g.rig.log.size();
    finish_axe(g.rig, id, t0, wolf);
    auto heals = confirms_of(g.rig, m.cheerful_hit, from);
    // a heal the caster lands on himself is confirmed with no target (the hit on the caster is his own)
    CHECKF(heals.size() == 1 && heals[0].targets.empty() && heals[0].mods.size() == 1 && heals[0].mods[0].effective > 0, "the solo Bard's own heal landed (%zu)", heals.size());
    CHECKF(g.rig.health() > hurt + 1.0, "his Health rose (%.1f over %.1f)", g.rig.health(), hurt);
    CHECK(stacks_of(g.rig, m.resonance) == 0, "(the Resonance was spent)");
    // at full Health the targeting record still picks the nearest ally (the Bard alone): its heal reports nothing and the Resonance is spent, as the data's
    // sorting orders the allies (an injured one first, then the nearest) rather than leaving the whole ones out
    g.rig.set_current(content::kGuidHealth, g.rig.max_health());
    CHECK(grant(g.rig, m.cheerful_self, 1, t0 + 19.5) && grant(g.rig, m.resonance, 1, t0 + 19.5), "(Cheerful with one Resonance again)");
    from = g.rig.log.size();
    finish_axe(g.rig, id, t0 + 20.0, wolf);
    heals = confirms_of(g.rig, m.cheerful_hit, from);
    CHECKF(heals.size() == 1 && heals[0].mods.size() == 1 && heals[0].mods[0].effective == 0.0f, "at full Health the nearest ally is still healed, for nothing effective (%zu heals)", heals.size());
    CHECKF(stacks_of(g.rig, m.resonance) == 0, "and the Resonance is spent all the same (%d stacks)", stacks_of(g.rig, m.resonance));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// The heal grows with the Resonance the data lets it read (0.8 x Clamp(stacks,1,3) x HealingMod), and at three it adds Glee; at none the reaction does not fire.
void test_reactions2_cheerful_resonance_scales(Fixture& f) {
    std::printf("Cheerful heals 0.8 x Clamp(Resonance,1,3) x HealingMod: one, three (with Glee), and none\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    struct Round {
        int     stacks = 0;
        double  heal = 0;
        bool    glee = false;
        int     left = -1;
        int     fired = 0;
    };
    int32_t id = 500;
    double t = 100.0;
    const auto run = [&](int stacks) {
        Round out;
        out.stacks = stacks;
        d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.5);
        CHECK(grant(d.a, m.cheerful_self, 1, t - 0.5), "(Cheerful is up)");
        if (stacks > 0) CHECK(grant(d.a, m.resonance, stacks, t - 0.5), "(its Resonance is given)");
        const size_t from = d.a.log.size();
        const size_t fired = d.a.pipe->reactions_fired();
        finish_axe(d.a, id, t, wolf);
        t += 20.0;
        const auto heals = confirms_of(d.a, m.cheerful_hit, from);
        if (heals.size() == 1 && heals[0].targets.size() == 1 && !heals[0].targets[0].mods.empty()) out.heal = heals[0].targets[0].mods[0].value;
        out.glee = d.b.pawn.effect(m.glee) != nullptr;
        out.left = stacks_of(d.a, m.resonance);
        out.fired = static_cast<int>(d.a.pipe->reactions_fired() - fired);
        d.a.pipe->remove_effect(m.cheerful_self, t - 19.0);
        d.a.take();
        return out;
    };
    const Round one = run(1);
    const Round three = run(3);
    const Round none = run(0);
    CHECKF(one.fired == 1 && one.heal > 0 && one.left == 0, "Resonance 1: one heal of %.2f and the stack spent (%d left, %d reactions)", one.heal, one.left, one.fired);
    CHECKF(three.fired == 1 && three.heal > 0 && near(three.heal / one.heal, 3.0, 1e-3), "Resonance 3: the heal is three times the one-stack heal (%.2f over %.2f = %.4f)", three.heal,
           one.heal, one.heal != 0 ? three.heal / one.heal : 0.0);
    CHECKF(three.left == 0, "Resonance 3: the heal spends three stacks (0 left)");
    CHECKF(three.glee && !one.glee, "Glee comes with three stacks and not with one (%d, %d)", three.glee, one.glee);
    CHECKF(none.fired == 0 && none.heal == 0, "Resonance 0: the trigger is false, so nothing fires and nothing lands (%d reactions)", none.fired);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// Pensive at a finishing hit: the ally is short of Mana (the targeting record's filter) and the projectile restores 15 x Clamp x ManaCostMult plus
// 0.05 x Clamp x MagicalPower, as the stat mod's own input terms say; the Resonance goes.
void test_reactions2_pensive_restores_mana(Fixture& f) {
    std::printf("Pensive at a finisher restores Mana by the data's formula and spends the Resonance\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.pensive_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 1, t0 - 0.5), "(Pensive with one Resonance)");
    d.b.set_current(content::kGuidMana, d.b.pawn.stat(content::kGuidMaxMana) * 0.3);
    const double before = d.b.pawn.stat(content::kGuidMana);
    const stats::StatValue* power = d.a.cs->find(kMagicalPower);
    CHECK(power != nullptr, "(the Bard's MagicalPower is in the stat engine)");
    if (!power) return;
    int32_t id = 500;
    const size_t from = d.a.log.size();
    finish_axe(d.a, id, t0, wolf);
    const auto heals = confirms_of(d.a, m.pensive_hit, from);
    CHECKF(heals.size() == 1 && heals[0].targets.size() == 1 && heals[0].targets[0].component.ObjectId == d.pb.actor_id(), "the Mana hit landed on the ally (%zu)", heals.size());
    double restored = 0;
    if (heals.size() == 1 && !heals[0].targets.empty() && !heals[0].targets[0].mods.empty()) restored = heals[0].targets[0].mods[0].value;
    const double expected = 15.0 * kManaMult25 + 0.05 * power->value;
    const abilities::HitDef* pensive = f.catalog->hit_named("Bard_ResonantWeapon_End_Pensive_Projectile");
    CHECKF(heals.size() == 1 && !heals[0].targets.empty() && !heals[0].targets[0].mods.empty() && pensive && !pensive->mods.empty() &&
               heals[0].targets[0].mods[0].stat_mod_guid == pensive->mods[0].stat_mod_guid,
           "its number is the hit's Mana mod");
    CHECKF(near(restored, expected, 0.05), "the Mana restored is 15 x 5.6875 + 0.05 x MagicalPower(%.1f) = %.3f (%.3f)", power->value, expected, restored);
    CHECKF(d.b.pawn.stat(content::kGuidMana) > before + restored - 0.01, "the ally's Mana rose (%.3f over %.3f)", d.b.pawn.stat(content::kGuidMana), before);
    CHECKF(stacks_of(d.a, m.resonance) == 0, "the Resonance was spent (1 -> %d)", stacks_of(d.a, m.resonance));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// Epic at a finishing hit: the targeting record wants the allies under crowd control (or without Pep), up to Clamp(Resonance,1,3).  The projectile takes
// up to three CrowdControlConstraint effects off each and gives Pep; the data's Cost1 spends one Resonance when the targeting hit lands.
void test_reactions2_epic_cleanses_and_gives_pep(Fixture& f) {
    std::printf("Epic at a finisher cleanses up to three crowd-control effects, gives Pep and spends one Resonance\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    CHECK(grant(d.a, m.pep, 1, t0 - 0.5), "(the Bard holds Pep, so the targeting record leaves him out)");
    int cc = 0;
    for (const int64_t e : {m.rooted, m.snared, m.stunned, m.chilled}) cc += grant(d.b, e, 1, t0 - 0.5) ? 1 : 0;
    CHECKF(cc == 4, "(the ally holds four crowd-control effects: %d)", cc);
    int32_t id = 500;
    const size_t from = d.a.log.size();
    finish_axe(d.a, id, t0, wolf);
    const auto hits = confirms_of(d.a, m.epic_hit, from);
    CHECKF(hits.size() == 1 && hits[0].targets.size() == 1 && hits[0].targets[0].component.ObjectId == d.pb.actor_id(), "the Epic projectile landed on the ally alone (%zu)", hits.size());
    const int left = stacks_of(d.b, m.rooted) + stacks_of(d.b, m.snared) + stacks_of(d.b, m.stunned) + stacks_of(d.b, m.chilled);
    CHECKF(left == 1, "three of the four crowd-control effects are taken off, one stays (%d left)", left);
    CHECKF(d.b.pawn.effect(m.pep) != nullptr, "the ally holds Pep");
    CHECKF(stacks_of(d.a, m.resonance) == 2, "the data's Cost1 spent one Resonance (3 -> %d)", stacks_of(d.a, m.resonance));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// Two Bards, each with the Resonant Weapon's passive and a melody up: each one's finisher heals the other, and each spends his own Resonance.
void test_reactions2_two_bards_each_heal(Fixture& f) {
    std::printf("two Bards: each finisher fires his own melody at the other\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    d.b.know({"WeaponCombo_Melee_1H_Axe"});
    d.pa.party_with = d.pb.actor_id();   // the two Bards are of one party: each favours the other when he is the one hurt
    d.pb.party_with = d.pa.actor_id();
    // the first Bard's finisher heals the second
    const double t0 = 100.0, t1 = 140.0;
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 1, t0 - 0.5), "(the first Bard has Cheerful up)");
    d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.5);
    double hurt_b = d.b.health();
    int32_t id = 500;
    finish_axe(d.a, id, t0, wolf);
    CHECKF(d.b.health() > hurt_b + 1.0 && stacks_of(d.a, m.resonance) == 0, "the first Bard's melody healed the second (%.1f -> %.1f)", hurt_b, d.b.health());
    // ... and the second's heals the first
    CHECK(grant(d.b, m.cheerful_self, 1, t1 - 0.5) && grant(d.b, m.resonance, 1, t1 - 0.5), "(the second Bard has Cheerful up)");
    d.a.set_current(content::kGuidHealth, d.a.max_health() * 0.5);
    const double hurt_a = d.a.health();
    finish_axe(d.b, id, t1, wolf);
    CHECKF(d.a.health() > hurt_a + 1.0 && stacks_of(d.b, m.resonance) == 0, "the second Bard's melody healed the first (%.1f -> %.1f)", hurt_a, d.a.health());
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// The same finisher twice: the first spends the Resonance the heal read, so the second finds the trigger false and neither heals nor spends.
void test_reactions2_the_same_finisher_twice(Fixture& f) {
    std::printf("the same finisher twice: after the first spends the Resonance the second does nothing\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 1.0) && grant(d.a, m.resonance, 2, t0 - 1.0), "(Cheerful with two Resonance: the heal spends up to three)");
    d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.5);
    const double hurt = d.b.health();
    int32_t id = 500;
    size_t from = d.a.log.size();
    const size_t fired = d.a.pipe->reactions_fired();
    finish_axe(d.a, id, t0, wolf);
    const auto first = confirms_of(d.a, m.cheerful_hit, from);
    from = d.a.log.size();
    finish_axe(d.a, id, t0 + 20.0, wolf);
    const auto second = confirms_of(d.a, m.cheerful_hit, from);
    CHECKF(first.size() == 1 && d.a.pipe->reactions_fired() == fired + 1, "the first finisher fires the reaction (%zu heals, %zu fired)", first.size(), d.a.pipe->reactions_fired() - fired);
    CHECKF(second.empty() && d.a.pipe->reactions_fired() == fired + 1 && stacks_of(d.a, m.resonance) == 0,
           "the second finds no Resonance: no heal, no second reaction (%zu heals, %d stacks)", second.size(), stacks_of(d.a, m.resonance));
    CHECKF(d.b.health() > hurt + 1.0, "the first heal still stands (%.1f over %.1f)", d.b.health(), hurt);
}

// ---- the other classes' finishers ---------------------------------------------------------------------------------------------------------

// The Fighter's Momentum: its finishing hit fires a further hit at the Fighter himself (Default_Self), which the server did not follow before: the hit's
// own numbers land on the enemy and the child gives the Fighter the Momentun stat and its degen-delay effect.
void test_reactions2_fighter_momentum_fires_its_child(Fixture& f) {
    std::printf("the Fighter's Momentum finisher fires its child on the caster\n");
    constexpr int64_t kMomentum = 6064629959589298751ll;   // the stat the child adds
    const abilities::EffectDef* carrier = f.catalog->effect_named("Fighter_WeaponFinisher_Momentum");
    const abilities::EffectDef* degen = f.catalog->effect_named("Fighter_CombatMomentum_DegenDelay");
    CHECK(carrier != nullptr && degen != nullptr, "the Momentum carrier and its child's effect are in the data");
    if (!carrier || !degen) return;
    Forge g(f, 4, {});
    CHECKF(g.ok(), "the Fighter and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    if (wolf == 0) return;
    g.rig.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(g.rig, carrier->row.guid, 1, t0 - 0.5), "(the Fighter holds the Momentum effect)");
    const double before = g.rig.pawn.stat(kMomentum);
    const size_t fired = g.rig.pipe->reactions_fired();
    int32_t id = 600;
    finish_axe(g.rig, id, t0, wolf);
    CHECKF(g.rig.pipe->reactions_fired() > fired, "the Momentum reaction fired (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
    CHECKF(g.rig.pawn.effect(degen->row.guid) != nullptr, "the child's degen-delay effect is on the Fighter");
    const double after = g.rig.pawn.stat(kMomentum);
    CHECKF(after > before, "the child added Momentum (%.2f over %.2f)", after, before);
    // the passive's own reaction gives a little Momentum at the same hit; the child's formula (Min(2 x targets, 4) plus the stat's own share) gives more:
    // on the old code the finisher's child was not followed and only the passive's 0.75 came
    CHECKF(after - before >= 2.0, "the child's own share is there (%.2f over %.2f)", after, before);
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// The weapon procs whose effect casts the reaction from the hit's source or instigator (the Heartseeker buff's removal and the Bullseye buff): the masks 4
// and 16 land on the caster himself for a hit he made, as the hit instigator and source are his own.
void test_reactions2_heartseeker_and_bullseye(Fixture& f) {
    std::printf("Heartseeker's buff is removed by a spell hit; Bullseye's buff comes with a proc'd finisher\n");
    const abilities::EffectDef* heartseeker = f.catalog->effect_named("Weapon_Passive_Heartseeker_Buff");
    const abilities::EffectDef* bullseye = f.catalog->effect_named("Weapon_Proc_Bullseye");
    const abilities::EffectDef* bullseye_buff = f.catalog->effect_named("Weapon_Proc_Bullseye_2");
    CHECK(heartseeker && bullseye && bullseye_buff, "the two carrier effects and the Bullseye buff are in the data");
    if (!heartseeker || !bullseye || !bullseye_buff) return;
    {
        Forge g(f, 2, {});
        CHECKF(g.ok(), "the Bard and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return;
        g.rig.know({"Bard_Misc_QuickWit"});
        const double t0 = 100.0;
        CHECK(grant(g.rig, heartseeker->row.guid, 1, t0 - 0.5), "(the Bard holds the Heartseeker buff)");
        g.rig.set_current(content::kGuidHealth, g.rig.max_health() * 0.5);
        const size_t fired = g.rig.pipe->reactions_fired();
        g.rig.clock = t0;
        CHECK(g.rig.trigger("Bard_Misc_QuickWit", t0, true) == Refusal::None, "Quick Wit is cast at the caster");
        g.rig.run_to(t0 + 1.5);
        CHECKF(g.rig.pipe->stacks_of(heartseeker->row.guid) == 0, "the Heartseeker buff is taken off by the healing projectile's hit (%d left)", g.rig.pipe->stacks_of(heartseeker->row.guid));
        CHECKF(g.rig.pipe->reactions_fired() > fired, "the Heartseeker removal fired (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
        CHECK(g.rig.health() > g.rig.max_health() * 0.5 && g.rig.pawn.broken.empty(), "(the heal landed and the pawn took every change)");
    }
    {
        Forge g(f, 4, {});
        CHECKF(g.ok(), "the Fighter and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return;
        const uint64_t wolf = g.wolf(600, 0);
        if (wolf == 0) return;
        g.rig.know({"WeaponCombo_Melee_1H_Axe"});
        const double t0 = 100.0;
        CHECK(grant(g.rig, bullseye->row.guid, 1, t0 - 0.5), "(the Fighter holds the Bullseye proc)");
        const size_t fired = g.rig.pipe->reactions_fired();
        int32_t id = 600;
        finish_axe(g.rig, id, t0, wolf);
        CHECKF(g.rig.pipe->reactions_fired() > fired, "the Bullseye reaction fired at the proc'd finishing hit (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
        CHECKF(g.rig.pawn.effect(bullseye_buff->row.guid) != nullptr, "the Bullseye buff is on the Fighter");
        CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
    }
}

// The Rogue's bomb: the effect sits on the creature and its being-hit reaction fires when a finisher strikes that creature: the explosion hurts it and the
// enemies within 5 m.  The removal of the bomb itself is the data's own effect-filter, which the exported hit does not name: it stays (said in the report).
void test_reactions2_the_rogue_bomb_explodes(Fixture& f) {
    std::printf("the Rogue's bomb on a creature explodes at a finisher that strikes it\n");
    const abilities::EffectDef* bomb = f.catalog->effect_named("Rogue_Throw_Explosive");
    const abilities::HitDef* blast = f.catalog->hit_named("Rogue_Shuriken_Explosive_Damage");
    CHECK(bomb != nullptr && blast != nullptr, "the bomb effect and its explosion hit are in the data");
    if (!bomb || !blast) return;
    Forge g(f, 5, {});
    CHECKF(g.ok(), "the Rogue and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    const uint64_t near = g.wolf(900, 0);
    if (wolf == 0 || near == 0) return;
    g.rig.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    effects::Applied a;
    a.def = bomb;
    a.stacks = 1;
    a.duration = 30.0;
    a.in_place_limit = 1;
    g.world.apply_effects(wolf, g.player.actor_id(), {a}, t0 - 0.5);
    CHECKF(g.world.effect_stacks(wolf, bomb->row.guid) == 1, "(the bomb is on the Wolf)");
    wake(g.world, near, g.player.actor_id(), t0 - 1.0);
    const double hurt = g.world.max_health_of(wolf) - g.world.health_of(wolf);
    const double near_hurt = g.world.max_health_of(near) - g.world.health_of(near);
    const double t_before = g.world.health_of(near);
    int32_t id = 600;
    finish_axe(g.rig, id, t0, wolf);
    CHECKF(g.world.max_health_of(wolf) - g.world.health_of(wolf) > hurt + 1.0, "the Wolf the bomb was on took the explosion (%.1f over %.1f)",
           g.world.max_health_of(wolf) - g.world.health_of(wolf), hurt);
    CHECKF(g.world.max_health_of(near) - g.world.health_of(near) > near_hurt + 1.0, "the Wolf within 5 m took it too (%.1f over %.1f)",
           g.world.max_health_of(near) - g.world.health_of(near), near_hurt);
    bool dealt = false;
    for (const combat::DealtHit& h : g.player.dealt) dealt = dealt || h.hit_guid == blast->row.guid;
    CHECKF(dealt, "the explosion's damage was shown to the Rogue's client");
    CHECKF(g.world.effect_stacks(wolf, bomb->row.guid) == 1, "(the bomb stays: the exported hit names no effect to remove)");
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// The Summoner's Hex: the reaction fires on the Hexed creature, but its only hit changes the Hex's duration (ModifyEffectsIds), which this server does not
// read: the effect is unchanged.  The check is the data's shape (no number, no effect, no removal) and the effect still there (the report's reason).
void test_reactions2_the_hex_fires_and_changes_nothing(Fixture& f) {
    std::printf("the Summoner's Hex reaction fires but its duration change is not served\n");
    const abilities::EffectDef* hex = f.catalog->effect_named("Summoner_Hex");
    const abilities::HitDef* reduc = f.catalog->hit_named("Summoner_Hex_DurReduc");
    CHECK(hex != nullptr && reduc != nullptr, "the Hex effect and its reaction hit are in the data");
    if (!hex || !reduc) return;
    CHECK(reduc->mods.empty() && reduc->effects.empty() && reduc->remove_effects.empty() && reduc->recursive.empty() && reduc->fires.empty(),
          "the hit carries no number, effect, removal, projectile or child: only the duration change the server does not read");
    Forge g(f, 6, {});
    CHECKF(g.ok(), "the Summoner and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    if (wolf == 0) return;
    g.rig.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    effects::Applied a;
    a.def = hex;
    a.stacks = 1;
    a.duration = 20.0;
    a.in_place_limit = 1;
    g.world.apply_effects(wolf, g.player.actor_id(), {a}, t0 - 0.5);
    CHECKF(g.world.effect_stacks(wolf, hex->row.guid) == 1, "(the Hex is on the Wolf)");
    const double left = g.world.effect_remaining(wolf, hex->row.guid, t0 - 0.4);
    int32_t id = 600;
    finish_axe(g.rig, id, t0, wolf);
    CHECKF(g.world.effect_stacks(wolf, hex->row.guid) == 1 && near(g.world.effect_remaining(wolf, hex->row.guid, t0 + 1.0), left - 1.4, 0.6),
           "the Hex is still on the Wolf with its own time left: the -2 s modify effect is not read (%.2f over %.2f)", g.world.effect_remaining(wolf, hex->row.guid, t0 + 1.0), left);
}

// The Mage's Earth finisher: the reaction fires and its hit shows only its FX (a confirmation with the target and no numbers); the EssenceHarvest reaction's
// trigger does not compile in the data, so it can never fire (the report's reason).
void test_reactions2_mage_earth_and_essence_harvest(Fixture& f) {
    std::printf("the Mage's Earth finisher shows its FX hit; the EssenceHarvest trigger does not compile\n");
    const abilities::EffectDef* earth = f.catalog->effect_named("Mage_EE_Earth");
    const abilities::HitDef* fx = f.catalog->hit_named("Mage_EE_Earth_Damage");
    const abilities::ReactionDef* harvest = f.catalog->reaction(record_guid(real_db_path(), "ReactionRecordBase", "Weapon_Proc_EssenceHarvest"));
    CHECK(earth && fx, "the Earth carrier and its hit are in the data");
    CHECKF(harvest && f.catalog->program(harvest->row.trigger_expr) < 0, "the EssenceHarvest trigger does not compile, so it stays inert");
    if (!earth || !fx) return;
    Forge g(f, 8, {});
    CHECKF(g.ok(), "the Mage and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    if (wolf == 0) return;
    g.rig.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(g.rig, earth->row.guid, 1, t0 - 0.5), "(the Mage holds the Earth enchant)");
    const size_t fired = g.rig.pipe->reactions_fired();
    const size_t from = g.rig.log.size();
    int32_t id = 600;
    finish_axe(g.rig, id, t0, wolf);
    const auto hits = confirms_of(g.rig, fx->row.guid, from);
    CHECKF(g.rig.pipe->reactions_fired() > fired && hits.size() == 1 && hits[0].targets.size() == 1, "the Earth reaction fired and its FX hit was confirmed on the Wolf (%zu over %zu, %zu hits)",
           g.rig.pipe->reactions_fired() - fired, fired, hits.size());
    CHECKF(hits.size() == 1 && hits[0].targets[0].component.ObjectId == wolf && hits[0].targets[0].mods.empty(), "the FX hit names the Wolf and carries no number");
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// The 65 finisher reactions the milestone names, by their guids: every one is in the catalog, and the data's shapes are the groups the report counts.
void test_reactions2_the_65_finisher_reactions(Fixture& f) {
    std::printf("the 65 finisher reactions: 2 being-hit, 2 cast by the hit's source or instigator, 5 that only fire a further hit, 1 that does not compile\n");
    static const int64_t kReactions[] = {
        6064632077112575721ll, 6064632077134726891ll, 6064630775794307814ll, 6064632469023570856ll, 6064631238524207273ll, 6064631091496884132ll, 6064631194143725961ll,
        6064631159496049287ll, 6064631159634202679ll, 109196139255421ll,   6064632734569202506ll, 6064632734821852105ll, 6064630491990524149ll, 6064629975657632027ll,
        6064630498464899259ll, 6064629105808572416ll, 6064629761748903312ll, 6064629784867475790ll, 6064629784579180498ll, 6064633176577806536ll, 6064630356495564801ll,
        6064632989859529212ll, 6064632938622162611ll, 6064633221730809163ll, 6064633498815368128ll, 6064633272554558482ll, 6064634092470539243ll, 6064629507020624814ll,
        6064629500132000169ll, 6064631077012833586ll, 6064631954154171635ll, 6064631867605844239ll, 6064631954156072181ll, 6064631846326960582ll, 6064631954155416820ll,
        6064631954110392800ll, 6064631926665839239ll, 6064631940665509322ll, 6064631867605057806ll, 6064631954118388196ll, 6064631846306709957ll, 6064631940664591817ll,
        6064631954119371237ll, 6064631783074955386ll, 6064631783784449315ll, 6064631754164404224ll, 6064631754228629890ll, 6064632322034834397ll, 6064632322035227615ll,
        6064632322318216170ll, 6064632328641093851ll, 6064630493278585975ll, 6064630560394119145ll, 6064632310110553824ll, 6064631749776965634ll, 6064631749796429828ll,
        6064632716560171011ll, 6064631940814015993ll, 6064631941782060308ll, 6064631443002533085ll, 6064631456704510999ll, 6064632060973093409ll, 6064631748531126415ll,
        6064631086385792262ll, 6064631454155443090ll,
    };
    CHECK(sizeof(kReactions) / sizeof(kReactions[0]) == 65, "(the milestone's 65 reactions)");
    size_t found = 0, being_hit = 0, from_source = 0, broken = 0, further_only = 0, other = 0;
    for (const int64_t guid : kReactions) {
        const abilities::ReactionDef* r = f.catalog->reaction(guid);
        if (!r) continue;
        ++found;
        const content::ReactionRow& row = r->row;
        if ((row.bind_hit & 2) != 0) ++being_hit;
        if (row.instigator == "HitSource" || row.instigator == "HitInstigator") ++from_source;
        if (f.catalog->program(row.trigger_expr) < 0) {
            ++broken;
            continue;
        }
        bool content = false, child = false;
        for (const int64_t hit_guid : row.hits) {
            const abilities::HitDef* hit = f.catalog->hit(hit_guid);
            if (!hit) continue;
            content = content || !hit->mods.empty() || !hit->effects.empty() || !hit->remove_effects.empty() || !hit->fires.empty() || !hit->modify_cooldowns.empty() ||
                              hit->row.has_fx;
            child = child || !hit->recursive.empty();
        }
        if (!content && child) {
            ++further_only;
        } else {
            ++other;
        }
    }
    CHECKF(found == 65, "every one of the 65 reactions is in the catalog (%zu)", found);
    CHECKF(being_hit == 2, "2 are bound to the holder being hit (the Rogue's bomb and the Summoner's Hex: a creature carries them) (%zu)", being_hit);
    CHECKF(from_source == 2, "2 are cast from the hit's own source or instigator (Heartseeker's removal and Bullseye) (%zu)", from_source);
    CHECKF(broken == 1, "1 trigger does not compile (EssenceHarvest) (%zu)", broken);
    CHECKF(further_only == 5, "5 hits only fire a further hit (the three melodies, the Fighter's Momentum and the Rogue's explosion) (%zu)", further_only);
    CHECKF(other == 59, "the other 59 carry a number, an effect, a removal, a cooldown or a FX (%zu)", other);
}

// ---- being hit ------------------------------------------------------------------------------------------------------------------------------

// The Tank's own effects at a creature's hit: the Reflect answers the attacker with its damage and Tripped, the block's Refresh gives the Tank Mana, and
// the General Block FX shows.  The incoming hit is the recorded one (the Deer's head swipe), the attacker a real Wolf of the world.
void test_reactions2_tank_reflect_on_a_creatures_hit(Fixture& f) {
    std::printf("a creature's hit on the Tank fires his Reflect, Tripped, Refresh and Block FX\n");
    const abilities::EffectDef* reflect = f.catalog->effect_named("Tank_Reflect");
    const abilities::EffectDef* block_mit = f.catalog->effect_named("Tank_Reflect_BlockMitigation");
    const abilities::EffectDef* block_fx = f.catalog->effect_named("General_Block_FX_Reaction");
    const abilities::HitDef* swipe = f.catalog->hit_named("Deer_HeadSwipe_Hit");
    const abilities::HitDef* reflect_hit = f.catalog->hit_named("Tank_Deflect_Reflect_Hit");
    const abilities::HitDef* tripped_hit = f.catalog->hit_named("Tank_Deflect->Tripped");
    const abilities::HitDef* refreshing = f.catalog->hit_named("Tank_Deflect_Refreshing");
    const abilities::HitDef* fx_hit = f.catalog->hit_named("General_Block_FX_Reaction_Hit");
    CHECK(reflect && block_mit && block_fx && swipe && reflect_hit && tripped_hit && refreshing && fx_hit, "the Tank's two effects, the block FX effect and the hits are in the data");
    if (!reflect || !block_mit || !block_fx || !swipe || !reflect_hit || !tripped_hit || !refreshing || !fx_hit) return;
    Forge g(f, 7, {"Tank_Passive_Reflect_Refreshing", "Tank_Passive_Reflect_Oppressive"});
    CHECKF(g.ok(), "the Tank and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    if (wolf == 0) return;
    const double t0 = 100.0;
    CHECK(grant(g.rig, reflect->row.guid, 1, t0 - 0.5) && grant(g.rig, block_mit->row.guid, 1, t0 - 0.5) && grant(g.rig, block_fx->row.guid, 1, t0 - 0.5),
          "(the Tank holds the Reflect, the block mitigation and the block FX)");
    wake(g.world, wolf, g.player.actor_id(), t0 - 1.0);
    g.rig.set_current(content::kGuidMana, g.rig.pawn.stat(content::kGuidMaxMana) * 0.5);
    const double wolf_before = g.world.health_of(wolf);
    const double mana_before = g.rig.pawn.stat(content::kGuidMana);
    const double own_health = g.rig.health();
    combat::IncomingHit in;
    in.instigator = wolf;
    in.hit_guid = swipe->row.guid;
    in.source_type = content::kSourceTypeAbility;
    in.source_guid = 0;
    in.flags = 0;
    in.mods.push_back({1, content::kGuidHealth, -30.0});
    in.at = t0;
    const size_t fired = g.rig.pipe->reactions_fired();
    const size_t from = g.rig.log.size();
    g.rig.pipe->receive_hit(in, t0);
    g.rig.take();
    CHECKF(g.rig.health() < own_health - 29.0 && g.rig.health() > own_health - 31.0, "the hit took its 30 from the Tank (%.1f)", g.rig.health());
    CHECKF(g.world.health_of(wolf) < wolf_before - 1.0, "the Wolf took the reflect (%.1f of %.1f)", g.world.health_of(wolf), wolf_before);
    const auto reflects = confirms_of(g.rig, reflect_hit->row.guid, from);
    CHECKF(reflects.size() == 1 && reflects[0].source_type == content::kSourceTypeReaction && reflects[0].targets.size() == 1 && reflects[0].targets[0].component.ObjectId == wolf,
           "the Reflect hit was confirmed on the Wolf as the reaction's (%zu)", reflects.size());
    CHECKF(reflects.size() == 1 && !reflects[0].targets[0].mods.empty() && reflects[0].targets[0].mods[0].value < 0, "the reflect's number is a loss on the Wolf");
    CHECKF(g.world.effect_stacks(wolf, 109122787956344ll) > 0, "the Wolf is Tripped by the same reaction");
    CHECKF(g.rig.pawn.stat(content::kGuidMana) > mana_before + 0.01, "the block's Refresh gave the Tank Mana (%.2f over %.2f)", g.rig.pawn.stat(content::kGuidMana), mana_before);
    CHECKF(!confirms_of(g.rig, fx_hit->row.guid, from).empty(), "the General Block FX hit was confirmed");
    CHECKF(g.rig.pipe->reactions_fired() >= fired + 3, "three reactions of the Tank's effects fired (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// Two creatures hit the Tank at the same moment: each hit is its own event, and each reflect lands on the creature that struck him.
void test_reactions2_two_creatures_hit_the_tank_at_once(Fixture& f) {
    std::printf("two creatures hit the Tank at once: each reflect answers its own attacker\n");
    const abilities::EffectDef* reflect = f.catalog->effect_named("Tank_Reflect");
    const abilities::HitDef* swipe = f.catalog->hit_named("Deer_HeadSwipe_Hit");
    const abilities::HitDef* reflect_hit = f.catalog->hit_named("Tank_Deflect_Reflect_Hit");
    CHECK(reflect && swipe && reflect_hit, "the Reflect effect and the two hits are in the data");
    if (!reflect || !swipe || !reflect_hit) return;
    Forge g(f, 7, {});
    CHECKF(g.ok(), "the Tank and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t first = g.wolf(600, 0);
    const uint64_t second = g.wolf(0, 600);
    if (first == 0 || second == 0) return;
    const double t0 = 100.0;
    CHECK(grant(g.rig, reflect->row.guid, 1, t0 - 0.5), "(the Tank holds the Reflect)");
    wake(g.world, first, g.player.actor_id(), t0 - 1.0);
    wake(g.world, second, g.player.actor_id(), t0 - 1.0);
    const double first_before = g.world.health_of(first), second_before = g.world.health_of(second);
    const size_t from = g.rig.log.size();
    for (const uint64_t wolf : {first, second}) {
        combat::IncomingHit in;
        in.instigator = wolf;
        in.hit_guid = swipe->row.guid;
        in.source_type = content::kSourceTypeAbility;
        in.flags = 0;
        in.mods.push_back({1, content::kGuidHealth, -20.0});
        in.at = t0;
        g.rig.pipe->receive_hit(in, t0);
    }
    g.rig.take();
    std::set<uint64_t> struck;
    for (const bootstrap::HitConfirm& c : confirms_of(g.rig, reflect_hit->row.guid, from)) {
        for (const bootstrap::HitTargetOut& t : c.targets) struck.insert(t.component.ObjectId);
    }
    CHECKF(struck.size() == 2 && struck.count(first) == 1 && struck.count(second) == 1, "each Wolf was struck by one reflect of its own (%zu)", struck.size());
    CHECKF(g.world.health_of(first) < first_before - 1.0 && g.world.health_of(second) < second_before - 1.0, "both Wolves lost Health to it");
}

// Another player's hit on the Tank: the PvP rules take a harmful hit between players to the target's session inbox (IncomingHit, receive_hit), which
// applies it in the Tank's own next step; the same event fires there, and the Reflect lands back on that player through the same harmful route.  The attacker
// holds the PvP key, so his first harmful action on the Tank is allowed and makes him a combatant; the Tank's reflect may then harm a combatant.
void test_reactions2_tank_reflect_on_another_players_hit(Fixture& f) {
    std::printf("another player's hit on the Tank fires his Reflect onto that player\n");
    const abilities::EffectDef* reflect = f.catalog->effect_named("Tank_Reflect");
    const abilities::HitDef* strike = f.catalog->hit_named("WeaponCombo_Axe_1H_Hit4+");
    const abilities::HitDef* reflect_hit = f.catalog->hit_named("Tank_Deflect_Reflect_Hit");
    CHECK(reflect && strike && reflect_hit, "the Reflect effect and the hits are in the data");
    if (!reflect || !strike || !reflect_hit) return;
    Duo d(f, 7, 5, {"Tank_Passive_Reflect_Refreshing"}, {});
    CHECKF(d.ok(), "the two players and the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pa.pvp_rules = true;           // the PvP rules judge a hit between the two
    d.pb.pvp_rules = true;
    d.pb.pvp_force = true;           // the attacker holds the PvP key: his first harmful action on a non-combatant is allowed and flags him
    d.pa.pipeline_hits = true;       // his session applies what reaches his inbox through the pipeline (PlayerSession::take_hit)
    d.pa.queue_player_hits = true;   // ... in his own next step, not inside the attacker's call (PlayerSession::CombatSide)
    const double t0 = 100.0;
    CHECK(grant(d.a, reflect->row.guid, 1, t0 - 0.5), "(the Tank holds the Reflect)");
    const double attacker_before = d.b.health();
    combat::Resolved r;
    r.mods.push_back({1, content::kGuidHealth, -40.0});
    combat::HitOrigin origin;
    origin.hit = strike;
    origin.source_type = content::kSourceTypeAbility;
    origin.source_guid = 0;
    const double tank_before = d.a.health();
    const combat::Delivery hit_on_tank = d.portb.deliver_from(d.pa.actor_id(), r, t0, origin);   // the caster's pipeline delivers a harmful hit to another player
    CHECKF(hit_on_tank.applied, "the other player's hit reached the Tank's session");
    d.pa.take_incoming(t0);   // (the Tank's own next step applies it and raises his being-hit reactions there)
    d.a.run_to(t0 + 0.1);
    d.a.take();
    d.b.take();
    CHECKF(d.a.health() < tank_before - 39.0 && d.a.health() > tank_before - 41.0, "the other player's hit took its 40 from the Tank (%.1f)", d.a.health());
    CHECKF(d.b.health() < attacker_before - 0.5, "the Tank's Reflect took its part from the other player (%.1f of %.1f)", d.b.health(), attacker_before);
    bool through_inbox = false;
    for (const combat::IncomingHit& h : d.pb.hits) through_inbox = through_inbox || h.hit_guid == reflect_hit->row.guid;
    CHECKF(through_inbox, "the reflect landed through the other player's own ally path (apply_ally_landing) no more: the PvP harmful route took it to his session's inbox (receive_hit)");
    bool through_ally = false;
    for (const int64_t h : d.pb.ally_lands) through_ally = through_ally || h == reflect_hit->row.guid;
    CHECKF(!through_ally, "the reflect is a harmful hit: it does not take the ally path (apply_ally_landing)");
    const auto reflects = confirms_of(d.a, reflect_hit->row.guid);
    CHECKF(reflects.size() == 1 && reflects[0].targets.size() == 1 && reflects[0].targets[0].component.ObjectId == d.pb.actor_id(),
           "the Tank's runtime confirmed the reflect on the other player (%zu)", reflects.size());
    CHECKF(d.pb.pvp_flag == combat::pvp::Flag::Combatant, "the attacker who harmed the Tank is a combatant now: his first harmful action flags him");
    CHECKF(d.pa.pvp_flag == combat::pvp::Flag::Combatant, "the Tank's harmful reflect flags the Tank in turn (harming a combatant)");
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// An ally who reconnects (his session leaves the world and a new one joins with a new actor): the Bard's targeting reads the world's players when the
// finisher fires, so the new actor is healed, and the one who left is not.
void test_reactions2_a_reconnected_ally_is_still_healed(Fixture& f) {
    std::printf("a reconnected ally is still the one a finisher's melody heals\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    int32_t id = 500;
    CHECK(grant(d.a, m.cheerful_self, 1, 99.5) && grant(d.a, m.resonance, 1, 99.5), "(Cheerful with one Resonance)");
    d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.5);
    const double hurt = d.b.health();
    finish_axe(d.a, id, 100.0, wolf);
    CHECKF(d.b.health() > hurt + 1.0, "(the first ally is healed before he leaves: %.1f over %.1f)", d.b.health(), hurt);
    const double healed = d.b.health();
    // the first ally's session leaves and a new one joins at his place: a new player and a new actor id
    d.world.remove_player(&d.pb);
    Rig c(2, 25, {}, true, 103);
    if (!c.ok()) return;
    combat_rig::FakePlayer pc(c, 80, {300, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    c.set_current(content::kGuidHealth, c.max_health() * 0.5);
    const double before = c.health();
    CHECK(grant(d.a, m.cheerful_self, 1, 119.5) && grant(d.a, m.resonance, 1, 119.5), "(Cheerful with one Resonance again)");
    const size_t from = d.a.log.size();
    finish_axe(d.a, id, 120.0, wolf);
    const auto heals = confirms_of(d.a, m.cheerful_hit, from);
    CHECKF(heals.size() == 1 && !heals[0].targets.empty() && heals[0].targets[0].component.ObjectId == pc.actor_id(), "the heal went to the new session's actor (80) (%zu hits)", heals.size());
    CHECKF(c.health() > before + 1.0, "the reconnected ally's Health rose (%.1f over %.1f)", c.health(), before);
    CHECKF(d.b.health() == healed, "(the session that left is not healed any more: %.1f)", d.b.health());
    d.world.remove_player(&pc);
    CHECK(d.a.pawn.broken.empty() && c.pawn.broken.empty(), "(both pawns took every change)");
}

}  // namespace reactions2

using reactions2::test_reactions2_cheerful_heals_the_injured_ally;
using reactions2::test_reactions2_cheerful_picks_the_hurt_ally;
using reactions2::test_reactions2_cheerful_resonance_scales;
using reactions2::test_reactions2_cheerful_solo;
using reactions2::test_reactions2_epic_cleanses_and_gives_pep;
using reactions2::test_reactions2_fighter_momentum_fires_its_child;
using reactions2::test_reactions2_heartseeker_and_bullseye;
using reactions2::test_reactions2_mage_earth_and_essence_harvest;
using reactions2::test_reactions2_pensive_restores_mana;
using reactions2::test_reactions2_a_reconnected_ally_is_still_healed;
using reactions2::test_reactions2_tank_reflect_on_a_creatures_hit;
using reactions2::test_reactions2_tank_reflect_on_another_players_hit;
using reactions2::test_reactions2_the_65_finisher_reactions;
using reactions2::test_reactions2_the_hex_fires_and_changes_nothing;
using reactions2::test_reactions2_the_rogue_bomb_explodes;
using reactions2::test_reactions2_the_same_finisher_twice;
using reactions2::test_reactions2_two_bards_each_heal;
using reactions2::test_reactions2_two_creatures_hit_the_tank_at_once;

AOC_REGISTER_TEST(test_reactions2_cheerful_heals_the_injured_ally);
AOC_REGISTER_TEST(test_reactions2_cheerful_picks_the_hurt_ally);
AOC_REGISTER_TEST(test_reactions2_cheerful_solo);
AOC_REGISTER_TEST(test_reactions2_cheerful_resonance_scales);
AOC_REGISTER_TEST(test_reactions2_pensive_restores_mana);
AOC_REGISTER_TEST(test_reactions2_epic_cleanses_and_gives_pep);
AOC_REGISTER_TEST(test_reactions2_two_bards_each_heal);
AOC_REGISTER_TEST(test_reactions2_the_same_finisher_twice);
AOC_REGISTER_TEST(test_reactions2_fighter_momentum_fires_its_child);
AOC_REGISTER_TEST(test_reactions2_heartseeker_and_bullseye);
AOC_REGISTER_TEST(test_reactions2_the_rogue_bomb_explodes);
AOC_REGISTER_TEST(test_reactions2_the_hex_fires_and_changes_nothing);
AOC_REGISTER_TEST(test_reactions2_mage_earth_and_essence_harvest);
AOC_REGISTER_TEST(test_reactions2_the_65_finisher_reactions);
AOC_REGISTER_TEST(test_reactions2_tank_reflect_on_a_creatures_hit);
AOC_REGISTER_TEST(test_reactions2_tank_reflect_on_another_players_hit);
AOC_REGISTER_TEST(test_reactions2_two_creatures_hit_the_tank_at_once);
AOC_REGISTER_TEST(test_reactions2_a_reconnected_ally_is_still_healed);

#pragma pop_macro("CHECKF")
#pragma pop_macro("CHECK")
