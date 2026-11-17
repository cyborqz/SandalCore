// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_self_effects_tests.inc
//
//  Registered tests of test_worldd_combat: the effects a creature carries, and the tree's "carries effect E" condition that asks about them.  A creature's own effects are the
//  ones its abilities put on it (a hit's `self` row included), the ones the ability itself lists (the Skeletons' Block), and the ones its Blueprint gives it from spawn; a
//  condition can ask about the creature itself or about its primary target.  A reset keeps the effects its Blueprint gave it and takes the fight's off.  An effect another
//  creature's alert (the goblins' Rally) or a player's hit puts on it is in its effect book, and a condition on an effect nothing can ever put on it holds its row.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// This file's checks print their own passing line too, so the proof of the eight angles can quote the [ok ] line of a check as well as its [FAIL] line.
// (One printf for the whole line: the world's log writes to the same console between two calls.)
#define SELF_CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define SELF_CHECKF(cond, ...) do { \
    char self_line[1024]; \
    std::snprintf(self_line, sizeof self_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", self_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", self_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace self_effects {

/// A cast of one creature's tree, as the client saw it begin.
struct Cast {
    double  t = 0;
    int64_t ability = 0;
};

/// The casts of one creature while the test drives the world itself.
struct CastLog {
    std::vector<Cast> casts;
    GremCastView      last;

    void look(world::ActorRegistry& registry, uint64_t creature, double now) {
        const GremCastView view = grem_cast_view(registry, creature);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) casts.push_back({now, view.ability});
        last = view;
    }
    size_t count(int64_t ability) const {
        size_t n = 0;
        for (const Cast& c : casts) n += c.ability == ability ? 1 : 0;
        return n;
    }
    double first(int64_t ability) const {
        for (const Cast& c : casts) if (c.ability == ability) return c.t;
        return -1;
    }
    /// The first cast of `ability` after `when`; -1 when it never comes.
    double first_after(int64_t ability, double when) const {
        for (const Cast& c : casts) if (c.ability == ability && c.t > when) return c.t;
        return -1;
    }
    /// How many casts of `ability` began within (from, to].
    size_t between(int64_t ability, double from, double to) const {
        size_t n = 0;
        for (const Cast& c : casts) n += c.ability == ability && c.t > from && c.t <= to ? 1 : 0;
        return n;
    }
    std::string text(const aoc::worldd::abilities::Catalog& catalog) const {
        std::string line;
        for (const Cast& c : casts) {
            const aoc::worldd::abilities::AbilityDef* d = catalog.ability(c.ability);
            line += (line.empty() ? "" : ", ") + std::string(d ? d->row.name : "?") + " " + std::to_string(c.t).substr(0, 5);
        }
        return line;
    }
};

/// The header of the creature tables, as tools/npcabilities writes it.
constexpr const char* kHeader = "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";

/// A row of the creature tables: the 17 columns named above, in that order.
std::string row(const std::string& path, int64_t ability, const std::string& task, double cooldown, int stop, int order, double min_distance, double max_distance, double max_angle,
                const std::string& gates, int unknown, int fight, const std::string& conds, const std::string& why, const std::string& starts, const std::string& rotate, double arrival) {
    const auto f = [](double v) { return v == static_cast<int>(v) ? std::to_string(static_cast<long long>(v)) : std::to_string(v); };
    return path + "\t" + std::to_string(ability) + "\t" + task + "\t" + f(cooldown) + "\t" + std::to_string(stop) + "\t" + std::to_string(order) + "\t" + f(min_distance) + "\t" + f(max_distance) + "\t" +
           f(max_angle) + "\t" + gates + "\t" + std::to_string(unknown) + "\t" + std::to_string(fight) + "\t" + conds + "\t" + why + "\t" + starts + "\t" + rotate + "\t" + f(arrival);
}

/// Writes a table for one Blueprint and loads it with the real montages and targeting.  `what` names the table in the check, so its passing line and its failing line say the same thing
/// (the error of a table that does not read is printed under it).
bool load_self_table(const std::string& table, const std::vector<std::string>& rows, const std::string& what, combat::NpcAbilityData& out) {
    std::ofstream o(table, std::ios::binary);
    o << kHeader << "\n";
    for (const std::string& r : rows) o << r << "\n";
    o.close();
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string err;
    const bool loaded = out.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    if (!loaded) std::printf("    (%s)\n", err.c_str());
    SELF_CHECK(loaded, what.c_str());
    return loaded;
}

/// One fight the test drives itself: the real creature table (or the table a test wrote), a player, the creature and the casts as the client sees them.
struct Field {
    Fixture&                       f;
    combat::NpcAbilityData         tables;
    bool                           loaded = false;
    Rig                            rig;
    combat_rig::World              w;
    combat::NpcStatBook            book;
    combat::CombatWorld            world;
    combat_rig::FakePlayer         player;
    BanditOpen                     made;
    bool                           opened = false;
    uint64_t                       creature = 0;
    CastLog                        log;

    explicit Field(Fixture& fixture, const std::string& table = "", double px = 0, double py = 700)
        : f(fixture), rig(4, 20, {}), book(*fixture.ctx->bundle, *fixture.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *fixture.data, *fixture.catalog, book, fixture.motion, config()), player(rig, 77, {px, py, 1098}) {
        std::string dir = NPC_MOTION_PATH;
        dir.erase(dir.find_last_of("/\\") + 1);
        std::string err;
        loaded = table.empty() ? load_creature_tables(tables) : tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
        world.set_ability_data(&tables);
        world.add_player(&player);
    }
    static combat::CombatWorldConfig config() {
        combat::CombatWorldConfig c;
        c.seed = 5;
        return c;
    }
    bool ok() const { return loaded && rig.ok() && w.problem.empty(); }
    /// Puts the creature of `path` at (x, y) with its own open (the table's abilities in its list of records) and returns its actor id.
    uint64_t spawn(const std::string& path, double x, double y, double aggro = 900) {
        const auto bp = f.motion.find(path);
        if (!bp || !loaded) return 0;
        if (!made.open && !make_bandit_open(f, tables, path, made)) return 0;
        opened = made.open != nullptr;
        creature = w.spawn(path, x, y, 7, aggro, bp->half_height, made.open, &*bp);
        return creature;
    }
    void tick(double now) {
        if (const stats::StatValue* max = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max->value);   // (the fight is about the mobs, not the player's life)
        const combat::Vec3 at = player.rig_fighter().position();
        w.npcs->note_player(77, at.x, at.y, now);
        w.npcs->tick(now);
        world.tick(now);
        log.look(w.registry, creature, now);
    }
    stats::CharacterStats& stats_of(uint64_t actor) {
        combat::Fighter* fighter = world.fighter_of(actor);
        return fighter->stats();
    }
    void set_health_fraction(uint64_t actor, double fraction) {
        combat::Fighter* fighter = world.fighter_of(actor);
        if (!fighter) return;
        const stats::StatValue* max = fighter->stats().find(content::kGuidMaxHealth);
        if (max) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(max->value * fraction));
    }
    double health(uint64_t actor) {
        combat::Fighter* fighter = world.fighter_of(actor);
        const stats::StatValue* h = fighter ? fighter->stats().find(content::kGuidHealth) : nullptr;
        return h ? h->value : -1;
    }
    int stacks(uint64_t actor, int64_t effect) const { return world.effect_stacks(actor, effect); }
    bool holds(uint64_t actor, int64_t effect, double now) const { return world.holds_effect(actor, effect, now); }
    bool shows(uint64_t actor, int64_t effect) const {
        const world::WorldActor* a = w.registry.find(actor);
        if (!a) return false;
        for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
            if (it.item.effect_guid == effect) return true;
        }
        return false;
    }
};

}  // namespace self_effects

