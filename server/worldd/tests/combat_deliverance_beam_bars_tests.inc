// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_deliverance_beam_bars_tests.inc: the Cleric's Deliverance (its held charge, one heal and the bar's end),
//  the Resplendent Beam's bounces to the allies near the one just hit, and the Ranger's Disengage whose bar ends on the client's
//  fall branch (included from combat_registered.inc; the rules and the numbers are in the abilities' records).
//
//    the charge   the Deliverance is a held cast: its item, its montage jump (2.9629629 s into a full charge), the cost step and the
//                 clear of its montage follow the release.  An early release sends its held time in montage seconds, so the fill is that
//                 time over the full charge's montage time; a full charge is the client's branch request at the recorded bar's length.
//    the heal     one heal on the target the client names, by the data's formula: the heal's BaseValue term (Lerp(10 with the Redemptive
//                 passive, 5, the target's Health over its MaxHealth) x the progress ratio) times its formula (the HealingMod).  Another
//                 player's pawn takes it through his own session's ally path, and his client is told.
//    the bounces  the Beam's hit names itself in its RecursiveHit: the data's maximum is progress x 4 + 1 hits, each to an ally within 1,250 cm
//                 of the one just hit, the injured (Health < MaxHealth) first and the nearest first; the heal formula counts the hits before it
//                 (GetHitTriggerCount), and the confirmations share one call, each naming the previous target as its Source.
//    the bar      the Disengage's montage ends on the client's CancelIfFalling branch request, not at a fixed moment; with no request it
//                 keeps the recording's clear, and the movement the backflip covers stays allowed.
// ============================================================================

