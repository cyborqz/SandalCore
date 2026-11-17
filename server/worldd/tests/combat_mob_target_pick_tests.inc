// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/combat_mob_target_pick_tests.inc
//
//  Registered tests of test_worldd_combat: a creature's tree scores the actors it knows and plays its move at the best one (the utility-score selector the table
//  folds to a `pick` condition).  The tests use the real ability rows and montages (the Spider's Pounce, the Giant's BackSwipe, the Raptor's Smell of Blood, the
//  Najash Mender's Mend) and put the pick condition on the row exactly as the reader writes it, so the world is driven over the whole path the player sees:
//  the tree allows the move, the selector picks, the cast names the picked actor, the hit or the effect reaches it, and nobody else.  A dead or gone candidate
//  is not picked, the same choice comes twice, and a reset drops the stored pick.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// These checks print their own passing line too, so the proof of the eight angles can quote the [ok ] line of a check as well as its [FAIL] line.
// (One printf for the whole line: the world's log writes to the same console between two calls.)
#define PICK_CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define PICK_CHECKF(cond, ...) do { \
    char pick_line[1024]; \
    std::snprintf(pick_line, sizeof pick_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", pick_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", pick_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace fs = std::filesystem;

namespace pick_tests {

/// A row of the real creature table with the condition the reader writes for the state's selector: `unknown` 0, the pick atom in `conds`, and the task's own cooldown
/// as the gate it is held by (what the tree's cooldown condition does).
std::string row_with_pick(const std::string& path, int64_t ability, const std::string& task, double cooldown, int order, const std::string& pick, double arrival, const std::string& rotate) {
    return self_effects::row(path, ability, task, cooldown, 1, order, -1, -1, -1, task, 0, 1, "c0:" + pick, "", "", rotate, arrival);
}

/// One cast as the client's element names it: when, the ability, and the actor the cast's target list names (0: no target).
struct PickCast {
    double   t = 0;
    int64_t  ability = 0;
    uint64_t target = 0;   // the first target's ObjectId (a player's cylinder is his pawn's ObjectId + 20)
};

/// Watches the creature's cast element and records every new cast with the actor it names.
struct PickLog {
    std::vector<PickCast> casts;
    GremCastView          last;

    void look(world::ActorRegistry& registry, uint64_t creature, double now) {
        const GremCastView view = grem_cast_view(registry, creature);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) casts.push_back({now, view.ability, view.target.ObjectId});
        last = view;
    }
    double first(int64_t ability) const {
        for (const PickCast& c : casts) if (c.ability == ability) return c.t;
        return -1;
    }
    /// When the `which`-th cast of `ability` happened; -1 when it never did.
    double time_of(int64_t ability, size_t which = 0) const {
        size_t seen = 0;
        for (const PickCast& c : casts) {
            if (c.ability != ability) continue;
            if (seen++ == which) return c.t;
        }
        return -1;
    }
    size_t count(int64_t ability) const {
        size_t n = 0;
        for (const PickCast& c : casts) n += c.ability == ability ? 1 : 0;
        return n;
    }
    /// The ObjectId of the first cast's target and how many casts name it; -1 when the move was never cast.
    uint64_t target_of(int64_t ability, size_t which = 0) const {
        size_t seen = 0;
        for (const PickCast& c : casts) {
            if (c.ability != ability) continue;
            if (seen++ == which) return c.target;
        }
        return 0;
    }
    std::string text(const aoc::worldd::abilities::Catalog& catalog) const {
        std::string line;
        for (const PickCast& c : casts) {
            const aoc::worldd::abilities::AbilityDef* d = catalog.ability(c.ability);
            line += (line.empty() ? "" : ", ") + std::string(d ? d->row.name : "?") + " at " + std::to_string(c.t).substr(0, 5) + " targeting " + std::to_string(c.target);
        }
        return line;
    }
};


/// The creature's place and facing now.
combat::Vec3 place_of(combat_rig::World& w, uint64_t creature) {
    const world::WorldActor* a = w.registry.find(creature);
    return a ? combat::Vec3{a->x, a->y, a->z} : combat::Vec3{};
}
double facing_of(combat_rig::World& w, uint64_t creature) {
    const world::WorldActor* a = w.registry.find(creature);
    return a ? a->yaw_degrees : 0.0;
}

}  // namespace pick_tests

