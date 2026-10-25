// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/aftermath.cpp (see aftermath.h)
// ============================================================================
#include "combat/aftermath.h"

#include "items/rarity.h"
#include "protocol/repl/value.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aoc::worldd::combat {

namespace {

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

}  // namespace

int Aftermath::rarity_of(int64_t item, int quality) const {
    return rarity_of_ ? rarity_of_(item, quality) : items::default_rarity_facts().rarity_of(static_cast<double>(quality));
}

Aftermath::Aftermath(world::ActorRegistry& registry, CombatWorld& combat, const RewardBook* book, world::Minter mint, AftermathConfig config)
    : registry_(registry), combat_(combat), book_(book), mint_(std::move(mint)), cfg_(std::move(config)) {
    rng_.state = cfg_.seed * 0x9E3779B97F4A7C15ull + 11;
}

Aftermath::~Aftermath() = default;

void Aftermath::install() {
    combat_.set_death_hook([this](const DeathEvent& d) { on_death(d); });
}

void Aftermath::on_death(const DeathEvent& d) {
    ++stats_.deaths;
    const bool huntable = huntable_ && d.info && huntable_(*d.info);
    if (!cfg_.enabled) {
        // the corpse still goes: nothing more is made of the death
        Corpse c;
        c.npc = d.npc;
        c.died_at = d.at;
        c.close_at = d.at + cfg_.corpse_seconds;
        if (huntable) {   // (a huntable's corpse is hunted, not looted: it stays for the hunt whatever happens to the rewards)
            c.huntable = true;
            c.close_at = d.at + cfg_.hunt_corpse_seconds;
        }
        corpses_[d.npc] = std::move(c);
        return;
    }
    Corpse c;
    c.npc = d.npc;
    c.died_at = d.at;
    c.close_at = d.at + cfg_.corpse_seconds;
    if (huntable) {   // Gathering stage 3: no loot pile, a corpse that waits for the hunter
        c.huntable = true;
        c.close_at = d.at + cfg_.hunt_corpse_seconds;
    }
    // (the Nodes 1 test of 2026-10-10: the death and loot lines had an empty creature name) the creature's name, else its Blueprint's
    c.name = d.info && !d.info->name.empty() ? d.info->name : d.blueprint;
    std::string looter_note;   // for the death line: the highest looter's level, the level gap and the data's Loot_Falloff
    std::string roll_note;     // ... and what the rarity rolls did (item rarities, step 1)

    // who fought it and is here to be rewarded
    std::vector<PlayerPort*> fighters;
    if (d.gm) {
        // a game master's kill: nobody fought it, so every player in the world may take its loot, and nobody is given experience
        fighters = combat_.players();
    } else {
        if (const auto killer = combat_.find_player(d.killer)) fighters.push_back(killer);
        for (const uint64_t id : d.attackers) {
            PlayerPort* p = combat_.find_player(id);
            if (p && std::find(fighters.begin(), fighters.end(), p) == fighters.end()) fighters.push_back(p);
        }
    }
    for (PlayerPort* p : fighters) c.rights.insert(p->actor_id());

    // the respawn: from the death, not from the corpse's closing
    if (d.info) schedule_respawn(*d.info, d.at, d.npc);
// C.Y.B and C-O-A

    if (book_ && (!fighters.empty() || d.gm)) {
        Fighter* npc = combat_.fighter_of(d.npc);
        std::string err;
        std::unique_ptr<stats::CharacterStats::Evaluation> ev = npc ? npc->stats().evaluation({}, err) : nullptr;
        if (ev) {
            std::vector<std::string> tags = d.motion ? tags_of(d.motion->tags) : std::vector<std::string>{};
            double max_level = 1;
            for (PlayerPort* p : fighters) max_level = std::max<double>(max_level, p->fighter().level());
            if (d.gm) max_level = std::max(1, d.level);   // rolled as for a player of the creature's own level: no level gap takes anything off
            RollContext ctx;
            ctx.npc = ev.get();
            ctx.npc_tags = &tags;
            ctx.loot_max_level = max_level;
            ctx.chance_mult = cfg_.loot_chance_mult;
            ctx.rarity_boost = cfg_.loot_rarity_boost;
            ctx.rng = &rng_;
            // Loot_Falloff is 1 for a gap of 0 to 9 levels and 0 from 10 (the reward tables' own, settled against retail 2026-10-08): said on the line so a drop that did not come can be read
            const int gap = static_cast<int>(std::lround(std::fabs(max_level - static_cast<double>(d.level))));
            looter_note = ", looter level " + std::to_string(static_cast<int>(std::lround(max_level))) + " (" + std::to_string(gap) + " level" + (gap == 1 ? "" : "s") + " apart: Loot_Falloff " +
                          (gap >= 10 ? "0, no gear, recipe or Glint loot" : "1") + ")" + (cfg_.loot_chance_mult != 1.0 ? ", loot chances x" + std::to_string(cfg_.loot_chance_mult).substr(0, 5) : std::string());
            if (d.gm) looter_note = ", a game master's kill, rolled as for a player of the creature's own level" + (cfg_.loot_chance_mult != 1.0 ? ", loot chances x" + std::to_string(cfg_.loot_chance_mult).substr(0, 5) : std::string());
            if (cfg_.loot_rarity_boost != 1.0) {   // the test boost of the rarity rolls (worldd --loot-rarity-boost): said on every kill's line
                char boost[32];
                std::snprintf(boost, sizeof boost, "%g", cfg_.loot_rarity_boost);
                looter_note += std::string(", rarity odds x") + boost + " (test boost)";
            }
            if (nodes_) {
                // the zone of influence where it died: its biome and node answer the tables' node reads
                const world::NodeSite site = nodes_->node_at(d.position.x, d.position.y);
                ctx.nodes = nodes_;
                ctx.node = site.node_rank;
                ctx.biome = site.biome;
            }
            std::vector<int64_t> roots;
            if (d.motion) roots = d.motion->rewards;
            if (d.info) roots.insert(roots.end(), d.info->loot_tables.begin(), d.info->loot_tables.end());
            ctx.player_level = max_level;
            Rolled rolled = book_->roll(roots, ctx);
            if (huntable) {   // the kill's experience stays; its items (the Glint certificates, the Bezoar roll) are not dropped: the corpse's reward is the hunt's
                rolled.loot.clear();
                rolled.direct.clear();
            }
            // what the rarity rolls did, for the death line: the creature's elite rank (a gear roll adds 2 per rank), the qualities rolled, and the Common outcomes the test boost threw back
            if (rolled.quality_rolls > 0 || rolled.quality_failures > 0) {
                roll_note = ", " + std::to_string(rolled.quality_rolls) + " quality roll(s) at elite rank " + std::to_string(static_cast<int>(std::lround(npc->stats().npc().elite_rank))) +
                            (rolled.boost_redraws > 0 ? " (" + std::to_string(rolled.boost_redraws) + " Common outcome(s) thrown back by the rarity boost)" : std::string()) +
                            (rolled.quality_failures > 0 ? ", " + std::to_string(rolled.quality_failures) + " quality formula(s) did not run" : std::string());
            }
            // the loot: a stack has the quality its variation rolled (a gear or a resource drop), else the item record's default (a Glint tier keeps its 1, 21 or 41)
            std::vector<LootStack> stacks;
            for (const std::vector<RolledItem>* list : {&rolled.loot, &rolled.direct}) {
                for (const RolledItem& it : *list) {
                    LootStack s;
                    s.item = it.item;
                    s.variation = it.variation;
                    s.quantity = it.quantity;
                    s.quality = it.quality > 0 ? std::clamp(it.quality, items::kMinQuality, items::kMaxQuality) : (quality_of_ ? std::max(1, quality_of_(it.item)) : 1);
                    stacks.push_back(s);
                }
            }
            if (!stacks.empty() && !fighters.empty()) {
                int top = 0;
                for (const LootStack& s : stacks) top = std::max(top, rarity_of(s.item, s.quality));
                std::vector<protocol::repl::GuidValue> guids;
                for (PlayerPort* p : fighters) {
                    const auto w = p->character_guid();
                    guids.push_back({w[0], w[1], w[2], w[3]});
                }
                std::string why;
                const auto parts = world::add_loot(registry_, d.npc, mint_, guids.front(), guids, top, why);
                if (parts) {
                    c.parts = *parts;
                    c.loot = std::move(stacks);
                    c.top_rarity = top;
                    c.close_at = d.at + cfg_.loot_corpse_seconds;
                    ++stats_.corpses_with_loot;
                    stats_.loot_stacks += c.loot.size();
                } else {
                    spdlog::warn("[loot] {} ({}) died with {} stack(s) of loot but its corpse takes no components: {}", c.name, d.blueprint, stacks.size(), why);
                }
            } else if (!stacks.empty() && d.gm) {
                // a game master's kill with nobody in the world to take it: the roll stays on the corpse (the log and Inspect say what it holds), but it gets no loot components
                c.loot = std::move(stacks);
            }
            // the experience: each fighter's own decay (a game master's kill gives none)
            for (PlayerPort* p : fighters) {
                if (d.gm) break;
                ctx.player_level = p->fighter().level();
                KillReward kr;
                kr.npc = d.npc;
                kr.name = c.name;
                kr.blueprint = d.blueprint;
                kr.level = d.level;
                for (const RolledXp& x : rolled.xp) {
                    const int64_t amount = book_->xp_for(x.amount, ctx);
                    if (x.type == "Adventuring") kr.adventuring_xp += amount;
                    else if (x.type == "Weapon") kr.weapon_xp += amount;
                    else if (x.type == "NodeActivity") kr.node_xp += amount;
                }
                kr.loot = c.parts.has_value();
                p->receive_kill(kr, d.at);
                stats_.xp_awarded += static_cast<uint64_t>(std::max<int64_t>(0, kr.adventuring_xp));
            }
        } else {
            spdlog::debug("[loot] {} has no stats to roll its rewards with: {}", d.blueprint, err);
        }
    }
    std::string listing;
    for (const LootStack& s : c.loot) {
        const std::string n = name_of_ ? name_of_(s.item) : std::string();
        // each stack names its quality and the rarity the client shows for it (a Glint tier is always its colour)
        listing += (listing.empty() ? "" : ", ") + (n.empty() ? "item " + std::to_string(s.item) : n) + " x" + std::to_string(s.quantity) + " q" + std::to_string(s.quality) + " " +
                 std::string(items::rarity_name(rarity_of(s.item, s.quality)));
    }
    const std::string who = d.gm ? std::to_string(fighters.size()) + " player(s) in the world may take its loot" : std::to_string(fighters.size()) + " fought it";
    spdlog::info("[loot] {} ({}, level {}) died: {}{}{}, {} stack(s) of loot{}{}{}, closes in {:.0f} s{}{}", c.name, d.blueprint, d.level, who, looter_note, roll_note, c.loot.size(),
                 listing.empty() ? "" : " (", listing, listing.empty() ? "" : ")", c.close_at - d.at, respawns_.empty() ? "" : "; its spawn point refills it",
                 huntable ? "; a huntable: its corpse is hunted with a Bow (no loot)" : "");
    corpses_[d.npc] = std::move(c);
}