// This file's checks print their passing line too, so the proof of the eight angles can quote the [ok ] line as well as the [FAIL] one.
#undef CHECK
#undef CHECKF
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define CHECKF(cond, ...) do { \
    char deliverance_line[1024]; \
    std::snprintf(deliverance_line, sizeof deliverance_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", deliverance_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", deliverance_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace deliverance {

using namespace aoc::worldd;

constexpr uint64_t kCaster = 91, kFirstAlly = 92;
constexpr double   kGroundZ = 1098.0;

constexpr int64_t kDeliverance = 6064629291401084936ll;
constexpr int64_t kDeliveranceHit = 6064629291423367179ll;
constexpr int64_t kDeliveranceBranch = 6064629301877604633ll;
constexpr int64_t kBeam = 109195828876921ll;
constexpr int64_t kBeamHit = 109201325905529ll;
constexpr int64_t kBeamBranch = 6064629659855553923ll;
constexpr int64_t kDisengage = 6064629739446014827ll;
constexpr int64_t kDisengageBranch = 6064630690278670336ll;

// The two branch positions and the montage times a release is read over (the abilities' records): the Deliverance's jump is to the
// "CastLoopComplete" section 2.9629629 s into its montage; the Beam's is 1.3333334 s into its own.
constexpr double kDeliveranceFullMontage = 2.9629629;
constexpr float  kDeliveranceJump = 2.9629629f;
constexpr float  kBeamJump = 1.3333334f;

/// One caster and the allies of his world, each with its own ability runtime attached to the same combat world as a session's is.
struct Ring {
    Rig                                     caster;
    combat_rig::World                       w;
    combat::NpcStatBook                     book;
    combat::CombatWorld                     world;
    combat_rig::FakePlayer                  caster_port;
    combat::WorldHitPort                    caster_hit;
    std::vector<std::unique_ptr<Rig>>       allies;
    std::vector<std::unique_ptr<combat_rig::FakePlayer>> ally_ports;
    std::vector<std::unique_ptr<combat::WorldHitPort>>   ally_hits;
    std::string                             problem;

    /// `allies_at`: where each ally stands (metres along +X) and whether he is of the caster's party.  The caster's `skills` are the
    /// talents he holds (Cleric_Passive_Deliverance_Redemptive and the like); `caster_class` is his class enum (the Disengage is a Ranger's).
    Ring(Fixture& f, const std::vector<std::pair<double, bool>>& allies_at = {}, const std::vector<std::string>& skills = {}, uint64_t seed = 11, int caster_class = 3)
        : caster(caster_class, 25, skills, true, seed),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          caster_port(caster, kCaster, {0, 0, kGroundZ}),
          caster_hit(world, w.registry, caster_port.fighter()) {
        caster.asks = false;
        caster.pipe->set_data_timings(true);
        if (!caster.ok()) {
            problem = "the caster: " + caster.problem;
            return;
        }
        world.add_player(&caster_port);
        caster.pipe->set_combat(&caster_hit);
        for (size_t i = 0; i < allies_at.size(); ++i) {
            auto rig = std::make_unique<Rig>(3, 25, std::vector<std::string>{}, true, seed + i + 1);
            if (!rig->ok()) {
                problem = "an ally: " + rig->problem;
                return;
            }
            rig->asks = false;
            rig->pipe->set_data_timings(true);
            const uint64_t id = kFirstAlly + i;
            auto port = std::make_unique<combat_rig::FakePlayer>(*rig, id, combat::Vec3{allies_at[i].first * 100.0, 0, kGroundZ});
            if (allies_at[i].second) port->party_with = kCaster;
            auto hit = std::make_unique<combat::WorldHitPort>(world, w.registry, port->fighter());
            world.add_player(port.get());
            rig->pipe->set_combat(hit.get());
            allies.push_back(std::move(rig));
            ally_ports.push_back(std::move(port));
            ally_hits.push_back(std::move(hit));
        }
    }
    bool ok() const { return problem.empty() && w.problem.empty(); }

    /// Advances the caster, every ally and the world to `t`.
    void run_to(double t) {
        caster.run_to(t);
        for (auto& r : allies) r->run_to(t);
        world.tick(t);
    }
    /// Advances only the caster (and the world): what another player's runtime holds did not run its own steps.
    void run_caster_to(double t) {
        caster.run_to(t);
        world.tick(t);
    }
};

/// The caster's trigger of an ability at `t`: `at_self` names the caster's own pawn (a heal at himself), `targets` name another actor.
int32_t cast(Rig& r, const char* name, double t, bool at_self, const std::vector<uint64_t>& targets = {}) {
    r.asks = false;
    r.know({name});
    r.clock = t;
    r.a7->tick(t);
    TriggerInfo info;
    info.animation_index = 0;
    info.target_count = (at_self || !targets.empty()) ? 1 : 0;
    info.target_actors = targets;
    const int32_t id = r.next_client_id++;
    const Refusal refusal = r.pipe->trigger(r.ability(name), id, t, info);
    if (refusal != Refusal::None) std::printf("    (the trigger of %s at %.2f s was refused: %s)\n", name, t, refusal_name(refusal));
    r.take();
    return refusal == Refusal::None ? id : -1;
}

/// The client's held-key release: ServerNotifyHeldAbilityCancelled with its montage time.
void release(Rig& r, int32_t id, double t, double cancelled_time) {
    r.clock = t;
    r.a7->tick(t);
    r.pipe->cancel(id, t, cancelled_time);
    r.take();
}

/// The client's cast-bar branch request (ServerRequestBranchVerification).
bool branch(Rig& r, int32_t id, int64_t branch_guid, double t) {
    r.clock = t;
    r.a7->tick(t);
    const bool asked = r.pipe->branch_request(id, branch_guid, t);
    r.take();
    return asked;
}

/// RequestHitVerification_Server at `t`: the caster's own component (`own`) and/or the actors named.
AbilityPipeline::HitVerdict ask_hit(Rig& r, const char* ability, const char* hit_name, double t, bool own, const std::vector<uint64_t>& targets = {}) {
    r.clock = t;
    r.a7->tick(t);
    const abilities::AbilityDef* def = r.catalog->ability_named(ability);
    int64_t targeting = content::kTargetingDefaultSelf, hit_guid = 0;
    for (const content::AbilityHitRow& h : def->hits) {
        const abilities::HitDef* hd = r.catalog->hit(h.hit_guid);
        if (!hd || hd->row.name != hit_name) continue;
        hit_guid = h.hit_guid;
        for (const content::TargetingRow& tg : def->targetings) {
            if (tg.key == static_cast<int>(h.key)) targeting = tg.guid;
        }
    }
    const auto verdict = r.pipe->hit_request(def->row.guid, targeting, hit_guid, static_cast<size_t>(own ? 1 : 0) + targets.size(), own ? 1 : 0, t, targets);
    r.take();
    return verdict;
}

/// A hit confirmation of `record`, copied out of the rig's log: its event (one call), the Source, the targets and the mods.
struct Conf {
    double                                  at = 0;
    size_t                                  event = 0;
    bool                                    has_source = false;
    uint64_t                                source = 0;
    std::vector<bootstrap::HitTargetOut>    targets;
    std::vector<bootstrap::HitStatModOut>   mods;   // a hit at the caster carries its mods here
};
std::vector<Conf> confirms_of(Rig& r, int64_t record, double from = -1e9) {
    std::vector<Conf> out;
    for (size_t i = 0; i < r.log.size(); ++i) {
        if (r.log[i].at <= from) continue;
        for (const bootstrap::HitConfirm& h : r.log[i].confirmations) {
            if (h.record_guid != record) continue;
            Conf c;
            c.at = r.log[i].at;
            c.event = i;
            if (h.source) {
                c.has_source = true;
                c.source = h.source->ObjectId;
            }
            c.targets = h.targets;
            c.mods = h.mods;
            out.push_back(std::move(c));
        }
    }
    return out;
}
/// The Health a confirmation raised: the first target's mod, or the caster's own.
double heal_of(const Conf& c) {
    if (!c.targets.empty() && !c.targets[0].mods.empty()) return c.targets[0].mods[0].value;
    return c.mods.empty() ? 0.0 : c.mods[0].value;
}
/// The target actor of a confirmation (0: the caster's own pawn, which a self hit does not name).
uint64_t target_of(const Conf& c) { return c.targets.empty() ? 0 : c.targets[0].component.ObjectId; }

/// One change of the ability item the rig's client holds.
struct Item {
    double      at = 0;
    int         kind = 0;   // 0: added, 1: changed
    bool        jump = false;
    float       position = 0;
    std::string path;
};
std::vector<Item> items_of(Rig& r, int64_t ability) {
    std::vector<Item> out;
    for (const Seen& s : r.log) {
        for (const auto& a : s.change.abilities_added) {
            if (a.ability_guid == ability) out.push_back({s.at, 0, a.may_jump_to_position, a.montage_position, a.montage_path});
        }
        for (const auto& a : s.change.abilities_changed) {
            if (a.ability_guid == ability) out.push_back({s.at, 1, a.may_jump_to_position, a.montage_position, a.montage_path});
        }
    }
    return out;
}
/// When the ability's item came first (0: never).
double item_at(Rig& r, int64_t ability) {
    for (const Item& i : items_of(r, ability)) {
        if (i.kind == 0) return i.at;
    }
    return 0;
}
/// The montage jump of the ability's item after `from` (its time and position); false when the item never jumped.
bool jump_of(Rig& r, int64_t ability, double& at, float& position, double from = -1e9) {
    for (const Item& i : items_of(r, ability)) {
        if (i.at <= from) continue;
        if (i.kind == 1 && i.jump) {
            at = i.at;
            position = i.position;
            return true;
        }
    }
    return false;
}
/// When the montage of the ability's item was cleared after `from` (0: never).
double cleared_at(Rig& r, int64_t ability, double from = -1e9) {
    for (const Item& i : items_of(r, ability)) {
        if (i.at <= from) continue;
        if (i.kind == 1 && i.path.empty()) return i.at;
    }
    return 0;
}
/// The first step in which the caster's Mana changed after `from`: its time and the new value (0 when it never did).
double mana_step(Rig& r, double from, double& value) {
    for (const Seen& s : r.log) {
        if (s.at <= from) continue;
        for (const auto& st : s.change.stats) {
            if (st.record != content::kGuidMana) continue;
            value = st.value;
            return s.at;
        }
    }
    return 0;
}
/// The caster's Health at `fraction` of its MaxHealth, on the client and in the engine.
void hurt(Rig& r, double fraction) { r.set_current(content::kGuidHealth, r.max_health() * fraction); }

// ---- the Deliverance's charge and its heal ---------------------------------------------------------------------------------------

void test_deliverance_the_full_charge(Fixture& f) {
    std::printf("a Deliverance held to its full charge: the bar at the recorded length, one heal by the data's formula, the montage cleared after the release\n");
    Ring g(f);
    CHECK(g.ok(), "a world for the Deliverance");
    if (!g.ok()) return;
    Rig& r = g.caster;
    hurt(r, 0.6);
    const double t0 = 1.0;
    const int32_t id = cast(r, "Cleric_Deliverance", t0, true);
    CHECK(id > 0, "the Deliverance is cast at the caster");
    if (id <= 0) return;
    g.run_to(t0 + 0.08);
    CHECKF(std::fabs(item_at(r, kDeliverance) - (t0 + 0.071)) <= 0.02, "its item comes at the recorded 71 ms (%.3f s)", item_at(r, kDeliverance) - t0);
    // (the character's own mana is full, so the data's cost is its 12 x the ManaCostMult)
    double full_mana = r.mana();
    // the client holds the key until the bar is full and asks for the branch at the recorded full charge (3.534 s)
    g.run_to(t0 + 3.53);
    CHECK(branch(r, id, kDeliveranceBranch, t0 + 3.534), "the client's branch request at the full charge belongs to the cast");
    g.run_to(t0 + 3.62);
    double jump_at = 0;
    float jump_to = 0;
    const bool jumped = jump_of(r, kDeliverance, jump_at, jump_to);
    CHECKF(jumped && std::fabs(jump_at - (t0 + 3.600)) <= 0.03 && std::fabs(jump_to - kDeliveranceJump) <= 1e-4,
           "its montage jumps to 2.9629629 s 66 ms after the release (%.3f s, %.6f)", jump_at - t0, jump_to);
    // the client asks for the hit 150 ms after the release, as the recorded ones do (136 to 168 ms)
    const auto verdict = ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, true);
    CHECK(verdict == AbilityPipeline::HitVerdict::Scheduled, "the heal is asked for after the release");
    g.run_to(t0 + 3.78);
    // the cost: the data's Mana (12 x the character's ManaCostMult) after the jump
    double after_mana = 0;
    const double cost_at = mana_step(r, t0 + 3.62, after_mana);
    CHECKF(cost_at > 0 && std::fabs(cost_at - (t0 + 3.670)) <= 0.05 && full_mana - after_mana >= 5.0, "the cost step comes after the jump and takes the data's Mana (%.3f s after the trigger, %.1f)",
           cost_at - t0, full_mana - after_mana);
    g.run_to(t0 + 3.9);
    const std::vector<Conf> confs = confirms_of(r, kDeliveranceHit);
    CHECKF(confs.size() == 1, "one heal is confirmed by the client's one request (got %zu)", confs.size());
    if (confs.size() == 1) {
        CHECKF(std::fabs(confs[0].at - (t0 + 3.764)) <= 0.05, "the heal lands about 230 ms after the release (%.3f s after the trigger)", confs[0].at - t0);
        // the data's formula: BaseValue 5 at the full charge (the character's own HealingMod times 5)
        CHECKF(heal_of(confs[0]) > 5.0, "its amount is the whole formula at the full charge (%.1f: BaseValue 5 x the HealingMod)", heal_of(confs[0]));
    }
    // the same request again is not a second heal
    const auto again = ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.95, true);
    g.run_to(t0 + 4.2);
    CHECKF(again == AbilityPipeline::HitVerdict::TooMany && confirms_of(r, kDeliveranceHit).size() == 1, "a second request of the same cast heals nobody again (%s)",
           AbilityPipeline::verdict_name(again));
    // the montage clears 1.29 to 1.36 s after the release (the recorded casts)
    g.run_to(t0 + 5.2);
    const double cleared = cleared_at(r, kDeliverance);
    CHECKF(cleared > 0 && std::fabs(cleared - (t0 + 4.840)) <= 0.06, "its montage is cleared 1.31 s after the release (%.3f s after the trigger, not at 3.314)", cleared - t0);
}

void test_deliverance_the_early_release(Fixture& f) {
    std::printf("a Deliverance released early: the fill is its held time in montage seconds over the full charge's montage time; the same cast twice heals the same\n");
    Ring g(f);
    CHECK(g.ok(), "a world for the Deliverance");
    if (!g.ok()) return;
    Rig& r = g.caster;
    hurt(r, 0.6);
    double values[3] = {0, 0, 0};
    double jump_at = 0, cleared[3] = {0, 0, 0}, heal_at[3] = {0, 0, 0};
    float jump_to = 0;
    // 1 and 2: the same early release twice (the recorded 1.212 s release sends 1.011 s of montage time)
    for (int i = 0; i < 2; ++i) {
        const double t0 = 1.0 + 6.0 * i;
        const int32_t id = cast(r, "Cleric_Deliverance", t0, true);
        CHECKF(id > 0, "the early cast %d is accepted", i + 1);
        if (id <= 0) return;
        g.run_to(t0 + 0.08);
        release(r, id, t0 + 1.212, 1.011);
        g.run_to(t0 + 1.30);
        const bool jumped = jump_of(r, kDeliverance, jump_at, jump_to, t0);
        CHECKF(jumped && std::fabs(jump_at - (t0 + 1.278)) <= 0.03, "cast %d jumps 66 ms after the release (%.3f s after the trigger)", i + 1, jump_at - t0);
        ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 1.362, true);
        g.run_to(t0 + 1.7);
        const std::vector<Conf> confs = confirms_of(r, kDeliveranceHit, t0);
        CHECKF(confs.size() == 1, "cast %d confirms one heal", i + 1);
        if (confs.size() != 1) return;
        values[i] = heal_of(confs[0]);
        heal_at[i] = confs[0].at;
        g.run_to(t0 + 3.2);
        cleared[i] = cleared_at(r, kDeliverance, t0);
        CHECKF(cleared[i] > 0 && std::fabs(cleared[i] - (t0 + 2.518)) <= 0.07, "cast %d's montage is cleared 1.31 s after the release (%.3f s after the trigger)", i + 1,
               cleared[i] - t0);
        hurt(r, 0.6);
    }
    CHECKF(std::fabs(values[0] - values[1]) <= 0.01 * std::max(1.0, values[0]), "the same cast twice heals the same (%.2f and %.2f)", values[0], values[1]);
    // 3: a full charge, for the ratio of the early fill to 1
    {
        const double t0 = 14.0;
        const int32_t id = cast(r, "Cleric_Deliverance", t0, true);
        CHECKF(id > 0, "the full-charge cast is accepted");
        if (id <= 0) return;
        g.run_to(t0 + 3.53);
        branch(r, id, kDeliveranceBranch, t0 + 3.534);
        g.run_to(t0 + 3.62);
        ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, true);
        g.run_to(t0 + 3.9);
        const std::vector<Conf> confs = confirms_of(r, kDeliveranceHit, t0);
        CHECKF(confs.size() == 1, "the full charge's heal is confirmed after the two early ones (%zu)", confs.size());
        if (confs.size() != 1) return;
        const double full = heal_of(confs.back());
        const double fill = 1.011 / kDeliveranceFullMontage;
        CHECKF(full > 5.0 && std::fabs(values[0] / full - fill) <= 0.01, "the early heal is %.3f of the full one: its montage time 1.011 s over the full charge's 2.963 s (%.3f of it)",
               values[0] / full, fill);
        CHECKF(std::fabs((heal_at[0] - (1.212 + 1.0)) - 0.230) <= 0.05, "the early heal lands about 230 ms after the release (%.3f s)", heal_at[0] - (1.212 + 1.0));
    }
}