// R1, two players at different distances: a Webweaver fights the player at its feet (3 m, outside the Pounce's 5-15 m band) and the second player stands at
// 10 m, then 20 m, then 12 m.  The Pounce is aimed at the player in the band only: the one the spider fights is never its pick, and with the second player out
// of the band the spider bites the one at its feet instead.
void test_a_webweaver_pounces_only_on_the_player_in_its_band(Fixture& f) {
    std::printf("a Webweaver's Pounce picks the player its selector scores in the 5-15 m band while it fights another at 3 m, and is not played with that player outside the band\n");
    const std::string path = creature_path(f, "/Beast/Arachnid/GiantSpider/Spider_Timid.Default__Spider_Timid_C");
    const abilities::AbilityDef* pounce = f.catalog->ability_named("Spider_Pounce");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Spider_Bite");
    PICK_CHECK(!path.empty() && f.motion.find(path) && pounce && bite, "the Blueprint table has the Webweaver and the ability data has its Pounce and Bite");
    if (path.empty() || !f.motion.find(path) || !pounce || !bite) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_pounce_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    // the real rows: the Pounce's condition is the selector's score of the 500-1500 cm band (the reader's atom), the Bite keeps the table's own
    const std::string pick = "pick(hated,highest,t,,,d;500;gt;d;1500;lt;and;r;0;if)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, pounce->row.guid, "Pounce", 20, 36, pick, 65, "360:0"),
        self_effects::row(path, bite->row.guid, "Bite", 6, 1, 39, -1, 300, 45, "Bite", 0, 1, "", "", "", "360:0", 65),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Webweaver's table with the Pounce's selector reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 300);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Webweaver's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t spider = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(spider != 0, "the Webweaver is put in the world");
    if (spider == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 1000, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;

    const double dt = 1.0 / 15.0;
    const double at_b = other.actor_id() + 20;
    double far_at = -1, back_at = -1;
    for (double now = dt; now <= 52.0 + 1e-9; now += dt) {
        field.world.add_threat(spider, field.player.actor_id(), 200.0, now);
        field.world.add_threat(spider, other.actor_id(), 100.0, now);
        // the player the spider fights stands at its feet; the second one is 10 m, then 20 m, then 12 m away from the spider
        const combat::Vec3 at = pick_tests::place_of(field.w, spider);
        field.player.rig_fighter().move_to({at.x, at.y + 300, at.z});
        if (now >= 30.0) {
            if (back_at < 0) back_at = now;
            other.rig_fighter().move_to({at.x, at.y + 1200, at.z});
        } else if (now >= 15.0) {
            if (far_at < 0) far_at = now;
            other.rig_fighter().move_to({at.x, at.y + 2000, at.z});
        } else {
            other.rig_fighter().move_to({at.x, at.y + 1000, at.z});
        }
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, spider, now);
    }
    std::printf("  casts: %s\n", log.text(*f.catalog).c_str());
    PICK_CHECKF(log.count(pounce->row.guid) >= 2, "the Webweaver pounces while the second player is in the band (%zu Pounces)", log.count(pounce->row.guid));
    PICK_CHECKF(log.count(pounce->row.guid) >= 1 && log.target_of(pounce->row.guid, 0) == at_b, "the first Pounce names the player 10 m away (%llu), not the one it fights at 3 m",
                static_cast<unsigned long long>(log.target_of(pounce->row.guid, 0)));
    PICK_CHECKF(field.log.count(bite->row.guid) >= 1, "with the second player out of the band the spider bites the one it fights (%zu bites)", field.log.count(bite->row.guid));
    const double pounce_two = log.time_of(pounce->row.guid, 1);
    PICK_CHECKF(pounce_two > far_at && back_at > 0, "a Pounce follows the second player back into the band (at %.2f s, the return at %.2f s)", pounce_two, back_at);
    PICK_CHECKF(pounce_two > 0 && log.target_of(pounce->row.guid, 1) == at_b, "it names that player again (%llu)", static_cast<unsigned long long>(log.target_of(pounce->row.guid, 1)));
    size_t pounces_far = 0;
    for (const pick_tests::PickCast& c : log.casts) pounces_far += c.ability == pounce->row.guid && c.t > far_at && c.t < back_at ? 1 : 0;
    PICK_CHECKF(far_at > 0 && pounces_far == 0, "no Pounce is played while the second player is 20 m away (%zu casts between %.2f and %.2f s)", pounces_far, far_at, back_at);
}

