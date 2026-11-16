// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mob_child_hits_tests.inc
//
//  Registered tests of test_worldd_combat: a creature's carrier hit and the hits it fires (RecursiveHit), the effects they put on a player and the lingering zones they open.
//  The Flamespeakers' Fire Bomb is fought through on the real rows: the cast its tree allows, the projectile, the impact child (Health, the Burning debuff) and the 6 s fire, the
//  confirmations the player gets (the parent projectile, the child, the burn's ticks, the fire's ticks) and their shapes; then a second player, a player who walks out of the fire,
//  a caster that resets mid-throw, two Flamespeakers side by side and the same caster's second throw; the Ashen Haunt War Mage's Fireball is the second readable carrier of the data.
//  The fire's own targeting record is not in the content.db yet (the reader change is handed over): its tests give the zone the record's 300 cm, as the data holds it.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace mob_children {

// The program's CHECK prints only a failure; these checks print the pass too, so the green run's log holds the line of the same check that failed red.
#define MOB_CHILD(cond, ...) do { \
    if (cond) { std::printf("  [ok ] "); ++g_pass; } \
    else { std::printf("  [FAIL] "); ++g_fail; } \
    std::printf(__VA_ARGS__); std::printf("\n"); \
} while (0)

/// A player the way the session stands behind its PlayerPort: the fighter is the rig's character and every hit the world lands goes through the ability runtime's
/// receive_hit, so the stats, the effects the client shows and the confirmations are the real ones.
class PipelinePlayer final : public combat::PlayerPort {
public:
    PipelinePlayer(Rig& rig, uint64_t id, combat::Vec3 pos) : rig_(rig), fighter_(rig, id, pos) {}
    uint64_t actor_id() const override { return fighter_.id(); }
    combat::Fighter& fighter() override { return fighter_; }
    combat_rig::RigFighter& rig_fighter() { return fighter_; }
    void receive_hit(const combat::IncomingHit& hit, double now) override {
        hits.push_back(hit);
        rig_.pipe->receive_hit(hit, now);   // (the session's take_hit, without the connection)
        rig_.take();
    }
    void set_in_combat(bool in_combat, double now) override {
        combat_flips.emplace_back(now, in_combat);
        this->in_combat = in_combat;
    }
    std::array<uint32_t, 4> character_guid() const override { return guid; }
    aoc::protocol::emit::FIntrepidNetworkGUID pawn_guid() const override { return combat_rig::rig_pawn_guid(fighter_.id()); }
    aoc::protocol::emit::FIntrepidNetworkGUID cylinder_guid() const override { return combat_rig::rig_cylinder_guid(fighter_.id()); }
    bool holds_effect(int64_t effect_guid) const override { return rig_.pipe && rig_.pipe->stacks_of(effect_guid) > 0; }
    ability_rig::Rig& rig() { return rig_; }
    const ability_rig::Rig& rig() const { return rig_; }
    std::array<uint32_t, 4>              guid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    std::vector<combat::IncomingHit>     hits;
    std::vector<std::pair<double, bool>> combat_flips;
    bool                                 in_combat = false;

private:
    ability_rig::Rig&        rig_;
    combat_rig::RigFighter   fighter_;
};

/// The zone's reach as the game's own targeting record holds it (the extraction the draft reads does not carry the zone targetings yet: see the reader change): a sphere.
void give_zone_radius(const abilities::Catalog& catalog, int64_t zone, double radius) {
    auto* row = const_cast<content::ZoneRow*>(catalog.zone(zone));
    if (!row) return;
    row->volume.shape = "EHitboxShape::SHAPE_Sphere";
    row->volume.radius = radius;
}

/// A world with the real creature tables, the world's fights and one or two players whose hits go through their ability runtime.
struct Field {
    Fixture&                      f;
    const combat::NpcAbilityData& tables;
    Rig                           rig;
    Rig                           rig2;
    combat_rig::World             w;
    combat::NpcStatBook           book;
    combat::CombatWorld           world;
    PipelinePlayer                player;
    PipelinePlayer                player2;
    combat::WorldHitPort          port;
    combat::WorldHitPort          port2;
    self_effects::CastLog         casts1, casts2;
    bool                          with_second = false;

    Field(Fixture& fixture, const combat::NpcAbilityData& loaded, double px, double py, bool second = false)
        : f(fixture), tables(loaded), rig(4, 20, {}), rig2(4, 20, {}), book(*fixture.ctx->bundle, *fixture.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *fixture.data, *fixture.catalog, book, fixture.motion, config()), player(rig, 77, {px, py, 1098}),
          player2(rig2, 78, {0, 0, 1098}), port(world, w.registry, player.fighter()), port2(world, w.registry, player2.fighter()), with_second(second) {
        world.set_ability_data(&tables);
        world.add_player(&player);
        if (with_second) world.add_player(&player2);
        rig.pipe->set_combat(&port);
        if (with_second) rig2.pipe->set_combat(&port2);
    }
    static combat::CombatWorldConfig config() {
        combat::CombatWorldConfig c;
        c.seed = 5;
        return c;
    }
    bool ok() const { return rig.ok() && rig2.ok() && w.problem.empty(); }
    uint64_t spawn(const std::string& path, double x, double y, double aggro = 1, const spawns::OpenTemplate* made = nullptr, int level = 7) {
        const auto bp = f.motion.find(path);
        if (!bp) return 0;
        return w.spawn(path, x, y, level, aggro, bp->half_height, made, &*bp);
    }
    void place_second(double x, double y) {
        if (with_second) player2.rig_fighter().move_to({x, y, 1098});
    }
    void tick(double now) {
        for (PipelinePlayer* p : {&player, &player2}) {
            if (p == &player2 && !with_second) continue;
            if (const stats::StatValue* max = p->rig().cs->find(content::kGuidMaxHealth)) p->rig().cs->set_current(content::kGuidHealth, max->value);
            const combat::Vec3 at = p->rig_fighter().position();
            w.npcs->note_player(p->actor_id(), at.x, at.y, now);
            p->rig().run_to(now);
        }
        w.npcs->tick(now);
        world.tick(now);
        if (now > 0) {
            casts1.look(w.registry, caster, now);
            if (caster2 != 0) casts2.look(w.registry, caster2, now);
        }
    }
    int stacks(int64_t effect) const { return static_cast<int>(player.rig().pipe->stacks_of(effect)); }
    uint64_t caster = 0, caster2 = 0;
};