void test_deliverance_the_redemptive_passive(Fixture& f) {
    std::printf("the Deliverance's heal follows the target's missing Health with the Redemptive passive (BaseValueMax 10), and is 5 without it\n");
    // with the passive: the heal at 10 % of the target's Health is 1.9 times the heal at full Health
    {
        Ring g(f, {}, {"Cleric_Passive_Deliverance_Redemptive"});
        CHECK(g.ok(), "a world for the Deliverance held by a Cleric with the Redemptive passive");
        if (!g.ok()) return;
        Rig& r = g.caster;
        double heals[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            const double t0 = 1.0 + 6.0 * i;
            hurt(r, i == 0 ? 1.0 : 0.10);
            const int32_t id = cast(r, "Cleric_Deliverance", t0, true);
            CHECKF(id > 0, "the passive's cast %d is accepted", i + 1);
            if (id <= 0) return;
            g.run_to(t0 + 3.53);
            branch(r, id, kDeliveranceBranch, t0 + 3.534);
            g.run_to(t0 + 3.62);
            hurt(r, i == 0 ? 1.0 : 0.10);   // (the ratio the heal reads is the target's at its landing)
            ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, true);
            g.run_to(t0 + 3.9);
            const std::vector<Conf> confs = confirms_of(r, kDeliveranceHit);
            CHECKF(confs.size() == i + 1, "the passive's cast %d heals", i + 1);
            if (confs.size() != static_cast<size_t>(i + 1)) return;
            heals[i] = heal_of(confs.back());
        }
        CHECKF(heals[1] / heals[0] > 1.85 && heals[1] / heals[0] < 1.95, "at 10 %% Health the heal is 1.9 times the one at full Health (%.3f): BaseValueMax 10 over 5",
               heals[1] / heals[0]);
    }
    // without it: the same Health does not change the heal
    {
        Ring g(f);
        CHECK(g.ok(), "a world for the Deliverance held by a Cleric without the passive");
        if (!g.ok()) return;
        Rig& r = g.caster;
        double heals[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            const double t0 = 1.0 + 6.0 * i;
            hurt(r, i == 0 ? 1.0 : 0.10);
            const int32_t id = cast(r, "Cleric_Deliverance", t0, true);
            CHECKF(id > 0, "the plain cast %d is accepted", i + 1);
            if (id <= 0) return;
            g.run_to(t0 + 3.53);
            branch(r, id, kDeliveranceBranch, t0 + 3.534);
            g.run_to(t0 + 3.62);
            hurt(r, i == 0 ? 1.0 : 0.10);
            ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, true);
            g.run_to(t0 + 3.9);
            const std::vector<Conf> confs = confirms_of(r, kDeliveranceHit);
            CHECKF(confs.size() == i + 1, "the plain cast %d heals", i + 1);
            if (confs.size() != static_cast<size_t>(i + 1)) return;
            heals[i] = heal_of(confs.back());
        }
        CHECKF(std::fabs(heals[1] / heals[0] - 1.0) <= 0.02, "without the passive a nearly dead target takes the same heal as a whole one (%.3f)", heals[1] / heals[0]);
    }
}