AOC_REGISTER_TEST(test_a_webweaver_pounces_only_on_the_player_in_its_band);

// R1: a target that walks behind the Giant.  The BackSwipe's selector scores only a candidate behind the creature within 4.2 m (the real condition); with the
// player the Giant fights in front of it and a second player behind, the move is aimed at the one behind.  With nobody behind it the move is not played.
void test_the_giant_flanks_the_player_behind_it(Fixture& f) {
    std::printf("a Giant's BackSwipe picks the player behind it (within 4.2 m), not the one in front it fights, and is not played with nobody behind it\n");
    const std::string path = creature_path(f, "/Humanoid/Giants/Giant_Base.Default__Giant_Base_C");
    const abilities::AbilityDef* back = f.catalog->ability_named("Giant_BackSwipe");
    const abilities::AbilityDef* swipe = f.catalog->ability_named("Giant_CleavingSwipe");
    PICK_CHECK(!path.empty() && f.motion.find(path) && back && swipe, "the Blueprint table has the Giant and the ability data has its BackSwipe and Swipe");
    if (path.empty() || !f.motion.find(path) || !back || !swipe) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_giant_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::string pick = "pick(hated,highest,t,,,face;-0.5;lt;d;420;lt;and)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, back->row.guid, "BackSwipe", 12, 13, pick, -1, ""),
        self_effects::row(path, swipe->row.guid, "Swipe", 8, 1, 18, -1, 260, 60, "Swipe", 0, 1, "c0:fc(ge,8)", "", "", "360:0", -1),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Giant's table with the BackSwipe's selector reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 300);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Giant's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t giant = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(giant != 0, "the Giant is put in the world");
    if (giant == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 200, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0;
    double alone_from = -1;
    for (double now = dt; now <= 42.0 + 1e-9; now += dt) {
        field.world.add_threat(giant, field.player.actor_id(), 200.0, now);
        field.world.add_threat(giant, other.actor_id(), 100.0, now);
        const combat::Vec3 at = pick_tests::place_of(field.w, giant);
        const double yaw = pick_tests::facing_of(field.w, giant) * 3.14159265358979323846 / 180.0;
        // the fought player stands 3 m in front of the Giant; the other walks round behind it at 2 m, and after 28 s out of reach
        field.player.rig_fighter().move_to({at.x + std::cos(yaw) * 300, at.y + std::sin(yaw) * 300, at.z});
        if (now >= 28.0) {
            if (alone_from < 0) alone_from = now;
            other.rig_fighter().move_to({at.x + std::cos(yaw) * 3000, at.y + std::sin(yaw) * 3000, at.z});
        } else {
            other.rig_fighter().move_to({at.x - std::cos(yaw) * 200, at.y - std::sin(yaw) * 200, at.z});
        }
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, giant, now);
    }
    std::printf("  casts: %s\n", log.text(*f.catalog).c_str());
    size_t behind_casts = 0, alone_casts = 0;
    bool behind_named_other = false;
    for (const pick_tests::PickCast& c : log.casts) {
        if (c.ability != back->row.guid) continue;
        if (c.t < 28.0) {
            ++behind_casts;
            behind_named_other = behind_named_other || c.target == other.actor_id() + 20;
        } else {
            ++alone_casts;
        }
    }
    PICK_CHECKF(behind_casts >= 1, "the Giant flanks while a player stands behind it (%zu BackSwipes before %.1f s)", behind_casts, alone_from);
    PICK_CHECKF(behind_named_other, "the BackSwipe is aimed at the player behind it, not at the one it fights in front");
    PICK_CHECKF(behind_casts > 0 && log.target_of(back->row.guid, 0) != field.player.actor_id() + 20, "the first BackSwipe does not name the player in front");
    PICK_CHECKF(alone_from > 0 && alone_casts == 0, "no BackSwipe is played with nobody behind it (%zu casts after %.1f s)", alone_casts, alone_from);
}

