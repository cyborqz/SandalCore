// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_aftermath.cpp: what follows an NPC's death (plan E1, stage 3, part 2) on the REAL content.db and dist/content/npc_motion.tsv
//
//    the kill       the reward tables roll at the death: the experience reaches the killer's port (after the decay of the level difference), the loot
//                   becomes stacks of the corpse and its three dynamic components (LootComponent, ItemStorageComponent, InteractableComponent), the NPC's
//                   InteractInfo points at them; a kill with nothing to give gets none
//    the corpse     a corpse with no loot closes 15.0 s after the death; with loot, 4.0 s after the stacks are all taken; its channel closes with the actor's
//                   removal; the loot's stacks are taken once each
//    the respawn    a spawn point makes its NPC again after the instance's respawn time with the variance (a new actor), never for NoRespawn or a point with
//                   no recipe
//    hostile        a take of a slot that is none or was taken, a death nobody fought (no experience, no loot), the kill switch (the corpse still goes)
// Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"
#include "combat_rig.h"
#include "combat/aftermath.h"
#include "combat/combat_data.h"
#include "combat/npc_stats.h"
#include "combat/rewards.h"
#include "npc/motion_table.h"
#include "protocol/server_guid_allocator.h"
#include "world/city_nodes.h"
#include "world/corpse.h"

#include <cmath>
#include <set>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

using namespace ability_rig;
using namespace aoc::worldd;
namespace repl = ::aoc::protocol::repl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

namespace {

struct Fixture {
    std::string                         path;
    std::optional<content::ContentDb>   db;
    const bootstrap::StatContext*       ctx = nullptr;
    npc::MotionTable                    motion;
    std::optional<combat::CombatData>   data;
    std::optional<abilities::Catalog>   catalog;
    std::optional<combat::RewardBook>   rewards;
    std::string                         wolf_path;
    std::string                         problem;
// C:Y:B / CoA

    Fixture() {
        path = real_db_path();
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            problem = "content.db: " + error;
            return;
        }
        ctx = &world_stats::of(*db);
        if (!motion.load(NPC_MOTION_PATH, error)) {
            problem = std::string("npc_motion.tsv: ") + error;
            return;
        }
        const auto rows = db->combat_rows(error);
        if (!rows) {
            problem = "the combat tables: " + error;
            return;
        }
        data = combat::CombatData::build(*rows, *ctx->bundle, error);
        if (!data) {
            problem = "the combat data: " + error;
            return;
        }
        catalog = abilities::Catalog::load(path, *ctx->bundle, error);
        if (!catalog) {
            problem = "the ability tables: " + error;
            return;
        }
        std::set<int64_t> set;
        for (const auto& [p, row] : motion.rows()) set.insert(row.rewards.begin(), row.rewards.end());
        auto tables = db->loot_tables(std::vector<int64_t>(set.begin(), set.end()), error);
        const auto decay = db->xp_rule("adventuring_decay", error);
        if (!tables || !decay) {
            problem = "the reward tables: " + error;
            return;
        }
        rewards.emplace(*catalog, std::move(*tables), *decay);
        for (const auto& [p, row] : motion.rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
        }
        if (wolf_path.empty()) problem = "the Blueprint table has no Wolf";
    }
    bool ok() const { return problem.empty(); }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

/// A world with a player at the origin's side, an Aftermath on a CombatWorld, and a clock.
struct Scene {
    Fixture&                         f;
    Rig                              rig;
    combat_rig::World                w;
    combat::NpcStatBook              book;
    std::unique_ptr<combat::CombatWorld> world;
    std::unique_ptr<combat::Aftermath>   after;
    std::unique_ptr<combat_rig::FakePlayer> player;
    aoc::protocol::ServerGuidAllocator      guids{60, 0x6ee87ee4u};
    std::vector<uint32_t>            respawns;
    double                           now = 0;