void test_deliverance_another_player(Fixture& f) {
    std::printf("the Deliverance's heal lands on another player's own pawn, party or stranger, through his session's ally path, and his client is told\n");
    for (const bool party : {true, false}) {
        Ring g(f, {{6.0, party}});
        CHECKF(g.ok(), "a world with a %s player 6 m from the Cleric", party ? "party" : "stranger");
        if (!g.ok()) return;
        Rig& r = g.caster;
        Rig& ally = *g.allies[0];
        combat_rig::FakePlayer& port = *g.ally_ports[0];
        hurt(r, 0.6);
        hurt(ally, 0.4);
        const double ally_before = ally.health();
        const double caster_before = r.health();
        const uint64_t ally_id = port.actor_id();
        const double t0 = 1.0;
        const int32_t id = cast(r, "Cleric_Deliverance", t0, false, {ally_id});
        CHECKF(id > 0, "the %s cast names the other player", party ? "party" : "stranger");
        if (id <= 0) return;
        g.run_to(t0 + 3.53);
        CHECK(branch(r, id, kDeliveranceBranch, t0 + 3.534), "the full charge's branch belongs to the cast");
        g.run_to(t0 + 3.62);
        const double ally_clock = ally.clock;
        ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, false, {ally_id});
        const std::vector<Conf> before = confirms_of(r, kDeliveranceHit);
        g.run_caster_to(t0 + 3.9);   // (the ally's runtime is not advanced: his own session applies the hit)
        const std::vector<Conf> confs = confirms_of(r, kDeliveranceHit);
        CHECKF(confs.size() == before.size() + 1, "the caster's client is told the heal on the %s player (one confirmation)", party ? "party" : "stranger");
        if (confs.size() == before.size() + 1) {
            const Conf& c = confs.back();
            CHECKF(target_of(c) == ally_id, "the confirmation names his pawn (its target is %llu, his actor is %llu)", static_cast<unsigned long long>(target_of(c)), static_cast<unsigned long long>(ally_id));
            CHECKF(c.has_source == false, "the heal's Source is the caster's own (the recorded self casts name none)");
            CHECKF(heal_of(c) > 5.0, "the amount is the whole formula (%.1f)", heal_of(c));
        }
        CHECKF(!port.ally_lands.empty() && port.ally_lands.back() == kDeliveranceHit, "his session applied the hit through its ally path (apply_ally_landing)");
        CHECKF(ally.health() > ally_before + heal_of(confs.back()) * 0.9, "his Health rises by the heal (%.1f -> %.1f, %.1f healed)", ally_before, ally.health(), heal_of(confs.back()));
        CHECKF(std::fabs(r.health() - caster_before) <= 60.0, "the caster is not healed too (%.1f -> %.1f; the regeneration adds a little)", caster_before, r.health());
        CHECKF(ally.clock == ally_clock, "the other player's runtime was not advanced by the caster's step (its clock stays %.2f)", ally_clock);
        CHECKF(!ally.pawn.confirmations.empty() && ally.pawn.confirmations.back().record_guid == kDeliveranceHit, "his own client holds the heal's confirmation (his Health bar follows)");
    }
}