AOC_REGISTER_TEST(test_the_giant_flanks_the_player_behind_it);

// R2: the Raptor's Smell of Blood asks whether its fight's target bleeds.  The row is the reader's own: c0:teff(Status_Bleeding), the effect the Raptor's bite
// puts on its target.  With the target healthy the move is not played; the moment the target bleeds it is.  A second player's bleeding is not the fight's target.
void test_the_raptor_smells_blood_only_when_its_target_bleeds(Fixture& f) {
    std::printf("the Raptor's Smell of Blood is played only while its fight's target bleeds (the condition the reader writes), and never because another player does\n");
    const std::string path = creature_path(f, "/Beast/Raptor/Raptor_Tooth.Default__Raptor_Tooth_C");
    const abilities::AbilityDef* smell = f.catalog->ability_named("Raptor_SmellofBlood");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Raptor_Bite");
    const std::string bleed_guid = "6064629761775576467";   // Status_Bleeding, the game's bleeding record
    PICK_CHECK(!path.empty() && f.motion.find(path) && smell && bite && f.catalog->effect_named("Status_Bleeding"), "the Blueprint table has the Raptor and the ability data has its Smell and Bite and the bleeding record");
    if (path.empty() || !f.motion.find(path) || !smell || !bite) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_raptor_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::vector<std::string> rows = {
        self_effects::row(path, smell->row.guid, "Smell of Blood", 15, 1, 15, -1, -1, -1, "Smell of Blood", 0, 1, "c0:teff(" + bleed_guid + ")", "", "", "", -1),
        self_effects::row(path, bite->row.guid, "", 0, 1, 22, -1, 250, 90, "", 0, 1, "c0:in(250)", "", "", "360:1", -1),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Raptor's table with the reader's target-effect row reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 800);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Raptor's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t raptor = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(raptor != 0, "the Raptor is put in the world");
    if (raptor == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, -800, 1098});
    field.world.add_player(&other);
    const int64_t bleed = std::strtoll(bleed_guid.c_str(), nullptr, 10);
    rig_b.pipe->grant_effect(bleed, 1, 0.0);   // the player the Raptor does not fight bleeds from the start
    const double dt = 1.0 / 15.0, bleed_at = 10.0;
    bool given = false;
    for (double now = dt; now <= 26.0 + 1e-9; now += dt) {
        field.world.add_threat(raptor, field.player.actor_id(), 100.0, now);
        if (!given && now >= bleed_at) given = field.rig.pipe->grant_effect(bleed, 1, now);
        field.player.rig_fighter().move_to({0, 800, 1098});
        other.rig_fighter().move_to({0, -800, 1098});
        field.tick(now);
    }
    PICK_CHECK(given, "the bleed is put on the Raptor's fight target at 10 s");
    PICK_CHECKF(field.log.between(smell->row.guid, 0.0, bleed_at) == 0, "the Smell of Blood is not played while its target is healthy and only another player bleeds (%zu casts before %.1f s)",
                field.log.between(smell->row.guid, 0.0, bleed_at), bleed_at);
    PICK_CHECKF(field.log.first_after(smell->row.guid, bleed_at) > 0, "it is played once its fight target bleeds (first at %.2f s)", field.log.first_after(smell->row.guid, bleed_at));
}

AOC_REGISTER_TEST(test_the_raptor_smells_blood_only_when_its_target_bleeds);