/// The first hit a player took that a predicate names: null when there is none.
const combat::IncomingHit* first_hit(const PipelinePlayer& p, const std::function<bool(const combat::IncomingHit&)>& want) {
    for (const combat::IncomingHit& h : p.hits) {
        if (want(h)) return &h;
    }
    return nullptr;
}

/// The first IncomingHit of one record that landed without a critical, a block or a miss: the data's own number, which the damage ratios are read from.
const combat::IncomingHit* clean_hit(const PipelinePlayer& p, int64_t hit, int64_t source_type, int64_t source_guid) {
    for (const combat::IncomingHit& h : p.hits) {
        if (h.hit_guid != hit || h.source_type != source_type || h.source_guid != source_guid) continue;
        if ((h.flags & (stats::kHitCrit | stats::kHitBlock | stats::kHitMiss)) != 0) continue;
        return &h;
    }
    return nullptr;
}

/// The Health a hit took (negative), 0 when it moved it none.
double health_of(const combat::IncomingHit& h) {
    double v = 0;
    for (const combat::ResolvedMod& m : h.mods) {
        if (m.stat_guid == content::kGuidHealth) v += m.value;
    }
    return v;
}

/// The first confirmation of a record (the whole fight's, from the rig's log) and how many there are.
const bootstrap::HitConfirm* confirmation(const Rig& rig, int64_t record, size_t* count = nullptr) {
    const bootstrap::HitConfirm* found = nullptr;
    size_t n = 0;
    for (const Seen& s : rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != record) continue;
            ++n;
            if (!found) found = &c;
        }
    }
    if (count) *count = n;
    return found;
}

/// The Blueprint paths: the ordinary Riverlands Flamespeaker (elite rank 1), Boog (rank 2, the rare spawn), the Ashen Haunt War Mage and the Cinderclay Animyst.
const char* const kFlamespeaker = "/GoblinFlameShaman/Goblin_FlameShaman_Gravehide.Default__Goblin_FlameShaman_Gravehide_C";
const char* const kBoog = "/GoblinFlameShaman/Goblin_RareSpawn_Boog.Default__Goblin_RareSpawn_Boog_C";
const char* const kAshenHauntMage = "/Game/DesignAssets/Population/NPC/Monster/Undead/Spirit/Wraith/AshenHaunt_Mage.Default__AshenHaunt_Mage_C";   // (the population one: a quest event open shares the leaf)
const char* const kCinderclayMage = "/ClayDunzenkell_Mage_Gilded1.Default__ClayDunzenkell_Mage_Gilded1_C";   // (elite rank 3: its Shocked roll has the best chance the data gives)

/// The Fire Bomb's chain as the data holds it (the guids are read in the data test too).
constexpr int64_t kFireBomb = 6064631113649553408ll;
constexpr int64_t kFireBombCarrier = 6064631113653616642ll;
constexpr int64_t kFireBombChild = 6064631113687171079ll;
constexpr int64_t kGenericBurn = 6064631979899944981ll;
constexpr int64_t kGenericBurnTick = 6064631979906039830ll;
constexpr int64_t kFireBombZone = 6064631113682059270ll;
constexpr int64_t kFireBombZoneHit = 6064631113693659145ll;
constexpr int64_t kFireBombBurn = 6064632143989637120ll;
constexpr int64_t kFireBombBurnTick = 6064632143998812161ll;

}  // namespace mob_children