bool Aftermath::refills(const world::NpcInfo& info) const {
    return cfg_.respawn && respawner_ && info.spawn_ref != 0 && info.respawn_rule != "NoRespawn";
}

double Aftermath::respawn_delay(const world::NpcInfo& info) {
    if (!refills(info)) return 0.0;
    const double base = info.respawn_seconds > 0 ? info.respawn_seconds : cfg_.default_respawn_seconds;
    const double var = std::max(0.0, info.respawn_variance);
    return std::max(cfg_.min_respawn_seconds, base + var * (2.0 * rng_.rand_double() - 1.0));
}

void Aftermath::schedule_respawn(const world::NpcInfo& info, double at, uint64_t npc) {
    if (!refills(info)) return;
    respawns_.push_back({at + respawn_delay(info), info.spawn_ref, npc});
}

double Aftermath::respawn_in(uint64_t npc, double now) const {
    if (npc == 0) return 0.0;
    for (const Respawn& r : respawns_) {
        if (r.npc == npc) return std::max(0.0, r.at - now);
    }
    return 0.0;
}

uint64_t Aftermath::respawn_now(uint64_t npc, double now) {
    if (npc == 0 || !respawner_) return 0;
    for (auto it = respawns_.begin(); it != respawns_.end(); ++it) {
        if (it->npc != npc) continue;
        const uint32_t ref = it->spawn_ref;
        respawns_.erase(it);
        const uint64_t made = respawner_(ref, now);
        if (made != 0) ++stats_.respawns;
        return made;
    }
    return 0;
}