// R1, the support trees: a healer's selector scores the friendly actors that are hurt (the real Mend row's candidate set and 1-health score).  With an ally at 50 %
// and one at 70 % the Mend's healing effect lands on the 50 % one only; the healthy-at-70 % one is not healed.
void test_the_mender_mends_the_most_hurt_ally_only(Fixture& f) {
    std::printf("the Najash Mender's Mend picks the most hurt ally its selector scores (50 %% against 70 %%), and the other ally is not healed\n");
    const std::string path = creature_path(f, "/Humanoid/Najash/Mender/Najash_Mender_Jungle.Default__Najash_Mender_Jungle_C");
    const abilities::AbilityDef* mend = f.catalog->ability_named("Najash_Mender_Mend");
    const abilities::AbilityDef* swarm = f.catalog->ability_named("Najash_Mender_Swarm");
    const int64_t heal = 6064633417608134662ll;   // the effect the Mend's hit applies
    PICK_CHECK(!path.empty() && f.motion.find(path) && mend && swarm && f.catalog->effect(heal), "the Blueprint table has the Najash Mender and the ability data has its Mend, Swarm and the healing effect");
    if (path.empty() || !f.motion.find(path) || !mend || !swarm || !f.catalog->effect(heal)) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_mender_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    // the reader's atom: the perceived actors, friendly, within 25 m, hurt below 80 %, not already carrying the Mend's effect; scored by how hurt they are
    const std::string pick = "pick(perceived,product,g,c4,friendly;d;2500;lt;and;h;0.8;lt;and;eff:6064633417608134662;1;lt;and,1;h;-)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, mend->row.guid, "Mend", 12, 12, pick, 1300, ""),
        self_effects::row(path, swarm->row.guid, "Swarm", 18, 1, 18, -1, 1500, 35, "Swarm", 0, 1, "c0:fc(ge,8)", "", "", "", 1300),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Mender's table with the Mend's selector reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 600);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Mender's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t mender = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(mender != 0, "the Mender is put in the world");
    if (mender == 0) return;
    const auto bp = f.motion.find(path);
    if (!field.made.open || !bp) {
        PICK_CHECK(false, "the world makes an open for the Mender");
        return;
    }
    // two more Menders of the same Blueprint stand beside it as the healer's allies: one at 50 % and one at 70 % health
    const uint64_t hurt = field.w.spawn(path, 500, 0, 7, 0, bp->half_height, field.made.open, &*bp);
    const uint64_t hurt_less = field.w.spawn(path, -500, 100, 7, 0, bp->half_height, field.made.open, &*bp);
    PICK_CHECK(hurt != 0 && hurt_less != 0, "the two allies are put in the world");
    if (hurt == 0 || hurt_less == 0) return;
    field.set_health_fraction(hurt, 0.5);
    field.set_health_fraction(hurt_less, 0.7);
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 16.0 + 1e-9; now += dt) {
        field.world.add_threat(mender, field.player.actor_id(), 100.0, now);
        field.set_health_fraction(hurt, 0.5);
        field.set_health_fraction(hurt_less, 0.7);
        field.tick(now);
    }
    std::printf("  casts: %s; the healer put %d of the healing effect on the 50 %% ally and %d on the 70 %% one; it holds %d\n", field.log.text(*f.catalog).c_str(),
                field.world.effect_stacks_from(hurt, heal, mender), field.world.effect_stacks_from(hurt_less, heal, mender), field.world.effect_stacks_from(mender, heal, mender));
    PICK_CHECKF(field.log.count(mend->row.guid) >= 1, "the Mender mends in a 16 s fight (%zu Mends)", field.log.count(mend->row.guid));
    PICK_CHECKF(field.world.effect_stacks_from(hurt, heal, mender) > 0, "the healer's healing effect lands on the ally at 50 %% (%d stacks)", field.world.effect_stacks_from(hurt, heal, mender));
    PICK_CHECKF(field.world.effect_stacks_from(hurt_less, heal, mender) == 0, "the ally at 70 %% is not healed by it (%d stacks)", field.world.effect_stacks_from(hurt_less, heal, mender));
    PICK_CHECKF(field.world.effect_stacks_from(mender, heal, mender) == 0, "the Mender does not heal itself (%d stacks)", field.world.effect_stacks_from(mender, heal, mender));
}

AOC_REGISTER_TEST(test_the_mender_mends_the_most_hurt_ally_only);