// R1 and R2 (the data): every carrier of the 23 that passes the damage to its children is an attack now, the one whose child is only a stun is not, and the Fire Bomb's chain is
// exactly the records the dig names.  The old rule read only the ability's own hits, so the Fire Bomb and 16 more were not attacks and their trees never played them; the one whose
// child's damage expression does not compile stays out too (the dig's 18 counted the row, not a number the world can work out).
void test_the_mob_carriers_hurt_enemies_through_their_children(Fixture& f) {
    std::printf("the mob carriers whose children hurt enemies are attacks: the Fire Bomb's chain and the 17 that change, the one with only a stun and the 4 that already played\n");
    struct Carrier {
        const char* name;
        bool        own_hurts;   // an own hit of the ability already hurt enemies before the children were read
        bool        hurts;       // the ability is an attack now
    };
    // the 23 pure carriers of the creature tables (out/checks/reach.txt): 19 were not attacks, 17 of them through a child that moves Health; the VoidDispel's child formula does
    // not compile, so it can do nothing and stays out, and the Envenomed Bolas' child is a stun only
    const Carrier kCarriers[] = {
        {"GoblinShaman_FireBomb", false, true},
        {"GoblinShaman_FireBombHarass", false, true},
        {"GoblinShaman_OozingBomb", true, true},
        {"GoblinShaman_OozingBombHarass", false, true},   // (the Harass twin names only the projectile carrier: the zone's initial hit is the plain ability's own row)
        {"MagmaGolem_FlameBlast", false, true},
        {"Automata_Overseer_ArcaneTorrent", false, true},
        {"Automata_Overseer_EnergyNet", true, true},
        {"ClayDunzenkell_Mage_ChainShock", false, true},
        {"ClayDunzenkell_Ranger_ExplosiveShot", false, true},
        {"ConstructDunzenkell_Ranger_ExplosiveShot", false, true},
        {"ConstructDunzenkell_Mage_ChainEnfeeble", false, true},
        {"Ancient_Caster_GreaterVoidBolt", false, true},
        {"Ancient_One_Siren_WaveSmite", false, true},
        {"RockMonster_MetalStorm", false, true},
        {"SkeletonReaper_Rainofbone", false, true},
        {"AshenHaunt_Fireball", false, true},
        {"Bandit_Aerialist_LightningShot", false, true},
        {"Bandit_Arsonist_FlameThrow", true, true},
        {"Bandit_Pirate_Archer_AnchoringShot", false, true},
        {"Bandit_Pirate_Archer_BrinyShot", false, true},
        {"Najash_Predator_EnvenomedBolas", false, false},   // (its child carries only a stun: it stays a move the world does not play)
        {"CoalElemental_Rockling_Ignite", true, true},
        {"SpellRazer_VoidDispel", false, false},   // (its child's Health expression does not compile: GetBlackboardInt is not read, so the hit does nothing and it is no attack)
    };
    size_t found = 0, hurting = 0, changed = 0;
    for (const Carrier& c : kCarriers) {
        const abilities::AbilityDef* d = f.catalog->ability_named(c.name);
        MOB_CHILD(d != nullptr, "the data has %s", c.name);
        if (!d) continue;
        ++found;
        const bool now = combat::hurts_enemies(*d, *f.catalog, f.data->ids().health);
        if (now) ++hurting;
        if (now && !c.own_hurts) ++changed;
        MOB_CHILD(now == c.hurts, "%s hurts enemies through its children: %s", c.name, c.hurts ? "yes" : "no");
        if (!c.hurts) MOB_CHILD(!combat::is_alert_move(*d, *f.catalog, f.data->ids().health), "... and it is no alert either (its child reaches enemies)");
    }
    MOB_CHILD(found == 23 && hurting == 21 && changed == 17, "the 23 pure carriers: 21 hurt enemies now, 17 of them only through a child (%zu found, %zu hurting, %zu changed)", found, hurting, changed);
    // the Fire Bomb's chain, record by record
    const abilities::AbilityDef* bomb = f.catalog->ability_named("GoblinShaman_FireBomb");
    const abilities::HitDef* carrier = bomb ? f.catalog->hit(mob_children::kFireBombCarrier) : nullptr;
    const abilities::HitDef* child = f.catalog->hit(mob_children::kFireBombChild);
    const abilities::EffectDef* burn = f.catalog->effect(mob_children::kGenericBurn);
    const abilities::EffectDef* fire_burn = f.catalog->effect(mob_children::kFireBombBurn);
    const content::ZoneRow* zone = f.catalog->zone(mob_children::kFireBombZone);
    MOB_CHILD(bomb && bomb->row.guid == mob_children::kFireBomb && carrier && carrier->mods.empty() && carrier->effects.empty() && carrier->zones.empty() && carrier->recursive.size() == 1 &&
                  carrier->recursive.front().child_hit == mob_children::kFireBombChild,
              "the Fire Bomb's projectile hit is an empty carrier and chains exactly one child (%lld)", static_cast<long long>(mob_children::kFireBombChild));
    bool health = false;
    if (child) {
        for (const content::ModRow& m : child->mods) health = health || (m.stat_guid == content::kGuidHealth && m.value_expr == 658 && f.catalog->program(658) >= 0);
    }
    MOB_CHILD(health && child->row.applicable_targets == 0 && carrier && carrier->recursive.front().radius_cm == 24.0,
              "the child is a 24 cm ground-trace hit with Health 1.8 x the damage formula (expr 658)");
    MOB_CHILD(burn && burn->row.tick_timer == 1.0 && burn->row.ignore_initial_tick && !burn->hits.empty() && burn->hits.front().second == mob_children::kGenericBurnTick,
              "the child applies Generic_Burn: 1 s ticks, no tick at the application, its tick hit is the data's burn tick");
    // the burn's application row asks an elite rank of 2 of the caster: the ordinary Riverlands Flamespeakers (rank 1) hurt and set the fire, Boog (rank 2) also burns
    int64_t burn_requirement = 0;
    if (child) {
        for (const content::HitEffectRow& row : child->effects) {
            if (row.kind == "apply" && row.effect_guid == mob_children::kGenericBurn) burn_requirement = row.requirement_expr;
        }
    }
    const content::ExprRow* requirement = burn_requirement != 0 ? f.catalog->expr(burn_requirement) : nullptr;
    const std::string gravehide = creature_path(f, mob_children::kFlamespeaker);
    const std::string boog = creature_path(f, mob_children::kBoog);
    const auto rank_at = [&](const std::string& path, int level) {
        if (path.empty()) return 0;
        const auto bp = f.motion.find(path);
        int rank = 0;
        if (bp) {
            for (const auto& [from, r] : bp->elite) {
                if (from <= level) rank = r;
            }
        }
        return rank;
    };
    MOB_CHILD(requirement && requirement->text.find("GetNpcEliteRank") != std::string::npos && requirement->text.find("2") != std::string::npos && rank_at(gravehide, 7) == 1 &&
                  rank_at(boog, 7) == 2,
              "the child's Burning asks an elite rank of 2 of its caster: the Gravehide Flamespeaker is rank 1 and Boog rank 2");
    // the in-place stack limit is the expression "1" (not the id 2): an application refreshes the instance
    const content::ExprRow* limit = burn && burn->row.in_place_stack_limit_expr != 0 ? f.catalog->expr(burn->row.in_place_stack_limit_expr) : nullptr;
    MOB_CHILD(limit && limit->text == "1" && burn->row.stack_type.find("StackInPlace") != std::string::npos, "the Burning's in-place limit is 1: a new application refreshes it");
    bool zone_hit = false;
    if (zone) zone_hit = zone->life_time == 6.0 && zone->tick_rate == 1.0 && zone->hits.count("LingeringHitsIds") != 0 &&
                           zone->hits.at("LingeringHitsIds").front() == mob_children::kFireBombZoneHit;
    MOB_CHILD(zone && zone_hit && child && std::find(child->zones.begin(), child->zones.end(), mob_children::kFireBombZone) != child->zones.end(),
              "the child opens the 6 s / 1 s lingering fire and its Lingering hit is the data's 0.6 x hit");
    MOB_CHILD(fire_burn && !fire_burn->hits.empty() && fire_burn->hits.front().second == mob_children::kFireBombBurnTick && fire_burn->row.tick_timer == 2.0,
              "the fire's hit applies FireBombBurn: a 2 s tick hit that is the data's 0.10 x tick");
}

AOC_REGISTER_TEST(test_the_mob_carriers_hurt_enemies_through_their_children);