// R1: a `self` row of a creature's own hit (the Withered Savage's Bloodcraze: ab_hit_effect.kind = "self") now reaches its own effect set, so its tree's carries-effect
// conditions can see it; the effect is not the player's path and the effect book does not get it.  R7: the reset of the fight takes that effect off.  The tree is a fixture of
// the real Bloodcraze ability and a real attack (its own tree row is a fixture of the real ability: the test is about where the self-kind effect goes).
void test_a_self_kind_effect_lands_on_the_creature(Fixture& f) {
    std::printf("the Withered Savage's Bloodcraze (a self-kind effect row of its own hit) is in the creature's own set while it lasts, and a reset takes the fight's effect off\n");
    namespace fs = std::filesystem;
    const std::string path = creature_path(f, "/Humanoid/Withered/Savage/Withered_Savage.Default__Withered_Savage_C");
    const abilities::AbilityDef* aura = f.catalog->ability_named("Withered_Savage_Bloodcraze");
    const abilities::AbilityDef* attack = f.catalog->ability_named("Withered_Savage_ClawSwipes");
    const int64_t buff = 6064633612170756423;   // Withered_Savage_Bloodcraze_Buff
    SELF_CHECK(!path.empty() && f.motion.find(path) && aura && attack && f.catalog->effect(buff), "the Blueprint table has the Withered Savage and the ability data has its Bloodcraze, an attack and the Bloodcraze effect");
    if (path.empty() || !f.motion.find(path) || !aura || !attack || !f.catalog->effect(buff)) return;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_self_effects_aura_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::vector<std::string> rows = {
        self_effects::row(path, aura->row.guid, "", 0, 1, 10, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(path, attack->row.guid, "", 0, 0, 20, -1, 350, 90, "", 0, 1, "", "", "", "360:0", -1),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Bloodcraze's table reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string());
    fs::remove_all(tmp, ec);
    SELF_CHECKF(field.ok(), "the fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t tank = field.spawn(path, 0, 0);
    SELF_CHECK(tank != 0, "the Withered Savage is put in the world");
    if (tank == 0) return;
    const double dt = 1.0 / 15.0;
    double landed_at = -1, first_cast = -1;
    for (double now = dt; now <= 30.0 + 1e-9; now += dt) {
        field.tick(now);
        if (first_cast < 0 && field.log.count(aura->row.guid) > 0) first_cast = field.log.first(aura->row.guid);
        if (landed_at < 0 && field.holds(tank, buff, now)) landed_at = now;
    }
    std::printf("  casts: %s; the first Bloodcraze at %.2f s; the effect first held at %.2f s\n", field.log.text(*f.catalog).c_str(), first_cast, landed_at);
    SELF_CHECKF(first_cast >= 0, "the savage rages in a 30 s fight (first at %.2f s)", first_cast);
    SELF_CHECKF(landed_at >= 0, "its self-kind effect is in its own set after the hit (first held at %.2f s)", landed_at);
    SELF_CHECKF(landed_at >= 0 && landed_at - first_cast <= 5.0, "the effect lands with the cast's hit, not later (%+.2f s from the cast)", first_cast < 0 ? -1.0 : landed_at - first_cast);
    SELF_CHECKF(field.world.effect_stacks(tank, buff) == 0, "the self-kind row does not go into the creature's effect book (a player's self effects are untouched)");
    // R7: the fight's own effect goes with a reset; the Blueprint gave this creature none from spawn
    if (landed_at >= 0) {
        field.world.gm_reset(tank, landed_at + 1.0);
        SELF_CHECKF(!field.holds(tank, buff, landed_at + 1.0), "a reset takes the fight's own effect off (at %.2f s it is not held)", landed_at + 1.0);
    }
}
AOC_REGISTER_TEST(test_a_self_kind_effect_lands_on_the_creature);

// R5: the Skeleton Knight's Block is an empty hit the tree plays as a state: its own listed effect (SkeletonWarrior_Block_Effect, with its TempHealth shield) now goes on the
// Knight.  The tree is a fixture of the real Block ability behind its real condition (eff of the shield marker the Knight's open gives it from spawn, seeded here) and a real
// attack; the test player stands 12 m away.
void test_the_skeleton_knights_block_plays_and_puts_its_effect_on_it(Fixture& f) {
    std::printf("the Skeleton Knight's Block plays as the state its data says and puts its own effect and shield on the Knight\n");
    namespace fs = std::filesystem;
    const std::string path = creature_path(f, "/Monster/Undead/Skeleton/Skeleton_Knight.Default__Skeleton_Knight_C");
    const abilities::AbilityDef* block = f.catalog->ability_named("SkeletonWarrior_Block");
    const abilities::AbilityDef* attack = f.catalog->ability_named("SkeletonWarrior_AxeSwipe");
    const int64_t effect = 6064629343020319027;   // SkeletonWarrior_Block_Effect
    SELF_CHECK(!path.empty() && f.motion.find(path) && block && attack && f.catalog->effect(effect), "the Blueprint table has the Skeleton Knight and the ability data has its Block, an attack and the Block's effect");
    if (path.empty() || !f.motion.find(path) || !block || !attack || !f.catalog->effect(effect)) return;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_self_effects_block_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::vector<std::string> rows = {
        self_effects::row(path, block->row.guid, "", 0, 1, 10, -1, -1, -1, "", 0, 1, "c0:eff(6064629337196200799)", "", "", "", -1),
        self_effects::row(path, attack->row.guid, "", 0, 0, 20, -1, 350, 90, "", 0, 1, "", "", "", "360:0", -1),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Block's table reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 1200);
    fs::remove_all(tmp, ec);
    SELF_CHECKF(field.ok(), "the fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t knight = field.spawn(path, 0, 0);
    for (const int64_t starting : {6064629309380689954, 6064629303388603339, 6064630559943164163, 6064629337196200799}) field.world.seed_own_effect(knight, starting);
    SELF_CHECK(knight != 0, "the Skeleton Knight is put in the world");
    if (knight == 0) return;
    SELF_CHECKF(field.holds(knight, 6064629337196200799, 0.0), "the Knight carries the shield marker the Block waits for (the starting-effects fixture)");
    SELF_CHECKF(combat::is_state_move(*block, *f.catalog, f.data->ids().health), "the Block's ability is a state move (an empty hit and its own effects)");
    const double dt = 1.0 / 15.0;
    double first_block = -1, held_at = -1;
    double max_shield = 0;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.world.add_threat(knight, field.player.actor_id(), 100.0, now);
        field.tick(now);
        if (first_block < 0 && field.log.count(block->row.guid) > 0) first_block = field.log.first(block->row.guid);
        if (held_at < 0 && field.holds(knight, effect, now)) held_at = now;
        max_shield = std::max(max_shield, field.world.shield_of(knight, now));
    }
    std::printf("  casts: %s; the Block's state first held at %.2f s; the largest shield %.1f\n", field.log.text(*f.catalog).c_str(), held_at, max_shield);
    SELF_CHECKF(first_block >= 0, "the Knight blocks in a 20 s fight (first at %.2f s)", first_block);
    SELF_CHECKF(held_at >= 0, "the Block's own effect is on the Knight after the cast (first held at %.2f s)", held_at);
    SELF_CHECKF(max_shield > 0, "the effect's TempHealth is a shield on the Knight (largest %.1f)", max_shield);
}

AOC_REGISTER_TEST(test_the_skeleton_knights_block_plays_and_puts_its_effect_on_it);

// R7: the Skeleton Knight's four starting effects (the Black Lung tracker, the Black Lung, the Spirit of the Undying and the shield marker), which the records mark infinite
// and persisting through death, stay on the Knight through a reset; the Block the shield gates goes on playing after it.  Two Knights side by side each carry them; a Knight
// made again from the same Blueprint (a respawn) starts with its own.  The tree is the Block fixture (the real ability and condition) and a real attack.
void test_a_creature_starts_with_the_effects_its_blueprint_carried_and_a_reset_keeps_them(Fixture& f) {
    std::printf("two Skeleton Knights carry their Blueprint's starting effects; the one that resets keeps them and goes on playing the Block they gate, the other does too, and a Knight made again starts with them\n");
    namespace fs = std::filesystem;
    const std::string path = creature_path(f, "/Monster/Undead/Skeleton/Skeleton_Knight.Default__Skeleton_Knight_C");
    const abilities::AbilityDef* block = f.catalog->ability_named("SkeletonWarrior_Block");
    const abilities::AbilityDef* attack = f.catalog->ability_named("SkeletonWarrior_AxeSwipe");
    const int64_t tracker = 6064629303388603339;   // Skeleton_BlackLung_Tracker, one of the four the Knight's open carries
    SELF_CHECK(!path.empty() && f.motion.find(path) && block && attack, "the Blueprint table has the Skeleton Knight and the ability data has its Block and an attack");
    if (path.empty() || !f.motion.find(path) || !block || !attack) return;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_self_effects_start_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::vector<std::string> rows = {
        self_effects::row(path, block->row.guid, "", 0, 1, 10, -1, -1, -1, "", 0, 1, "c0:eff(6064629337196200799)", "", "", "", -1),
        self_effects::row(path, attack->row.guid, "", 0, 0, 20, -1, 350, 90, "", 0, 1, "", "", "", "360:0", -1),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the starting-effects table reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string(), 0, 1200);
    fs::remove_all(tmp, ec);
    SELF_CHECKF(field.ok(), "the fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const std::vector<int64_t> starting = {6064629309380689954, 6064629303388603339, 6064630559943164163, 6064629337196200799};   // the Knight's captured open (the author's probe)
    const uint64_t a = field.spawn(path, -120, 0);
    for (const int64_t effect : starting) field.world.seed_own_effect(a, effect);
    const uint64_t b = field.spawn(path, 120, 0);
    for (const int64_t effect : starting) field.world.seed_own_effect(b, effect);
    SELF_CHECK(a != 0 && b != 0, "the two Knights are put in the world");
    if (a == 0 || b == 0) return;
    SELF_CHECKF(field.holds(a, 6064629337196200799, 0.0) && field.holds(b, 6064629337196200799, 0.0), "both Knights carry the shield marker of the starting-effects fixture");
    self_effects::CastLog log_a, log_b, log_c;
    const double dt = 1.0 / 15.0, reset_at = 6.0, new_at = 12.0;
    bool reset = false;
    uint64_t c = 0;
    for (double now = dt; now <= 22.0 + 1e-9; now += dt) {
        field.world.add_threat(a, field.player.actor_id(), 100.0, now);
        field.world.add_threat(b, field.player.actor_id(), 100.0, now);
        if (!reset && now >= reset_at) reset = field.world.gm_reset(b, now) != combat::CombatWorld::GmReset::Dead;
        if (c == 0 && now >= new_at) {
            const auto cbp = f.motion.find(path);
            c = cbp ? field.w.spawn(path, 0, -150, 7, 900, cbp->half_height, field.made.open, &*cbp) : 0;
            for (const int64_t effect : starting) field.world.seed_own_effect(c, effect);
        }
        if (c != 0) field.world.add_threat(c, field.player.actor_id(), 100.0, now);
        field.tick(now);
        log_a.look(field.w.registry, a, now);
        log_b.look(field.w.registry, b, now);
        if (c != 0) log_c.look(field.w.registry, c, now);
    }
    std::printf("  the Knight that stays: %s\n  the Knight that resets: %s\n  the Knight made again at %.0f s: %s\n", log_a.text(*f.catalog).c_str(), log_b.text(*f.catalog).c_str(), new_at,
                log_c.text(*f.catalog).c_str());
    SELF_CHECK(reset, "the second Knight's reset runs");
    SELF_CHECKF(field.holds(b, tracker, reset_at + 1.0), "the reset Knight still carries the tracker its Blueprint gave it from spawn");
    SELF_CHECKF(log_a.count(block->row.guid) >= 1, "the Knight that stays plays the move its spawn shield gates (%zu Blocks)", log_a.count(block->row.guid));
    SELF_CHECKF(log_b.count(block->row.guid) >= 1, "the Knight that resets plays it before its reset (%zu Blocks)", log_b.count(block->row.guid));
    SELF_CHECKF(log_b.first_after(block->row.guid, reset_at + 0.5) > 0, "and still after: the reset kept its starting effects (first Block after the reset at %.2f s)", log_b.first_after(block->row.guid, reset_at + 0.5));
    SELF_CHECKF(c != 0 && log_c.count(block->row.guid) >= 1, "a Knight made again from the same Blueprint starts with its effects again and plays the move (%zu Blocks)", log_c.count(block->row.guid));
}

AOC_REGISTER_TEST(test_a_creature_starts_with_the_effects_its_blueprint_carried_and_a_reset_keeps_them);

// R2: the Raptor's Smell of Blood waits for its primary target to bleed.  The row is built from the real table's own Smell of Blood row (its task, cooldown and order), with the
// condition the reader writes once it follows the Raptor's own target picker: c0:teff(Status_Bleeding).  The real row is checked too: it is either that row or held with its class.
// Two players stand in the fight: only the one the Raptor hates is its primary target, so another player's bleeding is not what the condition asks about.
void test_the_raptor_smells_blood_only_on_a_bleeding_target(Fixture& f) {
    std::printf("the Raptor's Smell of Blood waits for its primary target's bleeding: a second player's bleed is not enough, and the move plays once the target has it\n");
    namespace fs = std::filesystem;
    const std::string path = creature_path(f, "/Beast/Raptor/Raptor_Tooth.Default__Raptor_Tooth_C");
    const abilities::AbilityDef* smell = f.catalog->ability_named("Raptor_SmellofBlood");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Raptor_Bite");
    const std::string bleed_guid = "6064629761775576467";   // Status_Bleeding, the game's bleeding record
    SELF_CHECK(!path.empty() && f.motion.find(path) && smell && bite && f.catalog->effect_named("Status_Bleeding"), "the Blueprint table has the Raptor and the ability data has its Smell of Blood and Bite and the bleeding record");
    if (path.empty() || !f.motion.find(path) || !smell || !bite) return;
    const auto bp = f.motion.find(path);
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    // the real table's Smell row, with only its condition replaced by the row the reader writes for the Raptor's own picker
    std::string error;
    const std::string table = sequences::table_with_next(dir, path,
                                                         {{smell->row.guid, "", {{10, "0"}, {12, "c0:teff(" + bleed_guid + ")"}, {13, ""}}},
                                                          {bite->row.guid, "", {{10, "0"}, {12, "c0:in(250)"}, {13, ""}}}},
                                                         error, sequences::NextColumn::Keep);
    SELF_CHECKF(!table.empty(), "the Raptor's tree with the real Smell row and the reader's target condition is written (%s)", error.c_str());
    if (table.empty()) return;
    // the real row: unknown with the class, or known with the primary-target condition (the lead's extractor writes the latter)
    {
        combat::NpcAbilityData real;
        if (load_creature_tables(real)) {
            bool ok = false;
            if (const std::vector<combat::CreatureAbility>* tasks = real.abilities_of(path)) {
                for (const combat::CreatureAbility& t : *tasks) {
                    if (t.ability != smell->row.guid) continue;
                    if (t.unknown) ok = t.why.find("STC_HasEffect_C") != std::string::npos;
                    else {
                        for (const combat::CondGroup& g : t.groups) {
                            for (const combat::CondToken& token : g) {
                                if (token.atom.kind == combat::CondAtom::Kind::TargetHasEffect && token.atom.effect == std::strtoll(bleed_guid.c_str(), nullptr, 10)) ok = true;
                            }
                        }
                    }
                }
            }
            SELF_CHECK(ok, "the build's table has the Raptor's Smell of Blood as the reader's teff row, or holds it with the condition class it cannot say");
        }
    }
    self_effects::Field field(f, table);
    SELF_CHECKF(field.ok(), "the Raptor's fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t raptor = field.spawn(path, 0, 0);
    SELF_CHECK(raptor != 0, "the Raptor is put in the world");
    if (raptor == 0) return;
    Rig rig_b(4, 20, {});
    if (!rig_b.ok()) {
        SELF_CHECK(false, "the second player's rig is made");
        return;
    }
    combat_rig::FakePlayer other(rig_b, 78, {0, -800, 1098});
    field.world.add_player(&other);
    const int64_t bleed = std::strtoll(bleed_guid.c_str(), nullptr, 10);
    rig_b.pipe->grant_effect(bleed, 1, 0.0);   // the player who is not the Raptor's target bleeds from the start
    const double dt = 1.0 / 15.0, bleed_at = 12.0;
    bool given = false;
    for (double now = dt; now <= 26.0; now += dt) {
        field.world.add_threat(raptor, field.player.actor_id(), 100.0, now);
        if (!given && now >= bleed_at) given = field.rig.pipe->grant_effect(bleed, 1, now);
        field.player.rig_fighter().move_to({0, 800, 1098});
        other.rig_fighter().move_to({0, -800, 1098});
        field.tick(now);
    }
    std::printf("  casts: %s; the hated player's bleed was granted at %.1f s (%s); the other player's bleed: %d stack(s)\n", field.log.text(*f.catalog).c_str(), bleed_at, given ? "yes" : "no",
                rig_b.pipe->stacks_of(bleed));
    SELF_CHECK(given, "the bleed is put on the hated player in the fight");
    SELF_CHECKF(field.log.between(smell->row.guid, 0.0, bleed_at) == 0, "the Smell of Blood is not played while only the other player bleeds (%zu casts before %.1f s)",
                field.log.between(smell->row.guid, 0.0, bleed_at), bleed_at);
    SELF_CHECKF(field.log.first_after(smell->row.guid, bleed_at) > 0, "it is played once the primary target bleeds (first at %.2f s)", field.log.first_after(smell->row.guid, bleed_at));
    SELF_CHECKF(field.log.count(bite->row.guid) >= 3, "the Raptor goes on biting (%zu bites)", field.log.count(bite->row.guid));
}

AOC_REGISTER_TEST(test_the_raptor_smells_blood_only_on_a_bleeding_target);


// A Battle Priest says its Daze prayer (a move that puts an effect on the creature itself) and the effect opens its Radiant Strike: the real table's Radiant Strike and
// Light Spear carry eff(Radiant), its Shield Bash carries neff(Radiant) and waits 7 s into the fight.  The Radiant Strike is never played before the prayer's effect lands,
// and the Shield Bash is never played while it is on, then is played once it has run out (30 s).  The tree is the real one.
void test_a_prayer_puts_its_effect_on_the_priest_and_opens_its_gated_strike(Fixture& f) {
    std::printf("a Battle Priest's Daze prayer puts Radiance on the priest, the Radiant Strike that waits for it is played only after it lands, and the Shield Bash that waits for it to go is never played while it is on\n");
    const std::string path = creature_path(f, "/Humanoid/Minotaur/BattlePriest/Minotaur_BattlePriest.Default__Minotaur_BattlePriest_C");
    const abilities::AbilityDef* prayer = f.catalog->ability_named("Minotaur_BattlePriest_BattlePrayer_Daze");
    const abilities::AbilityDef* radiant = f.catalog->ability_named("Minotaur_BattlePriest_SpearStrike_Radiant");
    const abilities::AbilityDef* bash = f.catalog->ability_named("Minotaur_BattlePriest_ShieldBash");
    SELF_CHECK(!path.empty() && f.motion.find(path) && prayer && radiant && bash, "the Blueprint table has the Battle Priest and the ability data has its Daze prayer, Radiant Strike and Shield Bash");
    if (path.empty() || !f.motion.find(path) || !prayer || !radiant || !bash) return;
    self_effects::Field field(f, "", 0, 1200);   // (the Shield Bash's own range starts at 9 m)
    SELF_CHECKF(field.ok(), "the fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t priest = field.spawn(path, 0, 0, 1500);   // (the Shield Bash's own range starts at 9 m)
    SELF_CHECK(priest != 0, "the Battle Priest is put in the world");
    if (priest == 0) return;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 48.0 + 1e-9; now += dt) field.tick(now);
    std::printf("  casts: %s\n", field.log.text(*f.catalog).c_str());
    const double prayer_at = field.log.first(prayer->row.guid);
    SELF_CHECKF(prayer_at >= 0 && prayer_at < 20.0, "the priest says its Daze prayer in the fight (at %.2f s)", prayer_at);
    if (prayer_at < 0) return;
    size_t before_land = 0, while_on = 0;
    bool first_radiant_before = false;
    for (const self_effects::Cast& c : field.log.casts) {
        if (c.ability == radiant->row.guid) {
            if (c.t < prayer_at + 0.643) first_radiant_before = true;
            before_land += c.t < prayer_at + 0.643 ? 1 : 0;
        }
        if (c.ability == bash->row.guid) {
            if (c.t >= prayer_at + 0.643 && c.t < prayer_at + 0.643 + 30.0) ++while_on;
        }
    }
    SELF_CHECKF(!first_radiant_before, "the gated strike is never played before the prayer's effect has landed (%zu casts before %.2f s)", before_land, prayer_at + 0.643);
    SELF_CHECKF(field.log.first_after(radiant->row.guid, prayer_at) > 0, "it is played once the effect is on the priest (first at %.2f s)", field.log.first_after(radiant->row.guid, prayer_at));
    SELF_CHECKF(while_on == 0, "the has-not move is never played while the effect is on the priest (%zu casts in its 30 s)", while_on);
    // the has-not row is the real ShieldBash: its own condition asks for the prayer's effect to be absent
    bool real_has_not = false;
    if (const std::vector<combat::CreatureAbility>* tasks = field.tables.abilities_of(path)) {
        for (const combat::CreatureAbility& t : *tasks) {
            if (t.ability != bash->row.guid) continue;
            for (const combat::CondGroup& g : t.groups) {
                for (const combat::CondToken& token : g) {
                    if (token.atom.kind == combat::CondAtom::Kind::NotHasEffect && token.atom.effect == 6064629461641003395) real_has_not = true;
                }
            }
        }
    }
    SELF_CHECK(real_has_not, "the table's Shield Bash waits for neff(Minotaur_BattlePriest_Radiant) (the real row, not a made-up one)");
}

AOC_REGISTER_TEST(test_a_prayer_puts_its_effect_on_the_priest_and_opens_its_gated_strike);

// R8 (the first half): an effect another creature's alert puts on a mob (the goblins' Rally) reaches the tree's carries-effect check, and so does a player's effect on the mob.
// The ally's own table is a fixture of two attacks: one gated on the Rally's effect, one plain; it is not the game's tree (the game's Rally effect is checked by no goblin row).
void test_effects_a_creature_ally_and_a_player_put_on_the_mob_reach_the_check(Fixture& f) {
    std::printf("a goblin's Rally effect on an ally, and a player's bleeding on a mob, are both held in the mob's book and answer its eff condition\n");
    namespace fs = std::filesystem;
    const abilities::AbilityDef* rally = f.catalog->ability_named("Goblin_Rally");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::AbilityDef* gated = f.catalog->ability_named("SkeletonWarrior_AxeSwipe");
    const abilities::EffectDef* effect = f.catalog->effect_named("Goblin_Rally_Effect");
    const int64_t bleed = 6064629761775576467;   // Status_Bleeding
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/Goblin.Default__Goblin_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    SELF_CHECK(!caster_path.empty() && !ally_path.empty() && rally && claw && gated && effect && f.catalog->effect(bleed),
               "the Blueprint table has the goblins, and the ability data has the Rally, its Claw, the gated axe swipe, its effect and the bleeding record");
    if (caster_path.empty() || ally_path.empty() || !rally || !claw || !gated || !effect) return;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_self_effects_book_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    combat::NpcAbilityData tables;
    const std::vector<std::string> rows = {
        self_effects::row(caster_path, rally->row.guid, "", 0, 1, 10, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(caster_path, claw->row.guid, "", 0, 0, 20, -1, 350, 90, "", 0, 1, "", "", "", "360:0", -1),
        self_effects::row(ally_path, gated->row.guid, "", 5, 0, 10, -1, 350, 90, "", 0, 1, "c0:eff(" + std::to_string(effect->row.guid) + ")", "", "", "360:0", -1),
        self_effects::row(ally_path, claw->row.guid, "", 0, 0, 20, -1, 350, 90, "", 0, 1, "", "", "", "360:0", -1),
    };
    const bool loaded = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the Rally book's two-Blueprint table reads", tables);
    if (!loaded) {
        fs::remove_all(tmp, ec);
        return;
    }
    self_effects::Field field(f, (tmp / "abilities.tsv").string());
    fs::remove_all(tmp, ec);
    SELF_CHECK(field.ok(), "the Rally book's fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500);
    SELF_CHECK(caster != 0, "the Rally's caster is put in the world");
    if (caster == 0) return;
    BanditOpen ally_open;
    SELF_CHECKF(make_bandit_open(f, tables, ally_path, ally_open), "the world makes the ally's open with its gated move (verdict: %s)", spawns::verdict_name(ally_open.why));
    if (!ally_open.open) return;
    const auto abp = f.motion.find(ally_path);
    if (!abp) return;
    const uint64_t ally = field.w.spawn(ally_path, 250, 500, 7, 900, abp->half_height, ally_open.open, &*abp);
    SELF_CHECK(ally != 0, "the ally is put in the world");
    if (ally == 0) return;
    self_effects::CastLog ally_log;
    const double dt = 1.0 / 15.0;
    double rally_at = -1;
    for (double now = dt; now <= 12.0 + 1e-9; now += dt) {
        field.tick(now);
        ally_log.look(field.w.registry, ally, now);
        if (rally_at < 0 && field.log.count(rally->row.guid) > 0) rally_at = field.log.first(rally->row.guid);
    }
    std::printf("  the caster: %s; the ally: %s; the first Rally at %.2f s\n", field.log.text(*f.catalog).c_str(), ally_log.text(*f.catalog).c_str(), rally_at);
    SELF_CHECKF(rally_at >= 0, "the goblin rallies in a 12 s fight (first at %.2f s)", rally_at);
    SELF_CHECKF(rally_at >= 0 && ally_log.between(gated->row.guid, 0.0, rally_at + 0.5) == 0,
                "the ally's move that waits for the Rally's effect is not played before the effect lands (%zu casts)", ally_log.between(gated->row.guid, 0.0, rally_at + 0.5));
    SELF_CHECKF(ally_log.first_after(gated->row.guid, rally_at + 0.5) > 0, "it is played once the book holds the Rally's effect (first at %.2f s)", ally_log.first_after(gated->row.guid, rally_at + 0.5));
    // the player's effect on a mob: the book answers the mob's own carries-effect condition too
    self_effects::Field field2(f);
    if (!field2.ok()) {
        SELF_CHECK(false, "the player's effect's fight's world is made");
        return;
    }
    const uint64_t mob = field2.spawn(caster_path, 0, 0);
    SELF_CHECK(mob != 0, "the mob that takes the player's effect is put in the world");
    if (mob == 0) return;
    effects::Applied applied;
    applied.def = f.catalog->effect(bleed);
    applied.stacks = 1;
    applied.duration = 20.0;
    field2.world.apply_effects(mob, 77, {applied}, 1.0);
    SELF_CHECKF(field2.world.effect_stacks(mob, bleed) > 0, "the player's bleeding is on the mob's book (%d stack(s))", field2.world.effect_stacks(mob, bleed));
    SELF_CHECKF(field2.holds(mob, bleed, 1.0), "the mob's carries-effect check reads the player's effect on it");
}

AOC_REGISTER_TEST(test_effects_a_creature_ally_and_a_player_put_on_the_mob_reach_the_check);

// R8 (the second half): an effect with a record that no source in the world can ever put on a creature holds its row: the Monkey's Back Flip and Versatile Throw wait for
// neff(Monkey_Branch), whose only applier is the BranchGrab the tree switches off, so those rows are not played; the Monkey's plain Claw Strike still is.
void test_an_effect_no_source_can_give_holds_its_row(Fixture& f) {
    std::printf("a row that waits for a carries-effect condition no source can ever satisfy is held (not played), while the rest of the creature's tree plays\n");
    const std::string path = creature_path(f, "/Humanoid/Monkey/Monkey.Default__Monkey_C");
    const abilities::AbilityDef* flip = f.catalog->ability_named("Monkey_BackFlip");
    const abilities::AbilityDef* toss = f.catalog->ability_named("Monkey_VersatileThrow");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Monkey_ClawStrike");
    const int64_t branch = 6064634183840170047;   // Monkey_Branch
    SELF_CHECK(!path.empty() && f.motion.find(path) && flip && toss && claw && f.catalog->effect(branch), "the Blueprint table has the Monkey and the ability data has its Back Flip, Versatile Throw, Claw Strike and the Branch effect");
    if (path.empty() || !f.motion.find(path) || !flip || !toss || !claw || !f.catalog->effect(branch)) return;
    self_effects::Field field(f);
    SELF_CHECKF(field.ok(), "the fight's world is made (%s)", field.ok() ? "yes" : "no");
    if (!field.ok()) return;
    const uint64_t mob = field.spawn(path, 0, 0);
    SELF_CHECK(mob != 0, "the Monkey is put in the world");
    if (mob == 0) return;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) field.tick(now);
    std::printf("  casts: %s\n", field.log.text(*f.catalog).c_str());
    SELF_CHECKF(field.log.count(flip->row.guid) == 0, "the row that waits for the Branch is not played (%zu casts)", field.log.count(flip->row.guid));
    SELF_CHECKF(field.log.count(toss->row.guid) == 0, "nor is the other row that waits for it (%zu casts)", field.log.count(toss->row.guid));
    SELF_CHECKF(field.log.count(claw->row.guid) > 0, "the creature's tree still plays its plain Claw Strike (%zu casts)", field.log.count(claw->row.guid));
}

AOC_REGISTER_TEST(test_an_effect_no_source_can_give_holds_its_row);