    explicit Scene(Fixture& fx, int player_level = 20, combat::AftermathConfig cfg = {}) : f(fx), rig(4, player_level, {}), book(*fx.ctx->bundle, *fx.ctx->gear) {
        if (!rig.ok() || !w.problem.empty()) return;
        combat::CombatWorldConfig cc;
        cc.seed = 9;
        world = std::make_unique<combat::CombatWorld>(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cc);
        player = std::make_unique<combat_rig::FakePlayer>(rig, 77, combat::Vec3{300, 0, 1098});
        world->add_player(player.get());
        cfg.seed = cfg.seed == 1 ? 3 : cfg.seed;
        after = std::make_unique<combat::Aftermath>(w.registry, *world, &*f.rewards, [this] { return guids.next(); }, cfg);
        after->set_quality_of([](int64_t) { return 1; });
        after->set_respawner([this](uint32_t ref, double) {
            respawns.push_back(ref);
            return uint64_t{0};
        });
        after->install();
    }
    bool ok() const { return world && after && player; }
// [C Y B :: CoA]

    /// A Wolf of `level` standing at (x, 0); its NpcInfo's respawn numbers as the data would give them.
    uint64_t wolf(int level, double x, uint32_t spawn_ref = 5, double respawn = 10, double variance = 0, const char* rule = "Refill") {
        const uint64_t id = w.spawn(f.wolf_path, x, 0, level, 0);
        if (id) {
            w.registry.modify(id, world::Change::State, [&](world::WorldActor& a) {
                a.npc.spawn_ref = spawn_ref;
                a.npc.respawn_seconds = respawn;
                a.npc.respawn_variance = variance;
                a.npc.respawn_rule = rule;
                a.npc.name = "Wolf";
            });
        }
        return id;
    }
    void kill(uint64_t id, uint64_t by = 77) {
        combat::Resolved r;
        r.mods.push_back({1, content::kGuidHealth, -1.0e9});
        world->apply_hit_to_npc(id, by, r, now);
    }
    void run(double seconds) {
        const double end = now + seconds;
        while (now < end) {
            now += 0.1;
            w.npcs->tick(now);
            world->tick(now);
            after->tick(now);
        }
    }
};

size_t count_sub(const world::WorldActor& a, const char* cls, bool dynamic_only) {
    size_t n = 0;
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls && (!dynamic_only || s.dynamic_class)) ++n;
    }
    return n;
}

int64_t top_rarity_of(const world::WorldActor& a) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "ULootComponent") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "CosmeticData.TopRarity");
        if (v && v->get_if<uint64_t>()) return static_cast<int64_t>(*v->get_if<uint64_t>());
    }
    return -1;
}

// ---- the kill ------------------------------------------------------------------------------------------------------------------------