// R1, R2 and R3 (end to end, the real rows): a Gravehide Flamespeaker the world makes from its Blueprint, at the 16 m its tree stands off at, throws its Fire Bomb; the bomb flies,
// the impact child lands on the player with its Health and Burning, the fire opens and ticks, the burn ticks, and every confirmation has the shape the game gives it (the parent a
// projectile; the child Source = the player and predicted on neither; the burn's ticks a Source = the player and predicted on both; the fire's ticks a zone, on neither).
void test_the_flamespeaker_throws_the_fire_bomb(Fixture& f) {
    std::printf("a Flamespeaker throws its Fire Bomb: the projectile lands, the child hits and burns, the fire ticks, and the confirmations are the game's\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kBoog);
    const abilities::AbilityDef* bomb = f.catalog->ability_named("GoblinShaman_FireBomb");
    const abilities::HitDef* carrier = f.catalog->hit(mob_children::kFireBombCarrier);
    const abilities::HitDef* child = f.catalog->hit(mob_children::kFireBombChild);
    const abilities::EffectDef* burn = f.catalog->effect(mob_children::kGenericBurn);
    const content::ZoneRow* zone = f.catalog->zone(mob_children::kFireBombZone);
    const abilities::EffectDef* fire_burn = f.catalog->effect(mob_children::kFireBombBurn);
    MOB_CHILD(!path.empty() && bomb && carrier && child && burn && zone && fire_burn, "the Blueprint table and the ability data have Boog (an elite Flamespeaker) and the Fire Bomb's chain");
    if (path.empty() || !bomb || !carrier || !child || !burn || !zone || !fire_burn) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for Boog (an elite Flamespeaker) (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    // the fire's own targeting record is not in this content.db; the record holds a 300 cm sphere (out/reader hands the extractor change over)
    mob_children::give_zone_radius(*f.catalog, mob_children::kFireBombZone, 300.0);
    rally_group::LogCapture log;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made (the captured data and the rig)");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);   // 16 m: the tree's stand-off distance
    MOB_CHILD(field.caster != 0, "Boog (an elite Flamespeaker) is put in the world 16 m from the player");
    if (field.caster == 0) return;
    // run to one cast's full Burning (the impact + 9.6 s: its 9 ticks).  The tree's ranged row starts only its own 3 s cooldown, so the caster is killed once its bomb has left:
    // the one bomb's chain is what this test reads, not a second cast's
    const double dt = 1.0 / 15.0;
    bool burning = false, fire_burning = false, killed = false;
    double burning_end = -1, impact_at = -1, killed_at = -1;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.tick(now);
        if (!killed && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) {
            std::string why;
            killed = field.world.gm_kill(field.caster, now, why);
            killed_at = now;
        }
        if (!burning && field.player.rig().pipe->stacks_of(mob_children::kGenericBurn) > 0) {
            burning = true;
            burning_end = field.player.rig().pipe->effect_end(mob_children::kGenericBurn).value_or(-1.0);
        }
        fire_burning = fire_burning || field.player.rig().pipe->stacks_of(mob_children::kFireBombBurn) > 0;
        if (impact_at < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) impact_at = h.at;
            }
        }
        if (impact_at >= 0 && now >= impact_at + 9.6) break;
    }
    MOB_CHILD(killed, "the caster is killed at %.2f s, after its bomb left, so only its one bomb's chain is read", killed_at);
    field.player.rig().take();
    const mob_children::PipelinePlayer& p = field.player;
    const std::string text = log.text();
    const abilities::HitDef* zone_hit = f.catalog->hit(mob_children::kFireBombZoneHit);
    const abilities::HitDef* burn_tick = f.catalog->hit(mob_children::kGenericBurnTick);
    const abilities::HitDef* fire_tick = f.catalog->hit(mob_children::kFireBombBurnTick);
    const combat::IncomingHit* parent = mob_children::first_hit(p, [](const combat::IncomingHit& h) { return h.hit_guid == mob_children::kFireBombCarrier; });
    const combat::IncomingHit* impact = mob_children::first_hit(p, [](const combat::IncomingHit& h) { return h.hit_guid == mob_children::kFireBombChild; });
    MOB_CHILD(field.casts1.count(mob_children::kFireBomb) >= 1, "Boog (an elite Flamespeaker) casts the Fire Bomb (its tree's ranged row: %zu casts)", field.casts1.count(mob_children::kFireBomb));
    MOB_CHILD(rally_group::lines_with(text, {"plays GoblinShaman_FireBomb", "0 hit(s) and 1 projectile(s)", "over 3.83 s"}) >= 1,
              "the cast line says the montage's 0 hit notifies and its 1 projectile, over 3.83 s");
    MOB_CHILD(parent != nullptr, "the player took the projectile's carrier hit");
    if (!parent) return;
    MOB_CHILD(parent->mods.empty(), "the carrier hit itself does no damage (no stat mod)");
    MOB_CHILD(impact != nullptr && mob_children::health_of(*impact) < 0, "the impact child landed on the player and took Health");
    if (!impact) return;
    MOB_CHILD(std::fabs(impact->at - parent->at) <= dt + 1e-9, "the child lands in the same delivery as the parent (%.4f s and %.4f s)", parent->at, impact->at);
    // the Burning debuff the child applies, and its 1 s ticks
    MOB_CHILD(burning && field.stacks(mob_children::kGenericBurn) == 1, "the child put Burning on the player (1 stack)");
    MOB_CHILD(burning_end > 0 && std::fabs(burning_end - impact->at - 10.0) <= 0.25, "Burning lasts the data's 10 s (ends at %.2f s, the impact at %.2f s)", burning_end, impact->at);
    const combat::IncomingHit* burn_clean = mob_children::clean_hit(p, mob_children::kGenericBurnTick, content::kSourceTypeEffect, mob_children::kGenericBurn);
    MOB_CHILD(burn_clean != nullptr, "a burn tick landed without a critical, a block or a miss");
    if (burn_clean) {
        const double ratio = mob_children::health_of(*burn_clean) / mob_children::health_of(*impact);
        MOB_CHILD(std::fabs(ratio - 0.20 / 1.8) <= 0.002, "the burn tick is 0.20 x the damage formula to the impact child's 1.8 x (%.4f against %.4f)", ratio, 0.20 / 1.8);
    }
    size_t burn_ticks = 0;
    for (const combat::IncomingHit& h : p.hits) burn_ticks += (h.hit_guid == mob_children::kGenericBurnTick && h.source_type == content::kSourceTypeEffect) ? 1 : 0;
    MOB_CHILD(burn_ticks == 9, "Burning ticked once a second for the 10 s (the data's no-tick-at-the-start, 9 ticks: %zu)", burn_ticks);
    // the lingering fire: 6 s, 1 s ticks
    size_t zone_ticks = 0;
    for (const combat::IncomingHit& h : p.hits) zone_ticks += (h.hit_guid == mob_children::kFireBombZoneHit && h.source_type == content::kSourceTypeZone) ? 1 : 0;
    MOB_CHILD(zone_ticks >= 1, "the child opened its lingering fire, whose ticks reached the player (%zu)", zone_ticks);
    const combat::IncomingHit* zone_clean = mob_children::clean_hit(p, mob_children::kFireBombZoneHit, content::kSourceTypeZone, mob_children::kFireBombZone);
    MOB_CHILD(zone_clean != nullptr, "the fire's Lingering hit landed on the player from the zone's record");
    if (zone_clean) {
        const double ratio = mob_children::health_of(*zone_clean) / mob_children::health_of(*impact);
        MOB_CHILD(std::fabs(ratio - 0.6 / 1.8) <= 0.002, "the fire's hit is 0.6 x the damage formula to the child's 1.8 x (%.4f against %.4f)", ratio, 0.6 / 1.8);
    }
    size_t zone_hits = 0;
    for (const combat::IncomingHit& h : p.hits) zone_hits += (h.hit_guid == mob_children::kFireBombZoneHit && h.source_type == content::kSourceTypeZone) ? 1 : 0;
    MOB_CHILD(zone_hits == 6, "the fire ticked 6 times over its 6 s (%zu)", zone_hits);
    MOB_CHILD(fire_burning, "the fire's hit put FireBombBurn on the player");
    const combat::IncomingHit* fire_clean = mob_children::clean_hit(p, mob_children::kFireBombBurnTick, content::kSourceTypeEffect, mob_children::kFireBombBurn);
    if (fire_clean) {
        const double ratio = mob_children::health_of(*fire_clean) / mob_children::health_of(*impact);
        MOB_CHILD(std::fabs(ratio - 0.10 / 1.8) <= 0.002, "FireBombBurn's tick is 0.10 x the damage formula (%.4f against %.4f)", ratio, 0.10 / 1.8);
    } else {
        MOB_CHILD(false, "FireBombBurn ticked on the player");
    }
    // the confirmations: the shapes the game gives them
    size_t parents = 0, children = 0, ticks = 0, zones = 0;
    const bootstrap::HitConfirm* parent_c = mob_children::confirmation(field.player.rig(), mob_children::kFireBombCarrier, &parents);
    const bootstrap::HitConfirm* child_c = mob_children::confirmation(field.player.rig(), mob_children::kFireBombChild, &children);
    const bootstrap::HitConfirm* burn_c = mob_children::confirmation(field.player.rig(), mob_children::kGenericBurnTick, &ticks);
    const bootstrap::HitConfirm* zone_c = mob_children::confirmation(field.player.rig(), mob_children::kFireBombZoneHit, &zones);
    MOB_CHILD(parent_c != nullptr && children >= 1, "the player got the parent's confirmation and the child's (%zu parents, %zu children)", parents, children);
    if (parent_c) {
        MOB_CHILD(parent_c->source_type == content::kSourceTypeProjectile && parent_c->source_guid == 6064631113651847169ll && !parent_c->source_is_actor && parent_c->predicted_autonomous &&
                      parent_c->predicted_simulated && parent_c->mods.empty(),
                  "the parent is the projectile's hit: its record is the source, no source actor, predicted on both sides, no number");
    }
    if (child_c) {
        const aoc::protocol::emit::FIntrepidNetworkGUID pawn = combat_rig::rig_pawn_guid(p.actor_id());
        MOB_CHILD(!child_c->predicted_autonomous && !child_c->predicted_simulated && !child_c->source_is_actor && child_c->source && *child_c->source == pawn,
                  "the child is predicted on neither side and names the player it was fired on as its Source");
    }
    if (burn_c) MOB_CHILD(burn_c->source_type == content::kSourceTypeEffect && burn_c->predicted_autonomous && burn_c->predicted_simulated && burn_c->source &&
                               *burn_c->source == combat_rig::rig_pawn_guid(p.actor_id()),
                           "the burn's tick is an effect's hit, predicted on both sides, its Source the player");
    MOB_CHILD(ticks >= 1 && zones >= 1, "the burn and the fire confirm their ticks to the player (%zu burn ticks, %zu fire ticks)", ticks, zones);
    if (zone_c) MOB_CHILD(zone_c->source_type == content::kSourceTypeZone && zone_c->source_guid == mob_children::kFireBombZone && !zone_c->source_is_actor && !zone_c->predicted_autonomous &&
                                  !zone_c->predicted_simulated,
                              "the fire's ticks are the zone's record, predicted on neither side and with no source actor");
    MOB_CHILD(burn_tick && fire_tick && zone_hit && mob_children::health_of(*impact) < 0, "the chain ran through the data's own hit records");
}

