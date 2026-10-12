// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_rewards.cpp: what a kill hands out (plan E1, stage 3, part 2), from the REAL content.db's reward tables
//
//    the data     content.db's loot_table (contentc version 10) holds the tables the Blueprints name and the sub tables they reach; the expressions of the
//                 kill table (Base_Kill_Reward_XP+Glint) compiled, the quest- and biome-gated ones did not (and read as "no")
//    experience   Base_Kill_XP at the creature's level times its reward multiplier, the same for the three kinds; an elite is worth more; the decay of the
//                 level difference (1 in the band, 0.05 nine below and further)
//    Glint        the chance (Glint_Chance x Loot_Falloff), the quantity (Glint_QMin .. QMax x the multiplier) and the falloff past nine levels
//    the rules    determinism by seed, a gated predicate gives nothing, an unknown root is skipped, a table named twice rolls once
//    the biome    (Nodes 1) the gear tables are gated by GetNodeBiome (BaseLoot_Melee_RVR, _TUR, _NONE ...): the kill's biome decides which of them roll
//  Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"
#include "combat/npc_stats.h"
#include "combat/rewards.h"
#include "npc/motion_table.h"

#include <cmath>
#include <set>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

using namespace ability_rig;
using namespace aoc::worldd;

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

constexpr int64_t kBaseKillTable = 6064630135340401618ll;   // Base_Kill_Reward_XP+Glint
constexpr int64_t kGlintT1 = 6064628753953390592ll;         // Certificate_T1_Glint
// ~ Cyb - C-O-A ~

struct Fixture {
    std::string                        path;
    std::optional<content::ContentDb>  db;
    const bootstrap::StatContext*      ctx = nullptr;
    npc::MotionTable                   motion;
    std::optional<abilities::Catalog>  catalog;
    std::optional<combat::NpcStatBook> book;
    std::optional<combat::RewardBook>  rewards;
    std::string                        problem;
    size_t                             roots = 0;

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
        catalog = abilities::Catalog::load(path, *ctx->bundle, error);
        if (!catalog) {
            problem = "the ability tables: " + error;
            return;
        }
        book.emplace(*ctx->bundle, *ctx->gear);
        std::set<int64_t> set;
        for (const auto& [p, row] : motion.rows()) set.insert(row.rewards.begin(), row.rewards.end());
        const std::vector<int64_t> all(set.begin(), set.end());
        roots = all.size();
        auto tables = db->loot_tables(all, error);
        if (!tables) {
            problem = "the loot tables: " + error;
            return;
        }
        const auto decay = db->xp_rule("adventuring_decay", error);
        if (!decay) {
            problem = "the xp rule: " + error;
            return;
        }
        rewards.emplace(*catalog, std::move(*tables), *decay);
    }
    bool ok() const { return problem.empty(); }

    std::optional<npc::MotionStats> blueprint(const std::string& leaf) const {
        const std::string suffix = "." + std::string("Default__") + leaf;
        for (const auto& [path, row] : motion.rows()) {
            if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) return row;
        }
        return std::nullopt;
    }
};

