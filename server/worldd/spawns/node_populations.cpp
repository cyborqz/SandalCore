// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/node_populations.cpp (see node_populations.h)
// ============================================================================
#include "spawns/node_populations.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::spawns {

namespace {

constexpr const char* kMasterMap = "Verra_World_Master";

std::string reasons_of(const NodePopStats& s) {
    std::vector<std::pair<std::string, size_t>> v;
    for (const auto& [why, n] : s.pop.filtered) v.emplace_back(why_name(why), n);
    if (s.pop.no_template) v.emplace_back("no template", s.pop.no_template);
    if (s.pop.no_asset) v.emplace_back("no asset", s.pop.no_asset);
    if (s.outside) v.emplace_back("outside the area", s.outside);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 5; ++i) out += (out.empty() ? "" : ", ") + v[i].first + " " + std::to_string(v[i].second);
    return out.empty() ? "nothing" : out;
}

}  // namespace

NodePopulations::NodePopulations(world::ActorRegistry& registry, const world::CityNodes& nodes, const world::NodeWire& wire, const NodePopulationData& data,
                                 std::shared_ptr<const content::PopulationData> population, const TemplateSource& templates, SpawnContext base, const npc::MotionTable* motion,
                                 const AreaConfig& area, Minter mint, uint32_t server_id, Hooks hooks, Config config)
    : registry_(registry), nodes_(nodes), wire_(wire), data_(data), population_(std::move(population)), templates_(templates), base_(std::move(base)), motion_(motion), area_(area),
      mint_(std::move(mint)), server_id_(server_id), hooks_(std::move(hooks)), config_(std::move(config)) {}

const NodePopulations::Entry* NodePopulations::entry_of(int64_t node) const {
    const auto it = entries_.find(node);
    return it == entries_.end() ? nullptr : &it->second;
}
// C.Y.B - C-O-A

