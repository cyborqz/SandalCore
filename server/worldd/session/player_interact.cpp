// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_interact.cpp
//
//  Interactions with the actors of the world: what a click on an actor starts (a mount, a resource node, a station, a shrine, a quest object, a hunted creature, a loot pile, a conversation, a shop).
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

// ============================================================================================================================
//  plan E1: the GM vendor
// ============================================================================================================================

const world::WorldActor* PlayerSession::open_vendor(std::string& why) {
    if (vendor_actor_ == 0) {
        why = "no vendor window is open";
        return nullptr;
    }
    const world::WorldActor* a = registry_ ? registry_->find(vendor_actor_) : nullptr;
    const GoodPosition at = checker_.good();
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const bool town = a && !a->npc.is_gm_vendor && shops && shops->is_open(vendor_def_) && (vendor_by_talk_ || town_shop_of(*a) == vendor_def_);
    if (!a) {
        why = "the vendor is gone";
    } else if (!a->npc.is_gm_vendor && !town) {
        why = "the NPC is no GM vendor and opens no shop";
    } else if (town) {
        // a town shop: the shopkeeper must be less than 12 m away in three dimensions (the shop re-checks on every request)
        const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
        if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
            why = "the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away";
        } else {
            return a;
        }
    } else if (std::hypot(a->x - at.x, a->y - at.y) > kInteractRangeCm) {
        why = "the vendor is " + std::to_string(static_cast<int>(std::hypot(a->x - at.x, a->y - at.y) / 100.0)) + " m away";
    } else {
        return a;
    }
    vendor_actor_ = 0;
    vendor_def_ = 0;
    vendor_by_talk_ = false;
    return nullptr;
}
// -- C.O.A x C-Y-B --

void PlayerSession::on_interact(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_) return;
    const auto req = repl::parse_interact_request(rpc.call);
    if (!req) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerInteractWithInteractableComponent does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerInteractWithInteractableComponent", now)) return;
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(interact_logged_), "[session] {}: an interaction with component {} is ignored: {}", name(), req->interactable.ObjectId, why);
    };
    if (!registry_ || !view_) return ignore("the world has no actors");
    if (mounted()) return ignore("a rider does not interact (it is on its mount)");   // the mounts milestone, stage 3
    const world::WorldActor* a = registry_->find_by_component(req->interactable.ObjectId);
    if (!a) return ignore("it is no component of an actor of this world");
    if (a->kind == world::ActorKind::Mount) return on_mount_interact(conn, *a, now);   // getting on
    if (a->kind == world::ActorKind::Gatherable) return on_gather_interact(conn, *a, now);   // Gathering stage 2: a resource node
    if (a->kind == world::ActorKind::Station) return on_station_interact(conn, *a, now);     // crafting, step 0: a crafting or processing station
    if (a->kind == world::ActorKind::Shrine) return on_shrine_interact(conn, *a, now);       // Embersprings: the prompt to make it the home
    if (a->kind == world::ActorKind::Prop) return on_prop_interact(conn, *a, now);           // quests part 3: a quest object
    if (view_->state_of(a->id) != world::ClientState::Awake) return ignore("the client does not have that actor open");
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a->x - at.x, a->y - at.y);
    if (distance > kInteractRangeCm) return ignore("the actor is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    // Gathering stage 3, "Hunting": the interactable of a huntable creature starts its hunt, alive as it stands (or dead, if it was killed some other way), not a loot window
    if (a->kind == world::ActorKind::Npc && a->npc.interact_def != 0 && npc_world_ && npc_world_->gathering() && npc_world_->gathering()->hunt_kind(a->npc.interact_def)) {
        return on_hunt_interact(conn, *a, now);
    }
    if (npc_world_ && npc_world_->aftermath()) {
        if (const combat::Corpse* corpse = npc_world_->aftermath()->corpse_by_interactable(req->interactable.ObjectId)) return open_loot(conn, *corpse, *a, now);
    }
    // quests: an NPC with a conversation opens the talking window
    if (a->kind == world::ActorKind::Npc && (a->npc.dialogue_root != 0 || npc_offers_quests(*a)) && open_dialogue(conn, *a, now)) return;
    const vendor::GmVendors* gm = world_ ? world_->gm_vendors() : nullptr;
    const vendor::GmVendor* def = (gm && a->npc.is_gm_vendor) ? gm->find(a->npc.vendor_guid) : nullptr;
    // vendors, step 0: a shopkeeper of a town whose data links it to a switched-on shop opens that shop's window
    if (!def && a->kind == world::ActorKind::Npc) {
        if (const int64_t shop = town_shop_of(*a)) {
            const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
            if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
                return ignore("the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away (a shop opens within 12 m)");
            }
            if (!room_for(1)) return;
            const vendor::TownShops* shops = world_->town_shops();
            vendor_actor_ = a->id;
            vendor_def_ = shop;
            vendor_by_talk_ = false;
            ++vendor_opens_;
            const FIntrepidNetworkGUID npc = a->actor.guid;
            spdlog::log(level_for(vendor_logged_), "[session] {}: opens the shop {} of {}", name(), shops->def(shop) ? shops->def(shop)->name : std::to_string(shop), a->npc.name);
            queue("ClientOpenVendor", [shop, npc](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(shop, npc); }, conn, /*must_build=*/true);
            return;
        }
    }
    if (!def) return ignore("the actor " + a->npc.name + " has nothing to offer yet");
    if (!room_for(1)) return;
    vendor_actor_ = a->id;
    vendor_def_ = def->guid;
    vendor_by_talk_ = false;
    ++vendor_opens_;
    const int64_t merch = def->guid;
    const FIntrepidNetworkGUID npc = a->actor.guid;
    spdlog::log(level_for(vendor_logged_), "[session] {}: opens the window of {} ({}, {} listing(s) in {} tab(s))", name(), def->label, def->name, def->listings, def->tabs);
    queue("ClientOpenVendor", [merch, npc](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(merch, npc); }, conn, /*must_build=*/true);
}

/// The client calls of interactions with the actors of the world (session/rpc_table.h).
void PlayerSession::register_interact_rpcs(RpcTable& t) {
    t.add(RpcScope::Pawn, "ServerInteractWithInteractableComponent", [](PlayerSession& s, const RpcContext& c) { s.on_interact(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