std::vector<std::string> tags_of(const std::string& csv) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < csv.size()) {
        size_t end = csv.find(',', at);
        if (end == std::string::npos) end = csv.size();
        if (end > at) out.push_back(csv.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

/// One kill of a creature: its stats' run, a seed and the players' side.
struct Kill {
    std::unique_ptr<stats::CharacterStats>             cs;
    std::unique_ptr<stats::CharacterStats::Evaluation> ev;
    std::vector<std::string>                           tags;
    std::vector<int64_t>                               roots;
    stats::SplitMix                                    rng;
    combat::RollContext                                ctx;
// built with C O A and C Y B

    bool make(Fixture& f, const std::string& leaf, int level, uint64_t seed) {
        const auto bp = f.blueprint(leaf);
        if (!bp) return false;
        std::string err;
        cs = f.book->make(*bp, level, err);
        if (!cs) return false;
        ev = cs->evaluation({}, err);
        if (!ev) return false;
        tags = tags_of(bp->tags);
        roots = bp->rewards;
        rng.state = seed;
        ctx.npc = ev.get();
        ctx.npc_tags = &tags;
        ctx.rng = &rng;
        ctx.loot_max_level = level;
        ctx.player_level = level;
        return true;
    }
};

void test_data(Fixture& f) {
    std::printf("the reward tables\n");
    CHECKF(f.roots > 100, "the Blueprints name %zu reward tables", f.roots);
    CHECKF(f.rewards->tables() >= 500, "with their sub tables %zu tables are read", f.rewards->tables());
    const content::LootTable* base = f.rewards->table(kBaseKillTable);
    CHECK(base != nullptr && base->name == "Base_Kill_Reward_XP+Glint", "Base_Kill_Reward_XP+Glint is there");
    if (!base) return;
    CHECK(base->containers.size() == 2, "it has two containers: the experience (granted at once) and the Glint (by chance)");
    if (base->containers.size() == 2) {
        CHECK(base->containers[0].grant == "Auto" && base->containers[0].rewards.size() == 1 && base->containers[0].rewards[0].xp.size() == 3,
              "the first container grants three kinds of experience");
        CHECK(base->containers[1].grant == "Always" && base->containers[1].pred != 0 && base->containers[1].rewards.size() == 1 &&
                  base->containers[1].rewards[0].items.size() == 1 && base->containers[1].rewards[0].items[0].item == kGlintT1,
              "the second is the Certificate_T1_Glint, behind a predicate");
        // the kill expressions compiled
        bool all = true;
        for (const content::LootXp& x : base->containers[0].rewards[0].xp) all = all && f.catalog->program(x.expr) >= 0;
        all = all && f.catalog->program(base->containers[1].pred) >= 0 && f.catalog->program(base->containers[1].rewards[0].items[0].qty) >= 0;
        CHECK(all, "the experience values, the chance and the quantity are programs of the stat program");
    }
    // the gated ones have no program (a quest, a biome ...): counted, read as "no"
    size_t gated = 0, total = 0;
    for (const int64_t g : {int64_t{6064630405967118341}, int64_t{6064630610665209856}}) {   // Goblin_Base, Bandit_Base
        if (const content::LootTable* t = f.rewards->table(g)) {
            ++total;
            gated += (t->pred != 0 && f.catalog->program(t->pred) < 0) ? 1 : 0;
        }
    }
    CHECKF(total == 2, "Goblin_Base and Bandit_Base are there (%zu of 2)", total);
    std::string why;
    CHECK(f.db->xp_rule("adventuring_decay", why).value_or(0) != 0, "the Adventuring decay rule is in the file");
}

void test_experience(Fixture& f) {
    std::printf("kill experience\n");
    Kill rabid;
    CHECK(rabid.make(f, "Rabid_C", 8, 7), "a level-8 Rabid");
    const combat::Rolled r = f.rewards->roll(rabid.roots, rabid.ctx);
    int64_t adv = 0, weapon = 0, node = 0;
    for (const combat::RolledXp& x : r.xp) {
        if (x.type == "Adventuring") adv = x.amount;
        if (x.type == "Weapon") weapon = x.amount;
        if (x.type == "NodeActivity") node = x.amount;
    }
    std::printf("  a level-8 Rabid is worth Adventuring %lld, Weapon %lld, NodeActivity %lld (Base_Kill_XP(8) = 450)\n", static_cast<long long>(adv), static_cast<long long>(weapon),
                static_cast<long long>(node));
    CHECK(adv > 0 && adv == weapon, "Adventuring and Weapon experience come from the same expression: equal");
    CHECK(adv >= 100 && adv <= 1500, "a level-8 creature's worth is Base_Kill_XP(8) = 450 times its multiplier, near it");
    CHECKF(r.tables >= 1, "%zu tables rolled", r.tables);

    Kill wolf, bear;
    CHECK(wolf.make(f, "Wolf_C", 12, 1) && bear.make(f, "Bear_C", 12, 1), "a Wolf and a Bear of level 12");
    auto adv_of = [&](Kill& k) {
        const combat::Rolled x = f.rewards->roll(k.roots, k.ctx);
        for (const combat::RolledXp& e : x.xp) {
            if (e.type == "Adventuring") return e.amount;
        }
        return int64_t{0};
    };
    const int64_t w = adv_of(wolf), b = adv_of(bear);
    std::printf("  at level 12: a Wolf %lld, a Bear %lld (elite rank %d)\n", static_cast<long long>(w), static_cast<long long>(b), static_cast<int>(bear.cs->npc().elite_rank));
    CHECK(w > 0 && b > 0, "both are worth experience");
    CHECK(bear.cs->npc().elite_rank == 0 || b > w, "a creature of a higher elite rank is worth more");

    Kill tough;
    if (tough.make(f, "Minotaur_Berserker_C", 12, 5)) {
        const int64_t t = adv_of(tough);
        std::printf("  a Minotaur Berserker of level 12 (elite rank %d): %lld\n", static_cast<int>(tough.cs->npc().elite_rank), static_cast<long long>(t));
        CHECK(t > w, "an elite is worth more than a plain creature of its level");
    }

    // the decay of the level difference: 1 in the band, 0.05 nine below and further, as the Adventuring leveling type says
    Kill k;
    CHECK(k.make(f, "Rabid_C", 8, 3), "a Rabid for the decay");
    auto worth = [&](int player_level) {
        k.ctx.player_level = player_level;
        return f.rewards->xp_for(1000, k.ctx);
    };
    CHECKF(worth(8) == 1000, "a player of the creature's level gets all of it (%lld)", static_cast<long long>(worth(8)));
    CHECKF(worth(7) >= 1000, "a creature above the player is worth at least as much (%lld)", static_cast<long long>(worth(7)));
    CHECKF(worth(17) == 50 && worth(25) == 50, "nine levels below and further it is worth 5 %% (%lld at 17, %lld at 25)", static_cast<long long>(worth(17)), static_cast<long long>(worth(25)));
    CHECKF(worth(12) == 550 || worth(12) == 600 || (worth(12) > 50 && worth(12) < 1000), "four levels below it is a share (%lld)", static_cast<long long>(worth(12)));
    CHECK(worth(9) > worth(10) && worth(10) > worth(11), "the decay falls with the difference");
}

void test_glint(Fixture& f) {
    std::printf("Glint\n");
    // Glint_Chance(8) = 0.3756; Loot_Falloff(0) = 1: about 3 kills in 8 give some
    size_t kills = 0, with_glint = 0;
    int64_t total = 0, lo = 1 << 30, hi = 0;
    for (uint64_t seed = 1; seed <= 3000; ++seed) {
        Kill k;
        if (!k.make(f, "Rabid_C", 8, seed * 7919)) {
            CHECK(false, "a Rabid");
            return;
        }
        const combat::Rolled r = f.rewards->roll(k.roots, k.ctx);
        ++kills;
        for (const combat::RolledItem& it : r.loot) {
            if (it.item != kGlintT1) continue;
            ++with_glint;
            total += it.quantity;
            lo = std::min<int64_t>(lo, it.quantity);
            hi = std::max<int64_t>(hi, it.quantity);
        }
    }
    const double rate = static_cast<double>(with_glint) / static_cast<double>(kills);
    std::printf("  %zu of %zu level-8 kills dropped Certificate_T1_Glint (%.3f), %lld to %lld each\n", with_glint, kills, rate, static_cast<long long>(lo), static_cast<long long>(hi));
    CHECKF(rate > 0.33 && rate < 0.42, "the chance is Glint_Chance(8) = 0.376 (measured %.3f)", rate);
    CHECK(lo >= 1 && hi <= 20 && hi > lo, "a drop is a few certificates, Glint_QMin(8) = 4 to Glint_QMax(8) = 8 times the multiplier");
// [C-O-A - Cyb]

    // the same kill with a player 12 levels below the creature's level: Loot_Falloff(12) = 0: nothing
    size_t far = 0;
    for (uint64_t seed = 1; seed <= 500; ++seed) {
        Kill k;
        if (!k.make(f, "Rabid_C", 8, seed)) continue;
        k.ctx.loot_max_level = 20;
        for (const combat::RolledItem& it : f.rewards->roll(k.roots, k.ctx).loot) far += it.item == kGlintT1 ? 1 : 0;
    }
    CHECKF(far == 0, "a looter 12 levels above the creature gets no Glint (Loot_Falloff) (%zu in 500 kills)", far);
}

/// The knob of 2026-10-10 (the user: drops are too rare): RollContext::chance_mult multiplies the chance of every RollEachByPercent draw and nothing else.
void test_chance_mult(Fixture& f) {
    std::printf("the loot-chance knob\n");
    struct Tally {
        size_t kills = 0, kills_with_items = 0, stacks = 0, glint = 0;
        int64_t quantity = 0;
    };
    const auto run = [&](const char* leaf, int level, double mult, size_t n) {
        Tally t;
        for (uint64_t seed = 1; seed <= n; ++seed) {
            Kill k;
            if (!k.make(f, leaf, level, seed * 7919)) return t;
            k.ctx.chance_mult = mult;
            ++t.kills;
            bool any = false;
            for (const combat::RolledItem& it : f.rewards->roll(k.roots, k.ctx).loot) {
                if (it.item == kGlintT1) {
                    ++t.glint;
                    continue;
                }
                ++t.stacks;
                t.quantity += it.quantity;
                any = true;
            }
            t.kills_with_items += any ? 1 : 0;
        }
        return t;
    };
    // a creature whose tables give items by percent, in the band of its level (the kind tables: Bandit_Base ...)
    const char* chosen = nullptr;
    Tally one, three, many, none;
    for (const char* leaf : {"Bandit_Raider_Dunir_C", "Wolf_C", "Rabid_C", "Bear_C", "Goblin_Bonebinder_C"}) {
        one = run(leaf, 10, 1.0, 3000);
        std::printf("  %s, level 10, 3000 kills at the data's own odds: %zu with items (%zu stacks), Glint in %zu\n", leaf, one.kills_with_items, one.stacks, one.glint);
        if (one.kills > 0 && one.stacks >= 30 && one.kills_with_items * 2 < one.kills) {   // chances small enough for a multiplier to show
            chosen = leaf;
            break;
        }
    }
    CHECK(chosen != nullptr, "a creature of the data gives items by percent, at small chances");
    if (!chosen) return;
    three = run(chosen, 10, 3.0, 3000);
    many = run(chosen, 10, 1000.0, 3000);
    none = run(chosen, 10, 0.0, 3000);
    std::printf("  %s: stacks of items in 3000 kills: x0 %zu, x1 %zu, x3 %zu, x1000 %zu; kills with items: %zu, %zu, %zu, %zu; Glint stacks x1 %zu, x3 %zu\n", chosen, none.stacks, one.stacks, three.stacks, many.stacks, none.kills_with_items,
                one.kills_with_items, three.kills_with_items, many.kills_with_items, one.glint, three.glint);
    CHECK(three.stacks * 10 >= one.stacks * 18, "x3 makes the item drops clearly more frequent (at least 1.8 times as many stacks in the same kills)");
    // (a table picks its sub tables by percent and a sub table's items by percent again: the two draws are multiplied, so x3 gives up to about 9 times the stacks, never more)
    CHECK(three.stacks * 10 <= one.stacks * 95 + 60, "...but not more than the multiplier gives twice over (two nested percent draws: at most about 9 times the stacks)");
    CHECK(many.stacks >= three.stacks && none.stacks <= one.stacks, "a larger multiplier gives more, 0 gives fewer than the data's own (each chance is capped at 1)");
    // the quantities are the data's: the mean size of a stack does not move with the multiplier
    const double mean1 = one.stacks ? static_cast<double>(one.quantity) / one.stacks : 0, mean3 = three.stacks ? static_cast<double>(three.quantity) / three.stacks : 0;
    std::printf("  the mean stack: %.2f at x1, %.2f at x3\n", mean1, mean3);
    CHECK(mean1 > 0 && std::fabs(mean3 - mean1) <= 0.25 * mean1 + 0.3, "a stack is as big as it was: the multiplier never touches a quantity");
    // Glint is not a percent draw (its chance is a predicate on Glint_Chance): untouched
    CHECK(three.glint * 10 <= one.glint * 13 + 40 && three.glint * 13 + 40 >= one.glint * 10, "and the Glint certificates (a predicate, not a percent draw) are as frequent as before");
    // the default is the data's own odds: a context nobody set rolls as chance_mult 1
    Kill a, b;
    a.make(f, chosen, 10, 777);
    b.make(f, chosen, 10, 777);
    b.ctx.chance_mult = 1.0;
    const combat::Rolled ra = f.rewards->roll(a.roots, a.ctx), rb = f.rewards->roll(b.roots, b.ctx);
    CHECK(ra.loot.size() == rb.loot.size() && combat::RollContext{}.chance_mult == 1.0, "a context nobody set rolls the data's own odds (chance_mult is 1)");
}

void test_rules(Fixture& f) {
    std::printf("the rules\n");
    Kill a, b;
    CHECK(a.make(f, "Goblin_Bonebinder_C", 5, 99) || a.make(f, "Wolf_C", 5, 99), "a creature with a kind's own table");
    CHECK(b.make(f, a.cs ? "Wolf_C" : "Wolf_C", 5, 99), "another");
    // the same seed gives the same roll
    Kill c, d;
    c.make(f, "Wolf_C", 6, 4242);
    d.make(f, "Wolf_C", 6, 4242);
    const combat::Rolled rc = f.rewards->roll(c.roots, c.ctx), rd = f.rewards->roll(d.roots, d.ctx);
    CHECK(rc.loot.size() == rd.loot.size() && rc.xp.size() == rd.xp.size() && (rc.xp.empty() || rc.xp[0].amount == rd.xp[0].amount), "the same seed rolls the same kill");
    // an unknown root is skipped; a root named twice rolls once
    std::vector<int64_t> roots = {123456789, kBaseKillTable, kBaseKillTable};
    Kill e;
    e.make(f, "Wolf_C", 6, 5);
    const combat::Rolled re = f.rewards->roll(roots, e.ctx);
    CHECK(re.tables == 1 && re.xp.size() == 3, "an unknown table is skipped and a table named twice rolls once");
    // no stats, no rolls of an expression: nothing is made up
    combat::RollContext bare;
    bare.rng = e.ctx.rng;
    const combat::Rolled rb = f.rewards->roll({kBaseKillTable}, bare);
    CHECK(rb.xp.empty() && rb.loot.empty(), "without the creature's stats no expression runs: nothing is given");
    // a table that is gated by a quest: no program, nothing
    size_t gated_tables = 0;
    for (const int64_t g : e.roots) {
        if (const content::LootTable* t = f.rewards->table(g)) gated_tables += (t->pred != 0 && f.catalog->program(t->pred) < 0) ? 1 : 0;
    }
    (void)gated_tables;
    Kill g2;
    g2.make(f, "Wolf_C", 6, 6);
    const combat::Rolled none = f.rewards->roll({}, g2.ctx);
    CHECK(none.xp.empty() && none.loot.empty() && none.tables == 0, "no roots, no reward");
    // the kind tables of the data give gear, not only Glint: Bandit_Base and its sub tables across many kills
    size_t kinds = 0;
    std::set<int64_t> items;
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        Kill k;
        if (!k.make(f, "Bandit_Raider_Dunir_C", 10, seed * 31)) break;
        for (const combat::RolledItem& it : f.rewards->roll(k.roots, k.ctx).loot) {
            items.insert(it.item);
            ++kinds;
        }
    }
    std::printf("  400 Bandit Raiders of level 10 dropped %zu stacks of %zu kinds of item\n", kinds, items.size());
    CHECK(items.size() >= 1, "a bandit's tables give items");
}
// Cyb + C.O.A

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

void test_biome(Fixture& f) {
    std::printf("the biome-gated gear\n");
    // BaseLoot_Melee is RollEachByPercent over BaseLoot_Melee_RVR, _TUR, _SAN, _JUN, _ANV and _NONE, each behind a table predicate that reads GetNodeBiome(); _NONE
    // (every one of the six biomes ruled out) gives the same Gear, Recipe and Reagent tables as _RVR
    constexpr int64_t kMeleeRvr = 6064633918165090314ll, kMeleeTur = 6064633918182522894ll, kMeleeNone = 6064633933201475221ll;
    constexpr int kDefault = 0, kRiverlands = 14, kTropics = 18;
    {
        size_t compiled = 0;
        for (const int64_t table : {kMeleeRvr, kMeleeTur, kMeleeNone}) {
            const content::LootTable* t = f.rewards->table(table);
            if (t && t->pred != 0 && f.catalog->program(t->pred) >= 0) ++compiled;
        }
        CHECKF(compiled == 3, "the predicates of BaseLoot_Melee_RVR, _TUR and _NONE are programs now (%zu of 3)", compiled);
    }
    std::set<int64_t> rvr_items, tur_items, none_items, seen;
    items_under(*f.rewards, kMeleeRvr, rvr_items, seen);
    seen.clear();
    items_under(*f.rewards, kMeleeTur, tur_items, seen);
    seen.clear();
    items_under(*f.rewards, kMeleeNone, none_items, seen);
    CHECKF(!rvr_items.empty() && !tur_items.empty() && none_items == rvr_items, "BaseLoot_Melee_RVR gives %zu kinds of item, _TUR %zu, _NONE the same as _RVR (%zu)", rvr_items.size(), tur_items.size(),
           none_items.size());
    std::set<int64_t> only_rvr, only_tur;
    for (const int64_t i : rvr_items) {
        if (!tur_items.count(i)) only_rvr.insert(i);
    }
    for (const int64_t i : tur_items) {
        if (!rvr_items.count(i)) only_tur.insert(i);
    }
    CHECKF(!only_rvr.empty() && !only_tur.empty(), "each has items of its own: %zu, %zu", only_rvr.size(), only_tur.size());
    // a melee creature whose tables reach BaseLoot_Melee
    std::string leaf;
    for (const char* candidate : {"Bandit_Raider_Dunir_C", "Minotaur_Berserker_C", "Goblin_Gravehide_C", "Bandit_Brute_C", "Minotaur_Warrior_C"}) {
        Kill k;
        if (!k.make(f, candidate, 10, 1)) continue;
        std::set<int64_t> under, seen_roots;
        for (const int64_t r : k.roots) items_under(*f.rewards, r, under, seen_roots);
        bool reaches = false;
        for (const int64_t i : only_rvr) reaches = reaches || under.count(i);
        if (reaches) {
            leaf = candidate;
            break;
        }
    }
    CHECK(!leaf.empty(), "a melee creature of the area whose tables reach BaseLoot_Melee");
    if (leaf.empty()) return;
    // the same creature killed in the Riverlands, in the Tropics and where the zone has the Default biome, many times
    struct Drops {
        size_t rvr = 0, tur = 0, stacks = 0, tables = 0;
    };
    auto kills = [&](int biome) {
        Drops d;
        for (uint64_t seed = 1; seed <= 600; ++seed) {
            Kill k;
            k.make(f, leaf, 10, seed * 104729);
            k.ctx.biome = biome;
            const combat::Rolled r = f.rewards->roll(k.roots, k.ctx);
            d.tables += r.tables;
            for (const combat::RolledItem& it : r.loot) {
                ++d.stacks;
                d.rvr += only_rvr.count(it.item);
                d.tur += only_tur.count(it.item);
            }
        }
        return d;
    };
    const Drops rv = kills(kRiverlands), tr = kills(kTropics), no = kills(kDefault);
    std::printf("  600 kills of a %s: Riverlands %zu RVR-only / %zu TUR-only items (%zu stacks, %zu tables rolled); Tropics %zu / %zu (%zu, %zu); Default %zu / %zu (%zu, %zu)\n", leaf.c_str(), rv.rvr,
                rv.tur, rv.stacks, rv.tables, tr.rvr, tr.tur, tr.stacks, tr.tables, no.rvr, no.tur, no.stacks, no.tables);
    CHECK(rv.rvr > 0 && rv.tur == 0, "in the Riverlands the _RVR gear drops and the _TUR gear does not");
    CHECK(tr.tur > 0 && tr.rvr == 0, "in the Tropics the _TUR gear drops and the _RVR gear does not");
    CHECK(no.rvr > 0 && no.tur == 0, "where the zone has the Default biome the _NONE table rolls (the same gear as _RVR)");
    // and the registry answers the same: a kill where the node registry puts the biome and the node
    {
        std::vector<world::CityNodeInfo> infos(2);
        infos[0].guid = 5;
        infos[1].guid = 6;
        world::CityNodes registry(std::move(infos), {});
        size_t rvr = 0;
        for (uint64_t seed = 1; seed <= 600; ++seed) {
            Kill k;
            k.make(f, leaf, 10, seed * 104729);
            k.ctx.nodes = &registry;
            k.ctx.node = 1;
            k.ctx.biome = kRiverlands;
            for (const combat::RolledItem& it : f.rewards->roll(k.roots, k.ctx).loot) rvr += only_rvr.count(it.item);
        }
        CHECK(rvr == rv.rvr, "with a registry in the context the roll is the same as with the biome alone");
    }
}

}  // namespace

int main() {
    if (!real_db_present()) {
        std::printf("test_worldd_rewards: no content.db at %s (set AOC_CONTENT_DB): skipped\n", real_db_path().c_str());
        return 0;
    }
    Fixture f;
    if (!f.ok()) {
        std::printf("  [FAIL] fixture: %s\n", f.problem.c_str());
        return 1;
    }
    test_data(f);
    test_experience(f);
    test_glint(f);
    test_chance_mult(f);
    test_rules(f);
    test_biome(f);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