// ---- the Resplendent Beam's bounces ----------------------------------------------------------------------------------------------

void test_beam_the_bounces(Fixture& f) {
    std::printf("the Resplendent Beam's hit bounces to the allies near the one just hit, the injured first: progress x 4 + 1 hits, all in one call, each Source the previous target\n");
    Ring g(f, {{2.0, true}, {4.0, true}, {6.0, true}, {8.0, true}, {10.0, true}, {12.0, true}});
    CHECK(g.ok(), "a world with six party members");
    if (!g.ok()) return;
    Rig& r = g.caster;
    for (auto& a : g.allies) hurt(*a, 0.5);
    // a partial charge: the recorded bar is 1.241 s of the ability's 3 s hold -> progress 0.414 -> 1 + 4 x 0.414 = 2.65 -> three hits
    const double t0 = 1.0;
    const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
    CHECKF(id > 0, "the beam is cast at the caster");
    if (id <= 0) return;
    g.run_to(t0 + 1.20);
    CHECK(branch(r, id, kBeamBranch, t0 + 1.241), "the client's bar ends at the recorded 1.241 s");
    g.run_to(t0 + 1.40);
    double jump_at = 0;
    float jump_to = 0;
    CHECKF(jump_of(r, kBeam, jump_at, jump_to) && std::fabs(jump_to - kBeamJump) <= 1e-4, "its montage jumps to 1.3333334 s (%.6f)", jump_to);
    ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 1.44, true);
    g.run_to(t0 + 1.7);
    const std::vector<Conf> confs = confirms_of(r, kBeamHit);
    CHECKF(confs.size() == 3, "the 41 %% charge gives three hits (got %zu): 1 + 4 x 0.414 rounds up like a maximum", confs.size());
    if (confs.size() != 3) return;
    CHECKF(confs[0].event == confs[1].event && confs[1].event == confs[2].event, "all three confirmations are in one call (one packet)");
    CHECKF(target_of(confs[0]) == 0 && target_of(confs[1]) == g.ally_ports[0]->actor_id() && target_of(confs[2]) == g.ally_ports[1]->actor_id(),
           "the hits go to the caster, then the nearest injured ally, then the next (the 3 first of the six)");
    CHECKF(confs[0].has_source && confs[0].source == kCaster && confs[1].has_source && confs[1].source == kCaster, "the first two Sources are the caster (the previous target)");
    CHECKF(confs[2].has_source && confs[2].source == g.ally_ports[0]->actor_id(), "the third Source is the first ally hit (the previous target)");
    // the heal's formula counts the hits before it: 2.86/(x+2.6)-0.1 for x = 0, 1, 2, times the same HealingMod
    const auto shape = [](double x) { return 2.86 / (x + 2.6) - 0.1; };
    CHECKF(std::fabs(heal_of(confs[1]) / heal_of(confs[0]) - shape(1.0)) <= 1e-3, "the second heal is 0.694 of the first (GetHitTriggerCount 1; %.4f)", heal_of(confs[1]) / heal_of(confs[0]));
    CHECKF(std::fabs(heal_of(confs[2]) / heal_of(confs[0]) - shape(2.0)) <= 1e-3, "the third heal is 0.522 of the first (GetHitTriggerCount 2; %.4f)", heal_of(confs[2]) / heal_of(confs[0]));
    CHECKF(heal_of(confs[0]) > 5.0, "the heal carries its BaseValue term (3.0 x the formula: %.1f)", heal_of(confs[0]));
    // a full charge: 3.0 s gives the maximum, five hits
    const double t1 = 10.0;
    hurt(r, 0.6);
    for (auto& a : g.allies) hurt(*a, 0.5);
    r.pipe->clear_cooldowns(t1);   // (the Beam's 20 s cooldown; this test casts it twice)
    const int32_t full = cast(r, "Cleric_ResplendentBeam", t1, true);
    CHECKF(full > 0, "the full-charge beam is accepted");
    if (full <= 0) return;
    g.run_to(t1 + 3.0);
    CHECK(branch(r, full, kBeamBranch, t1 + 3.0), "the client holds the key to the 3 s hold and asks for the branch");
    g.run_to(t1 + 3.1);
    ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t1 + 3.12, true);
    g.run_to(t1 + 3.5);
    const std::vector<Conf> all = confirms_of(r, kBeamHit);
    CHECKF(all.size() == 8, "the 3 s charge gives five more hits (three then five: got %zu)", all.size());
    if (all.size() == 8) {
        CHECKF(all[3].event == all[7].event, "the full charge's five confirmations are in one call");
        std::set<uint64_t> targets;
        for (size_t i = 3; i < 8; ++i) targets.insert(target_of(all[i]));
        CHECKF(targets.size() == 5, "one hit per target, five different ones (%zu)", targets.size());
        CHECKF(std::fabs(heal_of(all[7]) / heal_of(all[3]) - shape(4.0)) <= 1e-3, "the fifth heal counts four hits before it (%.4f of the first)", heal_of(all[7]) / heal_of(all[3]));
    }
}

