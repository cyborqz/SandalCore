// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mage_fixes_tests.inc: the four Mage findings of the game test (included by combat_registered.inc inside
//  test_worldd_combat's anonymous namespace, at the end)
//
//    the Magma Field    the ability item carries the record's DynamicCastDuration, the time the client's cast bar shows
//    the Blizzard       its damage ticks are taken for the whole channel, not only 5 s from the cost
//    the Arcane Circle  the buff its circle puts on the caster ends the moment he steps out, and comes back when he steps in
//    the Prismatic Beam its tick's damage keeps being taken (and keeps its threat) after eight requests, as the recorded cast does
// ============================================================================

#undef CHECK
#undef CHECKF
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define CHECKF(cond, ...) do { \
    char mage_line[1024]; \
    std::snprintf(mage_line, sizeof mage_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", mage_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", mage_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace mage_fixes {

/// The ability item the cast put on the pawn, by the ability's record.
const bootstrap::ActiveAbilityItem* item_of(const Rig& rig, int64_t ability) {
    for (const auto& [key, item] : rig.pawn.abilities)
        if (item.ability_guid == ability) return &item;
    return nullptr;
}

/// How many effects of `guid` the caster holds (the client's CurrentEffects items).
size_t effects_of(const Rig& rig, int64_t guid) {
    size_t n = 0;
    for (const auto& [key, e] : rig.pawn.effects) n += e.effect_guid == guid ? 1 : 0;
    return n;
}

/// The confirmations of a hit that named a target, from everything the rig has seen.
int confirmations_of(const Rig& rig, int64_t hit_guid) {
    int n = 0;
    for (const Seen& s : rig.log)
        for (const bootstrap::HitConfirm& c : s.confirmations) n += c.record_guid == hit_guid && !c.targets.empty() ? 1 : 0;
    return n;
}

/// How many zone actors of the kind the rig has seen.
size_t zone_ops(const Rig& rig, bootstrap::ZoneOp::Kind kind) {
    size_t n = 0;
    for (const Seen& s : rig.log)
        for (const bootstrap::ZoneOp& op : s.zones) n += op.kind == kind ? 1 : 0;
    return n;
}

/// The client's RequestHitVerification for one hit at `at`, on the caster's own component and `target` (0: nobody else).
AbilityPipeline::HitVerdict ask_tick(Rig& rig, const abilities::AbilityDef& def, const abilities::HitDef& hit, double at, uint64_t target, bool own) {
    std::vector<uint64_t> targets;
    if (target != 0) targets.push_back(target);
    return rig.pipe->hit_request(def.row.guid, content::kTargetingDefaultSelf, hit.row.guid, own ? 1 : 0, own ? 1 : 0, at, targets);
}

}  // namespace mage_fixes

// ---- F1: the Magma Field's cast bar ----------------------------------------------------------------
// The client's cast bar for a cast whose time the record gives (DynamicCastDuration) has no montage time to read: the ability item's
// ReplicatedProgressInfo.CastDuration is the only time on the wire, and worldd left it 0, so the bar never started although the server ran the
// cast to its deadline. The data's time is SelectFloat(the Pyromania talent, 1.0, 2.0) s (the ability's CastExpr).
void test_mage_fixes_the_magma_field_item_carries_its_cast_time(Fixture& f) {
    std::printf("the Magma Field: the ability item carries the record's cast time (1.0 s, 2.0 s with Pyromania), the time the client's cast bar shows\n");
    const auto cast_once = [&](const std::vector<std::string>& skills, double expected, const char* what) {
        Forge m(f, 8, skills);
        CHECKF(m.ok(), "the Mage with %s and his world: %s %s", what, m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        m.rig.know({"Mage_MagmaField"});
        const int64_t ability = m.rig.ability("Mage_MagmaField");
        const double t0 = 100.0;
        TriggerInfo info;
        info.place = AimPlace{300, 0, 1000};   // the ground under the Wolf, as the client casts a field
        const int32_t id = m.rig.next_client_id++;
        const Refusal refusal = m.rig.pipe->trigger(ability, id, t0, info);
        m.rig.take();
        CHECKF(refusal == Refusal::None, "the Magma Field is cast at a place (%s)", refusal_name(refusal));
        if (refusal != Refusal::None) return;
        m.rig.run_to(t0 + 0.2);
        const bootstrap::ActiveAbilityItem* item = mage_fixes::item_of(m.rig, ability);
        CHECKF(item != nullptr, "the cast's item is on the pawn (the client's cast bar is built from it)");
        if (!item) return;
        CHECKF(std::fabs(item->cast_duration - expected) < 1e-3, "the item's CastDuration is %.3f s (the data's DynamicCastDuration for %s: %.1f s)", item->cast_duration, what, expected);
        CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
    };
    cast_once({}, 2.0, "no talent");
    cast_once({"Mage_Passive_MagmaField_Pyromania"}, 1.0, "Pyromania");
}
AOC_REGISTER_TEST(test_mage_fixes_the_magma_field_item_carries_its_cast_time);

// ---- F2: the Blizzard's channel --------------------------------------------------------------------
// The ability's text: "Channel a massive blizzard around you, dealing ... every second." The client asks the damage hit again and again while the
// key is down; the cast's request window was 5 s from the cost, so the later ticks were refused. The channel's own end (the montage's clear) is the
// window.
void test_mage_fixes_the_blizzard_ticks_for_its_whole_channel(Fixture& f) {
    std::printf("the Blizzard: the damage hit is taken at every tick of the channel (9 s), not only 5 s from the cost\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    CHECK(def && damage, "the data has the Blizzard's damage hit");
    if (!def || !damage) return;
    const uint64_t wolf = m.wolf(300, 0);
    CHECK(wolf != 0, "a Wolf 3 m from the Mage");
    if (!wolf) return;
    m.rig.know({"Mage_Blizzard"});
    m.rig.pipe->set_data_timings(true);   // (the Blizzard's recorded channel end is the window this test is about)
    m.rig.asks = false;   // (the ticks are asked below, one by one, as the channel's client does)
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
    m.rig.run_to(t0 + 0.2);
    int taken = 0;
    for (int i = 0; i < 9; ++i) {
        const double at = t0 + 0.7 + 0.634 * static_cast<double>(i);
        if (mage_fixes::ask_tick(m.rig, *def, *damage, at, wolf, false) == AbilityPipeline::HitVerdict::Scheduled) ++taken;
        m.rig.run_to(at + 0.1);
    }
    CHECKF(taken == 9, "the channel's nine ticks are all taken (%d of 9, the last %.2f s after the trigger)", taken, 0.7 + 0.634 * 8.0);
    CHECKF(m.world.health_of(wolf) < m.world.max_health_of(wolf) - 0.01, "the Wolf is hurt by the ticks (%.1f of %.1f Health)", m.world.health_of(wolf), m.world.max_health_of(wolf));
    CHECKF(mage_fixes::confirmations_of(m.rig, damage->row.guid) == 9, "the Wolf shows nine damage numbers (%d)", mage_fixes::confirmations_of(m.rig, damage->row.guid));
    // the same cast twice: after the cooldown a second channel works, with its own window
    m.rig.run_to(t0 + 191.0);   // (the Blizzard's cooldown is 90 s)
    CHECK(m.rig.trigger("Mage_Blizzard", t0 + 191.0, false) == Refusal::None, "a second Blizzard is cast after the cooldown");
    m.rig.run_to(t0 + 191.2);
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 196.2, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "the second channel takes a tick 5.1 s after its cost, as the first does");
    m.rig.run_to(t0 + 196.4);
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_mage_fixes_the_blizzard_ticks_for_its_whole_channel);

// ---- F2, the channel cut short ---------------------------------------------------------------------
// A tick after the key's release, after a Dodge that took the channel, after another ability's press and after the caster's death is not taken; a
// Dodge after a tick (the channel has gone out) changes nothing.
void test_mage_fixes_the_blizzard_channel_ends_with_its_cause(Fixture& f) {
    std::printf("the Blizzard: its channel ends with the key's release, a Dodge before a tick, another ability, or the caster's death; a Dodge after a tick changes nothing\n");
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    CHECK(def && damage, "the data has the Blizzard's damage hit");
    if (!def || !damage) return;
    const double t0 = 100.0;
    {
        // the key goes up after a tick: the record is dropped; a tick afterwards finds no cast
        Forge m(f, 8, {});
        CHECKF(m.ok(), "the Mage of the release: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const uint64_t wolf = m.wolf(300, 0);
        m.rig.know({"Mage_Blizzard"});
        m.rig.pipe->set_data_timings(true);   // (the Blizzard's recorded channel end is the window this test is about)
        m.rig.asks = false;
        const int32_t id = m.rig.next_client_id;
        CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
        m.rig.run_to(t0 + 0.2);
        CHECK(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "its first tick is taken");
        m.rig.run_to(t0 + 0.8);
        m.rig.pipe->cancel(id, t0 + 1.0);
        m.rig.run_to(t0 + 1.2);
        CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 1.8, wolf, false) == AbilityPipeline::HitVerdict::NoCast, "a tick after the key went up (and past the moment the channel's last hit is still taken) finds no cast");
    }
    {
        // a Dodge before the first tick takes the channel
        Forge m(f, 8, {});
        CHECKF(m.ok(), "the Mage of the early Dodge: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const uint64_t wolf = m.wolf(300, 0);
        m.rig.know({"Mage_Blizzard"});
        m.rig.pipe->set_data_timings(true);   // (the Blizzard's recorded channel end is the window this test is about)
        m.rig.asks = false;
        CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast again");
        CHECK(m.rig.pipe->end_by_dodge(t0 + 0.5) >= 1, "a Dodge before its first tick takes the channel");
        m.rig.run_to(t0 + 0.7);
        CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 1.5, wolf, false) == AbilityPipeline::HitVerdict::NoCast, "after the Dodge the channel takes no tick");
    }
    {
        // a Dodge after a tick changes nothing
        Forge m(f, 8, {});
        CHECKF(m.ok(), "the Mage of the late Dodge: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const uint64_t wolf = m.wolf(300, 0);
        m.rig.know({"Mage_Blizzard"});
        m.rig.pipe->set_data_timings(true);   // (the Blizzard's recorded channel end is the window this test is about)
        m.rig.asks = false;
        CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast once more");
        m.rig.run_to(t0 + 0.2);
        CHECK(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "its first tick is taken");
        m.rig.run_to(t0 + 0.8);
        CHECK(m.rig.pipe->end_by_dodge(t0 + 1.0) == 0, "a Dodge after its first tick ends nothing");
        CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 1.5, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "the channel goes on after that Dodge");
    }
    {
        // another ability's press ends the channel of the first
        Forge m(f, 8, {});
        CHECKF(m.ok(), "the Mage of the second ability: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const uint64_t wolf = m.wolf(300, 0);
        m.rig.know({"Mage_Blizzard", "Mage_Frostbolt"});
        m.rig.pipe->set_data_timings(true);   // (the Blizzard's recorded channel end is the window this test is about)
        m.rig.asks = false;
        CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
        m.rig.run_to(t0 + 0.2);
        CHECK(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "its first tick is taken");
        m.rig.run_to(t0 + 1.0);
        TriggerInfo info;
        info.target_count = 1;
        info.target_actors = {wolf};
        const int32_t id = m.rig.next_client_id++;
        const Refusal frost = m.rig.pipe->trigger(m.rig.ability("Mage_Frostbolt"), id, t0 + 1.0, info);
        m.rig.take();
        CHECKF(frost == Refusal::None, "a Frostbolt is cast at the Wolf (%s)", refusal_name(frost));
        m.rig.run_to(t0 + 1.2);
        CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 2.0, wolf, false) == AbilityPipeline::HitVerdict::NoCast, "the Frostbolt ended the Blizzard's channel: its next tick finds no cast");
    }
    {
        // the caster's death ends the channel
        Forge m(f, 8, {});
        CHECKF(m.ok(), "the Mage of the death: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const uint64_t wolf = m.wolf(300, 0);
        m.rig.know({"Mage_Blizzard"});
        m.rig.pipe->set_data_timings(true);   // (the Blizzard's recorded channel end is the window this test is about)
        m.rig.asks = false;
        CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
        m.rig.run_to(t0 + 0.2);
        CHECK(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "its first tick is taken");
        m.rig.run_to(t0 + 0.8);
        m.rig.pipe->set_dead(true);
        m.rig.run_to(t0 + 1.2);
        CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 1.5, wolf, false) == AbilityPipeline::HitVerdict::NoCast, "a dead caster takes no tick of the channel");
    }
}
AOC_REGISTER_TEST(test_mage_fixes_the_blizzard_channel_ends_with_its_cause);