// R1, the same choice twice: the Webweaver fights one player at its feet and its selector is asked again for its second Pounce on the player in the band; both
// casts name that same player, with no stored target from the first cast changing it.
void test_the_same_choice_comes_twice(Fixture& f) {
    std::printf("a Webweaver's selector asked twice with the same player in its band picks that player both times\n");
    const std::string path = creature_path(f, "/Beast/Arachnid/GiantSpider/Spider_Timid.Default__Spider_Timid_C");
    const abilities::AbilityDef* pounce = f.catalog->ability_named("Spider_Pounce");
    PICK_CHECK(!path.empty() && f.motion.find(path) && pounce, "the Blueprint table has the Webweaver and the ability data has its Pounce");
    if (path.empty() || !f.motion.find(path) || !pounce) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_twice_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::string pick = "pick(hated,highest,t,,,d;500;gt;d;1500;lt;and;r;0;if)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, pounce->row.guid, "Pounce", 20, 36, pick, 65, "360:0"),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Webweaver's table for the twice-asked selector reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 300);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Webweaver's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t spider = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(spider != 0, "the Webweaver is put in the world");
    if (spider == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 1000, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 46.0 + 1e-9; now += dt) {
        field.world.add_threat(spider, field.player.actor_id(), 200.0, now);
        field.world.add_threat(spider, other.actor_id(), 100.0, now);
        const combat::Vec3 at = pick_tests::place_of(field.w, spider);
        field.player.rig_fighter().move_to({at.x, at.y + 300, at.z});
        other.rig_fighter().move_to({at.x, at.y + 1000, at.z});
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, spider, now);
    }
    const uint64_t at_other = other.actor_id() + 20;
    std::printf("  casts: %s\n", log.text(*f.catalog).c_str());
    PICK_CHECKF(log.count(pounce->row.guid) >= 2, "the Webweaver pounces twice (%zu Pounces)", log.count(pounce->row.guid));
    PICK_CHECKF(log.count(pounce->row.guid) >= 2 && log.target_of(pounce->row.guid, 0) == at_other && log.target_of(pounce->row.guid, 1) == at_other,
                "both Pounces name the same player (%llu and %llu)", static_cast<unsigned long long>(log.target_of(pounce->row.guid, 0)),
                static_cast<unsigned long long>(log.target_of(pounce->row.guid, 1)));
}

AOC_REGISTER_TEST(test_the_same_choice_comes_twice);

