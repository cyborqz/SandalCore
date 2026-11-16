// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_bard_saga_tests.inc: the Bard's Saga, its variants and the projectiles that carry its heals and blessings to allies
//  (included from combat_registered.inc inside test_worldd_combat's anonymous namespace)
//
//    the variant   the montage's AnimExpression picks one of six, from the notes the Bard carries: three of one theme (Apocalypse,
//                  Return, Pantheon), one of each (Creation), or two and one with the Volume II passive (Horrors, Heroes).  The notes
//                  themselves are carried as three instances of the one note effect, the Bard_Notes group's rule (a fourth takes the
//                  oldest's place), which the stories' note hits give.
//    the landing   for a cast at nobody the client asks for the launcher hit; the server fires a projectile at each ally the cast's
//                  targeting record picks (injured party members first, within its sphere, up to the hit's targets: the caster alone
//                  when solo) and confirms the landed hit with the launcher's, in one event (one packet), a third of a second after the
//                  request at 0 m plus the flight; the landed confirmation's source is the projectile.
//    the allies    another player's pawn takes the heal and the effects through the world (its session applies them to its own pawn):
//                  its Health rises and its client holds the effect item.
//    the zones     the Silent Pantheon's four 1 m blessing zones open where their projectiles land (the data gates each spawn hit on
//                  RandDouble() < 0.5); the Saga's own zone opens at the caster with the cast.
// ============================================================================