void test_beam_the_reach(Fixture& f) {
    std::printf("the beam's bounces want an ally within 1,250 cm of the one just hit: a nearer one wins over a 12.6 m one, and 12.4 m is still reached\n");
    // a full charge (five hits) but only two injured allies: one 3 m from the caster, one 12.6 m from the first: the second is out of reach
    {
        Ring g(f, {{3.0, true}, {15.6, true}});
        CHECK(g.ok(), "a world with an ally 3 m away and another 12.6 m from him");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        hurt(*g.allies[1], 0.5);
        const double t0 = 1.0;
        const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
        CHECKF(id > 0, "the beam is accepted");
        if (id <= 0) return;
        g.run_to(t0 + 3.0);
        branch(r, id, kBeamBranch, t0 + 3.0);
        g.run_to(t0 + 3.1);
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 3.12, true);
        g.run_to(t0 + 3.5);
        const std::vector<Conf> confs = confirms_of(r, kBeamHit);
        CHECKF(confs.size() == 2, "the ally 12.6 m from the one just hit is not reached (the two hits are the caster and the 3 m ally; got %zu)", confs.size());
        if (confs.size() == 2) CHECKF(target_of(confs[1]) == g.ally_ports[0]->actor_id(), "the hit that lands is the nearer ally's");
    }
    // the same at 12.4 m: reached
    {
        Ring g(f, {{3.0, true}, {15.4, true}});
        CHECK(g.ok(), "a world with an ally 3 m away and another 12.4 m from him");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        hurt(*g.allies[1], 0.5);
        const double t0 = 1.0;
        const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
        CHECKF(id > 0, "the second beam is accepted");
        if (id <= 0) return;
        g.run_to(t0 + 3.0);
        branch(r, id, kBeamBranch, t0 + 3.0);
        g.run_to(t0 + 3.1);
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 3.12, true);
        g.run_to(t0 + 3.5);
        const std::vector<Conf> confs = confirms_of(r, kBeamHit);
        CHECKF(confs.size() == 3, "the ally 12.4 m from the one just hit is reached (three hits; got %zu)", confs.size());
        if (confs.size() == 3) CHECKF(target_of(confs[2]) == g.ally_ports[1]->actor_id(), "the third hit is the far ally's");
    }
    // an unhurt ally comes after the hurt ones (the record's second sorting entry has no filter): the beam's third hit, after the hurt ally's
    {
        Ring g(f, {{4.0, true}, {2.0, true}});
        CHECK(g.ok(), "a world with one hurt and one whole ally");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        // the second ally, the nearer one, stays at full Health
        const double t0 = 1.0;
        const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
        CHECKF(id > 0, "the third beam is accepted");
        if (id <= 0) return;
        g.run_to(t0 + 3.0);
        branch(r, id, kBeamBranch, t0 + 3.0);
        g.run_to(t0 + 3.1);
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 3.12, true);
        g.run_to(t0 + 3.5);
        const std::vector<Conf> confs = confirms_of(r, kBeamHit);
        CHECKF(confs.size() == 3, "an ally at full Health is a target of a bounce too (three hits: the caster, the hurt one, the whole one; got %zu)", confs.size());
        if (confs.size() == 3) {
            CHECKF(target_of(confs[1]) == g.ally_ports[0]->actor_id() && target_of(confs[2]) == g.ally_ports[1]->actor_id(), "the hurt ally 4 m away is hit before the whole one 2 m away");
        }
    }
}

void test_beam_another_player(Fixture& f) {
    std::printf("the beam's bounce lands on another player, party or stranger, and his client sees it; the primary may be another player too\n");
    Ring g(f, {{2.0, true}, {4.0, false}});
    CHECK(g.ok(), "a world with a party member and a stranger");
    if (!g.ok()) return;
    Rig& r = g.caster;
    hurt(*g.allies[0], 0.5);
    hurt(*g.allies[1], 0.5);
    const double t0 = 1.0;
    const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
    CHECKF(id > 0, "the beam is accepted");
    if (id <= 0) return;
    g.run_to(t0 + 1.20);
    branch(r, id, kBeamBranch, t0 + 1.241);
    g.run_to(t0 + 1.40);
    ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 1.44, true);
    g.run_to(t0 + 1.7);
    const std::vector<Conf> confs = confirms_of(r, kBeamHit);
    CHECKF(confs.size() == 3, "three hits: the caster, the 2 m party member and the 4 m stranger (got %zu)", confs.size());
    if (confs.size() == 3) {
        CHECKF(target_of(confs[1]) == g.ally_ports[0]->actor_id() && target_of(confs[2]) == g.ally_ports[1]->actor_id(), "the bounce reaches the stranger too (the data's mask is Ally, no party filter)");
        CHECKF(!g.ally_ports[1]->ally_lands.empty() && g.ally_ports[1]->ally_lands.back() == kBeamHit, "his session applied the bounce through apply_ally_landing");
        CHECKF(!g.allies[1]->pawn.confirmations.empty(), "his own client holds the bounce's confirmation");
        CHECKF(g.allies[1]->health() > g.allies[1]->max_health() * 0.5, "his Health rose (%.1f of %.1f)", g.allies[1]->health(), g.allies[1]->max_health());
    }
    // the primary itself may name another player: the bounces start from him
    Ring h(f, {{5.0, true}, {7.0, true}});
    CHECK(h.ok(), "a world for a beam whose target is a party member");
    if (!h.ok()) return;
    Rig& r2 = h.caster;
    hurt(*h.allies[0], 0.5);
    hurt(*h.allies[1], 0.5);
    const double t1 = 1.0;
    const uint64_t primary = h.ally_ports[0]->actor_id();
    const int32_t id2 = cast(r2, "Cleric_ResplendentBeam", t1, false, {primary});
    CHECKF(id2 > 0, "the beam's cast names the party member");
    if (id2 <= 0) return;
    h.run_to(t1 + 3.0);
    branch(r2, id2, kBeamBranch, t1 + 3.0);
    h.run_to(t1 + 3.1);
    ask_hit(r2, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t1 + 3.12, false, {primary});
    h.run_to(t1 + 3.5);
    const std::vector<Conf> on_him = confirms_of(r2, kBeamHit);
    CHECKF(on_him.size() >= 2 && target_of(on_him[0]) == primary, "the first heal lands on the named player (%zu hits)", on_him.size());
    if (on_him.size() >= 2) CHECKF(on_him[1].has_source && on_him[1].source == primary, "the bounce's Source is the one just hit (his pawn)");
}