// R1, a target that dies or leaves mid-choice: the Giant aims its BackSwipe at the player behind it, that player dies before the hit lands (no hit on a dead
// player), and once the dead player is gone and the other stands behind, the next BackSwipe is aimed at the living one.
void test_a_chosen_target_that_dies_or_leaves_is_not_hit(Fixture& f) {
    std::printf("a BackSwipe whose chosen player dies before the hit lands hits nobody, and the next one picks the living player behind the Giant\n");
    const std::string path = creature_path(f, "/Humanoid/Giants/Giant_Base.Default__Giant_Base_C");
    const abilities::AbilityDef* back = f.catalog->ability_named("Giant_BackSwipe");
    PICK_CHECK(!path.empty() && f.motion.find(path) && back, "the Blueprint table has the Giant and the ability data has its BackSwipe");
    if (path.empty() || !f.motion.find(path) || !back) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_dies_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::string pick = "pick(hated,highest,t,,,face;-0.5;lt;d;420;lt;and)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, back->row.guid, "BackSwipe", 12, 13, pick, -1, ""),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Giant's table for the dying target reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 300);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Giant's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t giant = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(giant != 0, "the Giant is put in the world");
    if (giant == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        PICK_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, 200, 1098});
    field.world.add_player(&other);
    pick_tests::PickLog log;
    const double dt = 1.0 / 15.0;
    double first_cast = -1, killed_at = -1;
    bool killed = false, left = false;
    for (double now = dt; now <= 46.0 + 1e-9; now += dt) {
        field.world.add_threat(giant, field.player.actor_id(), 200.0, now);
        if (!left) field.world.add_threat(giant, other.actor_id(), 100.0, now);
        const combat::Vec3 at = pick_tests::place_of(field.w, giant);
        const double yaw = pick_tests::facing_of(field.w, giant) * 3.14159265358979323846 / 180.0;
        field.player.rig_fighter().move_to({at.x + std::cos(yaw) * 300, at.y + std::sin(yaw) * 300, at.z});
        other.rig_fighter().move_to({at.x - std::cos(yaw) * 200, at.y - std::sin(yaw) * 200, at.z});
        field.w.npcs->note_player(other.actor_id(), other.rig_fighter().position().x, other.rig_fighter().position().y, now);
        field.tick(now);
        log.look(field.w.registry, giant, now);
        // the first BackSwipe at the player behind: kill that player 0.15 s after the cast, before its hit is due
        if (!killed && first_cast < 0 && log.count(back->row.guid) >= 1) first_cast = log.first(back->row.guid);
        if (!killed && first_cast > 0 && now >= first_cast + 0.15) {
            killed = true;
            killed_at = now;
            rig_b.cs->set_current(content::kGuidHealth, 0.0f);   // the chosen player dies mid-cast
            PICK_CHECK(!other.rig_fighter().alive(), "the chosen player is dead at mid-cast");
        }
        // and after the cast the dead player leaves the world; the Giant keeps fighting the one in front
        if (killed && !left && now >= killed_at + 1.0) {
            left = true;
            field.world.remove_player(&other);
        }
    }
    std::printf("  casts: %s; the chosen player died at %.2f s\n", log.text(*f.catalog).c_str(), killed_at);
    PICK_CHECKF(first_cast > 0, "the Giant flanks the player behind it (first at %.2f s)", first_cast);
    PICK_CHECKF(first_cast > 0 && log.target_of(back->row.guid, 0) == other.actor_id() + 20, "the BackSwipe is aimed at the player behind it (%llu)",
                static_cast<unsigned long long>(log.target_of(back->row.guid, 0)));
    PICK_CHECKF(killed && killed_at > 0, "the chosen player dies before the BackSwipe's hit is due (at %.2f s)", killed_at);
    PICK_CHECKF(killed && other.hits.empty(), "the BackSwipe's hit does not land on the player who died before it (%zu hits)", other.hits.size());
    PICK_CHECKF(killed && left, "the dead player leaves the world afterwards");
    // the world still runs, keeps the fight on the living player and casts again without a stale target
    PICK_CHECKF(killed && log.count(back->row.guid) >= 1, "the Giant's world goes on after the player left (%zu BackSwipes)", log.count(back->row.guid));
}

AOC_REGISTER_TEST(test_a_chosen_target_that_dies_or_leaves_is_not_hit);