std::vector<uint64_t> NodePopulations::actors_of(int64_t node) const {
    std::vector<uint64_t> out;
    const auto it = states_.find(node);
    if (it == states_.end()) return out;
    for (const auto& [ident, l] : it->second.live) {
        if (l.actor != 0 && registry_.find(l.actor)) out.push_back(l.actor);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool NodePopulations::in_area(const world::CityNodeInfo& info) const {
    return info.map_name == kMasterMap && wire_.is_real_node(info.guid) && wire_.layout_of(info.guid) != 0 && area_.contains(info.x, info.y);
}

NodeSite NodePopulations::site_of(const world::CityNodeInfo& info, int level) const { return node_site_of(nodes_, wire_, info, level); }

SpawnContext NodePopulations::context_of(const world::CityNodeInfo& info, int level) const {
    return node_spawn_context(base_, site_of(info, level), config_.shown_humanoids);
}

bool NodePopulations::same_point(const NodeSpawn& a, const NodeSpawn& b) {
    const DataSpawn& x = a.spawn;
    const DataSpawn& y = b.spawn;
    const auto guid_of = [](const content::NpcAssetRow* r) { return r ? r->guid : 0; };
    const auto inst_of = [](const content::NpcInstanceRow* r) { return r ? r->guid : 0; };
    return guid_of(x.asset) == guid_of(y.asset) && inst_of(x.instance) == inst_of(y.instance) && x.level == y.level && std::fabs(x.x - y.x) < 1.0 && std::fabs(x.y - y.y) < 1.0 &&
           std::fabs(x.z - y.z) < 1.0 && std::fabs(x.yaw - y.yaw) < 0.01;
}

void NodePopulations::remove(uint64_t actor) {
    if (actor == 0) return;
    if (registry_.remove(actor)) ++removed_;
    if (hooks_.despawned) hooks_.despawned(actor);
}

uint64_t NodePopulations::make(Live& l, double now, const world::CityNodeInfo& info) {
    std::string why;
    auto w = make_data_actor(l.spawn.spawn, mint_, server_id_, now, nullptr, why);
    if (!w) {
        spdlog::warn("[people] {}: an NPC ({}) was refused: {}", info.display_name, l.spawn.spawn.asset ? l.spawn.spawn.asset->name : std::string("?"), why);
        l.actor = 0;
        return 0;
    }
    w->npc.spawn_ref = kNodeRefBase + l.slot;
    // a town's people keep the height the data gave them (the layout's, retail's to 1-168 cm): the baked terrain under a city node's bowl lies about 50 m below the town's floor.  A
    // random point of a PopulationArea has no height of its own (ground_by_terrain): the ground under it decides, as in the open world.
    w->npc.ground_by_data = !w->npc.ground_by_terrain;
    const uint64_t id = w->id;
    if (!registry_.add(std::move(*w))) {
        spdlog::warn("[people] {}: an NPC ({}) was refused by the registry", info.display_name, l.spawn.spawn.asset ? l.spawn.spawn.asset->name : std::string("?"));
        l.actor = 0;
        return 0;
    }
    l.actor = id;
    ++made_;
    if (hooks_.spawned) {
        const content::NpcInstanceRow* inst = l.spawn.spawn.instance;
        const content::NpcRouteRow* route = nullptr;
        if (inst && inst->patrol_route != 0) {
            if (const auto r = population_->routes.find(inst->patrol_route); r != population_->routes.end()) route = &r->second;
        }
        hooks_.spawned(id, inst, route);
    }
    return id;
}
// == C Y B | CoA ==

size_t NodePopulations::apply(const world::CityNodeInfo& info, int level, double now, bool first) {
    const NodeSite site = site_of(info, level);
    const SpawnContext ctx = node_spawn_context(base_, site, config_.shown_humanoids);
    const SpawnFilter filter(ctx, motion_);
    NodePopStats stats;
    std::vector<NodeSpawn> want = select_node_spawns(*population_, templates_, data_, wire_, site, area_, filter, stats);
    std::map<uint64_t, NodeSpawn> next;
    for (NodeSpawn& w : want) next.emplace(w.ident, std::move(w));

    State& st = states_[info.guid];
    size_t removed = 0, kept = 0, added = 0;
    for (auto it = st.live.begin(); it != st.live.end();) {
        const auto n = next.find(it->first);
        if (n == next.end() || !same_point(it->second.spawn, n->second)) {
            // the point is gone or stands for another NPC now: its NPC (or corpse) leaves, and a respawn that names it does nothing
            remove(it->second.actor);
            slots_[it->second.slot].open = false;
            it = st.live.erase(it);
            ++removed;
        } else {
            it->second.spawn = n->second;   // (the same point: refresh the pointers)
            ++kept;
            ++it;
        }
    }
    for (auto& [ident, w] : next) {
        if (st.live.count(ident)) continue;
        Live l;
        l.spawn = std::move(w);
        l.slot = static_cast<uint32_t>(slots_.size());
        slots_.push_back({info.guid, ident, true});
        make(l, now, info);
        st.live.emplace(ident, std::move(l));
        ++added;
    }
    Entry& e = entries_[info.guid];
    e.node = info.guid;
    e.level = level;
    e.desired = st.live.size();
    e.by_source[0] = e.by_source[1] = 0;
    e.npcs = 0;
    for (const auto& [ident, l] : st.live) {
        ++e.by_source[l.spawn.source == PopSource::Layout ? 0 : 1];
        if (l.actor != 0) ++e.npcs;
    }
    if (first) {
        spdlog::info("[people] {} ({}, {}): {} NPCs ({} from {} layout sets, {} from {} building sets); the rest dropped by {}", info.display_name, node_level_name(level),
                     node_type_name(info.type), e.npcs, stats.spawned[0], stats.sets[0], stats.spawned[1], stats.sets[1], reasons_of(stats));
    } else {
        spdlog::info("[people] {} -> {}: {} NPCs now ({} stay, {} leave, {} arrive; {} from the layout's {} sets, {} from {} building sets); the rest dropped by {}", info.display_name,
                     node_level_name(level), e.npcs, kept, removed, added, stats.spawned[0], stats.sets[0], stats.spawned[1], stats.sets[1], reasons_of(stats));
    }
    return added;
}

size_t NodePopulations::start(double now) {
    if (started_ || !config_.enabled) return 0;
    started_ = true;
    seen_version_ = nodes_.version();
    size_t total = 0;
    size_t nodes = 0;
    for (const world::CityNodeInfo& info : nodes_.nodes()) {
        if (!in_area(info)) continue;
        total += apply(info, nodes_.level_of(info.guid), now, true);
        ++nodes;
    }
    spdlog::info("[people] {} node(s) inside the area, {} NPCs made; they follow the nodes' levels (a level change makes the difference real)", nodes, total);
    return total;
}
// -- C_Y_B x C O A --

size_t NodePopulations::tick(double now) {
    if (!started_) return 0;
    const uint64_t v = nodes_.version();
    if (v == seen_version_) return 0;
    seen_version_ = v;
    size_t rebuilt = 0;
    for (const world::CityNodeInfo& info : nodes_.nodes()) {
        const auto e = entries_.find(info.guid);
        if (e == entries_.end()) continue;
        const int level = nodes_.level_of(info.guid);
        if (level == e->second.level) continue;
        apply(info, level, now, false);
        ++rebuilds_;
        ++rebuilt;
    }
    return rebuilt;
}

uint64_t NodePopulations::respawn(uint32_t ref, double now) {
    if (!is_node_ref(ref)) return 0;
    const size_t idx = ref - kNodeRefBase;
    if (idx >= slots_.size()) return 0;
    Slot& slot = slots_[idx];
    if (!slot.open) return 0;
    const auto st = states_.find(slot.node);
    if (st == states_.end()) return 0;
    const auto it = st->second.live.find(slot.ident);
    if (it == st->second.live.end() || it->second.slot != idx) {
        slot.open = false;
        return 0;
    }
    const world::CityNodeInfo* info = nodes_.by_guid(slot.node);
    if (!info) return 0;
    const uint64_t id = make(it->second, now, *info);
    if (id != 0) {
        ++respawned_;
        const auto e = entries_.find(slot.node);
        if (e != entries_.end()) e->second.npcs = actors_of(slot.node).size();
    }
    return id;
}

}  // namespace aoc::worldd::spawns
