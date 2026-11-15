// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_a3_resist_tests.inc: a mob's crowd control resisted (A3), on the real data (included by combat_registered.inc inside test_worldd_combat's
//  anonymous namespace)
//
//  The data's chance is the target's EffectEvasion less the caster's EffectAccuracy: the target's Physical or Magical DisableEvasion and the tier stat of the
//  crowd control, less the caster's Physical or Magical DisableAccuracy.  It is a fraction, clamped at 0 and rolled in steps of 1/1000, once for each effect on
//  each target.  A test may set a stat by hand to show the roll's arithmetic; the game's own data is never changed.
// ============================================================================

// This file's checks print their own passing line too, so the proof of the eight angles can quote the [ok ] line of a check as well as its [FAIL] line.
// (One printf for the whole line: the world's log writes to the same console between two calls.)
#define A3_CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define A3_CHECKF(cond, ...) do { \
    char a3_line[1024]; \
    std::snprintf(a3_line, sizeof a3_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", a3_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", a3_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace {

// The data's stat records this file reads (the combat data finds the same ones by name).
constexpr int64_t kA3MagicalDisableEvasion = 109343399171804ll;
constexpr int64_t kA3MagicalDisableAccuracy = 109343399171800ll;

/// Puts stat values on an NPC, as one extra effect's mods, so a test can set the roll's chance by hand.  The effect is the hidden build-up, which has no
/// crowd control and no hits of its own; its record only carries the numbers this call gives it.  A later call for the same NPC replaces the whole set (the
/// effect's one stack is refreshed with the new mods).
bool a3_set_stats(Fixture& f, combat::CombatWorld& world, uint64_t npc, const std::vector<std::pair<int64_t, double>>& stats, double now) {
    const abilities::EffectDef* carrier = f.catalog->effect_named("DisableEvasion_Movement_T1_Buildup");
    if (!carrier) return false;
    effects::Applied extra;
    extra.def = carrier;
    extra.stacks = 1;
    extra.duration = effects::kForever;
    for (const auto& [stat, value] : stats) extra.mods.push_back({stat, value});
    world.apply_effects(npc, 77, {extra}, now);
    return true;
}

bool a3_set_stat(Fixture& f, combat::CombatWorld& world, uint64_t npc, int64_t stat, double value, double now) {
    return a3_set_stats(f, world, npc, {{stat, value}}, now);
}

/// One application of the chill, as the hit's own record resolves it (its duration and its extension), without a hit or damage around it.  Each application adds a
/// stack that the next one can be told by (the test's own limit; the record's one-stack rule is not what is checked here): true when the stack went on, false
/// when the application was resisted.
bool a3_chill(combat::CombatWorld& world, const effects::Applied& chill, uint64_t wolf, double now) {
    effects::Applied one = chill;
    one.in_place_limit = 10000;
    const int before = world.effect_stacks(wolf, one.def->row.guid);
    world.apply_effects(wolf, 77, {one}, now);
    return world.effect_stacks(wolf, one.def->row.guid) > before;
}

uint64_t a3_wolf(dots::Bed& bed, double y, int level = 8) { return bed.w.spawn(bed.wolf_path, 3000.0, y, level, 100); }

double a3_accuracy(dots::Bed& bed) {
    std::string err;
    auto ev = bed.player.fighter().stats().evaluation(bed.player.fighter().worn(), err, true);
    return ev ? ev->stat(kA3MagicalDisableAccuracy) : -1.0;
}

/// The chill's application as the Frostbolt's own record resolves it, for a fresh Wolf that is not used again.
std::optional<effects::Applied> a3_resolved_chill(Fixture& f, dots::Bed& bed, const abilities::HitDef& frostbolt) {
    const uint64_t probe = a3_wolf(bed, 500.0);
    combat::Fighter* target = probe != 0 ? bed.world.fighter_of(probe) : nullptr;
    if (!target) return std::nullopt;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    combat::Resolved gate;
    std::string why;
    if (!bed.world.resolver().resolve_effects(bed.player.fighter(), *target, frostbolt, meta, gate, why)) return std::nullopt;
    for (const effects::Applied& a : gate.effects)
        if (a.def && a.def->row.name == "Status_Chilled") return a;
    return std::nullopt;
}

}  // namespace

// ---- the roll, with the real data -----------------------------------------------------------------------------------------------------------------------------
// A Wolf of the owner's starting area (level 8) has a MagicalDisableEvasion of about 2 %, and the level-25 Mage's MagicalDisableAccuracy is about 3.9 %: the
// chance is under one step of 1,000, so a first Frostbolt cannot be resisted.  The same check prints what the stat gives at higher levels (the data's own
// number, not a model's): a level-25 Wolf reaches 9.1 %, over the Mage's accuracy.
void test_a3_real_wolf_first_chill_cannot_be_resisted(Fixture& f) {
    std::printf("with the real data a level-8 Wolf's first chill cannot be resisted: the Mage's accuracy covers its evasion\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && frostbolt, "the data has the chill and the Frostbolt's hit");
    if (!chilled || !frostbolt) return;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    const double evasion = bed.stat_of(wolf, kA3MagicalDisableEvasion);
    const double accuracy = a3_accuracy(bed);
    const double chance = std::max(0.0, evasion - accuracy);
    std::printf("    the level-8 Wolf's MagicalDisableEvasion is %.8f (%.4f %%), the Mage's accuracy %.8f (%.4f %%): chance %.8f\n", evasion, evasion * 100.0, accuracy,
                accuracy * 100.0, chance);
    A3_CHECKF(evasion > 0.01 && evasion < 0.03, "the Wolf's MagicalDisableEvasion is the data's (%.8f, or %.4f %%)", evasion, evasion * 100.0);
    A3_CHECKF(accuracy > 0.03, "the Mage's MagicalDisableAccuracy is the data's (%.8f)", accuracy);
    const int steps = static_cast<int>(std::floor(chance * 1000.0 + 0.5));
    A3_CHECKF(steps == 0, "the chance %.8f gives %d step(s) of 1,000: its first chill cannot be resisted", chance, steps);
    int landed = 0;
    for (int i = 0; i < 3; ++i) {
        const uint64_t fresh = a3_wolf(bed, 100.0 * i);
        if (fresh == 0) continue;
        bed.land(fresh, "Mage_Frostbolt_Hit", 100.0 + i, bed.player, false);
        if (bed.world.effect_stacks(fresh, chilled->row.guid) == 1) ++landed;
    }
    A3_CHECKF(landed == 3, "three first Frostbolts each put the chill on a level-8 Wolf (%d of 3)", landed);
}
AOC_REGISTER_TEST(test_a3_real_wolf_first_chill_cannot_be_resisted);

// ---- the data's own evasion, at the levels it is read at ------------------------------------------------------------------------------------------------------
// The earlier 0.006 % to 0.05 % is the Blueprint's weight, not the Constitution stat the data's formulas read: the stat engine scales it with the NPC's level.  The
// chance is the mob's evasion less the caster's accuracy, so it is 0 at the owner's level-8 Wolf and 5.2 % at a level-25 one.  The test prints the curve; it does
// not change the data.
void test_a3_real_chance_is_the_evasion_less_the_accuracy(Fixture& f) {
    std::printf("the data's evasion by Wolf level: 2.0 %% at 8 (under the Mage's 3.9 %% accuracy), 9.1 %% at 25 (above it)\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const double accuracy = a3_accuracy(bed);
    A3_CHECKF(accuracy > 0.03, "the Mage's MagicalDisableAccuracy reads (%.8f)", accuracy);
    const uint64_t at8 = a3_wolf(bed, 0, 8), at25 = a3_wolf(bed, 100, 25), at45 = a3_wolf(bed, 200, 45);
    A3_CHECK(at8 != 0 && at25 != 0 && at45 != 0, "Wolves at levels 8, 25 and 45");
    if (!at8 || !at25 || !at45) return;
    const double e8 = bed.stat_of(at8, kA3MagicalDisableEvasion), e25 = bed.stat_of(at25, kA3MagicalDisableEvasion), e45 = bed.stat_of(at45, kA3MagicalDisableEvasion);
    std::printf("    level 8: %.8f; level 25: %.8f; level 45: %.8f; accuracy %.8f\n", e8, e25, e45, accuracy);
    A3_CHECKF(e8 > 0.01 && e8 < 0.03 && e25 > 0.08 && e25 < 0.11 && e45 > 0.16 && e45 < 0.20, "the evasion is the data's curve (%.4f, %.4f, %.4f)", e8, e25, e45);
    A3_CHECKF(std::max(0.0, e8 - accuracy) < 0.0005 && std::max(0.0, e25 - accuracy) > 0.04, "...so the level-8 chance is under one step and the level-25 chance is not (%.8f, %.8f)",
           std::max(0.0, e8 - accuracy), std::max(0.0, e25 - accuracy));
}
AOC_REGISTER_TEST(test_a3_real_chance_is_the_evasion_less_the_accuracy);

// ---- the roll's arithmetic, with a chance set by hand --------------------------------------------------------------------------------------------------------
// The target's MagicalDisableEvasion is set to 0.5: about half of 2,000 chills are resisted.  The number is the roll's own, not a model's.
void test_a3_resist_roll_with_a_hand_set_chance(Fixture& f) {
    std::printf("a hand-set chance of 0.5 resists about half of 2,000 chills (the roll's arithmetic)\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && frostbolt, "the data has the chill and the Frostbolt's hit");
    if (!chilled || !frostbolt) return;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
    A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
    if (!chill) return;
    const double base = bed.stat_of(wolf, kA3MagicalDisableEvasion), accuracy = a3_accuracy(bed);
    A3_CHECK(a3_set_stat(f, bed.world, wolf, kA3MagicalDisableEvasion, 0.5 + accuracy - base, 100.0), "the Wolf's chance is set to 0.5 by hand");
    const double chance = bed.stat_of(wolf, kA3MagicalDisableEvasion) - accuracy;
    A3_CHECKF(chance > 0.49 && chance < 0.51, "the Wolf's chance reads %.6f", chance);
    int resisted = 0;
    constexpr int kTrials = 2000;
    double now = 100.0;
    for (int i = 0; i < kTrials; ++i) {
        now += 0.001;
        if (!a3_chill(bed.world, *chill, wolf, now)) ++resisted;
    }
    std::printf("    %d of %d chills were resisted (%.3f %%)\n", resisted, kTrials, 100.0 * resisted / kTrials);
    A3_CHECKF(resisted > 900 && resisted < 1100, "the roll resists about half: %d of %d (900 to 1,100 allowed)", resisted, kTrials);
}
AOC_REGISTER_TEST(test_a3_resist_roll_with_a_hand_set_chance);

// ---- the roll counts in steps of 1/1000 ----------------------------------------------------------------------------------------------------------------------
// A chance of 0.0005 is the first step: it must resist some of 10,000 chills.  A chance of 0.0004 is under it: it must never resist.  A plain fraction would
// pass the second check and fail the first (or the other way round with another seed).
void test_a3_resist_is_rolled_in_steps_of_a_thousandth(Fixture& f) {
    std::printf("the roll's steps: a chance of 0.0005 resists, one of 0.0004 never does\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && frostbolt, "the data has the chill and the Frostbolt's hit");
    if (!chilled || !frostbolt) return;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
    A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
    if (!chill) return;
    const double base = bed.stat_of(wolf, kA3MagicalDisableEvasion);
    const double accuracy = a3_accuracy(bed);
    // 0.0004: under the first step
    A3_CHECK(a3_set_stat(f, bed.world, wolf, kA3MagicalDisableEvasion, 0.0004 + accuracy - base, 200.0), "the Wolf's chance is set to 0.0004 by hand");
    int resisted = 0;
    double now = 200.0;
    for (int i = 0; i < 1000; ++i) {
        now += 0.001;
        if (!a3_chill(bed.world, *chill, wolf, now)) ++resisted;
    }
    A3_CHECKF(resisted == 0, "a chance of 0.0004 never resists (%d of 1,000)", resisted);
    // 0.0005: exactly the first step (the extra effect's one stack replaces the last value, so the number is set against the Wolf's own base again)
    A3_CHECK(a3_set_stat(f, bed.world, wolf, kA3MagicalDisableEvasion, 0.0005 + accuracy - base, 300.0), "the Wolf's chance is set to 0.0005 by hand");
    const double chance = bed.stat_of(wolf, kA3MagicalDisableEvasion) - accuracy;
    A3_CHECKF(std::fabs(chance - 0.0005) < 1e-7, "the Wolf's chance reads %.8f", chance);   // (a double's arithmetic at the 1/1,000 step; the step itself is 0.001)
    resisted = 0;
    now = 300.0;
    for (int i = 0; i < 2000; ++i) {   // (the stacks the test counts stay under the stat engine's 4,096)
        now += 0.001;
        if (!a3_chill(bed.world, *chill, wolf, now)) ++resisted;
    }
    A3_CHECKF(resisted >= 1 && resisted <= 20, "a chance of 0.0005 resists about 2 of 2,000 (1 to 20 allowed): %d", resisted);
}
AOC_REGISTER_TEST(test_a3_resist_is_rolled_in_steps_of_a_thousandth);

// ---- the roll is per target ----------------------------------------------------------------------------------------------------------------------------------
// Two Wolves stand together and the same chill reaches both: the one whose chance is 0 takes it, the one set to always resist does not.  The roll of one target
// never decides another's.
void test_a3_resist_rolls_each_mob_on_its_own(Fixture& f) {
    std::printf("the roll is per target: of two Wolves, the one set to always resist keeps the chill off and the other takes it\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && frostbolt, "the data has the chill and the Frostbolt's hit");
    if (!chilled || !frostbolt) return;
    const uint64_t plain = a3_wolf(bed, 0), warded = a3_wolf(bed, 100);
    A3_CHECK(plain != 0 && warded != 0, "two level-8 Wolves");
    if (!plain || !warded) return;
    const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
    A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
    if (!chill) return;
    const double accuracy = a3_accuracy(bed), base = bed.stat_of(warded, kA3MagicalDisableEvasion);
    A3_CHECK(a3_set_stat(f, bed.world, warded, kA3MagicalDisableEvasion, 1.0 + accuracy - base, 100.0), "the second Wolf's chance is set to 1 by hand");
    const double plain_chance = std::max(0.0, bed.stat_of(plain, kA3MagicalDisableEvasion) - accuracy);
    const double warded_chance = bed.stat_of(warded, kA3MagicalDisableEvasion) - accuracy;
    A3_CHECKF(plain_chance < 0.0005 && warded_chance > 0.99, "the two chances are %.8f and %.6f", plain_chance, warded_chance);
    int taken = 0, kept = 0;
    for (int i = 0; i < 20; ++i) {
        const double now = 100.0 + i * 0.01;
        if (a3_chill(bed.world, *chill, plain, now)) ++taken;
        if (!a3_chill(bed.world, *chill, warded, now)) ++kept;
    }
    A3_CHECKF(taken == 20 && kept == 20, "the plain Wolf takes all 20 (%d) and the warded one resists all 20 (%d)", taken, kept);
}
AOC_REGISTER_TEST(test_a3_resist_rolls_each_mob_on_its_own);

// ---- a chance below 0 is clamped -----------------------------------------------------------------------------------------------------------------------------
// The formula clamps its chance at 0: a target with less evasion than the caster's accuracy can never resist.  The stat is set under 0 by hand and 500 chills are
// applied; each must land.
void test_a3_a_chance_below_zero_never_resists(Fixture& f) {
    std::printf("a chance clamped at 0 never resists: a hand-set -0.5 lands 500 of 500 chills\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && frostbolt, "the data has the chill and the Frostbolt's hit");
    if (!chilled || !frostbolt) return;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
    A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
    if (!chill) return;
    A3_CHECK(a3_set_stat(f, bed.world, wolf, kA3MagicalDisableEvasion, -0.5, 100.0), "the Wolf's chance is set under 0 by hand");
    int landed = 0;
    double now = 100.0;
    for (int i = 0; i < 500; ++i) {
        now += 0.001;
        if (a3_chill(bed.world, *chill, wolf, now)) ++landed;
    }
    A3_CHECKF(landed == 500, "all 500 chills landed (%d)", landed);
}
AOC_REGISTER_TEST(test_a3_a_chance_below_zero_never_resists);

// ---- the lockout window: the owner's first step ---------------------------------------------------------------------------------------------------------------
// A Wolf kept chilled with a Frostbolt every 3 s reaches the Movement T1 build-up's limit of 24 after about 11 s: the data's lockout takes the chill off and puts
// +100 on the tier stat for 10 s.  A new chill inside that window is resisted; one after it lands.  The owner's step is the one at 12 s.
void test_a3_owner_step_chill_resisted_after_twelve_seconds(Fixture& f) {
    std::printf("the owner's step: a level-8 Wolf kept chilled for 12 s resists the next chill, and one after the 10 s lockout lands\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* lockout = f.catalog->effect_named("DisableEvasion_Movement_T1_Lockout");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && lockout && frostbolt, "the data has the chill, its Movement T1 lockout and the Frostbolt's hit");
    if (!chilled || !lockout || !frostbolt) return;
    constexpr int64_t kMovementT1 = 6064631863198679042ll;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    constexpr double t0 = 100.0;
    // keep the chill up: a Frostbolt as it lands and every 3 s after (no damage: the hit's own numbers are not this test's)
    for (int i = 0; i < 4; ++i) bed.land(wolf, "Mage_Frostbolt_Hit", t0 + 3.0 * i, bed.player, false);
    bed.run(t0 + 0.05, t0 + 12.0);
    const int chill_at_lock = bed.world.effect_stacks(wolf, chilled->row.guid);
    const int lockout_on = bed.world.effect_stacks(wolf, lockout->row.guid);
    const double tier = bed.stat_of(wolf, kMovementT1);
    std::printf("    at 12 s: chill %d stack(s), lockout %d, Movement T1 stat %.2f\n", chill_at_lock, lockout_on, tier);
    A3_CHECKF(lockout_on >= 1 && tier > 99.0, "the kept-up chill reached its build-up's threshold: the lockout is on and the tier stat is over 99 (%.2f)", tier);
    A3_CHECKF(chill_at_lock == 0, "the lockout took the kept-up chill off (%d stack(s) left)", chill_at_lock);
    // the new chill at 12 s, inside the 10 s window
    bed.land(wolf, "Mage_Frostbolt_Hit", t0 + 12.0, bed.player, false);
    const int in_window = bed.world.effect_stacks(wolf, chilled->row.guid);
    A3_CHECKF(in_window == 0, "the new chill at 12 s is resisted: it is not on the Wolf (%d stack(s))", in_window);
    // wait the window out and cast again: the lockout has run out and the chill lands
    bed.run(t0 + 12.05, t0 + 22.0);
    const double tier_after = bed.stat_of(wolf, kMovementT1);
    A3_CHECKF(tier_after < 0.1, "after the 10 s window the tier stat is back to its own value (%.3f)", tier_after);
    bed.land(wolf, "Mage_Frostbolt_Hit", t0 + 22.0, bed.player, false);
    const int after = bed.world.effect_stacks(wolf, chilled->row.guid);
    A3_CHECKF(after == 1, "a chill after the window lands (%d stack(s))", after);
}
AOC_REGISTER_TEST(test_a3_owner_step_chill_resisted_after_twelve_seconds);

// ---- bosses: immune to every effect the data gates on the Boss effect -------------------------------------------------------------------------------------------
/// Runs `body` with the log going to a string, and returns it.
template <class Body>
std::string a3_capture_log(Body&& body) {
    std::ostringstream log;
    const auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("a3_resist_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    body();
    logger->flush();
    spdlog::set_default_logger(previous);
    return log.str();
}

/// The effects the data gates on the game's Boss effect: their requirement names the Boss effect record.
std::vector<const abilities::EffectDef*> a3_boss_gated(const Fixture& f) {
    std::vector<const abilities::EffectDef*> out;
    constexpr int64_t kBossEffect = 6064631680720502784ll;
    for (const auto& [guid, def] : f.catalog->all_effects()) {
        const content::ExprRow* req = def.row.requirement_expr ? f.catalog->expr(def.row.requirement_expr) : nullptr;
        if (req && req->text.find(std::to_string(kBossEffect)) != std::string::npos) out.push_back(&def);
    }
    return out;
}

void test_a3_boss_is_immune_to_every_boss_gated_effect(Fixture& f) {
    std::printf("a boss is immune to every effect the data gates on the Boss effect; a Wolf without the tag takes them\n");
    const std::string wolf_path = wolf_archetype(f);
    const std::vector<const abilities::EffectDef*> gated = a3_boss_gated(f);
    std::printf("    the data gates %zu effects on the Boss effect\n", gated.size());
    A3_CHECKF(gated.size() == 24, "24 exported effects carry the data's Boss gate (%zu)", gated.size());
    if (gated.empty()) return;
    npc::MotionTable boss_table;
    A3_CHECK(kb_boss_table(wolf_path, boss_table), "the Blueprint table with the Boss tag on the Wolf's row");
    if (!boss_table.rows().empty()) {
        std::swap(f.motion, boss_table);
        {
            dots::Bed bed(f);
            A3_CHECKF(bed.ok(), "the boss bed: %s", bed.problem().c_str());
            if (bed.ok()) {
                const uint64_t boss = a3_wolf(bed, 0);
                A3_CHECK(boss != 0, "a level-8 Wolf whose Blueprint carries the Boss tag");
                if (boss) {
                    const std::string log = a3_capture_log([&] {
                        double now = 100.0;
                        for (const abilities::EffectDef* def : gated) {
                            effects::Applied a;
                            a.def = def;
                            a.stacks = 1;
                            a.duration = 30.0;
                            bed.world.apply_effects(boss, 77, {a}, now += 0.01);
                        }
                    });
                    int immune = 0;
                    for (const abilities::EffectDef* def : gated) {
                        if (log.find("is immune to " + def->row.name + " from player 77") != std::string::npos) ++immune;
                    }
                    A3_CHECKF(immune == static_cast<int>(gated.size()), "each of the %zu effects is named as immune once (%d)", gated.size(), immune);
                    A3_CHECKF(bed.world.effect_count(boss) == 0, "none of them is on the boss (%zu effect(s))", bed.world.effect_count(boss));
                    // the order: a boss whose chance is set to 1 is still immune, not resisting
                    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
                    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
                    A3_CHECK(chilled && frostbolt, "the data has the chill and the Frostbolt's hit");
                    if (chilled && frostbolt) {
                        A3_CHECK(a3_set_stat(f, bed.world, boss, kA3MagicalDisableEvasion, 1.0, 200.0), "the boss's chance is set to 1 by hand");
                        const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
                        A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
                        if (chill) {
                            const std::string order = a3_capture_log([&] { bed.world.apply_effects(boss, 77, {*chill}, 200.0); });
                            A3_CHECK(order.find("is immune to Status_Chilled") != std::string::npos && order.find("resists Status_Chilled") == std::string::npos,
                                      "with a chance of 1 the boss is immune to the chill, not resisting it");
                        }
                    }
                }
            }
        }
        std::swap(f.motion, boss_table);
    }
    // a Wolf without the tag takes a gated effect (a stun, at its own chance of 0 at level 8)
    dots::Bed plain(f);
    A3_CHECKF(plain.ok(), "the plain bed: %s", plain.problem().c_str());
    if (plain.ok()) {
        const abilities::EffectDef* stunned = f.catalog->effect_named("Status_Stunned");
        A3_CHECK(stunned != nullptr, "the data has the stun");
        if (stunned) {
            const uint64_t wolf = a3_wolf(plain, 0);
            const std::string log = a3_capture_log([&] {
                effects::Applied a;
                a.def = stunned;
                a.stacks = 1;
                a.duration = 5.0;
                plain.world.apply_effects(wolf, 77, {a}, 100.0);
            });
            A3_CHECKF(plain.world.effect_stacks(wolf, stunned->row.guid) == 1, "a Wolf without the Boss tag takes the stun (%d stack(s))", plain.world.effect_stacks(wolf, stunned->row.guid));
            A3_CHECK(log.find("is immune to Status_Stunned") == std::string::npos, "no immune line is written for it");
        }
    }
}
AOC_REGISTER_TEST(test_a3_boss_is_immune_to_every_boss_gated_effect);

// ---- what is never resisted -----------------------------------------------------------------------------------------------------------------------------------
// A debuff that is not a crowd control and a damage over time carry no CrowdControlConstraint tag, so the roll never sees them, even when the target's chance is set
// to 1.  The chill, tagged, is resisted at the same chance.
void test_a3_non_crowd_control_effects_are_never_resisted(Fixture& f) {
    std::printf("a stat debuff and a bleed are never resisted; the tagged chill at the same hand-set chance is\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* shaken = f.catalog->effect_named("Status_Shaken");
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && shaken && bleeding && frostbolt, "the data has the chill, Shaken, the bleed and the Frostbolt's hit");
    if (!chilled || !shaken || !bleeding || !frostbolt) return;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    const double accuracy = a3_accuracy(bed), base = bed.stat_of(wolf, kA3MagicalDisableEvasion);
    A3_CHECK(a3_set_stat(f, bed.world, wolf, kA3MagicalDisableEvasion, 1.0 + accuracy - base, 100.0), "the Wolf's chance is set to 1 by hand");
    const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
    A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
    if (!chill) return;
    const auto apply = [&](const abilities::EffectDef& def) {
        effects::Applied a;
        a.def = &def;
        a.stacks = 1;
        a.duration = 30.0;
        bed.world.apply_effects(wolf, 77, {a}, 100.0);
        return bed.world.effect_stacks(wolf, def.row.guid);
    };
    A3_CHECKF(apply(*shaken) == 1, "Shaken, a debuff that is not a crowd control, is on the Wolf at a chance of 1 (%d stack(s))", bed.world.effect_stacks(wolf, shaken->row.guid));
    A3_CHECKF(apply(*bleeding) == 1, "the bleed, a damage over time, is on the Wolf at a chance of 1 (%d stack(s))", bed.world.effect_stacks(wolf, bleeding->row.guid));
    bed.world.apply_effects(wolf, 77, {*chill}, 101.0);
    A3_CHECKF(bed.world.effect_stacks(wolf, chilled->row.guid) == 0, "the tagged chill at the same chance is resisted (%d stack(s))", bed.world.effect_stacks(wolf, chilled->row.guid));
}
AOC_REGISTER_TEST(test_a3_non_crowd_control_effects_are_never_resisted);

// ---- a resisted crowd control changes nothing else -----------------------------------------------------------------------------------------------------------
// A damaging hit and a non-crowd-control effect of the same hit stand when its chill is resisted: the Health lost, the threat and the other effect are the hit's
// own, and the resist only keeps the chill off.
void test_a3_a_resisted_chill_leaves_damage_other_effects_and_threat(Fixture& f) {
    std::printf("a resisted chill leaves the hit's damage, its other effect and its threat in place\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* shaken = f.catalog->effect_named("Status_Shaken");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    A3_CHECK(chilled && shaken && frostbolt, "the data has the chill, Shaken and the Frostbolt's hit");
    if (!chilled || !shaken || !frostbolt) return;
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    const double accuracy = a3_accuracy(bed), base = bed.stat_of(wolf, kA3MagicalDisableEvasion);
    A3_CHECK(a3_set_stat(f, bed.world, wolf, kA3MagicalDisableEvasion, 1.0 + accuracy - base, 100.0), "the Wolf's chance is set to 1 by hand");
    const std::optional<effects::Applied> chill = a3_resolved_chill(f, bed, *frostbolt);
    A3_CHECK(chill.has_value(), "the Frostbolt's record resolves the chill");
    if (!chill) return;
    const double before = bed.world.health_of(wolf);
    combat::Resolved damage;
    damage.mods.push_back({1, content::kGuidHealth, -100.0});
    damage.threat = 100.0;
    bed.world.apply_hit_to_npc(wolf, 77, damage, 100.0);
    const double lost = before - bed.world.health_of(wolf);
    const npc::ai::NpcAi* ai = bed.world.ai_of(wolf);
    A3_CHECKF(lost > 50.0, "the hit took its Health from the Wolf (%.1f)", lost);
    A3_CHECKF(ai && !ai->threat().empty(), "the hit's threat is on the Wolf's table (%zu attacker(s))", ai ? ai->threat().size() : 0);
    effects::Applied other;
    other.def = shaken;
    other.stacks = 1;
    other.duration = 30.0;
    bed.world.apply_effects(wolf, 77, {other, *chill}, 101.0);
    A3_CHECKF(bed.world.effect_stacks(wolf, shaken->row.guid) == 1, "the hit's other effect, Shaken, is on the Wolf (%d stack(s))", bed.world.effect_stacks(wolf, shaken->row.guid));
    A3_CHECKF(bed.world.effect_stacks(wolf, chilled->row.guid) == 0, "only the chill is kept off (%d stack(s))", bed.world.effect_stacks(wolf, chilled->row.guid));
}
AOC_REGISTER_TEST(test_a3_a_resisted_chill_leaves_damage_other_effects_and_threat);

// ---- a throw or a pull is not a crowd-control effect --------------------------------------------------------------------------------------------------------
// A hit's velocity change and its teleport are not effects on the target: the resist gate only sees the effects a hit applies, so no roll can stop them.  The
// movement itself is covered by the knock-back tests (`test_kb_get_off_the_stage`, `test_kb_grapple_pull`); this check reads the data's shape.
void test_a3_a_throw_and_a_pull_are_not_effects_the_roll_sees(Fixture& f) {
    std::printf("a throw and a pull are a hit's impulse and teleport, not effects: the resist gate cannot see them\n");
    const abilities::HitDef* throw_hit = f.catalog->hit_named("Bard_Misc_GetOffTheStage");
    const abilities::HitDef* pull = f.catalog->hit_named("Tank_Grapple_Teleport_Hit");
    A3_CHECK(throw_hit && throw_hit->impulse && throw_hit->impulse->applies, "Get Off The Stage is an impulse hit whose throw applies (the knock-back tests measure the movement)");
    A3_CHECK(pull && pull->teleport, "the Grapple is a teleport hit (the knock-back tests measure the pull)");
    // (the hit may also apply ordinary effects, the throw's snare among them; those are effects and the roll sees them, but the movement is not one)
}
AOC_REGISTER_TEST(test_a3_a_throw_and_a_pull_are_not_effects_the_roll_sees);

// ---- the cost and the cooldown are the cast's ----------------------------------------------------------------------------------------------------------------
// A Rogue's Kick pays its Mana and starts its cooldown when it is cast; the crowd control of its hit is resisted, and the payment is not given back by the resist.
void test_a3_a_resisted_kick_still_pays_its_cost_and_cooldown(Fixture& f) {
    std::printf("a resisted Kick still pays its Mana and starts its cooldown\n");
    Forge g(f, 5, {});
    A3_CHECKF(g.ok(), "the Rogue rig: %s", g.rig.problem.c_str());
    if (!g.ok()) return;
    const abilities::AbilityDef* kick = f.catalog->ability_named("Rogue_Kick");
    const abilities::EffectDef* tripped = f.catalog->effect_named("Status_Tripped");
    A3_CHECK(kick && tripped, "the data has Rogue_Kick and its trip");
    if (!kick || !tripped) return;
    const uint64_t wolf = g.wolf(150, 0);
    A3_CHECK(wolf != 0, "a Wolf 1.5 m away");
    if (!wolf) return;
    constexpr int64_t kPhysicalDisableEvasion = 109343399106252ll;
    A3_CHECK(a3_set_stat(f, g.world, wolf, kPhysicalDisableEvasion, 2.0, 0.5), "the Wolf's PhysicalDisableEvasion is set to 2 by hand");
    g.rig.know({"Rogue_Kick"});
    const double mana_before = g.rig.mana();
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    const Refusal refused = g.rig.pipe->trigger(kick->row.guid, 10, 1.0, info);
    A3_CHECKF(refused == Refusal::None, "the Kick is cast (%s)", refusal_name(refused));
    if (refused != Refusal::None) return;
    g.rig.run_to(2.0);
    const double mana_after = g.rig.mana();
    A3_CHECKF(mana_after < mana_before, "the Kick paid its Mana (%.3f -> %.3f)", mana_before, mana_after);
    A3_CHECKF(g.rig.pipe->cooldowns_running() > 0, "the Kick started its cooldown (%zu group(s) running)", g.rig.pipe->cooldowns_running());
    // the trip of the same ability at the same chance is resisted
    effects::Applied trip;
    trip.def = tripped;
    trip.stacks = 1;
    trip.duration = 5.0;
    g.world.apply_effects(wolf, g.player.actor_id(), {trip}, 2.0);
    A3_CHECKF(g.world.effect_stacks(wolf, tripped->row.guid) == 0, "the trip of the same ability is resisted (%d stack(s))", g.world.effect_stacks(wolf, tripped->row.guid));
}
AOC_REGISTER_TEST(test_a3_a_resisted_kick_still_pays_its_cost_and_cooldown);

// ---- the two immunity buffs -----------------------------------------------------------------------------------------------------------------------------------
// Rafflesia's Steadfast Root and Shardmaw's Void Rage add 1 to the holder's DisableEvasion, which is 100 % for every tagged crowd control while they last.  The
// data's own stat would already resist the roll; the world calls it immune (the client's line is the immunity one), and the effect is kept off.
void test_a3_the_immunity_buffs_make_tagged_crowd_control_immune(Fixture& f) {
    std::printf("Rafflesia's Steadfast Root and Shardmaw's Void Rage: tagged crowd control is immune while the buff lasts\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const char* const buffs[] = {"Rafflesia_SteadfastRoot_Buff", "Shardmaw_VoidRage_Effect"};
    const abilities::EffectDef* stunned = f.catalog->effect_named("Status_Stunned");
    A3_CHECK(stunned != nullptr, "the data has the stun");
    if (!stunned) return;
    for (const char* name : buffs) {
        const abilities::EffectDef* buff = f.catalog->effect_named(name);
        A3_CHECKF(buff != nullptr, "the data has %s", name);
        if (!buff) continue;
        const uint64_t wolf = a3_wolf(bed, 0);
        A3_CHECKF(wolf != 0, "a level-8 Wolf for %s", name);
        if (!wolf) continue;
        effects::Applied ward;
        ward.def = buff;
        ward.stacks = 1;
        ward.duration = 30.0;
        ward.mods.push_back({6064628633961889796ll, 1.0});   // the data's DisableEvasion +1 (ab_mod value 1)
        bed.world.apply_effects(wolf, 77, {ward}, 100.0);
        const double evasion = bed.stat_of(wolf, kA3MagicalDisableEvasion);
        A3_CHECKF(evasion > 0.9, "%s raised the Wolf's MagicalDisableEvasion over 0.9 (%.6f)", name, evasion);
        const std::string log = a3_capture_log([&] {
            effects::Applied stun;
            stun.def = stunned;
            stun.stacks = 1;
            stun.duration = 5.0;
            bed.world.apply_effects(wolf, 77, {stun}, 101.0);
        });
        A3_CHECKF(log.find("is immune to Status_Stunned") != std::string::npos, "%s: the stun is named as immune", name);
        A3_CHECKF(log.find("resists Status_Stunned") == std::string::npos, "%s: the stun is not a resist", name);
        A3_CHECKF(bed.world.effect_stacks(wolf, stunned->row.guid) == 0, "%s: the stun is not on the Wolf (%d stack(s))", name, bed.world.effect_stacks(wolf, stunned->row.guid));
    }
    // the control: a Wolf without either buff takes the same stun at its own chance of 0
    const uint64_t plain = a3_wolf(bed, 300.0);
    A3_CHECK(plain != 0, "a Wolf without the buffs");
    if (plain) {
        effects::Applied stun;
        stun.def = stunned;
        stun.stacks = 1;
        stun.duration = 5.0;
        const std::string log = a3_capture_log([&] { bed.world.apply_effects(plain, 77, {stun}, 102.0); });
        A3_CHECKF(bed.world.effect_stacks(plain, stunned->row.guid) == 1, "without a buff the stun lands (%d stack(s))", bed.world.effect_stacks(plain, stunned->row.guid));
        A3_CHECK(log.find("is immune to Status_Stunned") == std::string::npos, "and no immune line is written for it");
    }
}
AOC_REGISTER_TEST(test_a3_the_immunity_buffs_make_tagged_crowd_control_immune);

// ---- the target's evasion reads the effect's own Element tag, not the hit's override ---------------------------------------------------------
// The data's EffectEvasion reads the EFFECT's Element.Physical / Element.Magical tags; the applying hit's scaling override belongs to the
// EffectAccuracy term.  Get Off The Stage is one of the five player hits whose tagged crowd control's element differs from its hit's override: its
// snare and its disarm are physical, while the hit says ability.MagicalScalingOverride.  The Wolf's physical evasion is set to 2 (always resist)
// and its magical evasion to -1 (never resist): the snare is resisted only when the evasion reads the effect's physical tag.
void test_a3_the_evasion_reads_the_effects_own_element(Fixture& f) {
    std::printf("the hit's scaling override does not move the target's evasion: Get Off The Stage's physical snare is resisted on a physical evasion of 2\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::HitDef* hit = f.catalog->hit_named("Bard_Misc_GetOffTheStage");
    const abilities::EffectDef* snared = f.catalog->effect_named("Status_Snared");
    A3_CHECK(hit && snared, "the data has Get Off The Stage and its snare");
    if (!hit || !snared) return;
    const auto has = [](const std::vector<std::string>& tags, const char* want) {
        for (const std::string& t : tags) {
            if (combat::tag_matches(t, want)) return true;
        }
        return false;
    };
    A3_CHECK(has(hit->tags, "ability.MagicalScalingOverride"), "the hit carries the data's magical scaling override");
    A3_CHECK(has(snared->row.tags, "Element.Physical") || has(snared->row.effect_tags, "Element.Physical"), "the snare's own Element tag is physical");
    A3_CHECK(!has(snared->row.tags, "Element.Magical") && !has(snared->row.effect_tags, "Element.Magical"), "...and it has no magical Element tag");
    const uint64_t wolf = a3_wolf(bed, 0);
    A3_CHECK(wolf != 0, "a level-8 Wolf");
    if (!wolf) return;
    constexpr int64_t kPhysicalDisableEvasion = 109343399106252ll;
    A3_CHECK(a3_set_stats(f, bed.world, wolf, {{kPhysicalDisableEvasion, 2.0}, {kA3MagicalDisableEvasion, -1.0}}, 100.0),
             "the Wolf's physical evasion is set to 2 and its magical evasion to -1 by hand");
    const double physical = bed.stat_of(wolf, kPhysicalDisableEvasion), magical = bed.stat_of(wolf, kA3MagicalDisableEvasion);
    A3_CHECKF(physical > 1.5 && magical < -0.5, "the Wolf's evasions read %.3f and %.3f", physical, magical);
    bed.land(wolf, "Bard_Misc_GetOffTheStage", 100.0, bed.player, false);
    A3_CHECKF(bed.world.effect_stacks(wolf, snared->row.guid) == 0, "the physical snare is resisted by the physical evasion, not landed by the hit's magical override (%d stack(s))",
              bed.world.effect_stacks(wolf, snared->row.guid));
}
AOC_REGISTER_TEST(test_a3_the_evasion_reads_the_effects_own_element);

// ---- a pet's crowd control reads the pet's own accuracy; its line stays the Summoner's ---------------------------------------------------------
// The data's EffectAccuracy reads the instigator's own DisableAccuracy.  The pet's damage is worked out from its owner's stats, but the crowd control the
// pet applies is the pet's: its own accuracy covers the target's evasion.  The Summoner is still told of a refusal, and the log still names him.
void test_a3_a_pets_crowd_control_reads_the_pets_accuracy(Fixture& f) {
    std::printf("a pet's crowd control rolls with the pet's own DisableAccuracy, and the Summoner is still told\n");
    dots::Bed bed(f);
    A3_CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* incap = f.catalog->effect_named("Summoner_Pet_Hunter_Incap");
    A3_CHECK(incap != nullptr && incap->row.guid == pets::kHunterIncapMelee, "the data has the Hunter's Incapacitate effect");
    if (!incap) return;
    const pets::Definition* hunter = nullptr;
    for (const pets::Definition& d : pets::kDefinitions) {
        if (std::string(d.name) == "Spirit Hunter") hunter = &d;
    }
    A3_CHECK(hunter != nullptr, "the data has the Spirit Hunter");
    if (!hunter) return;
    const uint64_t wolf = bed.w.spawn(bed.wolf_path, 500.0, 0.0, 8, 0), pet = bed.w.spawn(hunter->archetype, 300.0, 0.0, 25, 0);
    A3_CHECK(wolf != 0 && pet != 0, "a level-8 Wolf and a Spirit Hunter 2 m apart");
    if (!wolf || !pet) return;
    A3_CHECK(bed.world.pet_summoned(pet, bed.player.actor_id()), "the pet joins its Summoner's side");
    const double summoner_accuracy = a3_accuracy(bed);
    A3_CHECK(a3_set_stats(f, bed.world, pet, {{kA3MagicalDisableAccuracy, 2.0}}, 100.0), "the pet's MagicalDisableAccuracy is set to 2 by hand");
    const double pet_accuracy = bed.stat_of(pet, kA3MagicalDisableAccuracy);
    A3_CHECKF(pet_accuracy > 1.5 && summoner_accuracy < 0.5 && pet_accuracy - summoner_accuracy > 1.0, "the pet's accuracy %.3f and the Summoner's %.3f differ",
              pet_accuracy, summoner_accuracy);
    A3_CHECK(a3_set_stats(f, bed.world, wolf, {{kA3MagicalDisableEvasion, 1.0}}, 100.0), "the Wolf's MagicalDisableEvasion is set to 1 by hand");
    A3_CHECKF(bed.stat_of(wolf, kA3MagicalDisableEvasion) > 0.9, "the Wolf's evasion reads %.3f", bed.stat_of(wolf, kA3MagicalDisableEvasion));
    bed.player.refusals.clear();
    double now = 100.0;
    for (int i = 0; i < 100; ++i) {
        if (!bed.world.pet_use(pet, bed.player.actor_id(), pets::kHunterIncapacitate, wolf, hunter->reach_cm)) {
            A3_CHECK(false, "the pet takes the Incapacitate");
            return;
        }
        bed.run(now, now + 0.6);
        now += 0.6;
    }
    A3_CHECKF(bed.player.refusals.empty(), "the pet's accuracy (%.3f) covers the Wolf's %.3f evasion: none of the 100 casts is resisted (%zu refusal(s))", pet_accuracy,
              bed.stat_of(wolf, kA3MagicalDisableEvasion), bed.player.refusals.size());
    A3_CHECKF(bed.world.effect_stacks(wolf, incap->row.guid) > 0, "...and the stun is on the Wolf (%d stack(s))", bed.world.effect_stacks(wolf, incap->row.guid));
    // the line: the pet's resisted crowd control is refused to the Summoner, and the log names him
    A3_CHECK(a3_set_stats(f, bed.world, pet, {{kA3MagicalDisableAccuracy, -1.0}}, now), "the pet's accuracy is set under 0 by hand");
    A3_CHECKF(bed.stat_of(pet, kA3MagicalDisableAccuracy) < 0.5, "the pet's accuracy now reads %.3f", bed.stat_of(pet, kA3MagicalDisableAccuracy));
    bed.player.refusals.clear();
    const std::string log = a3_capture_log([&] {
        if (bed.world.pet_use(pet, bed.player.actor_id(), pets::kHunterIncapacitate, wolf, hunter->reach_cm)) bed.run(now, now + 0.6);
    });
    A3_CHECKF(bed.player.refusals.size() == 1, "the Summoner is told of the resisted crowd control (%zu line(s))", bed.player.refusals.size());
    if (bed.player.refusals.size() == 1) {
        A3_CHECKF(bed.player.refusals[0].effect_guid == incap->row.guid, "the line names the pet's crowd control (%lld)", static_cast<long long>(bed.player.refusals[0].effect_guid));
        A3_CHECKF(bed.player.refusals[0].target.ObjectId == wolf, "the line names the Wolf (%llu)", static_cast<unsigned long long>(bed.player.refusals[0].target.ObjectId));
    }
    A3_CHECKF(log.find("resists Summoner_Pet_Hunter_Incap from player 77") != std::string::npos, "the combat log names the Summoner as the caster");
}
AOC_REGISTER_TEST(test_a3_a_pets_crowd_control_reads_the_pets_accuracy);