// ---- the Disengage's bar ---------------------------------------------------------------------------------------------------------

void test_disengage_the_bar(Fixture& f) {
    std::printf("the Disengage's bar ends on the client's CancelIfFalling branch request and its montage clears on the recorded timeline; the movement the backflip needs stays allowed\n");
    Ring g(f, {}, {}, 11, 1);   // (a Ranger)
    CHECK(g.ok(), "a world for the Disengage");
    if (!g.ok()) return;
    Rig& r = g.caster;
    const double t0 = 1.0;
    const int32_t id = cast(r, "Ranger_Disengage", t0, false);
    CHECKF(id > 0, "the Disengage is accepted");
    if (id <= 0) return;
    g.run_to(t0 + 0.12);
    CHECKF(std::fabs(item_at(r, kDisengage) - (t0 + 0.093)) <= 0.02, "its item comes at the recorded 93 ms");
    // the movement the recorded backflip covered: 1,578 cm in 1.145 s, 1,518 cm by 1.0 s; the allowance must cover it
    const double allowance = r.pipe->movement_allowance(t0 + 0.5);
    CHECKF(allowance >= 1578.0 / 1.0, "the movement allowance is the modelled 3,750 cm/s (%.0f): the 1,578 cm of the backflip fit in the window", allowance);
    // the client's CancelIfFalling request at 935 ms: the bar ends, the montage clears about 370 ms later
    g.run_to(t0 + 0.93);
    CHECK(branch(r, id, kDisengageBranch, t0 + 0.935), "the client's CancelIfFalling branch belongs to the bar");
    g.run_to(t0 + 0.97);
    double jump_at = 0;
    float jump_to = 0;
    CHECK(!jump_of(r, kDisengage, jump_at, jump_to), "the branch moves the montage nowhere (the record's section is none, bMayJumpToPosition is 0)");
    CHECKF(r.pipe->movement_allowance(t0 + 0.5) >= 1578.0 / 1.0, "the branch request does not take the movement allowance away (%.0f cm/s)", r.pipe->movement_allowance(t0 + 0.5));
    g.run_to(t0 + 1.05);
    CHECK(branch(r, id, kDisengageBranch, t0 + 1.100), "a second CancelIfFalling request of the same bar is taken quietly");
    g.run_to(t0 + 1.8);
    const double cleared = cleared_at(r, kDisengage);
    CHECKF(cleared > 0 && std::fabs(cleared - (t0 + 1.305)) <= 0.06, "its montage is cleared 370 ms after the request (%.3f s after the trigger, not at a fixed 1,345 ms)", cleared - t0);
    const double end_after = cleared - (t0 + 0.935);
    CHECKF(end_after > 0.30 && end_after < 0.45, "the clear is in the recorded 85 to 551 ms after the request (%.3f s)", end_after);
    // with no request the montage keeps the timing table's clear (1,345 ms) and nothing else changes
    const double t1 = 6.0;
    r.pipe->clear_cooldowns(t1);   // (the Disengage's 20 s cooldown; this test casts it twice)
    const int32_t plain = cast(r, "Ranger_Disengage", t1, false);
    CHECKF(plain > 0, "a Disengage without the branch request is accepted");
    if (plain <= 0) return;
    g.run_to(t1 + 1.0);
    CHECKF(std::fabs(r.pipe->movement_allowance(t1 + 0.5) - allowance) <= 1e-6, "its movement allowance is the same with no request");
    g.run_to(t1 + 1.6);
    const double plain_cleared = cleared_at(r, kDisengage, t1);
    CHECKF(plain_cleared > 0 && std::fabs(plain_cleared - (t1 + 1.345)) <= 0.05, "with no request its montage clears at the recorded 1,345 ms (%.3f s)", plain_cleared - t1);
}

// ---- two players and an interruption ---------------------------------------------------------------------------------------------

void test_two_clerics_and_a_death(Fixture& f) {
    std::printf("two Clerics healing each other at once write only through the ally path (no runtime is advanced inside a step), and a caster who dies before the heal heals nobody\n");
    Ring g(f, {{5.0, true}});
    CHECK(g.ok(), "a world with the two Clerics");
    if (!g.ok()) return;
    Rig& a = g.caster;
    Rig& b = *g.allies[0];
    combat_rig::FakePlayer& pb = *g.ally_ports[0];
    hurt(a, 0.4);
    hurt(b, 0.4);
    const uint64_t id_a = g.caster_port.actor_id(), id_b = pb.actor_id();
    const double t0 = 1.0;
    const int32_t ca = cast(a, "Cleric_Deliverance", t0, false, {id_b});
    const int32_t cb = cast(b, "Cleric_Deliverance", t0, false, {id_a});
    CHECKF(ca > 0 && cb > 0, "both Clerics cast the Deliverance at each other at once");
    if (ca <= 0 || cb <= 0) return;
    // both bars reach their full charge at the same moment
    a.run_to(t0 + 3.53);
    b.run_to(t0 + 3.53);
    const bool asked_a = branch(a, ca, kDeliveranceBranch, t0 + 3.534);
    const bool asked_b = branch(b, cb, kDeliveranceBranch, t0 + 3.534);
    CHECKF(asked_a && asked_b, "both clients ask for the branch at the full charge");
    a.run_to(t0 + 3.62);
    b.run_to(t0 + 3.62);
    const double a_health = a.health(), b_health = b.health();
    const double b_clock = b.clock;
    ask_hit(a, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, false, {id_b});
    CHECKF(b.clock == b_clock, "the other Cleric's runtime was not advanced by the first one's step (its clock stays %.2f)", b_clock);
    ask_hit(b, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, false, {id_a});
    a.run_to(t0 + 3.9);
    b.run_to(t0 + 3.9);
    CHECKF(a.health() > a_health && b.health() > b_health, "both heal each other (%.1f -> %.1f and %.1f -> %.1f)", a_health, a.health(), b_health, b.health());
    CHECKF(!pb.ally_lands.empty() && pb.ally_lands.back() == kDeliveranceHit && !g.caster_port.ally_lands.empty(), "each hit landed through its target's ally path");
    // the caster dying before the heal's landing: no heal, no bounce
    const double t1 = 10.0;
    hurt(a, 0.4);
    const double before = a.health();
    const int32_t dead = cast(a, "Cleric_Deliverance", t1, true);
    CHECKF(dead > 0, "a Deliverance cast before the death is accepted");
    if (dead <= 0) return;
    g.run_to(t1 + 3.53);
    branch(a, dead, kDeliveranceBranch, t1 + 3.534);
    g.run_to(t1 + 3.62);
    a.pipe->set_dead(true);
    a.take();
    const size_t confs_before = confirms_of(a, kDeliveranceHit).size();
    ask_hit(a, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t1 + 3.684, true);
    g.run_to(t1 + 4.0);
    CHECKF(std::fabs(a.health() - before) <= 100.0 && confirms_of(a, kDeliveranceHit).size() == confs_before, "a caster who died before the heal's landing heals nobody (Health %.1f -> %.1f; the regeneration adds a little)", before,
           a.health());
    // ... and the beam, whose heal would bounce
    a.pipe->set_dead(false);
    hurt(a, 0.4);
    g.run_to(t1 + 6.0);
    const int32_t beam = cast(a, "Cleric_ResplendentBeam", t1 + 6.0, true);
    CHECKF(beam > 0, "a beam cast before the death is accepted");
    if (beam <= 0) return;
    g.run_to(t1 + 9.0);
    branch(a, beam, kBeamBranch, t1 + 9.0);
    g.run_to(t1 + 9.1);
    a.pipe->set_dead(true);
    a.take();
    const size_t beam_confs = confirms_of(a, kBeamHit).size();
    ask_hit(a, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t1 + 9.12, true);
    g.run_to(t1 + 9.5);
    CHECKF(confirms_of(a, kBeamHit).size() == beam_confs, "a dead caster's beam heals and bounces nobody");
}

