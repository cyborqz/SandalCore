// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_ground_circles_tests.inc: the abilities that leave a lasting area on the ground (included by combat_registered.inc inside
//  test_worldd_combat's anonymous namespace, after the other combat test files)
//
//    the caster's own effects   GetEffectCountByRecordAndInstigator(GetTarget(), effect, GetInstigator()) counts what that caster put on the target, and
//                               another caster's stacks do not count: the Vine Field's lockout, the Shatter's cooldown, the Mortal Blast's spread, the
//                               Rogue's Sap advantage and the Rogue's Backstab
//    the Vine Field             one cast lands its hit once; a second request of the same hit inside the cast does not land while the caster's own
//                               lockout is on the target
// ============================================================================

// This file's checks print their passing line too, so the proof of the eight angles can quote the [ok ] line of a check as well as its [FAIL] line.
// (One printf for the whole line: the world's log writes to the same console between two calls.)
#undef CHECK
#undef CHECKF
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define CHECKF(cond, ...) do { \
    char ground_line[1024]; \
    std::snprintf(ground_line, sizeof ground_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", ground_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", ground_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace ground {

/// What a target holds, as the reads of a hit's requirement see it, with the stacks one named caster put on it.  The real world gives the same answer
/// from the creature's effect book; this one states the book's content for the unit checks of the requirements.
class CasterEffects final : public combat::TargetEffects {
public:
    double remaining(int64_t) const override { return 0.0; }
    double stacks_from(int64_t effect_guid, uint64_t instigator) const override {
        const auto it = counts.find({effect_guid, instigator});
        return it == counts.end() ? 0.0 : it->second;
    }
    void put(int64_t effect_guid, uint64_t instigator, double stacks) { counts[{effect_guid, instigator}] = stacks; }
    void clear() { counts.clear(); }
    std::map<std::pair<int64_t, uint64_t>, double> counts;
};

/// Whether the requirement of `hit_name` holds for `caster` on `target`, with the effects `view` says the target holds and `caster_actor` as the
/// instigator.
bool requirement_with(Fixture& f, combat::CombatWorld& world, combat::Fighter& caster, combat::Fighter& target, const char* hit_name, const CasterEffects& view, uint64_t caster_actor,
                      uint64_t target_actor) {
    const abilities::HitDef* hit = f.catalog->hit_named(hit_name);
    if (!hit) return false;
    combat::HitMeta meta;
    meta.target_effects = &view;
    meta.instigator_actor = caster_actor;
    meta.target_actor = target_actor;
    return world.resolver().requirement_holds(caster, target, *hit, meta);
}

/// The guid of an effect the data has (0 when it has none).
int64_t effect_of(Fixture& f, const char* name) {
    const abilities::EffectDef* e = f.catalog->effect_named(name);
    return e ? e->row.guid : 0;
}

/// A Wolf of the world that takes an effect from `by` (the world's own door, as a hit's application does).
void put_effect(combat::CombatWorld& world, uint64_t wolf, const abilities::EffectDef* def, int stacks, double seconds, uint64_t by, double t) {
    effects::Applied a;
    a.def = def;
    a.stacks = stacks;
    a.in_place_limit = std::max(1, stacks);
    a.duration = seconds;
    world.apply_effects(wolf, by, {a}, t);
}

}  // namespace ground

// ---- the caster's own effects -----------------------------------------------------------------------------------------------------------------------------------------
// Five hits count what one caster put on their target: the Vine Field asks whether its own lockout is on (== 0, so it roots once per lockout), the Shatter's reaction asks whether its
// own cooldown is on (== 0), the Mortal Blast's spread asks whether the same Summoner's Spirit Shred is on (> 0), the Rogue's Sap advantage asks whether the same Rogue's poison is at
// ten stacks (== 10) and the Rogue's Backstab asks about the Throw's explosive mark (its by-instigator read is commented out in the data, so any caster's mark counts).
void test_ground_circles_the_casters_own_effects(Fixture& f) {
    std::printf("the caster's own effects: a requirement that counts what one caster put on the target reads his stacks, and another caster's do not count\n");
    Forge g(f, 8, {"Mage_Passive_Shatter"});
    CHECKF(g.ok(), "the Mage and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Mage");
    if (!wolf) return;
    combat::Fighter* target = g.world.fighter_of(wolf);
    CHECK(target != nullptr, "the Wolf fights");
    if (!target) return;
    combat::Fighter& mage = g.player.rig_fighter();
    constexpr uint64_t kMage = 77, kOther = 88;
    constexpr double kNow = 100.0;
    ground::CasterEffects view;

    // the Vine Field: its own lockout on the target blocks it, any other caster's does not
    const int64_t lockout = ground::effect_of(f, "Ranger_VineField_Lockout");
    CHECK(lockout != 0, "the data has the Vine Field's lockout");
    if (lockout != 0) {
        view.put(lockout, kMage, 1);
        CHECK(!ground::requirement_with(f, g.world, mage, *target, "Ranger_VineField", view, kMage, wolf),
              "the Vine Field does not land while its own lockout is on the Wolf from the same caster (== 0)");
        view.clear();
        view.put(lockout, kOther, 1);
        CHECK(ground::requirement_with(f, g.world, mage, *target, "Ranger_VineField", view, kMage, wolf),
              "another caster's lockout on the Wolf does not stop this caster's Vine Field");
        view.clear();
        CHECK(ground::requirement_with(f, g.world, mage, *target, "Ranger_VineField", view, kMage, wolf), "with no lockout on the Wolf it lands");
    }

    // the Shatter: a Frozen Wolf, the Mage's passive held; his own cooldown blocks the reaction
    const abilities::EffectDef* frozen = f.catalog->effect_named("Status_Frozen");
    const int64_t shattered_icd = ground::effect_of(f, "Mage_Status_Shattered_ICD");
    CHECK(frozen != nullptr && shattered_icd != 0, "the data has Frozen and the Shatter's cooldown");
    if (frozen && shattered_icd != 0) {
        ground::put_effect(g.world, wolf, frozen, 1, 30.0, kMage, kNow);
        view.clear();
        CHECK(ground::requirement_with(f, g.world, mage, *target, "Mage_Status_Frozen->Shattered", view, kMage, wolf),
              "the Shatter's reaction holds on a Frozen Wolf when the Mage's own cooldown is not on it (== 0)");
        view.put(shattered_icd, kMage, 1);
        CHECK(!ground::requirement_with(f, g.world, mage, *target, "Mage_Status_Frozen->Shattered", view, kMage, wolf),
              "...and does not while his own cooldown is on the Wolf");
        view.clear();
        view.put(shattered_icd, kOther, 1);
        CHECK(ground::requirement_with(f, g.world, mage, *target, "Mage_Status_Frozen->Shattered", view, kMage, wolf), "another caster's cooldown on the Wolf does not stop it");
    }

    // the Mortal Blast's spread: the same Summoner's Spirit Shred must be on the target
    const int64_t spirit_shred = ground::effect_of(f, "Summoner_SpiritShred");
    CHECK(spirit_shred != 0, "the data has the Summoner's Spirit Shred");
    if (spirit_shred != 0) {
        view.clear();
        CHECK(!ground::requirement_with(f, g.world, mage, *target, "Summoner_MortalBlast_Spread", view, kMage, wolf), "the Mortal Blast's spread holds for no target without Spirit Shred");
        view.put(spirit_shred, kMage, 1);
        CHECK(ground::requirement_with(f, g.world, mage, *target, "Summoner_MortalBlast_Spread", view, kMage, wolf),
              "...and holds on the target the same Summoner put Spirit Shred on (> 0)");
        view.clear();
        view.put(spirit_shred, kOther, 1);
        CHECK(!ground::requirement_with(f, g.world, mage, *target, "Summoner_MortalBlast_Spread", view, kMage, wolf), "...but not for another Summoner's Spirit Shred");
    }

    // the Rogue's Sap advantage: ten stacks of his own poison, no fewer, and none of another Rogue's
    const int64_t poisoned = ground::effect_of(f, "Status_Poisoned");
    CHECK(poisoned != 0, "the data has the Rogue's poison");
    if (poisoned != 0) {
        view.clear();
        view.put(poisoned, kMage, 10);
        CHECK(ground::requirement_with(f, g.world, mage, *target, "Rogue_Sap_Advantage", view, kMage, wolf), "the Sap advantage holds at ten stacks of the same Rogue's poison (== 10)");
        view.clear();
        view.put(poisoned, kMage, 9);
        CHECK(!ground::requirement_with(f, g.world, mage, *target, "Rogue_Sap_Advantage", view, kMage, wolf), "...not at nine");
        view.clear();
        view.put(poisoned, kOther, 10);
        CHECK(!ground::requirement_with(f, g.world, mage, *target, "Rogue_Sap_Advantage", view, kMage, wolf), "...and not for ten of another Rogue's");
    }

    // the Rogue's Backstab: its data asks only for the Throw's mark (GetEffectCount), and the by-instigator form is commented out there, so no caster count is read
    const abilities::HitDef* backstab = f.catalog->hit_named("Rogue_Backstab_Recurse_ThrowExplode");
    const content::ExprRow* backstab_req = backstab && backstab->row.requirement_expr != 0 ? f.catalog->expr(backstab->row.requirement_expr) : nullptr;
    CHECKF(backstab_req != nullptr && backstab_req->text.find("GetEffectCount(") != std::string::npos &&
               backstab_req->text.find("//GetEffectCountByRecordAndInstigator") != std::string::npos,
           "the Backstab's second hit reads the Throw's mark and its by-instigator call is commented out in the data (so its requirement is the same for every caster)");

    // a read whose caster is unknown answers none (0): the count of what "nobody" put on the target is none, and no caster's stacks are borrowed
    view.clear();
    view.put(lockout, kMage, 1);
    CHECK(ground::requirement_with(f, g.world, mage, *target, "Ranger_VineField", view, 0, wolf),
          "(a read with no caster answers 0, which the Vine Field's own lockout test reads as no lockout)");
}

AOC_REGISTER_TEST(test_ground_circles_the_casters_own_effects);

// ---- the Vine Field: one cast, one root -------------------------------------------------------------------------------------------------------------------------------
// The caster's own lockout makes the zone root and hurt a target once: its Initial hit puts the lockout on, and its Lingering hit (which asks for the lockout == 0) does not land while
// that lockout lasts.  The same requirement is on the direct hit the client asks for: a second request of it inside the cast finds no target.
void test_ground_circles_the_vine_fields_lockout(Fixture& f) {
    std::printf("the Vine Field: its first hit lands and puts its own lockout on; a second hit asked for in the same cast does not land\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    BardMobWorld bw(f, wolf_path, 1);   // a Ranger
    CHECKF(bw.problem.empty(), "the Ranger and his world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 10, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Ranger");
    if (!wolf) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Ranger_VineField");
    const abilities::HitDef* hit = f.catalog->hit_named("Ranger_VineField");
    const abilities::EffectDef* lockout = f.catalog->effect_named("Ranger_VineField_Lockout");
    CHECK(def && hit && lockout, "the data has the Vine Field, its hit and its lockout");
    if (!def || !hit || !lockout) return;
    bw.rig.know({"Ranger_VineField"});
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : def->hits) {
        if (h.hit_guid != hit->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : def->targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    constexpr double t0 = 100.0;
    TriggerInfo info;   // a cast at the ground names no actor (the client's click point): only the hit's request names the Wolf
    info.target_count = 0;
    const bool cast = bw.rig.pipe->trigger(def->row.guid, bw.rig.next_client_id++, t0, info) == Refusal::None;
    CHECKF(cast, "the Vine Field is cast at the Wolf");
    if (!cast) return;
    const auto first = bw.rig.pipe->hit_request(def->row.guid, targeting, hit->row.guid, 1, 0, t0 + 0.02, {wolf});
    CHECKF(first == AbilityPipeline::HitVerdict::Scheduled, "the client's first request for the hit is taken (%s)", AbilityPipeline::verdict_name(first));
    bard_run(bw, t0, t0 + 0.5);
    const int stacks_after_first = bw.world->effect_stacks(wolf, lockout->row.guid);
    CHECKF(stacks_after_first == 1, "the hit put its own lockout on the Wolf (%d stack(s))", stacks_after_first);
    const auto second = bw.rig.pipe->hit_request(def->row.guid, targeting, hit->row.guid, 1, 0, t0 + 0.55, {wolf});
    const bool taken = second == AbilityPipeline::HitVerdict::Scheduled || second == AbilityPipeline::HitVerdict::Empty;
    CHECKF(taken, "a second request of the same hit inside the cast is answered (%s)", AbilityPipeline::verdict_name(second));
    bard_run(bw, t0 + 0.5, t0 + 1.2);
    size_t landed = 0;
    for (const Seen& s : bw.rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != hit->row.guid || c.targets.empty()) continue;
            for (const auto& t : c.targets) landed += t.component.ObjectId == wolf ? 1 : 0;
        }
    }
    CHECKF(landed == 1, "the Wolf was told of the hit once in the two requests: its own lockout kept the second one off (%zu)", landed);
    CHECKF(bw.world->effect_stacks(wolf, lockout->row.guid) == 1, "the lockout is on it once (%d)", bw.world->effect_stacks(wolf, lockout->row.guid));
    CHECK(bw.rig.pawn.broken.empty(), "(the pawn took every change)");
}

AOC_REGISTER_TEST(test_ground_circles_the_vine_fields_lockout);

// ---- the ground circles: the data, the place, the ticks -----------------------------------------------------------------------------------------------------------------
// The zones' spawn (SpawnTransform, AttachType) and the volume their LingeringHitTargetingInfoId names are in the game data (contentc 44).  Most of these checks still give a zone
// its spawn and volume by hand, the values of its record (test_ground_circles_the_rows_the_checks_give_are_the_data_rows shows they are the data's own), and the reader check below
// shows the version-44 row being read.

namespace ground {

/// A zone row of the shared catalog as a test gave it its spawn and volume: the row goes back as it was when the guard does (the catalog is shared by every test of the program).
class GivenZone {
public:
    GivenZone() = default;
    explicit GivenZone(content::ZoneRow* row) : row_(row), saved_(*row) {}
    GivenZone(const GivenZone&) = delete;
    GivenZone& operator=(const GivenZone&) = delete;
    GivenZone(GivenZone&& other) noexcept : row_(other.row_), saved_(std::move(other.saved_)) { other.row_ = nullptr; }
    ~GivenZone() {
        if (row_) *row_ = saved_;
    }

private:
    content::ZoneRow* row_ = nullptr;
    content::ZoneRow  saved_;
};

/// The zone's spawn and volume as its record holds them (the values of its record in the game data).  Keep the result: it puts the row back.
[[nodiscard]] GivenZone give_zone(const abilities::Catalog& catalog, int64_t zone, const char* origin, const char* attach, double radius, bool no_first_tick = false) {
    auto* row = const_cast<content::ZoneRow*>(catalog.zone(zone));
    if (!row) return GivenZone();
    GivenZone guard(row);
    row->has_spawn = true;
    row->spawn.origin = origin;
    row->spawn.attach = attach;
    row->spawn.no_first_tick = no_first_tick;
    row->volume = content::ZoneVolume{};
    row->volume.shape = "EHitboxShape::SHAPE_Sphere";
    row->volume.radius = radius;
    return guard;
}

/// The zone the ability itself lists (its LingeringEffectsIds), or 0.
int64_t zone_of(Fixture& f, const char* ability) {
    const abilities::AbilityDef* d = f.catalog->ability_named(ability);
    return d && d->zones.size() == 1 ? d->zones.front() : 0;
}

/// Runs the ability runtime and the world to `to` (one tick of each 1/15 s, as the session's world runs).
void run(Rig& rig, combat::CombatWorld& world, double from, double to) {
    for (double now = from; now <= to + 1e-9; now += 1.0 / 15.0) {
        rig.run_to(now);
        world.tick(now);
    }
}

/// The confirmations of `record` whose Source is a zone, with the actors they named, by time.
struct ZoneHit {
    double  at = 0;
    int64_t source = 0;
    std::vector<uint64_t> actors;
};
std::vector<ZoneHit> zone_hits(const Rig& rig, int64_t record) {
    std::vector<ZoneHit> out;
    for (const Seen& s : rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != record || c.source_type != content::kSourceTypeZone || c.targets.empty()) continue;
            ZoneHit h;
            h.at = s.at;
            h.source = c.source_guid;
            for (const auto& t : c.targets) h.actors.push_back(t.component.ObjectId);
            out.push_back(std::move(h));
        }
    }
    return out;
}
size_t zone_hits_on(const std::vector<ZoneHit>& hits, uint64_t actor, double from = -1e18, double to = 1e18) {
    size_t n = 0;
    for (const ZoneHit& h : hits) {
        if (h.at < from || h.at > to) continue;
        for (const uint64_t a : h.actors) n += a == actor ? 1 : 0;
    }
    return n;
}

/// The cast of an ability at a place (`place`) or at `targets`; the zone opens where its data puts it.
struct Cast {
    bool cast = false;
    int32_t client_id = 0;
    double at = 0;
};
Cast cast_zone(Rig& rig, const char* ability, double t, const std::optional<combat::Vec3>& place = std::nullopt, const std::vector<uint64_t>& targets = {}) {
    Cast out;
    const int64_t guid = rig.ability(ability);
    const abilities::AbilityDef* def = rig.catalog->ability(guid);
    if (!def) return out;
    TriggerInfo info;
    info.target_count = static_cast<uint32_t>(targets.size());
    info.target_actors = targets;
    if (place) info.place = AimPlace{place->x, place->y, place->z};
    out.client_id = rig.next_client_id;
    out.cast = rig.pipe->trigger(guid, rig.next_client_id++, t, info) == Refusal::None;
    out.at = t;
    return out;
}

}  // namespace ground

