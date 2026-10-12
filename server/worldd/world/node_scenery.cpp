// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_scenery.cpp (see node_scenery.h)
// ============================================================================
#include "world/node_scenery.h"

#include "bootstrap/constants.h"
#include "protocol/repl/codecs.h"
#include "protocol/schema/generated/index.h"
#include "spawns/spawn_util.h"
#include "world/node_origins.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::world {

namespace {

const protocol::schema::ClassSchema* replicator_schema() { return protocol::schema::gen::find_class_schema(repl::kNodeLayoutClass); }

const node_origins::Origin* captured_origin(int64_t node) {
    for (const node_origins::Origin& o : node_origins::kOrigins) {
        if (o.node == node) return &o;
    }
    return nullptr;
}

/// The bits of the node's payload as an open sends them.
size_t snapshot_bits(const protocol::schema::ClassSchema& schema, const repl::FastArrays& arrays) {
    const auto state = repl::read_node_layout_state(schema, arrays);
    if (!state) return 0;
    protocol::emit::BunchWriter w(1024);
    return repl::write_node_layout_snapshot(schema, *state, w);
}

}  // namespace

NodeScenery::NodeScenery(ActorRegistry& registry, const CityNodes& nodes, const NodeWire& wire, const NodeLayoutData& data, Minter mint, Config config)
    : registry_(registry), nodes_(nodes), wire_(wire), data_(data), mint_(std::move(mint)), config_(config) {}

const NodeScenery::Entry* NodeScenery::entry_of(int64_t node) const {
    const auto it = entries_.find(node);
    return it == entries_.end() ? nullptr : &it->second;
}

size_t NodeScenery::actors() const {
    size_t n = 0;
    for (const auto& [node, e] : entries_) n += e.actor != 0 ? 1 : 0;
    return n;
}

size_t NodeScenery::payload_bits(int64_t node) const {
    const Entry* e = entry_of(node);
    const WorldActor* a = e && e->actor ? registry_.find(e->actor) : nullptr;
    const auto* schema = replicator_schema();
    return a && schema ? snapshot_bits(*schema, a->actor.obj.arrays) : 0;
}

NodeScenery::Built NodeScenery::build(const CityNodeInfo& info, int level, uint8_t plot_version) const {
    Built b;
    b.header.level = static_cast<uint8_t>(std::clamp(level, 0, 255));
    b.header.culture = config_.culture;
    b.header.season = config_.season;
    b.header.tags = {repl::kSiegeInactiveTag};
    b.header.plot_version = plot_version;
    LayoutRequest req;
    req.node = info.guid;
    req.context.level = level;
    req.context.culture = config_.culture;
    req.context.node_type = info.type;
    req.context.season = config_.season;
    const CityZoneInfo* zone = info.zoi_guid != 0 ? nodes_.zone_by_guid(info.zoi_guid) : nullptr;
    req.context.biome = zone ? zone->biome : -1;
    b.items = data_.items(wire_, req);
    return b;
}
// [C:Y:B ~ CoA]

bool NodeScenery::place(Entry& e, const CityNodeInfo& info) const {
    const node_origins::Origin* o = config_.captured_origins ? captured_origin(info.guid) : nullptr;
    e.captured_origin = o != nullptr;
    e.x = o ? o->x : info.x;
    e.y = o ? o->y : info.y;
    e.z = o ? o->z : info.z;
    e.yaw = o ? o->yaw : info.yaw;
    return std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && std::isfinite(e.yaw);
}

bool NodeScenery::make_actor(Entry& e, const CityNodeInfo& info, const Built& b) {
    const auto* schema = replicator_schema();
    if (!schema || !repl::node_layout_schema_ok(*schema)) {
        spdlog::error("[scenery] the client's ANodeLayoutReplicator schema is not the one the writer knows");
        return false;
    }
    if (!place(e, info)) return false;
    WorldActor w;
    w.kind = ActorKind::NodeLayout;
    repl::Actor& a = w.actor;
    a.guid = mint_();
    w.id = a.guid.ObjectId;
    a.obj.schema = schema;
    std::string err;
    namespace util = spawns::util;
    bool ok = util::set_path(a.obj, "AuthServerIDReplicated", repl::Value::u64(bootstrap::kAuthServerIdReplicated), err);
    {   // ReplicatedMovement: where it stands (retail sends it, 99 bits)
        repl::RepMovement m;
        m.location = repl::quantize_location(a.obj.opts, e.x, e.y, e.z);
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.rotation = repl::Rot{0, a.obj.opts.rep_movement_short_rotation ? util::angle16(e.yaw) : util::angle8(e.yaw), 0};
        ok = ok && util::set_path(a.obj, "ReplicatedMovement", repl::Value(m), err);
    }
    ok = ok && util::set_path(a.obj, "RemoteRole", repl::Value::u64(1), err);   // SimulatedProxy
    ok = ok && util::set_path(a.obj, "Role", repl::Value::u64(4), err);          // Authority (retail's value as the client receives it)
    ok = ok && util::set_path(a.obj, "NodeGuid", repl::Value::i64(info.guid), err);
    if (!ok || !repl::sync_node_layout(*schema, a.obj.arrays, b.header, b.items)) {
        spdlog::error("[scenery] the replicator of {} does not build{}{}", info.name, err.empty() ? "" : ": ", err);
        return false;
    }
    a.open.archetype = bootstrap::node_layout_archetype_export();
    a.open.level = bootstrap::persistent_level_export();
    a.open.spawn.location = repl::quantize_vector(10, e.x, e.y, e.z);
    a.open.spawn.rotation = repl::Rot{0, util::angle16(e.yaw), 0};
    a.open.spawn.velocity.reset();
    w.x = e.x;
    w.y = e.y;
    w.z = e.z;
    w.yaw_degrees = e.yaw;
    w.awake_radius_cm = config_.awake_cm;
    w.never_dormant = true;
    w.scenery.node = info.guid;
    w.scenery.name = info.display_name;
    w.scenery.level = b.header.level;
    w.scenery.items = b.items.size();
    w.scenery.plot_version = b.header.plot_version;
    w.scenery.payload_bits = snapshot_bits(*schema, a.obj.arrays);
    const size_t bits = w.scenery.payload_bits;
    const uint64_t id = w.id;
    if (!registry_.add(std::move(w))) {
        spdlog::error("[scenery] the registry refused the replicator of {}", info.name);
        return false;
    }
    e.actor = id;
    e.level = b.header.level;
    e.plot_version = b.header.plot_version;
    e.items = b.items.size();
    spdlog::info("[scenery] {} (node {}): level {}, {} items ({} layout + {} plots), PlotChangeVersion {}, {} payload bits; stands at ({:.0f}, {:.0f}, {:.0f}) facing {:.1f} ({}); awake within {:.0f} m",
                 info.display_name, info.guid, b.header.level, b.items.size(), [&] {
                     size_t n = 0;
                     for (const auto& it : b.items) n += it.owner_guid == info.guid ? 1 : 0;
                     return n;
                 }(),
                 [&] {
                     size_t n = 0;
                     for (const auto& it : b.items) n += it.owner_guid != info.guid ? 1 : 0;
                     return n;
                 }(),
                 b.header.plot_version, bits, e.x, e.y, e.z, e.yaw, e.captured_origin ? "retail's open location" : "the node record's location", config_.awake_cm / 100.0);
    return true;
}
// ~ C | Y | B :: C-O-A ~