// This file's checks print their passing line too, so the proof of the eight angles can quote the [ok ] line as well as the [FAIL] one.
#undef CHECK
#undef CHECKF
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define CHECKF(cond, ...) do { \
    char saga_line[1024]; \
    std::snprintf(saga_line, sizeof saga_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", saga_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", saga_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace saga {

using namespace aoc::worldd;

constexpr uint64_t kCaster = 77, kFirstAlly = 78;
constexpr double   kGroundZ = 1098.0;

// The six montages' soft paths (ab_montage, the rows of the Saga).
constexpr const char* kJoyousness = "/Game/Characters/Animations/00_ABLT/Bard/ABLT_Bard_Saga_Joyousness.ABLT_Bard_Saga_Joyousness";
constexpr const char* kFlameAndFang = "/Game/Characters/Animations/00_ABLT/Bard/ABLT_Bard_Saga_FlameAndFang.ABLT_Bard_Saga_FlameAndFang";
constexpr const char* kPantheon2 = "/Game/Characters/Animations/00_ABLT/Bard/ABLT_Bard_Saga_Pantheon2.ABLT_Bard_Saga_Pantheon2";
constexpr const char* kExaggerate = "/Game/Characters/Animations/00_ABLT/Bard/CM_M_ABLT_Saga_Exaggerate.CM_M_ABLT_Saga_Exaggerate";
constexpr const char* kHorrors = "/Game/Characters/Animations/00_ABLT/Bard/CM_M_ABLT_Saga_HorrorsUnknown.CM_M_ABLT_Saga_HorrorsUnknown";
constexpr const char* kHero = "/Game/Characters/Animations/00_ABLT/Bard/CM_M_ABLT_Saga_Hero.CM_M_ABLT_Saga_Hero";

// The recorded Joyousness montage's NetGUID (ab_montage); the other five rows have none, so the server mints one from the path.
constexpr uint64_t kJoyousnessNetGuid = 16213574640567394797ull;
// The Saga's own zone (ab_ability.LingeringEffectsIds) and the four Pantheon blessing zones (their hits' LingeringZonesIds).
constexpr int64_t kSagaZone = 6064631959812441886ll;
constexpr int64_t kPantheonZone[4] = {6064632066881492333ll, 6064632467954607296ll, 6064632467954934977ll, 6064632467955393730ll};

/// One caster and a party of allies in one world, the caster's and each ally's ability runtime attached to it as a session's is.
struct Band {
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

    /// `xs`: where each ally stands (metres along +X from the caster); `party`: they are of the caster's party; `invited`: which of them
    /// are (all of them when null).  `caster_skills`, `ally_skills`: the talents the caster and the allies hold.
    Band(Fixture& f, const std::vector<double>& xs = {}, bool party = true, const std::vector<std::string>& caster_skills = {},
         const std::vector<std::string>& ally_skills = {}, uint64_t seed = 7)
        : caster(2, 25, caster_skills, true, seed),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          caster_port(caster, kCaster, {0, 0, kGroundZ}),
          caster_hit(world, w.registry, caster_port.fighter()) {
        if (!caster.ok()) {
            problem = "the caster: " + caster.problem;
            return;
        }
        world.add_player(&caster_port);
        caster.pipe->set_combat(&caster_hit);
        for (size_t i = 0; i < xs.size(); ++i) {
            auto rig = std::make_unique<Rig>(2, 25, ally_skills, true, seed + i + 1);
            if (!rig->ok()) {
                problem = "an ally: " + rig->problem;
                return;
            }
            const uint64_t id = kFirstAlly + i;
            auto port = std::make_unique<combat_rig::FakePlayer>(*rig, id, combat::Vec3{xs[i] * 100.0, 0, kGroundZ});
            if (party) port->party_with = kCaster;
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
};

/// The item of the Saga on the pawn (null until its item step or after its montage ends).
const bootstrap::ActiveAbilityItem* saga_item(Rig& r) {
    const int64_t saga = r.ability("Bard_Saga");
    for (const auto& [key, item] : r.pawn.abilities) {
        if (item.ability_guid == saga) return &item;
    }
    return nullptr;
}

/// The name of the note hit a story applies ("Bard_ApplyComedy" and the like; "" when the story has none).
std::string story_note_hit(Rig& r, const char* story) {
    const abilities::AbilityDef* def = r.catalog->ability_named(story);
    if (!def) return {};
    for (const content::AbilityHitRow& h : def->hits) {
        const abilities::HitDef* hit = r.catalog->hit(h.hit_guid);
        if (hit && hit->row.name.rfind("Bard_Apply", 0) == 0) return hit->row.name;
    }
    return {};
}

/// A story cast and its note hit asked for, as the client does: the note effect goes on the Bard (`t` is the trigger's time).
bool story_note(Rig& r, const char* story, double t) {
    const std::string note = story_note_hit(r, story);
    if (note.empty()) return false;
    r.asks = false;
    r.know({story});
    const Refusal refusal = r.trigger(story, t);
    if (refusal != Refusal::None) {
        std::printf("    (the story %s at %.2f s was refused: %s)\n", story, t, refusal_name(refusal));
        return false;
    }
    r.hit_request(story, note.c_str(), t + 0.02);
    r.run_to(t + 2.5);   // (the story's cost step is 1.573 s after its trigger: its note lands just after)
    r.pipe->clear_cooldowns(t + 2.5);
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    return true;
}

/// How many effect items of `guid` the pawn holds (the note instances, one item each).
size_t effect_items(Rig& r, int64_t guid) {
    size_t n = 0;
    for (const auto& [key, item] : r.pawn.effects) n += item.effect_guid == guid ? 1 : 0;
    return n;
}

/// The confirmations of a hit record across what the rig has seen (one event may hold the launcher's and the landed hit's).
std::vector<const bootstrap::HitConfirm*> confirms_of(Rig& r, int64_t record) {
    std::vector<const bootstrap::HitConfirm*> out;
    for (const Seen& s : r.log)
        for (const bootstrap::HitConfirm& c : s.confirmations)
            if (c.record_guid == record) out.push_back(&c);
    return out;
}

/// The zone actors the rig opened, with the zone record and where each stood.
std::vector<bootstrap::ZoneOp> zones_opened(Rig& r, int64_t zone) {
    std::vector<bootstrap::ZoneOp> out;
    for (const Seen& s : r.log)
        for (const bootstrap::ZoneOp& z : s.zones)
            if (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == zone) out.push_back(z);
    return out;
}

/// The landed confirmations the caster sent for an ally actor: a target entry names the actor's pawn, and a confirmation with no target is a hit on the
/// caster's own pawn.  `self` is the caster's actor id.
size_t landed_on(Rig& r, int64_t hit, uint64_t actor, uint64_t self) {
    size_t n = 0;
    for (const bootstrap::HitConfirm* c : confirms_of(r, hit)) {
        if (c->targets.empty()) {
            n += actor == self ? 1 : 0;
            continue;
        }
        for (const bootstrap::HitTargetOut& t : c->targets) n += t.component.ObjectId == actor ? 1 : 0;
    }
    return n;
}

/// The Saga trigger's item step and the montage it chose: the item's soft path ("" when no item came).
std::string saga_montage(Rig& r, double t) {
    r.asks = false;
    r.know({"Bard_Saga"});
    if (r.trigger("Bard_Saga", t) != Refusal::None) return {};
    r.run_to(t + 0.45);
    const bootstrap::ActiveAbilityItem* item = saga_item(r);
    return item ? item->montage_path : std::string();
}

// ---- the six variants -------------------------------------------------------------------------------------------------------------

void test_saga_the_notes_choose_the_variant(Fixture& f) {
    std::printf("the Saga's variant: the montage's AnimExpression picks three of one theme, one of each, or two and one with the Volume II passive\n");
    // three of one theme (the 3-of-a-theme variants), one of each (Creation), and two and one with the passive (Horrors, Heroes)
    struct Case {
        const char* what;
        const char* stories[3];
        bool        volume_two;
        const char* montage;
    };
    const Case cases[] = {
        {"three Comedy (The Return to Verra)", {"Bard_Story_Joy", "Bard_Story_Joy", "Bard_Story_Joy"}, false, kJoyousness},
        {"three Tragedy (The Apocalypse)", {"Bard_Story_Conflict", "Bard_Story_Conflict", "Bard_Story_Conflict"}, false, kFlameAndFang},
        {"three Mystery (The Silent Pantheon)", {"Bard_Story_Wonder", "Bard_Story_Wonder", "Bard_Story_Wonder"}, false, kPantheon2},
        {"one Comedy, one Tragedy, one Mystery (The Myth of Creation)", {"Bard_Story_Joy", "Bard_Story_Conflict", "Bard_Story_Wonder"}, false, kExaggerate},
        {"two Tragedy and one Mystery (The Horrors of the Void)", {"Bard_Story_Conflict", "Bard_Story_Conflict", "Bard_Story_Wonder"}, true, kHorrors},
        {"two Mystery and one Comedy (Heroes of Ages Past)", {"Bard_Story_Wonder", "Bard_Story_Wonder", "Bard_Story_Joy"}, true, kHero},
    };
    for (const Case& c : cases) {
        Rig r(2, 25, c.volume_two ? std::vector<std::string>{"Bard_Passive_Saga_Vol2"} : std::vector<std::string>());
        CHECKF(r.ok(), "%s: the rig (%s)", c.what, r.problem.c_str());
        if (!r.ok()) continue;
        bool notes = true;
        for (int i = 0; i < 3; ++i) notes = notes && story_note(r, c.stories[i], 5.0 + 3.0 * static_cast<double>(i));
        CHECKF(notes, "%s: the three stories' note hits land on the Bard", c.what);
        const std::string path = saga_montage(r, 20.0);
        CHECKF(path == c.montage, "%s: the Saga plays %s", c.what, path.empty() ? "(no item)" : path.c_str());
        const bootstrap::ActiveAbilityItem* item = saga_item(r);
        CHECKF(item && item->montage_guid != 0, "%s: the item carries the montage's NetGUID (%llu)", c.what, item ? static_cast<unsigned long long>(item->montage_guid) : 0ull);
        if (item && path == kJoyousness) CHECKF(item->montage_guid == kJoyousnessNetGuid, "%s: the recorded Joyousness NetGUID is the table's", c.what);
        if (item && path != kJoyousness) {
            CHECKF(item->montage_guid == bootstrap::montage_id_for(path), "%s: the five unrecorded montages get the server's id from their path", c.what);
        }
    }
}
AOC_REGISTER_TEST(test_saga_the_notes_choose_the_variant);

void test_saga_the_notes_stack_three(Fixture& f) {
    std::printf("the Bard's notes: the Bard_Notes group holds three instances of one note, and a fourth takes the oldest's place\n");
    Rig r(2, 25, {});
    CHECKF(r.ok(), "the rig (%s)", r.problem.c_str());
    if (!r.ok()) return;
    const int64_t tragedy = r.effect("Bard_Note_Tragedy");
    CHECK(tragedy != 0, "the data has Bard_Note_Tragedy");
    if (tragedy == 0) return;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Conflict", 5.0 + 3.0 * static_cast<double>(i)), "the %d%s story's note lands", i + 1, i == 0 ? "st" : i == 1 ? "nd" : "rd");
    CHECKF(r.pipe->stacks_of(tragedy) == 3, "three note applications are three stacks (%d)", r.pipe->stacks_of(tragedy));
    CHECKF(effect_items(r, tragedy) == 3, "and three effect items on the pawn (%zu)", effect_items(r, tragedy));
    CHECKF(story_note(r, "Bard_Story_Conflict", 20.0), "a fourth note lands");
    CHECKF(r.pipe->stacks_of(tragedy) == 3, "the group keeps three (the oldest is gone; %d left)", r.pipe->stacks_of(tragedy));
    CHECKF(effect_items(r, tragedy) == 3, "and the pawn holds three items (%zu)", effect_items(r, tragedy));
    CHECKF(r.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_saga_the_notes_stack_three);

// ---- the landings -----------------------------------------------------------------------------------------------------------------

/// The three launcher requests of the recorded Return to Verra (the montage's notifies: 604, 1,377 and 2,207 ms after the trigger).
void ask_the_three_bursts(Rig& r, double t0) {
    r.asks = false;
    r.pipe->clear_cooldowns(t0);
    for (const double at : {0.604, 1.377, 2.207}) {
        r.run_to(t0 + at);
        const auto verdict = r.pipe->hit_request(r.ability("Bard_Saga"), content::kTargetingDefaultSelf, r.catalog->hit_named(at < 1.0 ? "Bard_Saga_RTV_AoE_1" : "Bard_Saga_RTV_AoE")->row.guid, 1, 1, t0 + at);
        (void)verdict;
    }
}

void test_saga_the_launcher_and_the_landed_hit(Fixture& f) {
    std::printf("the Return to Verra: for a cast at nobody the landed hit is confirmed with the launcher's, in one event, a third of a second after the request\n");
    Band b(f);
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "Comedy note %d lands", i + 1);
    r.asks = false;
    r.know({"Bard_Saga"});
    r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth) - 500.0);
    const double t0 = 20.0;
    CHECKF(r.trigger("Bard_Saga", t0) == Refusal::None, "the Saga is cast at nobody");
    ask_the_three_bursts(r, t0);
    r.run_to(t0 + 0.604 + 0.45);
    const bootstrap::HitConfirm* launcher = nullptr;
    const Seen* pair = nullptr;
    for (const Seen& s : r.log) {
        const bootstrap::HitConfirm* first = nullptr;
        const bootstrap::HitConfirm* landed = nullptr;
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == r.catalog->hit_named("Bard_Saga_RTV_AoE_1")->row.guid) first = &c;
            if (c.record_guid == r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid) landed = &c;
        }
        if (first && landed) {
            launcher = first;
            pair = &s;
            break;
        }
    }
    CHECKF(pair != nullptr, "the launcher's confirmation comes with the landed hit's in one event");
    if (!pair) return;
    const double at = pair->at - (t0 + 0.604);
    CHECKF(near(at, 0.34, 0.02), "0.33 s after the request at 0 m (%.3f s)", at);
    CHECKF(!launcher->targets.empty() && launcher->targets[0].component.ObjectId == b.caster_port.actor_id(), "the launcher's confirmation names the caster's pawn");
    CHECK(launcher->source_type == content::kSourceTypeAbility && launcher->response && launcher->predicted_autonomous && !launcher->predicted_simulated,
          "it is the client's answer and predicted on the autonomous side");
    const bootstrap::HitConfirm* landed = nullptr;
    for (const bootstrap::HitConfirm& c : pair->confirmations) {
        if (c.record_guid == r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid) landed = &c;
    }
    CHECK(landed != nullptr, "(the landed hit is in the same event)");
    if (!landed) return;
    const abilities::HitDef* landing = r.catalog->hit_named("Bard_Saga_RTV_Hit_1");
    const abilities::HitDef* launcher_hit = r.catalog->hit_named("Bard_Saga_RTV_AoE_1");
    CHECK(landing && launcher_hit && launcher_hit->fires.size() == 1, "the data has the launcher hit that fires the projectile and the landed hit it carries");
    if (!landing || !launcher_hit || launcher_hit->fires.empty()) return;
    const content::ProjectileRow* shot = r.catalog->projectile(launcher_hit->fires.front());
    CHECK(shot && std::find(shot->hits.begin(), shot->hits.end(), landing->row.guid) != shot->hits.end(), "the projectile carries that landed hit");
    CHECK(landed->source_type == content::kSourceTypeProjectile && landed->source_guid == launcher_hit->fires.front() && !landed->source_is_actor,
          "the landed hit's source is the projectile's record, with no source actor");
    CHECK(landed->predicted_autonomous && landed->predicted_simulated, "it is predicted on both sides (the client simulated it)");
    CHECKF(landed->mods.size() == 1 && landed->mods[0].value > 1.0f && landed->mods[0].effective > 1.0f, "it carries the heal, %.2f with %.2f effective", landed->mods[0].value,
           landed->mods[0].effective);
    // the caster's Health rose by the heal (the regeneration adds a little over the second the test spans)
    CHECKF(r.pawn.stat(content::kGuidHealth) > r.pawn.stat(content::kGuidMaxHealth) - 500.0 + landed->mods[0].effective - 1.0, "the caster's Health rose by the heal (%.1f of %.1f)",
           r.pawn.stat(content::kGuidHealth), r.pawn.stat(content::kGuidMaxHealth));
    CHECKF(effect_items(r, r.effect("Bard_Pep")) >= 1, "and Pep is on the pawn (%zu item(s))", effect_items(r, r.effect("Bard_Pep")));
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_saga_the_launcher_and_the_landed_hit);

void test_saga_the_allies_are_chosen_by_the_targetings_rule(Fixture& f) {
    std::printf("the Return to Verra's bursts fly at the allies the Joyousness targeting picks: the party within 40 m, injured first, at most 8, the caster alone when solo\n");
    // 10 party allies at 5 to 50 m: the burst's 8 are the caster and the 7 nearest party members (5 to 35 m); the 40 m one is the 8th and the 45 and 50 m ones are
    // outside the 40 m sphere.  The first burst's projectile flies 1,000 cm/s, so the 35 m one lands 3.5 s after the request.
    std::vector<double> xs;
    for (int m = 5; m <= 50; m += 5) xs.push_back(m);
    Band b(f, xs);
    CHECKF(b.ok(), "the world with ten allies (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "Comedy note %d lands", i + 1);
    r.asks = false;
    r.know({"Bard_Saga"});
    const double t0 = 20.0;
    CHECKF(r.trigger("Bard_Saga", t0) == Refusal::None, "the Saga is cast");
    ask_the_three_bursts(r, t0);
    r.run_to(t0 + 5.0);
    const int64_t hit = r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid;
    size_t hit_allies = 0;
    std::vector<size_t> hit_at;
    for (size_t i = 0; i < b.ally_ports.size(); ++i) {
        if (landed_on(r, hit, b.ally_ports[i]->actor_id(), b.caster_port.actor_id()) > 0) {
            ++hit_allies;
            hit_at.push_back(static_cast<size_t>(std::llround(xs[i])));
        }
    }
    CHECKF(hit_allies == 7 && landed_on(r, hit, b.caster_port.actor_id(), b.caster_port.actor_id()) >= 1,
           "the burst's eight are the caster and the seven nearest party members (%zu allies hit, %zu with the caster)", hit_allies, hit_allies + 1);
    CHECKF(hit_at.size() == 7 && hit_at.back() <= 35, "the farthest one hit is 35 m away (%zu hit, the last %s)", hit_at.size(), hit_at.empty() ? "-" : std::to_string(hit_at.back()).c_str());
    // the injured one displaces an unhurt one: 9 party members at 4 to 36 m, the farthest injured, are 10 candidates for the 8; the injured one (1000 - health * 100)
    // comes first, so the unhurt one at 32 m is the one left out
    std::vector<double> xs2 = {4, 8, 12, 16, 20, 24, 28, 32, 36};
    Band c(f, xs2);
    CHECKF(c.ok(), "the second world (%s)", c.problem.c_str());
    if (!c.ok()) return;
    Rig& r2 = c.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r2, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(the second caster's Comedy note %d)", i + 1);
    c.allies[8]->set_current(content::kGuidHealth, c.allies[8]->pawn.stat(content::kGuidMaxHealth) * 0.3);
    r2.asks = false;
    r2.know({"Bard_Saga"});
    CHECKF(r2.trigger("Bard_Saga", 20.0) == Refusal::None, "(the second Saga)");
    ask_the_three_bursts(r2, 20.0);
    r2.run_to(26.0);
    const int64_t hit2 = r2.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid;
    CHECKF(landed_on(r2, hit2, c.ally_ports[8]->actor_id(), c.caster_port.actor_id()) >= 1, "the injured party member 36 m away takes one of the first burst's landed hits");
    CHECKF(landed_on(r2, hit2, c.ally_ports[7]->actor_id(), c.caster_port.actor_id()) == 0, "the unhurt one at 32 m is the one the burst's eight leave out");
    // solo: a caster with no party at all is the only ally
    Band s(f, {25.0}, /*party=*/false);
    CHECKF(s.ok(), "the solo world (%s)", s.problem.c_str());
    if (!s.ok()) return;
    Rig& r3 = s.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r3, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(the solo caster's Comedy note %d)", i + 1);
    r3.asks = false;
    r3.know({"Bard_Saga"});
    CHECKF(r3.trigger("Bard_Saga", 20.0) == Refusal::None, "(the solo Saga)");
    ask_the_three_bursts(r3, 20.0);
    r3.run_to(23.0);
    const int64_t hit3 = r3.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid;
    CHECKF(landed_on(r3, hit3, s.caster_port.actor_id(), s.caster_port.actor_id()) >= 1, "solo, the caster takes the landed hit");
    CHECKF(landed_on(r3, hit3, s.ally_ports[0]->actor_id(), s.caster_port.actor_id()) == 0, "and the player 25 m away who is not of his party takes none");
    // the sphere's edge: an ally at 39 m is in the 40 m the targeting record gives, one at 41 m is not
    Band e(f, {39.0, 41.0});
    CHECKF(e.ok(), "the world at the edge of the 40 m (%s)", e.problem.c_str());
    if (!e.ok()) return;
    Rig& r4 = e.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r4, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(the edge world's Comedy note %d)", i + 1);
    r4.asks = false;
    r4.know({"Bard_Saga"});
    CHECKF(r4.trigger("Bard_Saga", 20.0) == Refusal::None, "(the edge Saga)");
    ask_the_three_bursts(r4, 20.0);
    r4.run_to(26.0);
    const int64_t hit4 = r4.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid;
    CHECKF(landed_on(r4, hit4, e.ally_ports[0]->actor_id(), e.caster_port.actor_id()) >= 1, "the ally at 39 m takes the burst");
    CHECKF(landed_on(r4, hit4, e.ally_ports[1]->actor_id(), e.caster_port.actor_id()) == 0, "the ally at 41 m, outside the 40 m, takes none");
}
AOC_REGISTER_TEST(test_saga_the_allies_are_chosen_by_the_targetings_rule);

void test_saga_another_players_pawn(Fixture& f) {
    std::printf("another player of the party takes the heal and Pep on his own pawn, and his client holds the effect item\n");
    Band b(f, {6.0});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    Rig& ally = *b.allies[0];
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "Comedy note %d lands", i + 1);
    const double max = ally.pawn.stat(content::kGuidMaxHealth);
    ally.set_current(content::kGuidHealth, max - 600.0);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the Saga is cast at nobody");
    ask_the_three_bursts(r, 20.0);
    b.run_to(23.0);
    const double after = b.ally_ports[0]->rig_fighter().stats().find(content::kGuidHealth)->value;
    // the landed hit the caster's client was told about names the ally, and what it healed is what the ally's Health took (the regeneration only adds)
    const bootstrap::HitConfirm* on_ally = nullptr;
    for (const bootstrap::HitConfirm* c : confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid)) {
        for (const bootstrap::HitTargetOut& t : c->targets) {
            if (t.component.ObjectId == b.ally_ports[0]->actor_id()) on_ally = c;
        }
    }
    CHECKF(on_ally != nullptr && on_ally->targets.size() == 1 && !on_ally->targets[0].mods.empty(), "the caster's client was told the landed hit on the ally");
    if (on_ally == nullptr || on_ally->targets.empty() || on_ally->targets[0].mods.empty()) return;
    CHECKF(after >= max - 600.0 + on_ally->targets[0].mods[0].effective - 1.0, "the ally's Health rose by the landed hit's effective amount (%.1f of %.1f, %.1f healed)", after, max,
           on_ally->targets[0].mods[0].effective);
    CHECKF(effect_items(ally, ally.effect("Bard_Pep")) >= 1, "Pep is on the ally's pawn (%zu item(s))", effect_items(ally, ally.effect("Bard_Pep")));
    CHECK(!b.ally_ports[0]->ally_lands.empty(), "his session took the landed hit");
    CHECKF(b.ally_ports[0]->ally_effects >= 1, "his session took the effect (%zu event(s))", b.ally_ports[0]->ally_effects);
    CHECK(ally.pawn.broken.empty(), "(the ally's pawn took every change)");
    CHECKF(landed_on(r, r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid, b.ally_ports[0]->actor_id(), b.caster_port.actor_id()) >= 1, "the caster's client was told the landed hit on the ally");
}
AOC_REGISTER_TEST(test_saga_another_players_pawn);

// ---- the zones --------------------------------------------------------------------------------------------------------------------

void test_saga_the_pantheon_zones(Fixture& f) {
    std::printf("the Silent Pantheon: each spawn projectile that fires opens its 1 m blessing zone where it lands (the data's coin flip), and the Saga's own zone opens at the caster\n");
    Band b(f, {3.0, 6.0});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Wonder", 5.0 + 3.0 * static_cast<double>(i)), "Mystery note %d lands", i + 1);
    // the data gates each of the four spawn hits on RandDouble() < 0.5: force the first cast's four draws to fire and the second's to fail
    const std::vector<double> fire = {0.1, 0.1, 0.1, 0.1};
    const std::vector<double> fail = {0.9, 0.9, 0.9, 0.9};
    r.pipe->set_rand_script(&fire);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the Saga is cast");
    b.run_to(22.5);
    // the Saga's own zone: at the caster, from the cast (a pin of what the ground circles do already)
    const std::vector<bootstrap::ZoneOp> saga_zone = zones_opened(r, kSagaZone);
    CHECKF(saga_zone.size() == 1 && saga_zone[0].x == 0.0 && std::fabs(saga_zone[0].y) < 1.0, "the Saga's zone opened at the caster (%zu opened)",
           saga_zone.size());
    // the four blessing zones: each of the four spawn hits fired, and each zone stands where its projectile landed (at the ally it was aimed at, the caster included)
    size_t pantheon_zones = 0, at_an_ally = 0;
    const combat::Vec3 me = b.caster_port.fighter().position();
    for (int i = 0; i < 4; ++i) {
        for (const bootstrap::ZoneOp& z : zones_opened(r, kPantheonZone[i])) {
            ++pantheon_zones;
            if (z.placed && std::fabs(z.x - me.x) < 150.0 && std::fabs(z.y - me.y) < 150.0) ++at_an_ally;
            for (size_t a = 0; a < b.ally_ports.size(); ++a) {
                if (z.placed && std::fabs(z.x - b.ally_hits[a]->self().position().x) < 150.0 && std::fabs(z.y - b.ally_hits[a]->self().position().y) < 150.0) {
                    ++at_an_ally;
                    break;
                }
            }
        }
    }
    CHECKF(pantheon_zones == 4, "the four spawn hits fired and opened their four zones (%zu)", pantheon_zones);
    CHECKF(pantheon_zones == at_an_ally, "every blessing zone opened where a projectile landed, at an ally (%zu of %zu)", at_an_ally, pantheon_zones);
    size_t blessed = 0;
    for (size_t a = 0; a < b.allies.size(); ++a) {
        bool any = false;
        for (const char* name : {"Bard_Saga_Pantheon_1", "Bard_Saga_Pantheon_2", "Bard_Saga_Pantheon_3", "Bard_Saga_Pantheon_4"}) any = any || effect_items(*b.allies[a], b.allies[a]->effect(name)) > 0;
        blessed += any ? 1 : 0;
    }
    CHECKF(blessed >= 1, "at least one ally holds a Blessing of the Pantheon (%zu)", blessed);
    // the data's coin flip: a cast whose four draws fail opens the zone at the caster only (its own), and no blessing zone
    const size_t zones_before = pantheon_zones;
    r.pipe->clear_cooldowns(22.5);
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    r.pipe->set_rand_script(&fail);
    CHECKF(r.trigger("Bard_Saga", 23.0) == Refusal::None, "a second Pantheon cast");
    b.run_to(26.0);
    size_t zones_after = 0;
    for (int i = 0; i < 4; ++i) zones_after += zones_opened(r, kPantheonZone[i]).size();
    CHECKF(zones_after == zones_before, "its four draws failed: no blessing zone opened (%zu before, %zu after)", zones_before, zones_after);
    CHECKF(zones_opened(r, kSagaZone).size() == 2, "the Saga's own zone opened again with the second cast (%zu)", zones_opened(r, kSagaZone).size());
    CHECK(r.pawn.broken.empty(), "(the caster's pawn took every change)");
}
AOC_REGISTER_TEST(test_saga_the_pantheon_zones);

// ---- the edges --------------------------------------------------------------------------------------------------------------------

void test_saga_two_bards_side_by_side(Fixture& f) {
    std::printf("two Bards side by side: each one's three notes are his own, and each one's Saga plays his own variant and heals\n");
    Band b(f, {0.0}, /*party=*/true, {"Bard_Passive_Saga_Vol2"}, {});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& first = b.caster;
    Rig& second = *b.allies[0];
    for (int i = 0; i < 3; ++i) CHECKF(story_note(first, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(the first Bard's Comedy note %d)", i + 1);
    for (int i = 0; i < 3; ++i) CHECKF(story_note(second, "Bard_Story_Conflict", 6.0 + 3.0 * static_cast<double>(i)), "(the second Bard's Tragedy note %d)", i + 1);
    CHECKF(first.pipe->stacks_of(first.effect("Bard_Note_Comedy")) == 3, "the first Bard holds three Comedy (%d)", first.pipe->stacks_of(first.effect("Bard_Note_Comedy")));
    CHECKF(second.pipe->stacks_of(second.effect("Bard_Note_Tragedy")) == 3, "the second holds three Tragedy (%d)", second.pipe->stacks_of(second.effect("Bard_Note_Tragedy")));
    CHECKF(first.pipe->stacks_of(first.effect("Bard_Note_Tragedy")) == 0, "the first holds no Tragedy of the second's (%d)", first.pipe->stacks_of(first.effect("Bard_Note_Tragedy")));
    // each one casts his own Saga: the first the Return (3 Comedy), the second the Apocalypse (3 Tragedy)
    first.asks = false;
    first.know({"Bard_Saga"});
    CHECKF(first.trigger("Bard_Saga", 20.0) == Refusal::None, "the first Bard casts his Saga");
    first.run_to(20.5);
    CHECKF(saga_item(first) && saga_item(first)->montage_path == kJoyousness, "the first plays the Return to Verra");
    second.asks = false;
    second.know({"Bard_Saga"});
    CHECKF(second.trigger("Bard_Saga", 21.0) == Refusal::None, "the second Bard casts his Saga");
    second.run_to(21.5);
    CHECKF(saga_item(second) && saga_item(second)->montage_path == kFlameAndFang, "the second plays the Apocalypse");
}
AOC_REGISTER_TEST(test_saga_two_bards_side_by_side);

void test_saga_the_caster_dies_while_his_projectiles_fly(Fixture& f) {
    std::printf("a caster who dies while his Saga's projectiles fly: the cast ends with him and nothing lands on the ally\n");
    Band b(f, {6.0});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    Rig& ally = *b.allies[0];
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "Comedy note %d lands", i + 1);
    ally.set_current(content::kGuidHealth, ally.pawn.stat(content::kGuidMaxHealth) - 600.0);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the Saga is cast");
    // the first burst is asked for and its projectile is in the air (it lands at +0.944); the caster dies before it
    r.run_to(20.604);
    CHECK(r.pipe->hit_request(r.ability("Bard_Saga"), content::kTargetingDefaultSelf, r.catalog->hit_named("Bard_Saga_RTV_AoE_1")->row.guid, 1, 1, 20.604) == AbilityPipeline::HitVerdict::Scheduled,
          "the first burst is asked for");
    r.run_to(20.75);
    r.pipe->set_dead(true);
    b.run_to(22.0);
    CHECKF(b.ally_ports[0]->ally_lands.empty(), "the ally's session took no landed hit (%zu)", b.ally_ports[0]->ally_lands.size());
    CHECKF(b.ally_ports[0]->rig_fighter().stats().find(content::kGuidHealth)->value < ally.pawn.stat(content::kGuidMaxHealth) - 500.0, "his Health did not rise");
}
AOC_REGISTER_TEST(test_saga_the_caster_dies_while_his_projectiles_fly);

void test_saga_the_same_cast_twice(Fixture& f) {
    std::printf("the same Saga cast twice: each cast's launcher asks for its own projectiles and each burst lands once per cast\n");
    Band b(f, {});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "Comedy note %d lands", i + 1);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the first cast");
    ask_the_three_bursts(r, 20.0);
    r.run_to(23.0);
    r.pipe->clear_cooldowns(23.0);
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    CHECKF(r.trigger("Bard_Saga", 24.0) == Refusal::None, "the second cast");
    ask_the_three_bursts(r, 24.0);
    r.run_to(27.0);
    const int64_t landed = r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid;
    size_t at_the_caster = 0;
    for (const bootstrap::HitConfirm* c : confirms_of(r, landed)) at_the_caster += c->targets.empty() ? 1 : 0;   // (a hit on the pawn itself)
    CHECKF(at_the_caster >= 2, "both casts landed their first burst on the caster (%zu landed confirmations)", at_the_caster);
    // the second and third bursts fire a different projectile record (Bard_Saga_RTV_Projectile), whose landed hit is the other record: both casts' are applied
    size_t later_bursts = 0;
    for (const bootstrap::HitConfirm* c : confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_Hit")->row.guid)) later_bursts += c->targets.empty() ? 1 : 0;
    CHECKF(later_bursts >= 2, "the second and third bursts' landed hits also came (%zu)", later_bursts);
}
AOC_REGISTER_TEST(test_saga_the_same_cast_twice);

void test_saga_a_target_who_leaves(Fixture& f) {
    std::printf("a target who leaves the world while the projectile flies: nothing lands on him, and a new session of his is not touched\n");
    Band b(f, {6.0});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    Rig& ally = *b.allies[0];
    for (int i = 0; i < 3; ++i) CHECKF(story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "Comedy note %d lands", i + 1);
    ally.set_current(content::kGuidHealth, ally.pawn.stat(content::kGuidMaxHealth) - 600.0);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the Saga is cast");
    ask_the_three_bursts(r, 20.0);
    r.run_to(20.7);   // (the launcher answered; the projectile is in the air)
    b.world.remove_player(b.ally_ports[0].get());   // (the session ends: its port leaves the world)
    Rig reconnected(2, 25, {});
    combat_rig::FakePlayer again(reconnected, 99, combat::Vec3{600, 0, kGroundZ});
    b.world.add_player(&again);
    b.run_to(22.0);
    CHECKF(reconnected.pawn.stat(content::kGuidHealth) >= reconnected.pawn.stat(content::kGuidMaxHealth) - 1.0, "the new session's pawn is untouched (a new actor id)");
    CHECKF(again.ally_lands.empty(), "its session took no landed hit (%zu)", again.ally_lands.size());
}
AOC_REGISTER_TEST(test_saga_a_target_who_leaves);

}  // namespace saga