AOC_REGISTER_TEST(test_the_flamespeaker_throws_the_fire_bomb);

// R1 on whom: the impact child's 24 cm ground trace reaches only the player it landed on, the fire's 300 cm sphere reaches the second player too, and a player who walks out of
// the fire takes no more of its ticks (his Burning stays: it is on him, not on the ground).
void test_the_fire_bomb_reaches_the_players_in_its_ground_trace_and_its_fire(Fixture& f) {
    std::printf("the bomb's impact hits the player it landed on, the fire reaches a second player in its 3 m sphere, and a player who walks out stops taking its ticks\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kFlamespeaker);
    const content::ZoneRow* zone = f.catalog->zone(mob_children::kFireBombZone);
    MOB_CHILD(!path.empty() && zone, "the Blueprint table has the Flamespeaker and the data has the fire");
    if (path.empty() || !zone) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::give_zone_radius(*f.catalog, mob_children::kFireBombZone, 300.0);
    mob_children::Field field(f, tables, 0, 100, true);   // the second player stands 2 m to the side
    MOB_CHILD(field.ok(), "the fight's world is made with two players");
    if (!field.ok()) return;
    field.place_second(200, 100);
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Flamespeaker is put in the world 16 m from the first player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);   // (the bomb is aimed at the first player, not the nearer-by-proximity second)
    const double dt = 1.0 / 15.0;
    bool walked_out = false, killed = false;
    double walked_at = -1, impact_at = -1, killed_at = -1;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.tick(now);
        // the caster's ranged Fire Bomb row starts only a 3 s cooldown: kill it after its first bomb left so one fire's reach is read
        if (!killed && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) {
            std::string why;
            killed = field.world.gm_kill(field.caster, now, why);
            killed_at = now;
        }
        if (impact_at < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) impact_at = h.at;
            }
        }
        // once the fire has ticked on both players, the second player walks 6 m away: out of the 3 m sphere
        if (!walked_out && impact_at >= 0 && now >= impact_at + 1.5) {
            walked_out = true;
            walked_at = now;
            field.place_second(600, 100);
        }
        // one cast's fire (6 s) and no second bomb's
        if (impact_at >= 0 && now >= impact_at + 8.0) break;
    }
    MOB_CHILD(killed && walked_out, "the caster is killed at %.2f s after its bomb left, and the second player walks out at %.2f s", killed_at, walked_at);
    field.player.rig().take();
    field.player2.rig().take();
    const mob_children::PipelinePlayer& p1 = field.player;
    const mob_children::PipelinePlayer& p2 = field.player2;
    const auto has = [](const mob_children::PipelinePlayer& p, int64_t hit) {
        for (const combat::IncomingHit& h : p.hits) {
            if (h.hit_guid == hit) return true;
        }
        return false;
    };
    MOB_CHILD(has(p1, mob_children::kFireBombChild), "the player the bomb homed to took the impact child");
    MOB_CHILD(!has(p2, mob_children::kFireBombChild), "the second player, 2 m from the impact, is outside the child's 24 cm ground trace and took no impact");
    size_t p2_zone_before = 0, p2_zone_after = 0, p2_fire_before = 0, p2_fire_after = 0;
    for (const combat::IncomingHit& h : p2.hits) {
        if (h.hit_guid == mob_children::kFireBombZoneHit) {
            ++(h.at > walked_at ? p2_zone_after : p2_zone_before);
        }
        if (h.hit_guid == mob_children::kFireBombBurnTick) {
            ++(h.at > walked_at ? p2_fire_after : p2_fire_before);
        }
    }
    MOB_CHILD(p2_zone_before >= 1, "the second player, 2 m from the impact, stood in the fire and took its ticks (%zu before he moved)", p2_zone_before);
    MOB_CHILD(walked_out && p2_zone_after == 0, "after he walked 6 m away the fire landed no more ticks on him (%zu after)", p2_zone_after);
    MOB_CHILD(p2_fire_before + p2_fire_after > 0, "the fire's own Burning is on him, not on the ground: it goes on ticking after he leaves (%zu before his walk, %zu after)", p2_fire_before, p2_fire_after);
    // the impact child's Burning is gated by the data on an elite-rank-2+ caster; this Flamespeaker is rank 1, so it did not put it on
    MOB_CHILD(field.stacks(mob_children::kGenericBurn) == 0, "the ordinary Flamespeaker's child put no Burning on the player: the data asks an elite rank of 2 (%d stack(s))", field.stacks(mob_children::kGenericBurn));
    size_t p1_zone_after = 0;
    for (const combat::IncomingHit& h : p1.hits) p1_zone_after += (h.hit_guid == mob_children::kFireBombZoneHit && h.at > impact_at + 6.5) ? 1 : 0;
    MOB_CHILD(p1_zone_after == 0, "the fire burned out after its 6 s: no tick after the impact's 6.5 s (%zu)", p1_zone_after);
}