// ---- F2, two Mages side by side --------------------------------------------------------------------
// Two Mages channel their own Blizzards at their own Wolves: each cast's window is its own.
void test_mage_fixes_two_blizzards_side_by_side(Fixture& f) {
    std::printf("two Mages side by side: each Blizzard's ticks are its own cast's, at its own Wolf\n");
    Forge a(f, 8, {}), b(f, 8, {});
    CHECKF(a.ok() && b.ok(), "the two Mages and their worlds: %s %s / %s %s", a.rig.problem.c_str(), a.w.problem.c_str(), b.rig.problem.c_str(), b.w.problem.c_str());
    if (!a.ok() || !b.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    const uint64_t wolf_a = a.wolf(300, 0), wolf_b = b.wolf(300, 0);
    CHECK(def && damage && wolf_a != 0 && wolf_b != 0, "the data and a Wolf for each Mage");
    if (!def || !damage || !wolf_a || !wolf_b) return;
    a.rig.know({"Mage_Blizzard"});
    b.rig.know({"Mage_Blizzard"});
    a.rig.pipe->set_data_timings(true);
    b.rig.pipe->set_data_timings(true);
    a.rig.asks = b.rig.asks = false;
    const double t0 = 200.0;
    CHECK(a.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the first Mage's Blizzard is cast");
    CHECK(b.rig.trigger("Mage_Blizzard", t0 + 0.05, false) == Refusal::None, "the second Mage's Blizzard is cast");
    a.rig.run_to(t0 + 0.3);
    b.rig.run_to(t0 + 0.35);
    int taken_a = 0, taken_b = 0;
    for (int i = 0; i < 9; ++i) {
        const double at = t0 + 0.7 + 0.634 * static_cast<double>(i);
        if (mage_fixes::ask_tick(a.rig, *def, *damage, at, wolf_a, false) == AbilityPipeline::HitVerdict::Scheduled) ++taken_a;
        a.rig.run_to(at + 0.1);
        if (mage_fixes::ask_tick(b.rig, *def, *damage, at + 0.05, wolf_b, false) == AbilityPipeline::HitVerdict::Scheduled) ++taken_b;
        b.rig.run_to(at + 0.15);
    }
    CHECKF(taken_a == 9 && taken_b == 9, "both channels take their nine ticks (%d and %d)", taken_a, taken_b);
    CHECKF(mage_fixes::confirmations_of(a.rig, damage->row.guid) == 9 && mage_fixes::confirmations_of(b.rig, damage->row.guid) == 9,
           "each Wolf shows its own Mage's nine numbers (%d and %d)", mage_fixes::confirmations_of(a.rig, damage->row.guid), mage_fixes::confirmations_of(b.rig, damage->row.guid));
    CHECK(a.rig.pawn.broken.empty() && b.rig.pawn.broken.empty(), "(both pawns took every change)");
}
AOC_REGISTER_TEST(test_mage_fixes_two_blizzards_side_by_side);

// ---- F3: the Arcane Circle's buff on the caster ----------------------------------------------------
// The circle's OnBeginOverlap hit puts its buff on the caster at the cost step, and the buff is an infinite effect. Its zone only ended it when the
// whole circle ended: the caster who stepped out kept it. It ends the moment he leaves and comes back when he returns, while the circle stands.
void test_mage_fixes_the_arcane_circle_buff_leaves_with_the_caster(Fixture& f) {
    std::printf("the Arcane Circle: the buff leaves the caster the moment he steps out and comes back when he steps in, while the circle stands\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::EffectDef* buff = f.catalog->effect_named("Mage_ArcaneCircle");
    CHECK(buff != nullptr, "the data has the Arcane Circle's buff");
    if (!buff) return;
    m.rig.know({"Mage_ArcaneCircle"});
    const auto held = [&] { return mage_fixes::effects_of(m.rig, buff->row.guid); };
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_ArcaneCircle", t0, false) == Refusal::None, "the circle is cast");
    m.rig.run_to(t0 + 0.5);
    CHECKF(held() >= 1, "the circle put its buff on the Mage (%zu instance(s))", held());
    CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open) == 1, "the circle's actor opened (%zu)", mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open));
    // he steps out of the 3.5 m circle at 1 s, before the next zone tick
    m.rig.run_to(t0 + 1.0);
    m.player.rig_fighter().move_to({500, 0, 1098});
    m.rig.run_to(t0 + 2.2);
    CHECKF(held() == 0, "the buff is gone the moment he stepped out, while the circle still stands (%zu instance(s) at 2.2 s)", held());
    CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Close) == 0, "the circle has not ended (%zu close(s))", mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Close));
    // back in at 3 s: the next zone tick puts it on him again
    m.player.rig_fighter().move_to({0, 0, 1098});
    m.rig.run_to(t0 + 4.2);
    CHECKF(held() >= 1, "the buff comes back when he steps in again (%zu instance(s) at 4.2 s)", held());
    CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Close) == 0, "the circle still stands (%zu close(s))", mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Close));
    // and it ends with the circle after all
    m.rig.run_to(t0 + 11.0);
    CHECKF(held() == 0, "the circle's 10 s over: the buff is gone (%zu instance(s) at 11 s)", held());
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_mage_fixes_the_arcane_circle_buff_leaves_with_the_caster);