// R1, a mob that resets: the selector's choice is kept in the creature's blackboard, a reset drops it with the fight, and the next fight picks by the scores
// again.  The healer's table gives the stored target an absolute score of 1 (the Mender's own "Heal Target" clause); before the reset the stored ally is picked
// even though the other is more hurt, after it the more hurt one is.
void test_a_reset_drops_the_stored_choice(Fixture& f) {
    std::printf("a Mender's stored heal target wins before the reset and is gone after it, so the next Mend takes the most hurt ally\n");
    const std::string path = creature_path(f, "/Humanoid/Najash/Mender/Najash_Mender_Jungle.Default__Najash_Mender_Jungle_C");
    const abilities::AbilityDef* mend = f.catalog->ability_named("Najash_Mender_Mend");
    const abilities::AbilityDef* swarm = f.catalog->ability_named("Najash_Mender_Swarm");
    const int64_t heal = 6064633417608134662ll;
    PICK_CHECK(!path.empty() && f.motion.find(path) && mend && swarm && f.catalog->effect(heal), "the Blueprint table has the Mender and the ability data has its Mend, Swarm and the healing effect");
    if (path.empty() || !f.motion.find(path) || !mend || !swarm || !f.catalog->effect(heal)) return;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("aoc_pick_reset_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    // the stored target (the selector's own variable) scores 1; anyone else scores 1 - health.  The first Mend takes the 50 % ally (the 60 % one is not the stored
    // target yet); after the two healths are swapped the stored target keeps scoring 1 and wins while it is the less hurt one.  A reset drops the variable, so the
    // next Mend takes the 50 % ally (the more hurt one).
    const std::string pick = "pick(perceived,product,g,c4;bb:MendTarget,friendly;d;2500;lt;and;h;0.9;lt;and,bb:MendTarget;1;1;h;-;if)";
    const std::vector<std::string> rows = {
        pick_tests::row_with_pick(path, mend->row.guid, "Mend", 2, 12, pick, 1300, ""),
        self_effects::row(path, swarm->row.guid, "Swarm", 18, 1, 18, -1, 1500, 35, "Swarm", 0, 1, "c0:fc(ge,8)", "", "", "", 1300),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Mender's table for the reset reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 600);
    fs::remove_all(tmp, ec);
    PICK_CHECKF(field.ok(), "the Mender's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t mender = field.spawn(path, 0, 0, 4000);
    PICK_CHECK(mender != 0, "the Mender is put in the world");
    if (mender == 0) return;
    const auto bp = f.motion.find(path);
    if (!field.made.open || !bp) {
        PICK_CHECK(false, "the world makes an open for the Mender");
        return;
    }
    const uint64_t first = field.w.spawn(path, 500, 0, 7, 0, bp->half_height, field.made.open, &*bp);       // 50 %, then 60 %
    const uint64_t second = field.w.spawn(path, -500, 100, 7, 0, bp->half_height, field.made.open, &*bp);   // 60 %, then 50 %
    PICK_CHECK(first != 0 && second != 0, "the two allies are put in the world");
    if (first == 0 || second == 0) return;
    const double dt = 1.0 / 15.0;
    const double swap_at = 3.0;
    double reset_at = -1;
    bool pre_hurt_only = true, pre_stored_healed = false, post_reset_healed = false;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.world.add_threat(mender, field.player.actor_id(), 100.0, now);
        field.set_health_fraction(first, now < swap_at ? 0.5 : 0.6);    // the first pick's winner at 50 %, then the less hurt one
        field.set_health_fraction(second, now < swap_at ? 0.6 : 0.5);   // ... the other at 60 %, then the more hurt one
        field.tick(now);
        if (reset_at < 0) {
            if (field.world.effect_stacks_from(first, heal, mender) > 0) pre_stored_healed = true;
            if (field.world.effect_stacks_from(second, heal, mender) > 0) pre_hurt_only = false;
        } else if (field.world.effect_stacks_from(second, heal, mender) > 0) {
            post_reset_healed = true;
        }
        if (reset_at < 0 && now >= 4.0) {
            // the second Mend (at about 3 s) went to the stored target although it was then the less hurt one: the variable was read
            reset_at = now;
            PICK_CHECKF(field.world.effect_stacks_from(first, heal, mender) > 0 && pre_stored_healed, "before the reset the stored ally is healed although the other is more hurt (%d and %d stacks)",
                        field.world.effect_stacks_from(first, heal, mender), field.world.effect_stacks_from(second, heal, mender));
            PICK_CHECKF(pre_hurt_only, "the more hurt ally is not healed before the reset (%d stacks)", field.world.effect_stacks_from(second, heal, mender));
            field.world.gm_reset(mender, now);
        }
    }
    std::printf("  casts: %s; reset at %.2f s; the heal effect from the healer: the stored ally %d, the more hurt ally %d\n", field.log.text(*f.catalog).c_str(), reset_at,
                field.world.effect_stacks_from(first, heal, mender), field.world.effect_stacks_from(second, heal, mender));
    PICK_CHECKF(field.log.count(mend->row.guid) >= 2, "the Mender mends twice before the reset (%zu Mends)", field.log.count(mend->row.guid));
    PICK_CHECKF(reset_at > 0, "the Mender is reset between two fights (at %.2f s)", reset_at);
    PICK_CHECKF(reset_at > 0 && field.log.count(mend->row.guid) >= 3, "it fights again after the reset and mends once more (%zu Mends)", field.log.count(mend->row.guid));
    PICK_CHECKF(post_reset_healed, "after the reset the Mend takes the more hurt ally (%d stacks)", field.world.effect_stacks_from(second, heal, mender));
}

AOC_REGISTER_TEST(test_a_reset_drops_the_stored_choice);