void test_ground_circles_the_vine_field_at_the_click_point(Fixture& f) {
    std::printf("the Vine Field: its circle stands where the cast was aimed, its hit roots once, and its own lockout keeps the next tick off\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    BardMobWorld bw(f, wolf_path, 1);   // a Ranger
    CHECKF(bw.problem.empty(), "the Ranger and his world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    const int64_t zone = ground::zone_of(f, "Ranger_VineField");
    const abilities::HitDef* hit = f.catalog->hit_named("Ranger_VineField");
    const abilities::EffectDef* root = f.catalog->effect_named("Status_Rooted");
    const abilities::EffectDef* lockout = f.catalog->effect_named("Ranger_VineField_Lockout");
    CHECK(zone != 0 && hit && root && lockout, "the data has the Vine Field's zone, hit, root and lockout");
    if (zone == 0 || !hit || !root || !lockout) return;
    const auto given_zone = ground::give_zone(*bw.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 650.0);
    const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 10, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Ranger");
    if (!wolf) return;
    CHECK(bw.world->fighter_of(wolf) != nullptr, "the Wolf fights");
    bw.rig.know({"Ranger_VineField"});
    const double t0 = 100.0;
    const double health_before = bw.world->health_of(wolf);
    const ground::Cast cast = ground::cast_zone(bw.rig, "Ranger_VineField", t0, combat::Vec3{600, 0, 1000});
    CHECKF(cast.cast, "the Vine Field is cast at the ground by the Wolf");
    if (!cast.cast) return;
    ground::run(bw.rig, *bw.world, t0, t0 + 0.6);
    // the actor opened where the cast was aimed, not at the caster
    const bootstrap::ZoneOp* open = nullptr;
    for (const Seen& s : bw.rig.log)
        for (const bootstrap::ZoneOp& z : s.zones)
            if (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == zone) open = &z;
    CHECKF(open != nullptr, "the zone's actor opened");
    if (!open) return;
    CHECKF(open->placed && std::fabs(open->x - 600.0) < 1.0 && std::fabs(open->y) < 1.0, "at the click point (%.0f, %.0f), not at the caster (0, 0)", open->x, open->y);
    CHECK(!open->attached, "the record's AttachType None: the actor stands on its own");
    CHECK(open->ability == bw.rig.ability("Ranger_VineField") && open->zone == zone, "its SourceRecordGuid is the ability and its LingeringEffectId the zone");
    // the first look: the Wolf is inside and takes the Initial hit (damage, its own lockout and the root)
    CHECKF(bw.world->effect_stacks(wolf, lockout->row.guid) == 1, "the Wolf took the zone's Initial hit: its own lockout is on it (%d stack)", bw.world->effect_stacks(wolf, lockout->row.guid));
    CHECKF(bw.world->health_of(wolf) < health_before, "...and it lost Health (%.1f -> %.1f)", health_before, bw.world->health_of(wolf));
    const world::WorldActor* actor = bw.w.registry.find(wolf);
    CHECK(actor && (cc_now(*actor).flags & content::kCcRoot) != 0, "the Wolf is rooted (the root hit's requirement holds on a target without the lockout)");
    const double after_first = bw.world->health_of(wolf);
    const size_t hits_first = ground::zone_hits(bw.rig, hit->row.guid).size();
    // the next tick: its Lingering hit asks the same requirement, and the caster's own lockout is on the Wolf now
    ground::run(bw.rig, *bw.world, t0 + 0.6, t0 + 1.8);
    CHECKF(ground::zone_hits(bw.rig, hit->row.guid).size() == hits_first, "the zone's tick 1 s later lands nothing while its own lockout is on the Wolf (%zu confirmations before, %zu after)",
           hits_first, ground::zone_hits(bw.rig, hit->row.guid).size());
    CHECKF(std::fabs(bw.world->health_of(wolf) - after_first) < 1e-6, "...and the Wolf's Health does not move (%.1f)", bw.world->health_of(wolf));
    // its life is over at 8 s: the actor fades and closes
    ground::run(bw.rig, *bw.world, t0 + 1.8, t0 + 10.0);
    bool faded = false, closed = false;
    for (const Seen& s : bw.rig.log) {
        for (const bootstrap::ZoneOp& z : s.zones) {
            if (z.zone != zone) continue;
            faded = faded || z.kind == bootstrap::ZoneOp::Kind::Fade;
            closed = closed || z.kind == bootstrap::ZoneOp::Kind::Close;
        }
    }
    CHECKF(faded && closed, "the actor faded out and closed at the end of the zone's life (fade %d, close %d)", faded, closed);
    CHECK(bw.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_the_vine_field_at_the_click_point);

void test_ground_circles_the_lament_under_the_bard(Fixture& f) {
    std::printf("the Chilling Lament: its zone stands under the Bard and no circle is sent to the client\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    BardMobWorld bw(f, wolf_path, 2);   // a Bard
    CHECKF(bw.problem.empty(), "the Bard and his world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    const int64_t zone = ground::zone_of(f, "Bard_Song_Lament");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    CHECK(zone != 0 && chilled, "the data has the Lament's zone and Chilled");
    if (zone == 0 || !chilled) return;
    const auto given_zone = ground::give_zone(*bw.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 300.0, true);
    // the cast named a Wolf 8 m away; the zone is his own cone, so it stands at him (0, 0): the Wolf 2 m away is in it, the one 8 m away is not
    const uint64_t near_wolf = bw.w.spawn(wolf_path, 200, 0, 10, 0);
    const uint64_t far_wolf = bw.w.spawn(wolf_path, 800, 0, 10, 0);
    CHECK(near_wolf != 0 && far_wolf != 0, "a Wolf 2 m from the Bard and one 8 m from him");
    if (!near_wolf || !far_wolf) return;
    bw.rig.know({"Bard_Song_Lament"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(bw.rig, "Bard_Song_Lament", t0, std::nullopt, {far_wolf});
    CHECKF(cast.cast, "the Lament is cast at the Wolf 8 m away");
    if (!cast.cast) return;
    ground::run(bw.rig, *bw.world, t0, t0 + 0.8);
    size_t opens = 0;
    for (const Seen& s : bw.rig.log)
        for (const bootstrap::ZoneOp& z : s.zones)
            opens += (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == zone) ? 1 : 0;
    CHECKF(opens == 0, "the server sends no actor for the Lament's blue circle, which the game does not show (%zu open(s))", opens);
    CHECKF(bw.world->effect_stacks(near_wolf, chilled->row.guid) >= 1, "the Wolf 2 m away took the zone's Chilled hit, so the zone stands at the Bard (%d stack)",
           bw.world->effect_stacks(near_wolf, chilled->row.guid));
    CHECKF(bw.world->effect_stacks(far_wolf, chilled->row.guid) == 0, "the Wolf 8 m away the cast named took no Chilled hit from the zone (%d stack)",
           bw.world->effect_stacks(far_wolf, chilled->row.guid));
    CHECK(bw.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_the_lament_under_the_bard);

void test_ground_circles_the_chains_tick_each_second(Fixture& f) {
    std::printf("the Chains of Restraint: the zone lands its damage tick every second on the mobs that stand in it, and on no mob outside\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 3, {});   // a Cleric
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
    const uint64_t inside = g.wolf(400, 0), outside = g.wolf(3000, 0);
    CHECK(inside != 0 && outside != 0, "a Wolf 4 m from the Cleric and one 30 m away");
    if (!inside || !outside) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    const double t0 = 100.0;
    // the zone stands under the Cleric: a Wolf that comes for him stays in it, so the ticks have someone to land on for the zone's whole life
    const ground::Cast cast = ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{0, 0, 1000});
    CHECKF(cast.cast, "the Chains are cast at the ground by the Cleric");
    if (!cast.cast) return;
    ground::run(g.rig, g.world, t0, t0 + 7.5);
    const std::vector<ground::ZoneHit> hits = ground::zone_hits(g.rig, damage->row.guid);
    const size_t on_inside = ground::zone_hits_on(hits, inside), on_outside = ground::zone_hits_on(hits, outside);
    CHECKF(on_inside >= 7, "the near Wolf took the zone's damage at least once a second for its life (%zu confirmations)", on_inside);
    CHECKF(on_outside == 0, "the Wolf 30 m away took none (%zu)", on_outside);
    CHECKF(hits.size() == on_inside, "every confirmation named a Wolf in the zone (%zu in all)", hits.size());
    double prev = -1;
    bool each_second = true;
    for (const ground::ZoneHit& h : hits) {
        if (prev >= 0 && std::fabs(h.at - prev - 1.0) > 0.2) each_second = false;
        prev = h.at;
    }
    CHECK(each_second, "the hits came about one second apart");
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_the_chains_tick_each_second);

void test_ground_circles_a_mob_walks_in_and_out(Fixture& f) {
    std::printf("a zone's hits come and go with the mobs that stand in it: a Wolf that walks in is hit from the next tick, one that walks out is not\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
    const uint64_t wolf = g.wolf(2000, 0);
    CHECK(wolf != 0, "a Wolf 20 m from the Cleric, outside the ground the cast reaches");
    if (!wolf) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{1000, 0, 1000});
    CHECKF(cast.cast, "the Chains are cast at a place 10 m ahead");
    if (!cast.cast) return;
    ground::run(g.rig, g.world, t0, t0 + 1.0);
    CHECKF(ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf) == 0, "it stands outside: the zone lands nothing on it");
    g.w.npcs->warp(wolf, 1000, 0, 1000.0);   // it walks in
    ground::run(g.rig, g.world, t0 + 1.0, t0 + 3.0);
    const size_t inside_hits = ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf);
    CHECKF(inside_hits >= 1, "it walks in: the zone's ticks hit it (%zu confirmations)", inside_hits);
    g.w.npcs->warp(wolf, 2000, 0, 1000.0);   // and out
    const size_t before_out = ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf);
    // the window is short on purpose: the Wolf it just hit comes for the Cleric, and a longer run would walk it back into the zone
    ground::run(g.rig, g.world, t0 + 3.0, t0 + 4.2);
    CHECKF(ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf) == before_out, "it walks out: no more hits (the %zu it took stand)", before_out);
}
AOC_REGISTER_TEST(test_ground_circles_a_mob_walks_in_and_out);

void test_ground_circles_two_players_zones_side_by_side(Fixture& f) {
    std::printf("two players' zones stand side by side: each lands its own hits, and a friendly player in one takes no enemy hit\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the first Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    Rig second(3, 25, {});
    CHECKF(second.ok(), "the second Cleric: %s", second.problem.c_str());
    if (!second.ok()) return;
    combat_rig::FakePlayer other(second, 78, {200, 0, 1098});
    g.world.add_player(&other);
    combat::WorldHitPort port2(g.world, g.w.registry, other.fighter());
    second.pipe->set_combat(&port2);
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
    const uint64_t wolf = g.wolf(800, 0);
    CHECK(wolf != 0, "a Wolf 8 m from the first Cleric");
    if (!wolf) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    second.know({"Cleric_ChainsOfRestraint"});
    const double t0 = 100.0;
    const ground::Cast a = ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{600, 0, 1000});
    const ground::Cast b = ground::cast_zone(second, "Cleric_ChainsOfRestraint", t0 + 0.1, combat::Vec3{1000, 0, 1000});
    CHECKF(a.cast && b.cast, "both Clerics cast their Chains (first %d, second %d)", a.cast, b.cast);
    if (!a.cast || !b.cast) return;
    for (double now = t0; now <= t0 + 4.0 + 1e-9; now += 1.0 / 15.0) {
        g.rig.run_to(now);
        second.run_to(now);
        g.world.tick(now);
    }
    const std::vector<ground::ZoneHit> first_hits = ground::zone_hits(g.rig, damage->row.guid);
    const std::vector<ground::ZoneHit> second_hits = ground::zone_hits(second, damage->row.guid);
    CHECKF(ground::zone_hits_on(first_hits, wolf) >= 1, "the first Cleric's zone hit the Wolf (%zu)", ground::zone_hits_on(first_hits, wolf));
    CHECKF(ground::zone_hits_on(second_hits, wolf) >= 1, "the second Cleric's zone hit it too (%zu)", ground::zone_hits_on(second_hits, wolf));
    CHECKF(ground::zone_hits_on(first_hits, other.actor_id()) == 0 && ground::zone_hits_on(second_hits, other.actor_id()) == 0,
           "the other player, a friendly, took no enemy hit from either zone");
    CHECK(g.rig.pawn.broken.empty() && second.pawn.broken.empty(), "(both pawns took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_two_players_zones_side_by_side);

void test_ground_circles_the_same_cast_twice(Fixture& f) {
    std::printf("the same cast twice leaves two zones, each with its own actor and its own ticks\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
    const uint64_t wolf = g.wolf(600, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Cleric");
    if (!wolf) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    const double t0 = 100.0;
    CHECK(ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{600, 0, 1000}).cast, "the first cast");
    ground::run(g.rig, g.world, t0, t0 + 2.2);
    g.rig.pipe->clear_cooldowns(t0 + 2.2);
    g.rig.set_current(content::kGuidMana, g.rig.pawn.stat(content::kGuidMaxMana));
    CHECK(ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0 + 2.3, combat::Vec3{600, 0, 1000}).cast, "the second cast, after the first one is over and its cooldown cleared");
    ground::run(g.rig, g.world, t0 + 2.2, t0 + 6.0);
    std::set<int64_t> ids;
    for (const Seen& s : g.rig.log)
        for (const bootstrap::ZoneOp& z : s.zones)
            if (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == zone) ids.insert(z.id);
    CHECKF(ids.size() == 2, "two zone actors opened, each numbered on its own (%zu)", ids.size());
    CHECKF(ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf) >= 2, "both zones hit the Wolf (%zu hits)", ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf));
}
AOC_REGISTER_TEST(test_ground_circles_the_same_cast_twice);

void test_ground_circles_a_zone_ends_with_its_caster(Fixture& f) {
    std::printf("a zone ends with the caster's death, as everything a player put on the creatures does: no more hits after it, its actor fades and closes, and a respawn does not bring it back\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
    const uint64_t wolf = g.wolf(600, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Cleric");
    if (!wolf) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    const double t0 = 100.0;
    CHECK(ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{600, 0, 1000}).cast, "the Chains are cast");
    ground::run(g.rig, g.world, t0, t0 + 3.0);
    const size_t before = ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf);
    CHECKF(before >= 2, "the zone has hit the Wolf while he lives (%zu hits)", before);
    g.rig.pipe->set_dead(true);   // the caster dies: the cast is over and so is what he put on the creatures, the zone included (the world drops a dead player's effects at once)
    ground::run(g.rig, g.world, t0 + 3.0, t0 + 6.0);
    const size_t after = ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf);
    CHECKF(after == before, "the zone lands nothing after his death (%zu hits before, %zu after)", before, after);
    size_t faded = 0, closed = 0;
    for (const Seen& s : g.rig.log) {
        for (const bootstrap::ZoneOp& z : s.zones) {
            if (z.zone != zone) continue;
            faded += z.kind == bootstrap::ZoneOp::Kind::Fade ? 1 : 0;
            closed += z.kind == bootstrap::ZoneOp::Kind::Close ? 1 : 0;
        }
    }
    CHECKF(faded == 1 && closed == 1, "its actor fades out and closes at once, long before its life is over (%zu faded, %zu closed by 3 s after the death)", faded, closed);
    g.rig.pipe->set_dead(false);   // the respawn
    ground::run(g.rig, g.world, t0 + 6.0, t0 + 12.0);
    CHECKF(ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf) == before, "a respawn does not bring the zone back (%zu hits in all)", ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf));
}
AOC_REGISTER_TEST(test_ground_circles_a_zone_ends_with_its_caster);

// ---- a zone's own hits and the confirmation ---------------------------------------------------------------------------------------------------------------------------
// A zone's hit is the server's own, not one the client asks for: the client draws a floating number from a confirmation's stat mods, so a zone hit with no
// stat mod to draw is applied but not confirmed (the retail replies confirm the zone's damage hits and not its snare, its silence or its burning).
void test_ground_circles_a_zone_confirms_only_the_hits_that_show_something(Fixture& f) {
    std::printf("a zone's own hits: the damage hit is confirmed with its number; the snare and the burning, which have no stat mod to draw, land without a confirmation\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;

    // the Chains: the OnBeginOverlap Snare applies Status_Snared and has no stat mod; the damage hit has Health
    {
        Forge g(f, 3, {});   // a Cleric
        CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return;
        const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
        const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
        const abilities::HitDef* snare = f.catalog->hit_named("Cleric_ChainsOfRestraint_Snare");
        const abilities::EffectDef* snared = f.catalog->effect_named("Status_Snared");
        CHECK(zone != 0 && damage && snare && snared, "the data has the Chains' zone, its damage, its Snare and Snared");
        if (zone == 0 || !damage || !snare || !snared) return;
        const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
        const uint64_t wolf = g.wolf(400, 0);
        CHECK(wolf != 0, "a Wolf 4 m from the Cleric");
        if (!wolf) return;
        g.rig.know({"Cleric_ChainsOfRestraint"});
        const double t0 = 100.0;
        const ground::Cast cast = ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{0, 0, 1000});
        CHECKF(cast.cast, "the Chains are cast at the ground under the Cleric");
        if (!cast.cast) return;
        ground::run(g.rig, g.world, t0, t0 + 0.6);
        CHECKF(ground::zone_hits(g.rig, damage->row.guid).size() >= 1, "the zone's damage hit is confirmed, with the number the client draws (%zu)", ground::zone_hits(g.rig, damage->row.guid).size());
        CHECKF(ground::zone_hits(g.rig, snare->row.guid).empty(), "the Snare has no stat mod: no confirmation for it (%zu)", ground::zone_hits(g.rig, snare->row.guid).size());
        CHECKF(g.world.effect_stacks(wolf, snared->row.guid) >= 1, "...and the Wolf is still snared by it (%d stack)", g.world.effect_stacks(wolf, snared->row.guid));
        CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
    }

    // the Magma Field: the Burning hit's stat mods are the status' own (not drawn); the field's damage hit is confirmed
    {
        Forge m(f, 8, {});   // a Mage with no talent: the base field opens
        CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const abilities::HitDef* spawn = f.catalog->hit_named("Mage_MagmaField_SpawnField");
        int64_t zone = 0;
        if (spawn) {
            for (const int64_t guid : spawn->zones) {
                const content::ZoneRow* row = f.catalog->zone(guid);
                if (row && row->name == "Mage_MagmaField_LingeringField") zone = guid;
            }
        }
        const abilities::HitDef* damage = f.catalog->hit_named("Mage_MagmaField_LingeringHit");
        const abilities::HitDef* burning = f.catalog->hit_named("Mage_MagmaField_LingeringHitBurning");
        const abilities::EffectDef* burning_stat = f.catalog->effect_named("Status_BurningStat");
        CHECK(zone != 0 && damage && burning && burning_stat, "the data has the Magma Field's base zone, its damage, its Burning and BurningStat");
        if (zone == 0 || !damage || !burning || !burning_stat) return;
        const auto given_zone = ground::give_zone(*m.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
        const uint64_t wolf = m.wolf(300, 0);
        CHECK(wolf != 0, "a Wolf 3 m from the Mage");
        if (!wolf) return;
        m.rig.know({"Mage_MagmaField"});
        const double t0 = 100.0;
        const ground::Cast cast = ground::cast_zone(m.rig, "Mage_MagmaField", t0, combat::Vec3{300, 0, 1000});
        CHECKF(cast.cast, "the Magma Field is cast at the ground under the Wolf");
        if (!cast.cast) return;
        ground::run(m.rig, m.world, t0, t0 + 2.5);
        CHECKF(ground::zone_hits(m.rig, damage->row.guid).size() >= 1, "the field's damage hit is confirmed (%zu)", ground::zone_hits(m.rig, damage->row.guid).size());
        CHECKF(ground::zone_hits(m.rig, burning->row.guid).empty(), "the Burning hit's only stat mods are the status' own: no confirmation (%zu)", ground::zone_hits(m.rig, burning->row.guid).size());
        CHECKF(m.world.effect_stacks(wolf, burning_stat->row.guid) >= 1, "...and the Wolf still burns (%d stack)", m.world.effect_stacks(wolf, burning_stat->row.guid));
        CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
    }
}
AOC_REGISTER_TEST(test_ground_circles_a_zone_confirms_only_the_hits_that_show_something);

// ---- the reader: a zone row of contentc version 45 --------------------------------------------------------------------------------------------------------------------
// The extraction the draft reads carries the spawn column (contentc 44); the check makes a 44 file and a 43 file of the same tables, whatever version the real file
// is (the column is added to a 43 one and taken off a 44 one), and reads both.
bool zone_table_has_spawn(sqlite3* db) {
    bool found = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(ab_zone)", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(st, 1);
            if (text && std::string(reinterpret_cast<const char*>(text)) == "spawn") found = true;
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
}

bool file_zone_table_has_spawn(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const bool found = zone_table_has_spawn(db);
    sqlite3_close(db);
    return found;
}

void test_ground_circles_the_zone_row_the_reader_reads(Fixture& f) {
    std::printf("the zone row of contentc 44: its spawn and the volume its targeting record names are read, and a 43 file is still read\n");
    (void)f;
    // (a folder of this run's own: two runs of the program at the same time, or a file a run that died left open, must not share the files)
    const std::string dir =
        (std::filesystem::temp_directory_path() / ("aoc_zone_reader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::filesystem::create_directories(dir);
    const std::string db44 = dir + "/zone44.db", db43 = dir + "/zone43.db";
    {
        std::error_code gone;
        for (const std::string& p : {db44, db43}) std::filesystem::remove(p, gone);
    }
    const auto make_db = [&](const std::string& path, bool spawn, const char* version) {
        std::filesystem::remove(path);
        sqlite3* real = nullptr;
        if (sqlite3_open_v2(real_db_path().c_str(), &real, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return false;
        std::vector<std::string> creates;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(real, "SELECT sql FROM sqlite_master WHERE type = 'table'", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char* text = sqlite3_column_text(st, 0);
                if (text) creates.emplace_back(reinterpret_cast<const char*>(text));
            }
        }
        if (st) sqlite3_finalize(st);
        sqlite3_close(real);
        sqlite3* db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;
        bool ok = true;
        for (const std::string& sql : creates) ok = ok && sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        const bool has_spawn = zone_table_has_spawn(db);
        if (spawn && !has_spawn) ok = ok && sqlite3_exec(db, "ALTER TABLE ab_zone ADD COLUMN spawn TEXT NOT NULL DEFAULT ''", nullptr, nullptr, nullptr) == SQLITE_OK;
        if (!spawn && has_spawn) ok = ok && sqlite3_exec(db, "ALTER TABLE ab_zone DROP COLUMN spawn", nullptr, nullptr, nullptr) == SQLITE_OK;
        const std::string meta = std::string("INSERT INTO meta (key, value) VALUES ('schema_version', '1'), ('contentc.version', '") + version + "')";
        ok = ok && sqlite3_exec(db, meta.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        const std::string spawn_json =
            R"({"origin":"EAbilitySystemOrigin::HitLocation","orientation":"EAbilitySystemOrientation::SourceToOrigin","relative_location":[100,0,-5],"attach":"EAbilitySystemAttachType::None","class":"/Script/GameSystemsPlugin.AoCAbilityLingeringEffect","deactivate_fx":3,"net_cull":3000,"fx":5,"no_first_tick":true,"scale":[1,1,1]})";
        std::string zone = "INSERT INTO ab_zone (guid, name, life_time, tick_rate, use_global_tick_rate, charges, delay_time, delay_type, speed, expire_after_hits_expr, expire_if_expr, targeting_guid";
        if (spawn) zone += ", spawn";
        zone += ") VALUES (424242, 'Test_Zone', 6.1, 2, 1, 1, 0, 'ELingeringZoneTriggerDelay::DELAY_Time', 0, 0, 0, 424243";
        if (spawn) zone += ", '" + spawn_json + "'";
        zone += ")";
        ok = ok && sqlite3_exec(db, zone.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        ok = ok && sqlite3_exec(db, "INSERT INTO ab_targeting_def (guid, name, shape, radius, angle, volumes, lock_on, sorting, contextual) "
                                    "VALUES (424243, 'Test_LingerTarget', 'EHitboxShape::SHAPE_Sphere', 300, 90, '[{\"bEnableLOSCheck\":true}]', '{}', '[]', '{}')",
                                nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(db);
        return ok;
    };
    CHECK(make_db(db44, true, "44") && make_db(db43, false, "43"), "the two files are made from the draft's own ability tables");
    CHECK(file_zone_table_has_spawn(db44) && !file_zone_table_has_spawn(db43), "the 44 file holds the zones' spawn column and the 43 file holds none");
    std::string err;
    {
        auto d44 = content::AbilityContent::open(db44, err);
        CHECKF(d44.has_value(), "a contentc 44 file opens: %s", err.c_str());
        if (d44) {
            CHECK(d44->has_zone_spawn(), "it says it holds the zones' spawn");
            auto z = d44->zone(424242, err);
            CHECKF(z.has_value(), "its zone reads: %s", err.c_str());
            if (z) {
                CHECKF(z->has_spawn && z->spawn.origin == "EAbilitySystemOrigin::HitLocation", "the spawn's origin is read (%s)", z->spawn.origin.c_str());
                CHECKF(z->spawn.attach == "EAbilitySystemAttachType::None" && z->spawn.deactivate_fx == 3.0 && z->spawn.no_first_tick && z->spawn.scale_x == 1.0,
                       "its attachment, fade time and first-tick rule are read (%s, %.1f s, first tick skipped %d)", z->spawn.attach.c_str(), z->spawn.deactivate_fx, z->spawn.no_first_tick);
                CHECKF(z->spawn.rel_x == 100.0 && z->spawn.rel_y == 0.0 && z->spawn.rel_z == -5.0, "its offset from its origin is read (%.0f, %.0f, %.0f)", z->spawn.rel_x, z->spawn.rel_y, z->spawn.rel_z);
                CHECKF(z->life_time == 6.1 && z->tick_rate == 2.0 && z->targeting_guid == 424243, "its life, tick and targeting record are the row's (%.1f s, %.1f s, %lld)",
                       z->life_time, z->tick_rate, static_cast<long long>(z->targeting_guid));
            }
            auto t = d44->targeting_def(424243, err);
            CHECKF(t.has_value() && t->shape == "EHitboxShape::SHAPE_Sphere" && t->radius == 300.0, "the record its volume comes from is read (a %s of %.0f cm)",
                   t ? t->shape.c_str() : "?", t ? t->radius : 0.0);
        }
    }
    {
        auto d43 = content::AbilityContent::open(db43, err);
        CHECKF(d43.has_value(), "a contentc 43 file (no spawn column) still opens: %s", err.c_str());
        if (d43) {
            CHECK(!d43->has_zone_spawn(), "and says it holds no spawn");
            auto z = d43->zone(424242, err);
            CHECKF(z.has_value() && !z->has_spawn && z->spawn.origin.empty(), "its zone reads without one (the runtime then keeps the zone at the caster)");
        }
    }
    std::error_code ec;
    std::filesystem::remove(db44, ec);
    std::filesystem::remove(db43, ec);
    std::filesystem::remove(dir, ec);
}
AOC_REGISTER_TEST(test_ground_circles_the_zone_row_the_reader_reads);