void test_the_target_reconnects(Fixture& f) {
    std::printf("a target that reconnects between the bar and the heal: nothing lands on the old pawn or the new session; the beam's chain skips an ally who left\n");
    // the Deliverance: the client names an actor that has left the world by the time of the hit
    {
        Ring g(f, {{5.0, true}});
        CHECK(g.ok(), "a world with the ally who will reconnect");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        const double t0 = 1.0;
        const uint64_t gone = g.ally_ports[0]->actor_id();
        const int32_t id = cast(r, "Cleric_Deliverance", t0, false, {gone});
        CHECKF(id > 0, "the Deliverance names the ally who will leave");
        if (id <= 0) return;
        g.run_to(t0 + 3.53);
        branch(r, id, kDeliveranceBranch, t0 + 3.534);
        g.run_to(t0 + 3.62);
        const double old_health = g.allies[0]->health();
        g.world.remove_player(g.ally_ports[0].get());
        auto fresh = std::make_unique<Rig>(3, 25, std::vector<std::string>{}, true, 99);
        CHECKF(fresh->ok(), "the reconnecting player's new session is built");
        if (!fresh->ok()) return;
        fresh->asks = false;
        fresh->pipe->set_data_timings(true);
        auto fresh_port = std::make_unique<combat_rig::FakePlayer>(*fresh, 120, combat::Vec3{200, 0, kGroundZ});
        fresh_port->party_with = kCaster;
        auto fresh_hit = std::make_unique<combat::WorldHitPort>(g.world, g.w.registry, fresh_port->fighter());
        g.world.add_player(fresh_port.get());
        fresh->pipe->set_combat(fresh_hit.get());
        hurt(*fresh, 0.5);
        const double fresh_health = fresh->health();
        ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, false, {gone});
        g.run_caster_to(t0 + 3.9);
        fresh->run_to(t0 + 3.9);
        CHECKF(std::fabs(g.allies[0]->health() - old_health) <= 30.0, "the old session's pawn takes nothing after it left (%.1f -> %.1f)", old_health, g.allies[0]->health());
        CHECKF(std::fabs(fresh->health() - fresh_health) <= 30.0, "the new session takes nothing either: the client named an actor the world no longer has (%.1f -> %.1f)", fresh_health,
               fresh->health());
        CHECK(fresh_port->ally_lands.empty(), "no hit reaches the new session's ally path");
    }
    // the beam's chain: the nearest injured ally leaves before the hit; the one behind him is reached
    {
        Ring g(f, {{2.0, true}, {4.0, true}});
        CHECK(g.ok(), "a world with two allies in the chain");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        hurt(*g.allies[1], 0.5);
        const double t0 = 1.0;
        const int32_t beam = cast(r, "Cleric_ResplendentBeam", t0, true);
        CHECKF(beam > 0, "the beam is cast while both allies stand");
        if (beam <= 0) return;
        g.run_to(t0 + 3.0);
        branch(r, beam, kBeamBranch, t0 + 3.0);
        g.run_to(t0 + 3.1);
        g.world.remove_player(g.ally_ports[0].get());   // (the 2 m ally leaves; the 4 m one stays)
        const double gone_health = g.allies[0]->health();
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 3.12, true);
        g.run_to(t0 + 3.5);
        const std::vector<Conf> confs = confirms_of(r, kBeamHit);
        CHECKF(confs.size() == 2, "the chain skips the ally who left and reaches the one behind him (two hits; got %zu)", confs.size());
        if (confs.size() == 2) CHECKF(target_of(confs[1]) == g.ally_ports[1]->actor_id(), "the hit that lands is the 4 m ally's");
        CHECKF(std::fabs(g.allies[0]->health() - gone_health) <= 30.0, "the ally who left takes no bounce (%.1f -> %.1f)", gone_health, g.allies[0]->health());
        CHECKF(!g.allies[1]->pawn.confirmations.empty(), "the ally who stayed sees the heal on his own client");
    }
}

AOC_REGISTER_TEST(test_deliverance_the_full_charge);
AOC_REGISTER_TEST(test_deliverance_the_early_release);
AOC_REGISTER_TEST(test_deliverance_the_redemptive_passive);
AOC_REGISTER_TEST(test_deliverance_another_player);
AOC_REGISTER_TEST(test_beam_the_bounces);
AOC_REGISTER_TEST(test_beam_the_reach);
AOC_REGISTER_TEST(test_beam_another_player);
AOC_REGISTER_TEST(test_disengage_the_bar);
AOC_REGISTER_TEST(test_two_clerics_and_a_death);
AOC_REGISTER_TEST(test_the_target_reconnects);

}  // namespace deliverance