AOC_REGISTER_TEST(test_the_fire_bomb_reaches_the_players_in_its_ground_trace_and_its_fire);

// R1/R2 on the other order (a caster that resets mid-throw): the bomb already in the air is not the cast's to drop (the world drops the cast's later hits, not its projectile), so it
// lands its child, its Burning and its fire after the Flamespeaker's fight has ended.
void test_a_flamespeaker_reset_mid_throw_still_lands_its_bomb(Fixture& f) {
    std::printf("a Flamespeaker reset while its bomb is in the air: the bomb still lands its child and its fire\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kBoog);
    MOB_CHILD(!path.empty(), "the Blueprint table has Boog (an elite Flamespeaker)");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for Boog (an elite Flamespeaker) (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::give_zone_radius(*f.catalog, mob_children::kFireBombZone, 300.0);
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    MOB_CHILD(field.caster != 0, "Boog (an elite Flamespeaker) is put in the world 16 m from the player");
    if (field.caster == 0) return;
    const double dt = 1.0 / 15.0;
    bool reset = false;
    double reset_at = -1, impact_at = -1;
    for (double now = dt; now <= 16.0 + 1e-9; now += dt) {
        field.tick(now);
        // the fire notify is at 2.49 s: the reset comes while the bomb flies, before it arrives
        if (!reset && field.casts1.count(mob_children::kFireBomb) > 0 && now > field.casts1.first(mob_children::kFireBomb) + 2.9) {
            reset = field.world.gm_reset(field.caster, now) != combat::CombatWorld::GmReset::Dead;
            reset_at = now;
        }
        if (impact_at < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) impact_at = h.at;
            }
        }
        // the impact and a couple of the fire's ticks: before the tree's second bomb (10 s after the first cast)
        if (impact_at >= 0 && now >= impact_at + 2.5) break;
    }
    field.player.rig().take();
    size_t children = 0, zones = 0;
    for (const combat::IncomingHit& h : field.player.hits) {
        children += h.hit_guid == mob_children::kFireBombChild ? 1 : 0;
        zones += h.hit_guid == mob_children::kFireBombZoneHit ? 1 : 0;
    }
    MOB_CHILD(reset && reset_at > 0, "Boog (an elite Flamespeaker)'s fight is reset at %.2f s, while its bomb is in the air", reset_at);
    MOB_CHILD(children == 1, "the bomb still landed its child once after the reset (%zu)", children);
    MOB_CHILD(zones >= 1, "... and its fire (%zu of its ticks)", zones);
    MOB_CHILD(field.player.rig().pipe->stacks_of(mob_children::kGenericBurn) == 1, "Boog's rank-2 bomb still put its Burning on the player after the reset (%d)", static_cast<int>(field.player.rig().pipe->stacks_of(mob_children::kGenericBurn)));
}

AOC_REGISTER_TEST(test_a_flamespeaker_reset_mid_throw_still_lands_its_bomb);