// ---- F4: the Prismatic Beam's threat on its later ticks --------------------------------------------
// The tick the client asks for (Mage_PrismaticBeam_Recurse) carries the beam's damage in its children, and the damage's Health loss is what the
// combat world puts on the Wolf's threat table. A cast took only eight requests of a hit, so a channel held past its eighth tick (the recorded cast
// asks ten of them in three seconds) stopped hurting and stopped adding threat: the Wolf lost interest. The cast takes the channel's ticks now, and
// the Wolf that the beam hurts comes for the Mage.
void test_mage_fixes_the_prismatic_beam_keeps_its_ticks_and_its_threat(Fixture& f) {
    std::printf("the Prismatic Beam: every tick of the channel is taken, its damage keeps adding threat, and the Wolf comes for the Mage\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const uint64_t wolf = m.wolf(300, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_PrismaticBeam");
    const abilities::HitDef* recurse = f.catalog->hit_named("Mage_PrismaticBeam_Recurse");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_PrismaticBeam_Dmg");
    CHECK(wolf != 0 && def && recurse && damage, "a Wolf 3 m away and the Beam's Recurse tick and damage hit");
    if (!wolf || !def || !recurse || !damage) return;
    m.rig.know({"Mage_PrismaticBeam"});
    m.rig.asks = false;
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_PrismaticBeam", t0, false) == Refusal::None, "the Beam is cast");
    m.rig.run_to(t0 + 0.2);
    int taken = 0;
    for (int i = 0; i < 10; ++i) {
        const double at = t0 + 0.5 + 0.34 * static_cast<double>(i);
        if (mage_fixes::ask_tick(m.rig, *def, *recurse, at, wolf, true) == AbilityPipeline::HitVerdict::Scheduled) ++taken;
        m.rig.run_to(at + 0.1);
    }
    CHECKF(taken == 10, "the channel's ten ticks are all taken (%d of 10: the recorded beam's own count)", taken);
    CHECKF(mage_fixes::confirmations_of(m.rig, damage->row.guid) >= 9, "the Wolf shows the beam's damage numbers on the later ticks too (%d)", mage_fixes::confirmations_of(m.rig, damage->row.guid));
    // the same tick request twice is taken once (the per-hit gap refuses the repeat)
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *recurse, t0 + 0.5, wolf, true) == AbilityPipeline::HitVerdict::TooMany, "the same tick request twice is taken once");
    // the threat the damage added: the combat world's table, and the Wolf's choice of target after it looks
    m.w.npcs->note_player(77, 0, 0, t0 + 4.5);
    m.w.npcs->tick(t0 + 4.5);
    const npc::ai::NpcAi* ai = m.world.ai_of(wolf);
    CHECKF(ai != nullptr && ai->threat().of(77) > 0.0, "the beam's damage is threat on the Wolf's table (%.1f)", ai ? ai->threat().of(77) : 0.0);
    CHECKF(ai != nullptr && ai->target() == 77, "the Wolf has come for the Mage (its target is %llu)", ai ? static_cast<unsigned long long>(ai->target()) : 0ull);
    // a Dodge after a tick changes nothing: what went out stays out, and its threat with it
    CHECK(m.rig.pipe->end_by_dodge(t0 + 4.6) == 0, "a Dodge while the beam burns ends nothing");
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_mage_fixes_the_prismatic_beam_keeps_its_ticks_and_its_threat);
