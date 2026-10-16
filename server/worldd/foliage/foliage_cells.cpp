// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage_cells.cpp (see foliage_cells.h)
// ============================================================================
#include "foliage/foliage_cells.h"

#include "bootstrap/constants.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/fname.h"
#include "protocol/schema/generated/index.h"
#include "spawns/spawn_util.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc::worldd::foliage {

namespace repl = ::aoc::protocol::repl;
namespace gen = ::aoc::protocol::schema::gen;
namespace util = spawns::util;

namespace {

constexpr const char* kStatesProperty = "IndexedStates";

repl::ElementState item_values(uint32_t state, uint32_t index) {
    repl::ElementState es;
    es.values[1] = repl::Value::u64(state);   // Items.Value.States
    es.values[2] = repl::Value::u64(index);   // Items.Key
    return es;
}

}  // namespace

FoliageCells::FoliageCells(world::ActorRegistry& registry, const FoliageMap& map, Minter mint) : registry_(registry), map_(map), mint_(std::move(mint)), owners_(map.owners().size()) {}

size_t FoliageCells::actors() const {
    size_t n = 0;
    for (const Owner& o : owners_) n += o.actor != 0 ? 1 : 0;
    return n;
}

size_t FoliageCells::items_of(uint16_t owner) const { return owner < owners_.size() ? owners_[owner].items : 0; }

std::optional<uint32_t> FoliageCells::shown_state(uint32_t row) const {
    const auto it = state_of_row_.find(row);
    if (it == state_of_row_.end()) return std::nullopt;
    return it->second;
}
// built with C:Y:B and C O A

bool FoliageCells::make_actor(uint16_t owner_index) {
    const FoliageMap::Owner& owner = map_.owners()[owner_index];
    const protocol::schema::ClassSchema* schema = gen::find_class_schema("AIntrepidStateIFA");
    const protocol::schema::ClassSchema* component_schema = gen::find_class_schema("UIntrepidFoliageStateComponent");
    if (!schema || !component_schema || !repl::fast_array_layout(*component_schema, kStatesProperty)) {
        spdlog::error("[foliage] the client's AIntrepidStateIFA or UIntrepidFoliageStateComponent schema is missing: the cut instances of {} cannot be shown", owner.name);
        return false;
    }
    double cx = 0, cy = 0;
    map_.cell_center(owner_index, cx, cy);
    world::WorldActor w;
    w.kind = world::ActorKind::Foliage;
    repl::Actor& a = w.actor;
    a.guid = mint_();
    w.id = a.guid.ObjectId;
    a.obj.schema = schema;
    std::string err;
    bool ok = util::set_path(a.obj, "AuthServerIDReplicated", repl::Value::u64(bootstrap::kAuthServerIdReplicated), err);
    {   // ReplicatedMovement: the cell's centre [R: 1198's seven cells and 1428 pkt 139: (-1011200, 499200, 15438) for the cell (-40, 19)].  An AActor's FRepMovement carries whole cm (only
        // a Pawn raises its location to two decimals): the integers ARE the cm, which is scale 1.
        a.obj.opts.rep_movement_location_scale = 1;
        repl::RepMovement m;
        m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(cx), static_cast<int64_t>(cy), owner.centre_z_cm()};
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.rotation = repl::Rot{0, 0, 0};
        ok = ok && util::set_path(a.obj, "ReplicatedMovement", repl::Value(m), err);
    }
    ok = ok && util::set_path(a.obj, "RemoteRole", repl::Value::u64(1), err);   // SimulatedProxy
    ok = ok && util::set_path(a.obj, "Role", repl::Value::u64(4), err);          // Authority, as the client receives it
    ok = ok && util::set_path(a.obj, "FoliageName", repl::Value(repl::fname_parse(owner.name)), err);
    if (!ok) {
        spdlog::error("[foliage] the state actor of {} does not build: {}", owner.name, err);
        return false;
    }
    a.open.archetype = bootstrap::foliage_state_actor_archetype_export();
    a.open.level = bootstrap::persistent_level_export();
    // SerializeNewActor's location is at scale 10 (tenths of a cm) [R: 1198's seven cells: (-7808000, 4224000, 128075) for the cell (-31, 16)]
    a.open.spawn.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(cx) * 10, static_cast<int64_t>(cy) * 10, owner.centre_z_tenths()};
    Owner& entry = owners_[owner_index];
    entry.components.clear();
    for (const std::string& name : owner.components) {   // a component for every FISMC of the owner, in byte order [R: 1428 pkt 139: _0, _1, _10 ... _19, _2 ...]
        repl::Subobject sub;
        sub.guid = mint_();
        sub.dynamic_class = bootstrap::foliage_state_component_class_export();
        sub.obj.schema = component_schema;
        err.clear();
        if (!util::set_path(sub.obj, "FoliageComponentUUID", repl::Value(repl::fname_parse(name)), err) || !util::set_path(sub.obj, "bInitialized", repl::Value::boolean(true), err)) {
            spdlog::error("[foliage] the state component {} of {} does not build: {}", name, owner.name, err);
            return false;
        }
        entry.components.push_back(a.subobjects.size());
        a.subobjects.push_back(std::move(sub));
    }
    w.x = cx;
    w.y = cy;
    w.z = static_cast<double>(owner.centre_z_tenths()) / 10.0;
    w.label = owner.name;
    w.snapshot_only = true;
    w.keep_fast_shadow = true;
    w.dormancy_delay = kStateActorOpen;
    w.first_open_form = [this, owner_index](repl::Actor& shown) { first_open_form(shown, owner_index); };
    const uint64_t id = w.id;
    if (!registry_.add(std::move(w))) {
        spdlog::error("[foliage] the registry refused the state actor of {}", owner.name);
        return false;
    }
    entry.actor = id;
    spdlog::debug("[foliage] the state actor of {} is made: cell ({}, {}), {} components, {} instances of the cell", owner.name, owner.cell_x, owner.cell_y, owner.components.size(),
                  owner.rows.size());
    return true;
}