// R1/R2 with company: two Flamespeakers side by side each throw their own bomb at the same player; each lands its own child and opens its own fire (two zones), and the two
// Burning applications refresh the one instance the data's in-place limit allows.
void test_two_flamespeakers_side_by_side_each_throw_their_bomb(Fixture& f) {
    std::printf("two Flamespeakers side by side: two bombs, two impacts, two fires, and the one Burning the data's limit allows, refreshed\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kBoog);
    MOB_CHILD(!path.empty(), "the Blueprint table has Boog (an elite Flamespeaker)");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for Boog (an elite Flamespeaker) (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::give_zone_radius(*f.catalog, mob_children::kFireBombZone, 300.0);
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, -400, 1700, 2000, made.open, 25);   // a level-25 elite: its accuracy beats the level-20 player's evasion, so both bombs' children land
    field.caster2 = field.spawn(path, 400, 1700, 2000, made.open, 25);
    MOB_CHILD(field.caster != 0 && field.caster2 != 0, "two Flamespeakers are put in the world");
    if (field.caster == 0 || field.caster2 == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);   // (both throw at the first player)
    field.world.add_threat(field.caster2, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    bool two = false, stacked = false;
    bool killed1 = false, killed2 = false;
    double first_impact = -1;
    int burn_max = 0;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.tick(now);
        two = two || (field.casts1.count(mob_children::kFireBomb) >= 1 && field.casts2.count(mob_children::kFireBomb) >= 1);
        // each caster is killed after its first bomb left: the two first bombs are what is read, not their next casts (the ranged row's cooldown is 3 s)
        std::string why;
        if (!killed1 && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) killed1 = field.world.gm_kill(field.caster, now, why);
        if (!killed2 && field.casts2.count(mob_children::kFireBomb) >= 1 && now > field.casts2.first(mob_children::kFireBomb) + 2.7) killed2 = field.world.gm_kill(field.caster2, now, why);
        if (first_impact < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) first_impact = h.at;
            }
        }
        burn_max = std::max(burn_max, static_cast<int>(field.player.rig().pipe->stacks_of(mob_children::kGenericBurn)));
        stacked = stacked || burn_max == 2;
        // the two fires' 6 s
        if (stacked && first_impact >= 0 && now >= first_impact + 6.5) break;
    }
    field.player.rig().take();
    size_t children = 0, zone_hits = 0;
    for (const combat::IncomingHit& h : field.player.hits) {
        children += h.hit_guid == mob_children::kFireBombChild ? 1 : 0;
        zone_hits += h.hit_guid == mob_children::kFireBombZoneHit ? 1 : 0;
    }
    size_t applications = 0;
    std::printf("  two bombs: %zu children, %zu fire ticks, the player's Burning %d stack(s) at most; child flags:", children, zone_hits, burn_max);
    for (const combat::IncomingHit& h : field.player.hits) {
        if (h.hit_guid != mob_children::kFireBombChild) continue;
        std::printf(" %u (%zu effect row(s))", static_cast<unsigned>(h.flags), h.effects.size());
        applications += h.effects.empty() ? 0 : 1;
    }
    std::printf("\n");
    MOB_CHILD(killed1 && killed2, "both casters are killed after their first bomb left (%d and %d)", killed1 ? 1 : 0, killed2 ? 1 : 0);
    MOB_CHILD(two, "each Flamespeaker throws its bomb (%zu and %zu casts)", field.casts1.count(mob_children::kFireBomb), field.casts2.count(mob_children::kFireBomb));
    MOB_CHILD(children == 2, "the player took both impacts (%zu)", children);
    MOB_CHILD(zone_hits >= 10, "both fires stood and ticked on the player (%zu zone hits)", zone_hits);
    // the data's in-place stack limit is the expression "1": every application refreshes the one instance, it does not add a second
    MOB_CHILD(applications == 2 && burn_max == 1, "both bombs' children applied Burning, and the data's in-place limit of 1 keeps one instance (%zu applications, %d stack(s))", applications, burn_max);
}

AOC_REGISTER_TEST(test_two_flamespeakers_side_by_side_each_throw_their_bomb);