bool Aftermath::despawn(uint64_t npc, double now, bool refill, double* seconds) {
    if (seconds) *seconds = 0.0;
    const world::WorldActor* a = registry_.find(npc);
    if (!a) return false;
    const world::NpcInfo info = a->npc;   // (a copy: the actor goes)
    const bool waiting = respawn_in(npc, now) > 0.0 || std::any_of(respawns_.begin(), respawns_.end(), [&](const Respawn& r) { return r.npc == npc; });
    corpses_.erase(npc);
    registry_.remove(npc);   // every connection that holds it closes the channel (Destroyed)
    combat_.forget(npc);
    if (refill && !waiting) schedule_respawn(info, now, npc);
    if (seconds) *seconds = respawn_in(npc, now);
    return true;
}

void Aftermath::tick(double now) {
    for (auto it = corpses_.begin(); it != corpses_.end();) {
        if (now >= it->second.close_at && it->second.living) {
            it = corpses_.erase(it);   // (a hunt nobody ended: the creature is alive, it stays; only its record goes)
        } else if (now >= it->second.close_at) {
            const uint64_t id = it->first;
            it = corpses_.erase(it);
            registry_.remove(id);   // every connection that holds it closes the channel (Destroyed)
            combat_.forget(id);
            ++stats_.corpses_closed;
        } else {
            ++it;
        }
    }
    for (auto it = respawns_.begin(); it != respawns_.end();) {
        if (now >= it->at) {
            const uint32_t ref = it->spawn_ref;
            it = respawns_.erase(it);
            if (respawner_ && respawner_(ref, now) != 0) ++stats_.respawns;
        } else {
            ++it;
        }
    }
}