bool NodeScenery::refresh(Entry& e, const CityNodeInfo& info, int level) {
    const auto* schema = replicator_schema();
    if (!schema) return false;
    const bool first = e.level < 0;
    const uint8_t pv = first ? 1 : next_version(e.plot_version);
    Built b = build(info, level, pv);
    if (b.items.empty()) {   // nothing of this node to draw at this level
        if (e.actor != 0) {
            registry_.remove(e.actor);   // the clients that hold it close it
            spdlog::info("[scenery] {} (node {}): level {} draws nothing; its replicator is gone", info.display_name, info.guid, level);
        }
        e.actor = 0;
        e.level = level;
        e.items = 0;
        e.plot_version = pv;
        return true;
    }
    if (e.actor == 0) return make_actor(e, info, b);
    const int old_level = e.level;
    const size_t old_items = e.items;
    repl::NodeLayoutSync sync;
    bool ok = false;
    registry_.modify(e.actor, Change::State, [&](WorldActor& a) {
        ok = repl::sync_node_layout(*schema, a.actor.obj.arrays, b.header, b.items, &sync);
        if (!ok) return;
        a.scenery.level = level;
        a.scenery.items = b.items.size();
        a.scenery.plot_version = pv;
        a.scenery.payload_bits = snapshot_bits(*schema, a.actor.obj.arrays);
    });
    if (!ok) {
        spdlog::error("[scenery] {} (node {}): the new payload for level {} does not build; the clients keep the old one", info.display_name, info.guid, level);
        return false;
    }
    e.level = level;
    e.items = b.items.size();
    e.plot_version = pv;
    ++rebuilds_;
    spdlog::info("[scenery] {} (node {}): level {} -> {}: a new full payload of {} items (was {}), PlotChangeVersion {}, {} bits", info.display_name, info.guid, old_level, level,
                 b.items.size(), old_items, pv, payload_bits(info.guid));
    return true;
}

size_t NodeScenery::start() {
    if (started_ || !config_.enabled) return 0;
    started_ = true;
    seen_version_ = nodes_.version();   // before the levels: a change after this is seen by the next tick
    size_t n = 0;
    for (const int rank : wire_.roster()) {
        const CityNodeInfo* info = nodes_.by_rank(rank);
        if (!info || !wire_.is_real_node(info->guid) || wire_.layout_of(info->guid) == 0) continue;
        Entry e;
        e.node = info->guid;
        e.rank = rank;
        Entry& slot = entries_.emplace(info->guid, e).first->second;
        if (refresh(slot, *info, nodes_.level_of_rank(rank)) && slot.actor != 0) ++n;
    }
    spdlog::info("[scenery] {} town replicators for {} nodes (awake within {:.0f} m, culture {}, season {}, {} at retail's open location)", n, entries_.size(), config_.awake_cm / 100.0,
                 config_.culture, config_.season, [&] {
                     size_t c = 0;
                     for (const auto& [node, en] : entries_) c += en.captured_origin ? 1 : 0;
                     return c;
                 }());
    return n;
}

size_t NodeScenery::tick() {
    if (!started_) return 0;
    const uint64_t version = nodes_.version();
    if (version == seen_version_) return 0;
    seen_version_ = version;
    size_t n = 0;
    for (auto& [node, e] : entries_) {
        const int level = nodes_.level_of_rank(e.rank);
        if (level == e.level) continue;
        const CityNodeInfo* info = nodes_.by_rank(e.rank);
        if (info && refresh(e, *info, level)) ++n;
    }
    return n;
}

}  // namespace aoc::worldd::world