// R1/R4 (the same throw again): the second Fire Bomb is its own delivery: the child lands, the fire opens again and the Burning is applied again.
void test_a_flamespeaker_throws_its_bomb_twice(Fixture& f) {
    std::printf("the same Flamespeaker's second Fire Bomb lands its own child and opens its own fire\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kBoog);
    MOB_CHILD(!path.empty(), "the Blueprint table has Boog (an elite Flamespeaker)");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for Boog (an elite Flamespeaker) (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::give_zone_radius(*f.catalog, mob_children::kFireBombZone, 300.0);
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    MOB_CHILD(field.caster != 0, "Boog (an elite Flamespeaker) is put in the world");
    if (field.caster == 0) return;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 40.0 + 1e-9; now += dt) field.tick(now);
    field.player.rig().take();
    size_t children = 0, zone_hits = 0;
    double first_child = -1, second_child = -1;
    for (const combat::IncomingHit& h : field.player.hits) {
        if (h.hit_guid == mob_children::kFireBombChild) {
            ++children;
            if (first_child < 0) first_child = h.at;
            else if (second_child < 0) second_child = h.at;
        }
        zone_hits += h.hit_guid == mob_children::kFireBombZoneHit ? 1 : 0;
    }
    MOB_CHILD(field.casts1.count(mob_children::kFireBomb) >= 2, "Boog (an elite Flamespeaker) casts the Fire Bomb again (%zu casts)", field.casts1.count(mob_children::kFireBomb));
    MOB_CHILD(children >= 2 && second_child > first_child, "each throw landed its own impact child (%.2f s and %.2f s)", first_child, second_child);
    MOB_CHILD(zone_hits >= 10, "each throw opened its own fire on the player (%zu zone hits)", zone_hits);
}

AOC_REGISTER_TEST(test_a_flamespeaker_throws_its_bomb_twice);

// R1 and R3 (the recorded kin, a plain-hit carrier): the Cinderclay Animyst's Chain Shock is the ability whose empty hit and 500 cm child the retail recording shows: the parent is
// the ability's hit with no number, the child lands on both players inside its 500 cm around the one the parent hit, in the same instant, names the player as its Source and is
// predicted on neither side, and puts the recorded Shocked effect on both.
void test_the_chain_shock_lands_the_bounce_the_recording_shows(Fixture& f) {
    std::printf("the Chain Shock's empty hit and its 500 cm bounce: the child lands on both players in the same instant, Source the hit player, neither side predicted\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kCinderclayMage);
    const abilities::AbilityDef* shock = f.catalog->ability_named("ClayDunzenkell_Mage_ChainShock");
    const abilities::HitDef* carrier = f.catalog->hit_named("ClayDunzenkell_Mage_ChainShock_Hit");
    const abilities::HitDef* child = f.catalog->hit_named("ClayDunzenkell_Mage_ChainShockBounce_Hit");
    const abilities::EffectDef* shocked = f.catalog->effect_named("Status_Shocked");
    MOB_CHILD(!path.empty() && shock && carrier && child && shocked, "the Blueprint table has the Cinderclay Animyst and the data has Chain Shock, its empty hit, its child and Shocked");
    if (path.empty() || !shock || !carrier || !child || !shocked || carrier->recursive.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Cinderclay Animyst (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::Field field(f, tables, 0, 100, true);
    MOB_CHILD(field.ok(), "the fight's world is made with two players");
    if (!field.ok()) return;
    field.place_second(300, 100);   // 3 m from the first: inside the child's 500 cm
    field.caster = field.spawn(path, 0, 900, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Cinderclay Animyst is put in the world 8 m from the first player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    double child_at = -1;
    for (double now = dt; now <= 12.0 + 1e-9; now += dt) {
        field.tick(now);
        if (child_at >= 0 && now >= child_at + 1.0) break;
        for (const combat::IncomingHit& h : field.player.hits) {
            if (h.hit_guid == child->row.guid) child_at = h.at;
        }
    }
    field.player.rig().take();
    field.player2.rig().take();
    const auto has = [](const mob_children::PipelinePlayer& p, int64_t hit) {
        for (const combat::IncomingHit& h : p.hits) {
            if (h.hit_guid == hit) return true;
        }
        return false;
    };
    const auto health = [&](const mob_children::PipelinePlayer& p) {
        const combat::IncomingHit* h = mob_children::first_hit(p, [&](const combat::IncomingHit& x) { return x.hit_guid == child->row.guid; });
        return h ? mob_children::health_of(*h) : 0.0;
    };
    MOB_CHILD(field.casts1.count(shock->row.guid) >= 1, "the Animyst casts Chain Shock (%zu casts)", field.casts1.count(shock->row.guid));
    const combat::IncomingHit* parent = mob_children::first_hit(field.player, [&](const combat::IncomingHit& h) { return h.hit_guid == carrier->row.guid; });
    MOB_CHILD(parent != nullptr && parent->mods.empty() && parent->source_type == content::kSourceTypeAbility, "the parent hit is the ability's, with no number of its own");
    MOB_CHILD(has(field.player, child->row.guid) && has(field.player2, child->row.guid) && health(field.player) < 0 && health(field.player2) < 0,
              "the child landed on both players inside its 500 cm and took Health from both (%.1f, %.1f)", health(field.player), health(field.player2));
    MOB_CHILD(field.player.rig().pipe->stacks_of(shocked->row.guid) == 1 && field.player2.rig().pipe->stacks_of(shocked->row.guid) == 1, "both hold the recorded Shocked effect");
    if (parent) MOB_CHILD(std::fabs(parent->at - child_at) <= dt + 1e-9, "the child landed in the parent's instant (%.4f and %.4f s)", parent->at, child_at);
    size_t parents = 0, children = 0;
    const bootstrap::HitConfirm* parent_c = mob_children::confirmation(field.player.rig(), carrier->row.guid, &parents);
    const bootstrap::HitConfirm* child_c = mob_children::confirmation(field.player.rig(), child->row.guid, &children);
    if (parent_c) MOB_CHILD(parent_c->predicted_autonomous && !parent_c->predicted_simulated && parent_c->source_is_actor, "the parent is predicted on the autonomous side only, its Source the creature");
    if (child_c) {
        const aoc::protocol::emit::FIntrepidNetworkGUID pawn = combat_rig::rig_pawn_guid(field.player.actor_id());
        MOB_CHILD(!child_c->predicted_autonomous && !child_c->predicted_simulated && child_c->source_type == content::kSourceTypeAbility && child_c->source_guid == shock->row.guid && child_c->source &&
                      *child_c->source == pawn,
                  "the child is predicted on neither side, its source record the ability and its Source the player it was fired on, as the recording shows");
    }
}

AOC_REGISTER_TEST(test_the_chain_shock_lands_the_bounce_the_recording_shows);

// R2 (the second readable carrier of the Riverlands): the Ashen Haunt War Mage's Fireball is a projectile whose own hit is empty and whose child explodes in a 400 cm sphere;
// after the 30 s its tree waits it throws it and the child lands.
void test_the_ashen_haunt_war_mage_throws_its_fireball(Fixture& f) {
    std::printf("the Ashen Haunt War Mage's Fireball: its tree's 30 s wait, its projectile, and its 400 cm child explosion on the player\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = mob_children::kAshenHauntMage;
    const abilities::AbilityDef* fireball = f.catalog->ability_named("AshenHaunt_Fireball");
    MOB_CHILD(!path.empty() && fireball, "the Blueprint table has the Ashen Haunt War Mage and the data has its Fireball");
    if (path.empty() || !fireball) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Ashen Haunt War Mage (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 900, 2000, made.open);   // 8 m, inside the Fireball's 3.6 to 26 m
    MOB_CHILD(field.caster != 0, "the Ashen Haunt War Mage is put in the world 8 m from the player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);   // (it fights the player from the start, whatever its alignment)
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 45.0 + 1e-9; now += dt) field.tick(now);
    field.player.rig().take();
    std::printf("  the War Mage's casts:");
    for (const self_effects::Cast& c : field.casts1.casts) {
        const abilities::AbilityDef* def = f.catalog->ability(c.ability);
        std::printf(" %s@%.2f", def ? def->row.name.c_str() : "?", c.t);
    }
    std::printf("\n");
    if (const world::WorldActor* a = field.w.registry.find(field.caster)) {
        std::printf("  the War Mage stands at %.0f, %.0f; its AI state %d; the open knows the Fireball %d; the world calls it a shot %d\n", a->x, a->y,
                    static_cast<int>(field.world.ai_state(field.caster).value_or(npc::ai::State::Idle)), combat::npc_knows_ability(*a, fireball->row.guid) ? 1 : 0,
                    field.world.launches_projectiles(*fireball) ? 1 : 0);
    }
    if (const std::vector<combat::CreatureAbility>* rows = tables.abilities_of(path)) {
        for (const combat::CreatureAbility& t : *rows) {
            if (t.ability != fireball->row.guid) continue;
            std::printf("  its Fireball row: fight %d, unknown %d, off %d, arrival %.0f, distance %.0f..%.0f\n", t.fight ? 1 : 0, t.unknown ? 1 : 0, t.off ? 1 : 0, t.arrival_cm, t.min_distance,
                        t.max_distance);
        }
    }
    const combat::IncomingHit* child = mob_children::first_hit(field.player, [&](const combat::IncomingHit& h) {
        const abilities::HitDef* hit = f.catalog->hit(h.hit_guid);
        return hit && hit->row.name == "AshenHauntMage_FireballExplosion_Hit";
    });
    MOB_CHILD(field.casts1.count(fireball->row.guid) >= 1, "the War Mage casts its Fireball after the 30 s its tree waits (%zu casts)", field.casts1.count(fireball->row.guid));
    MOB_CHILD(child != nullptr && mob_children::health_of(*child) < 0, "the Fireball's child explosion landed on the player");
    MOB_CHILD(mob_children::first_hit(field.player, [](const combat::IncomingHit& h) { return h.source_type == content::kSourceTypeProjectile && h.mods.empty(); }) != nullptr,
              "the projectile's own carrier hit reached the player with no number of its own");
}

AOC_REGISTER_TEST(test_the_ashen_haunt_war_mage_throws_its_fireball);