const Corpse* Aftermath::corpse_of(uint64_t npc) const {
    const auto it = corpses_.find(npc);
    return it == corpses_.end() ? nullptr : &it->second;
}

const Corpse* Aftermath::corpse_by_interactable(uint64_t object_id) const {
    for (const auto& [id, c] : corpses_) {
        if (c.parts && c.parts->interactable == object_id) return &c;
    }
    return nullptr;
}

const LootStack* Aftermath::take(uint64_t npc, size_t slot) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || slot >= it->second.loot.size() || it->second.loot[slot].taken) return nullptr;
    it->second.loot[slot].taken = true;
    ++stats_.loot_taken;
    return &it->second.loot[slot];
}

void Aftermath::put_back(uint64_t npc, size_t slot) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || slot >= it->second.loot.size() || !it->second.loot[slot].taken) return;
    it->second.loot[slot].taken = false;
    if (stats_.loot_taken > 0) --stats_.loot_taken;
}
// Cyb captures, C O A code

uint64_t Aftermath::drop_pile(double x, double y, double z, const protocol::repl::GuidValue& owner, const std::string& name, std::vector<LootStack> stacks, double now,
                              std::string& error) {
    if (stacks.empty()) return 0;
    int top = 0;
    for (const LootStack& s : stacks) top = std::max(top, rarity_of(s.item, s.quality));
    const auto pile = world::add_pile(registry_, mint_, x, y, z, owner, name, {owner}, top, error);
    if (!pile) return 0;
    Corpse c;
    c.npc = pile->actor;
    c.name = name + "'s pile";
    c.died_at = now;
    c.close_at = now + cfg_.pile_seconds;
    c.loot = std::move(stacks);
    c.parts = pile->parts;
    c.top_rarity = top;
    c.is_pile = true;
    corpses_[c.npc] = std::move(c);
    ++stats_.piles;
    return pile->actor;
}

bool Aftermath::hunt_started(uint64_t npc, double now, double seconds) {
    auto it = corpses_.find(npc);
    if (it == corpses_.end()) {
        // a huntable creature that is alive: hunted as it stands
        const world::WorldActor* a = registry_.find(npc);
        if (!a || a->kind != world::ActorKind::Npc || !huntable_ || !huntable_(a->npc)) return false;
        Corpse c;
        c.npc = npc;
        c.name = a->npc.name;
        c.died_at = now;
        c.close_at = now + seconds + 5.0;
        c.huntable = true;
        c.living = true;
        corpses_[npc] = std::move(c);
        return true;
    }
    if (!it->second.huntable) return false;
    it->second.close_at = std::max(it->second.close_at, now + seconds + 5.0);   // the channel is never cut short by the corpse going
    return true;
}

void Aftermath::hunt_cancelled(uint64_t npc) {
    const auto it = corpses_.find(npc);
    if (it != corpses_.end() && it->second.living) corpses_.erase(it);
}

bool Aftermath::hunt_done(uint64_t npc, double now) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || !it->second.huntable) return false;
    Corpse& c = it->second;
    if (c.living) {   // the hunt is the creature's end: it dies for its clients with the done update, and its spawn point refills it
        c.living = false;
        c.died_at = now;
        if (const world::WorldActor* a = registry_.find(npc)) schedule_respawn(a->npc, now, npc);
    }
    c.hunted = true;
    c.close_at = std::min(c.close_at, now + cfg_.hunt_done_seconds);
    return true;
}

void Aftermath::emptied(uint64_t npc, double now) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || it->second.emptied) return;
    Corpse& c = it->second;
    c.emptied = true;
    world::empty_loot(registry_, npc);
    c.close_at = std::min(c.close_at, now + cfg_.emptied_seconds);
}

}  // namespace aoc::worldd::combat