void test_kill(Fixture& f) {
    std::printf("a kill: experience, loot and the corpse\n");
    Scene s(f, 12);   // a level-12 player: the Glint falloff (Loot_Falloff is 0 from ten levels apart) lets a level-10 Wolf drop
    CHECKF(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    // kills until one drops something (the Glint chance is about 0.38) and one drops nothing
    uint64_t with_loot = 0, without = 0;
    for (int i = 0; i < 40 && (!with_loot || !without); ++i) {
        const uint64_t id = s.wolf(10, 100.0 + 400.0 * i);
        if (!id) break;
        s.kill(id);
        const combat::Corpse* c = s.after->corpse_of(id);
        if (c && c->parts && !with_loot) with_loot = id;
        if (c && !c->parts && !without) without = id;
    }
    CHECK(with_loot != 0 && without != 0, "among forty kills some corpses have loot and some have none");
    CHECKF(s.player->kills.size() >= 1 && s.player->kills.size() == s.after->stats().deaths, "every kill reaches the killer's port (%zu kills of %llu deaths)", s.player->kills.size(),
           static_cast<unsigned long long>(s.after->stats().deaths));
    if (s.player->kills.empty()) return;
    const combat::KillReward& k = s.player->kills.front();
    CHECKF(k.adventuring_xp > 0 && k.weapon_xp == k.adventuring_xp && k.level == 10 && k.blueprint == "Wolf",
           "a level-10 Wolf is worth Adventuring %lld and the same in Weapon experience to a level-12 player", static_cast<long long>(k.adventuring_xp));
    // the decay: a level-25 player gets 5 % of what a level-10 one does (nine levels below and further)
    {
        Scene a(f, 10), b(f, 25);
        const uint64_t ia = a.wolf(10, 100), ib = b.wolf(10, 100);
        a.kill(ia);
        b.kill(ib);
        CHECK(!a.player->kills.empty() && !b.player->kills.empty(), "both kills pay");
        if (!a.player->kills.empty() && !b.player->kills.empty()) {
            CHECKF(b.player->kills.front().adventuring_xp * 15 < a.player->kills.front().adventuring_xp,
                   "a player of the Wolf's own level gets %lld, one 15 levels above it %lld (the decay curve: 5 %%)",
                   static_cast<long long>(a.player->kills.front().adventuring_xp), static_cast<long long>(b.player->kills.front().adventuring_xp));
        }
        // and the loot falls off the same way: a level-25 looter gets no Glint from a level-10 creature (Loot_Falloff)
        size_t loot = 0;
        for (int i = 0; i < 30; ++i) {
            const uint64_t id = b.wolf(10, 100.0 + 400.0 * i);
            b.kill(id);
            const combat::Corpse* c = b.after->corpse_of(id);
            loot += (c && c->parts) ? 1 : 0;
        }
        CHECKF(loot == 0, "a level-25 player's thirty kills of level-10 Wolves leave no loot (%zu corpses with some)", loot);
    }
    CHECK(with_loot != 0, "(loot)");
    if (!with_loot) return;
// Cyb captures, C | O | A code

    const world::WorldActor* a = s.w.registry.find(with_loot);
    CHECK(a != nullptr && combat_rig::combat_flag(*a, "bIsDead"), "the corpse is the same actor, dead");
    CHECK(a && count_sub(*a, "ULootComponent", true) == 1 && count_sub(*a, "UItemStorageComponent", true) >= 1 && a->actor.subobjects.size() >= 9,
          "it gained a LootComponent and an ItemStorageComponent as dynamic subobjects");
    size_t dyn_inter = 0;
    if (a) {
        for (const repl::Subobject& sub : a->actor.subobjects) {
            if (sub.obj.schema && std::string(sub.obj.schema->name) == "UInteractableComponent" && sub.dynamic_class) ++dyn_inter;
        }
    }
    CHECK(dyn_inter == 1, "and a dynamic InteractableComponent");
    const combat::Corpse* c = s.after->corpse_of(with_loot);
    CHECK(c && c->parts && c->parts->interactable != 0 && c->parts->container != c->parts->interactable && c->parts->loot_component != c->parts->container, "three distinct component ids");
    if (!c || !c->parts) return;
    CHECK(s.after->corpse_by_interactable(c->parts->interactable) == c && s.w.registry.find_by_component(c->parts->interactable) == a, "a client's interaction names the corpse by its interactable");
    CHECK(c->rights.count(77) == 1 && !c->loot.empty() && c->remaining() == c->loot.size(), "the killer may take its stacks");
    CHECK(top_rarity_of(*a) == c->top_rarity && c->top_rarity >= 0 && c->top_rarity < 7, "the LootComponent's TopRarity is the best stack's");
    // the NPC's InteractInfo points at the components
    bool pointed = false;
    for (const repl::Subobject& sub : a->actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UNPCSecondaryInfo") continue;
        const repl::Value* v = sub.obj.state.get(*sub.obj.schema, "LootComponent");
        const auto* g = v ? v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : nullptr;
        pointed = g && g->ObjectId == c->parts->loot_component;
    }
    CHECK(pointed, "its InteractInfo's LootComponent is the new component");
    CHECKF(c->close_at > c->died_at + 100, "an unlooted corpse with loot stays long (%.0f s)", c->close_at - c->died_at);

    // taking: each stack once
    const size_t n = c->loot.size();
    const combat::LootStack* first = s.after->take(with_loot, 0);
    CHECK(first != nullptr && first->taken && s.after->take(with_loot, 0) == nullptr, "a stack is taken once");
    CHECK(s.after->take(with_loot, 99) == nullptr && s.after->take(1234567, 0) == nullptr, "a slot that is none and a corpse that is none give nothing");
    s.after->put_back(with_loot, 0);
    CHECK(s.after->corpse_of(with_loot)->remaining() == n, "a stack put back is there again");
    for (size_t i = 0; i < n; ++i) s.after->take(with_loot, i);
    CHECK(s.after->corpse_of(with_loot)->remaining() == 0, "all the stacks taken: nothing remains");
    s.after->emptied(with_loot, s.now);
    CHECK(top_rarity_of(*s.w.registry.find(with_loot)) == world::kLootEmptied, "the loot component says emptied (TopRarity 7)");
    const double due = s.after->corpse_of(with_loot)->close_at;
    CHECKF(near(due, s.now + 4.0, 0.01), "the corpse closes 4.0 s after the loot is emptied (retail: 3.97 to 4.08) (%.2f)", due - s.now);
    s.run(3.0);
    CHECK(s.w.registry.find(with_loot) != nullptr, "3 s on it is still there");
    s.run(1.5);
    CHECK(s.w.registry.find(with_loot) == nullptr && s.after->corpse_of(with_loot) == nullptr && !s.world->is_combatant(with_loot), "4.5 s on its actor is gone (the channel closes) and the world forgot it");
    // the corpse without loot: 15.0 s
    const combat::Corpse* c2 = s.after->corpse_of(without);
    CHECK(c2 && !c2->parts && near(c2->close_at - c2->died_at, 15.0, 1e-6), "a corpse with nothing to loot lasts 15.0 s (retail: 7 of 7)");
    const world::WorldActor* b = s.w.registry.find(without);
    CHECK(b && count_sub(*b, "ULootComponent", true) == 0, "and has no loot components");
}

// ---- the respawn -----------------------------------------------------------------------------------------------------------------

void test_respawn(Fixture& f) {
    std::printf("the respawn\n");
    Scene s(f);
    if (!s.ok()) {
        CHECK(false, "the scene");
        return;
    }
    const uint64_t a = s.wolf(8, 100, 7, 20.0, 0.0);
    const uint64_t b = s.wolf(8, 500, 8, 20.0, 0.0, "NoRespawn");
    const uint64_t c = s.wolf(8, 900, 0, 20.0, 0.0);          // no recipe
    const uint64_t d = s.wolf(8, 1300, 9, 0.0, 0.0);           // the instance says 0: the default
    const uint64_t e = s.wolf(8, 1700, 10, 100.0, 50.0);       // a variance
    for (const uint64_t id : {a, b, c, d, e}) s.kill(id);
    CHECK(s.after->respawns_pending() == 3, "three of the five will be made again (not NoRespawn, not the one with no recipe)");
    s.run(19.0);
    CHECK(s.respawns.empty(), "19 s after the deaths nothing has respawned (20 s)");
    s.run(1.5);
    CHECK(s.respawns.size() == 1 && s.respawns[0] == 7, "the point with 20 s respawns at 20 s");
    s.run(45.0);
    CHECK(s.respawns.size() == 2 && s.respawns[1] == 9, "the one whose instance says 0 comes back after the default 60 s");
    s.run(100.0);
    CHECK(s.respawns.size() == 3 && s.respawns[2] == 10, "the one with a variance of 50 s comes back within 50 to 150 s");
    CHECK(s.after->respawns_pending() == 0 && s.after->stats().respawns == 0, "(the test's respawner makes nothing, so none counts as made)");
    // the corpses are gone long before: respawn does not wait for them
    CHECK(s.w.registry.find(a) == nullptr, "the corpses closed on their own");
}

// ---- hostile ------------------------------------------------------------------------------------------------------------------------
// C_Y_B.C | O | A

void test_hostile(Fixture& f) {
    std::printf("hostile: no fighter, the kill switch\n");
    {
        Scene s(f);
        if (!s.ok()) {
            CHECK(false, "the scene");
            return;
        }
        const uint64_t id = s.wolf(10, 100);
        s.kill(id, /*by=*/12345);   // nobody the world knows
        const combat::Corpse* c = s.after->corpse_of(id);
        CHECK(c && !c->parts && c->loot.empty() && s.player->kills.empty(), "a death no player fought gives no experience and no loot");
        CHECK(near(c->close_at - c->died_at, 15.0, 1e-6), "the corpse still goes after 15 s");
    }
    {
        combat::AftermathConfig cfg;
        cfg.enabled = false;
        Scene s(f, 20, cfg);
        if (!s.ok()) {
            CHECK(false, "the scene");
            return;
        }
        const uint64_t id = s.wolf(10, 100, 5, 10);
        s.kill(id);
        const combat::Corpse* c = s.after->corpse_of(id);
        CHECK(c && s.player->kills.empty() && s.after->respawns_pending() == 0, "the kill switch: no experience, no respawn");
        s.run(16.0);
        CHECK(s.w.registry.find(id) == nullptr, "the corpse still goes");
    }
    {
        combat::AftermathConfig cfg;
        cfg.respawn = false;
        Scene s(f, 20, cfg);
        if (!s.ok()) {
            CHECK(false, "the scene");
            return;
        }
        s.kill(s.wolf(10, 100, 5, 10));
        CHECK(s.after->respawns_pending() == 0 && !s.player->kills.empty(), "--no-respawn: experience yes, respawn no");
    }
    {
        Scene s(f);
        if (!s.ok()) {
            CHECK(false, "the scene");
            return;
        }
        // components added twice, or to an actor that is none
        const uint64_t id = s.wolf(10, 100);
        std::string why;
        const repl::GuidValue me{1, 2, 3, 4};
        auto first = world::add_loot(s.w.registry, id, [&] { return s.guids.next(); }, me, {me}, 1, why);
        CHECK(first.has_value(), "loot components can be added to an NPC");
        auto second = world::add_loot(s.w.registry, id, [&] { return s.guids.next(); }, me, {me}, 1, why);
        CHECK(!second.has_value() && !why.empty(), "but not twice");
        CHECK(!world::add_loot(s.w.registry, 4242424242ull, [&] { return s.guids.next(); }, me, {me}, 1, why).has_value(), "nor to an actor that is none");
        CHECK(world::empty_loot(s.w.registry, id) && top_rarity_of(*s.w.registry.find(id)) == world::kLootEmptied, "emptying says 7");
        CHECK(!world::empty_loot(s.w.registry, 4242424242ull), "emptying nothing fails");
    }
}

// ---- a huntable creature's corpse (Gathering stage 3, "Hunting") ------------------------------------------------------------------------------------

void test_huntable(Fixture& f) {
    std::printf("a huntable creature: no loot, the experience, a corpse that waits for the hunter\n");
    constexpr int64_t kHuntDef = 777000111;
    const auto make_huntable = [&](Scene& s, int level, double x) {
        const uint64_t id = s.wolf(level, x);
        if (id) s.w.registry.modify(id, world::Change::State, [&](world::WorldActor& a) { a.npc.interact_def = kHuntDef; });
        return id;
    };
    Scene s(f, 12);   // the Glint falloff lets a level-10 Wolf drop for a level-12 player: a plain Wolf leaves loot about 38 times in a hundred
    CHECKF(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    s.after->set_huntable([](const world::NpcInfo& i) { return i.interact_def == kHuntDef; });
    size_t plain_loot = 0, hunt_loot = 0, hunted_corpses = 0;
    uint64_t huntable = 0, plain = 0;
    for (int i = 0; i < 40; ++i) {
        const uint64_t h = make_huntable(s, 10, 100.0 + 400.0 * i), p = s.wolf(10, 300.0 + 400.0 * i);
        if (!h || !p) break;
        s.kill(h);
        s.kill(p);
        const combat::Corpse* ch = s.after->corpse_of(h);
        const combat::Corpse* cp = s.after->corpse_of(p);
        hunt_loot += (ch && (ch->parts || !ch->loot.empty())) ? 1 : 0;
        plain_loot += (cp && cp->parts) ? 1 : 0;
        hunted_corpses += (ch && ch->huntable && !ch->hunted) ? 1 : 0;
        if (!huntable) huntable = h;
        if (!plain) plain = p;
    }
    CHECKF(hunted_corpses == 40 && hunt_loot == 0, "forty huntable Wolves leave forty hunt corpses and no loot at all (%zu with loot)", hunt_loot);
    CHECKF(plain_loot > 0, "while the plain Wolves killed beside them left loot %zu times (the rolls work)", plain_loot);
    CHECKF(s.player->kills.size() == 80 && s.player->kills.front().adventuring_xp > 0, "every kill of both reached the killer: %zu kills with Adventuring XP", s.player->kills.size());
    const world::WorldActor* a = s.w.registry.find(huntable);
    CHECK(a && count_sub(*a, "ULootComponent", true) == 0 && combat_rig::combat_flag(*a, "bIsDead"), "a huntable's corpse is dead and has no loot components");
    const combat::Corpse* c = s.after->corpse_of(huntable);
    CHECK(c && !c->parts && near(c->close_at - c->died_at, 120.0, 1e-6), "it waits 120 s for the hunter");
    CHECK(s.after->corpse_by_interactable(a ? a->npc.interact_component : 1) == nullptr, "and the loot window finds nothing in it");
    CHECK(!s.after->hunt_started(plain, s.now, 14.0) && !s.after->hunt_done(plain, s.now) && !s.after->hunt_started(1234567, s.now, 14.0), "a plain corpse and a corpse that is none are not hunted");

    // the hunt: the corpse outlives its channel, and goes 4 s after it
    s.run(100.0);   // 100 s after the deaths
    CHECK(s.w.registry.find(huntable) != nullptr, "100 s on it stands");
    s.run(10.0);
    CHECK(s.after->hunt_started(huntable, s.now, 14.0), "a hunt starts at 110 s");
    CHECKF(s.after->corpse_of(huntable) && s.after->corpse_of(huntable)->close_at >= s.now + 14.0 + 5.0 - 1e-6, "the corpse now stays until the channel is over and 5 s more (%.1f s from now)",
           s.after->corpse_of(huntable) ? s.after->corpse_of(huntable)->close_at - s.now : -1.0);
    s.run(14.0);
    CHECK(s.w.registry.find(huntable) != nullptr, "124 s after its death (its 120 s are over) the corpse still stands: the hunt kept it");
    const bool done = s.after->hunt_done(huntable, s.now);
    CHECK(done, "the hunt is done");
    const combat::Corpse* hunted = s.after->corpse_of(huntable);
    CHECKF(hunted && near(hunted->close_at, s.now + 4.0, 0.01) && hunted->hunted, "the corpse goes 4.0 s after the hunt (%.2f)", hunted ? hunted->close_at - s.now : -1.0);
    s.run(3.0);
    CHECK(s.w.registry.find(huntable) != nullptr, "3 s on it is still there");
    s.run(1.5);
    CHECK(s.w.registry.find(huntable) == nullptr && s.after->corpse_of(huntable) == nullptr, "4.5 s on it is gone");
    // a hunt that never comes: the 120 s
    CHECK(s.w.registry.find(plain) == nullptr, "(the plain corpses are long gone)");

    // the kill switch: the corpse is still hunted
    {
        combat::AftermathConfig cfg;
        cfg.enabled = false;
        Scene d(f, 20, cfg);
        if (!d.ok()) {
            CHECK(false, "the scene");
            return;
        }
        d.after->set_huntable([](const world::NpcInfo& i) { return i.interact_def == kHuntDef; });
        const uint64_t id = make_huntable(d, 10, 100);
        d.kill(id);
        const combat::Corpse* cd = d.after->corpse_of(id);
        CHECK(cd && cd->huntable && near(cd->close_at - cd->died_at, 120.0, 1e-6) && d.player->kills.empty(), "--no-loot: no experience, the corpse still waits for the hunter");
    }
}

}  // namespace

// ---- the zone where it died (Nodes 1, stage 1) -------------------------------------------------------------------------------------------------

/// The items a table gives, with everything under it.
void items_under(const combat::RewardBook& book, int64_t guid, std::set<int64_t>& items, std::set<int64_t>& seen) {
    if (!seen.insert(guid).second) return;
    const content::LootTable* t = book.table(guid);
    if (!t) return;
    for (const content::LootContainer& c : t->containers) {
        for (const content::LootReward& r : c.rewards) {
            for (const content::LootItem& it : r.items) items.insert(it.item);
        }
    }
    for (const int64_t s : t->subs) items_under(book, s, items, seen);
}
// -- C-O-A x C_Y_B --

void test_zone(Fixture& f) {
    std::printf("the loot rolls in the zone of influence where the creature died\n");
    std::string error;
    std::unique_ptr<world::CityNodes> nodes = world::CityNodes::load(real_db_path(), error);
    CHECKF(nodes != nullptr, "the node registry: %s", error.c_str());
    if (!nodes) return;
    std::string path;
    for (const auto& [p, row] : f.motion.rows()) {
        if (p.find(".Default__Bandit_Raider_Dunir_C") != std::string::npos) path = p;
    }
    CHECK(!path.empty(), "the Blueprint table has a melee Bandit");
    if (path.empty()) return;
    // BaseLoot_Melee_RVR and _TUR: the items only one of them gives
    std::set<int64_t> rvr, tur, seen;
    items_under(*f.rewards, 6064633918165090314ll, rvr, seen);   // BaseLoot_Melee_RVR
    seen.clear();
    items_under(*f.rewards, 6064633918182522894ll, tur, seen);   // BaseLoot_Melee_TUR
    std::set<int64_t> only_rvr, only_tur;
    for (const int64_t i : rvr) {
        if (!tur.count(i)) only_rvr.insert(i);
    }
    for (const int64_t i : tur) {
        if (!rvr.count(i)) only_tur.insert(i);
    }
    CHECK(!only_rvr.empty() && !only_tur.empty(), "the two tables have items of their own");

    struct Drops {
        size_t kills = 0, rvr = 0, tur = 0;
    };
    // kills of level-10 Bandits where (x, y) is, by a level-10 player; the loot that stands in the corpses is read
    auto kill_at = [&](double x, double y, world::CityNodes* registry) {
        Scene s(f, 10);
        Drops d;
        if (!s.ok()) return d;
        s.after->set_city_nodes(registry);
        for (int i = 0; i < 300; ++i) {
            const uint64_t id = s.w.spawn(path, x, y, 10, 0);
            if (!id) break;
            s.kill(id);
            ++d.kills;
            const combat::Corpse* c = s.after->corpse_of(id);
            if (!c) continue;
            for (const combat::LootStack& st : c->loot) {
                d.rvr += only_rvr.count(st.item);
                d.tur += only_tur.count(st.item);
            }
        }
        return d;
    };
    const world::CityNodeInfo* windansea = nodes->by_name("Windansea");
    CHECK(windansea != nullptr, "Windansea is a node");
    if (!windansea) return;
    const Drops here = kill_at(-777664.5, 616578.7, nodes.get());   // the login spot: Ruins of Aela, the Riverlands
    const Drops there = kill_at(windansea->x, windansea->y, nodes.get());   // Windansea: the Tropics
    std::printf("  300 Bandits at the login spot: %zu RVR-only / %zu TUR-only items; at Windansea: %zu / %zu\n", here.rvr, here.tur, there.rvr, there.tur);
    CHECKF(here.kills == 300 && there.kills == 300, "the kills were made (%zu, %zu)", here.kills, there.kills);
    CHECK(here.rvr > 0 && here.tur == 0, "dying in the Riverlands gives the _RVR gear and not the _TUR gear");
    CHECK(there.tur > 0 && there.rvr == 0, "dying in the Tropics gives the _TUR gear and not the _RVR gear");
}

int main() {
    if (!real_db_present()) {
        std::printf("test_worldd_aftermath: no content.db at %s (set AOC_CONTENT_DB): skipped\n", real_db_path().c_str());
        return 0;
    }
    Fixture f;
    if (!f.ok()) {
        std::printf("  [FAIL] fixture: %s\n", f.problem.c_str());
        return 1;
    }
    test_kill(f);
    test_respawn(f);
    test_hostile(f);
    test_huntable(f);
    test_zone(f);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