size_t FoliageCells::make_all() {
    size_t made = 0;
    for (size_t o = 0; o < owners_.size(); ++o) {
        if (owners_[o].actor != 0) continue;
        if (make_actor(static_cast<uint16_t>(o))) ++made;
    }
    return made;
}

size_t FoliageCells::apply(const std::vector<Change>& changes) {
    size_t made = 0;
    // by owner, so each actor is modified once
    std::unordered_map<uint16_t, std::vector<const Change*>> by_owner;
    for (const Change& c : changes) {
        if (c.row >= map_.rows().size()) {
            ++skipped_;
            continue;
        }
        by_owner[map_.rows()[c.row].owner].push_back(&c);
    }
    for (const auto& [owner_index, list] : by_owner) {
        Owner& owner = owners_[owner_index];
        if (owner.actor == 0) {
            if (!make_actor(owner_index)) {
                skipped_ += list.size();
                continue;
            }
            ++made;
        }
        registry_.modify(owner.actor, world::Change::State, [&](world::WorldActor& w) {
            for (const Change* c : list) {
                const FoliageMap::Row& row = map_.rows()[c->row];
                repl::Subobject& sub = w.actor.subobjects[owner.components[row.component]];
                repl::FastArrayState* array = sub.obj.arrays.get(*sub.obj.schema, kStatesProperty);
                if (!array) {
                    ++skipped_;
                    continue;
                }
                const auto known = item_of_row_.find(c->row);
                if (c->painted) {   // back at its painted state: out of the list
                    if (known != item_of_row_.end() && array->remove(known->second)) {
                        item_of_row_.erase(known);
                        state_of_row_.erase(c->row);
                        changed_at_.erase(c->row);
                        --owner.items;
                        ++applied_;
                    }
                    continue;
                }
                if (known != item_of_row_.end()) {
                    if (array->change(known->second, item_values(static_cast<uint32_t>(c->state), row.index))) {
                        state_of_row_[c->row] = static_cast<uint32_t>(c->state);
                        changed_at_[c->row] = now_;
                        ++applied_;
                    } else {
                        ++skipped_;
                    }
                } else if (const auto id = array->add(item_values(static_cast<uint32_t>(c->state), row.index))) {
                    item_of_row_[c->row] = *id;
                    state_of_row_[c->row] = static_cast<uint32_t>(c->state);
                    changed_at_[c->row] = now_;
                    ++owner.items;
                    ++applied_;
                } else {
                    ++skipped_;
                }
            }
        });
    }
    return made;
}
// ~ CoA :: C.Y.B ~

void FoliageCells::first_open_form(repl::Actor& actor, uint16_t owner_index) const {
    const FoliageMap::Owner& owner = map_.owners()[owner_index];
    for (size_t c = 0; c < actor.subobjects.size() && c < owner.components.size(); ++c) {
        repl::Subobject& sub = actor.subobjects[c];
        if (!sub.obj.schema) continue;
        repl::FastArrayState* array = sub.obj.arrays.get(*sub.obj.schema, kStatesProperty);
        if (!array) continue;
        std::vector<std::pair<int32_t, repl::ElementState>> edits;
        for (const repl::FastArrayItem& item : array->items()) {
            const auto s = item.values.values.find(1);
            const auto k = item.values.values.find(2);
            const uint64_t* state = s == item.values.values.end() ? nullptr : s->second.get_if<uint64_t>();
            const uint64_t* index = k == item.values.values.end() ? nullptr : k->second.get_if<uint64_t>();
            if (!state || !index || *state != static_cast<uint64_t>(kEmpty)) continue;
            const auto row = map_.find(owner.name, owner.components[c], static_cast<uint32_t>(*index));
            const auto when = row ? changed_at_.find(*row) : changed_at_.end();
            if (when != changed_at_.end() && now_ - when->second <= kLiveWindow) continue;   // cut a moment ago: a live harvest, 3
            repl::ElementState es = item.values;
            es.values[1] = repl::Value::u64(kEmptyInFirstOpen);
            edits.emplace_back(item.id, std::move(es));
        }
        for (auto& [id, es] : edits) array->set_values(id, std::move(es));
    }
}

}  // namespace aoc::worldd::foliage
