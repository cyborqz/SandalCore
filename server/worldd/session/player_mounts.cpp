// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_mounts.cpp
//
//  Mounts and riding: summoning and desummoning, getting on and off, the controlled actor's bunches, the mount's dash and hits (session/mounts.h and session/riding.h have the rules).
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

#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

uint32_t PlayerSession::mount_channel() const { return ride_ && view_ ? view_->channel_of(ride_->actor_id()) : 0; }

// ============================================================================================================================
//  mounts (the mounts milestone, stage 2: session/mounts.h has the rules and the numbers)
// ============================================================================================================================

void PlayerSession::on_mount_item_used(const svc::InventoryItem& item) {
    ++mount_item_uses_;
    const content::MountRow* row = mounts_ ? mounts_->book().find(item.item_record_id) : nullptr;
    spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: a mount item is used (item {}, {}, a {} mount activated by {}): its summon or desummon follows as the next call, nothing is consumed", name(),
                item.item_record_id, row ? row->item_name : std::string("?"), row ? row->kind : std::string("?"), row ? row->activation : std::string("?"));
}

void PlayerSession::on_summon_trigger(Connection& conn, int32_t client_instance, int32_t slot, uint32_t location, Clock::time_point now) {
    ++mount_summon_requests_;
    const auto ignore = [&](const char* why) {
        ++mount_ignored_;
        spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: a Summon_Mount is ignored: {}", name(), why);
        if (pending_ends_.size() < 64) pending_ends_.push_back(client_instance);   // the client's own instance of the cast is ended
        flush_abilities(conn);
    };
    if (!mounts_) return ignore("worldd has no mounts");
    const auto container = container_of_location(static_cast<uint8_t>(location));
    if (!container || *container != svc::ItemContainer::Backpack || slot < 0) return ignore("the call names no item of the bag");
    const svc::InventoryItem* item = inventory_.at(*container, slot);
    if (!item || !mounts_->is_mount_item(item->item_record_id)) return ignore("the slot holds no mount item");
    const auto guid = items::guid_of(item->instance_guid);
    if (!guid) return ignore("the item has no readable instance guid");
    ability_runtime(now);   // the clock the mount runs on starts with the ability runtime's
    if (!abilities_) return ignore("abilities are off");
    // a mount item used while its owner rides: the rider gets off first (one mount per player: the old one is desummoned and nothing is summoned)
    if (mounted()) dismount(conn, now, DismountWhy::Summon);
    MountController::Request r;
    r.item_record = item->item_record_id;
    r.item_guid = *guid;
    r.client_instance = client_instance;
    r.lock_expiration = item->lock_expiration;
    r.now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const GoodPosition at = checker_.good();
    r.place = {at.x, at.y, at.z, at.yaw_degrees};
    // the mount inherits its owner's alignments as the pawn holds them now (stage 3 fix 2: never a donor's: a PvP flag, a node or a guild the owner has not)
    if (const repl::Subobject* alignment = pb_.pawn().find(pb_.setup().guids.alignment)) {
        if (const repl::Value* v = alignment->obj.state.get(*alignment->obj.schema, "Alignments")) {
            if (const auto* c = v->get_if<repl::AlignmentContainer>()) r.alignments = *c;
        }
    }
    r.owner_level = progress_.level;
    r.dead = dead_;
    // the mount comes back with the Health its item kept (a dead mount's is 0: the lock keeps it out; once the lock is over it comes back whole)
    if (item->persisted_health && *item->persisted_health > 0) r.health = static_cast<double>(*item->persisted_health);
    const double t = ability_seconds(now);
    r.world_time = pb_.setup().world_time + t;
    switch (mounts_->summon(r, t)) {
        case MountController::Outcome::Started:
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount: the mount of item {} is summoned (the ability item and the bar in {:.0f} ms, the mount in {:.0f} ms)", name(),
                        r.item_record, MountController::kItemDelay * 1000, MountController::kOpenDelay * 1000);
            break;
        case MountController::Outcome::Refused:
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount is refused (no ground in front of the player, a mount under lock or a dead player): \"{}\"", name(),
                        MountController::kRefusalText);
            break;
        case MountController::Outcome::Desummoning:
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount while a mount is out: the old mount is desummoned and nothing is summoned", name());
            if (pending_ends_.size() < 64) pending_ends_.push_back(client_instance);
            flush_abilities(conn);
            break;
        case MountController::Outcome::Ignored:
            ++mount_ignored_;
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount is ignored: a summon or a desummon is in progress", name());
            if (pending_ends_.size() < 64) pending_ends_.push_back(client_instance);
            flush_abilities(conn);
            break;
    }
    run_mounts(conn, now);
}
// [Cyb x C.O.A]

void PlayerSession::on_desummon_mount(Connection& conn, Clock::time_point now) {
    ++mount_desummon_requests_;
    if (!mounts_) return;
    if (!within_budget(item_budget_, "ServerDesummonMount", now)) return;
    ability_runtime(now);
    if (!abilities_) return;
    // a rider dismounts first.  The client asks for the dismount itself too (ServerDismountInteraction in the same packet): the two requests make two ClientRestarts [R: 893]
    if (mounted()) {
        dismount(conn, now, DismountWhy::Desummon);
    } else if (dismounted_valid_ && dismounted_at_ == now && !dismount_extra_sent_ && room_for(1)) {
        dismount_extra_sent_ = true;
        queue("ClientRestart (the own pawn) again", [](bootstrap::PlayerBootstrap& pb) { return pb.restart_own_pawn(1); }, conn, /*must_build=*/true);
    }
    if (mounts_->desummon(ability_seconds(now))) {
        spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: ServerDesummonMount: the mount dissolves and runs off, and is gone in {:.1f} s", name(), MountController::kDesummonClose);
    } else {
        ++mount_ignored_;
        spdlog::debug("[session] {}: ServerDesummonMount: no mount to desummon", name());
    }
    run_mounts(conn, now);
}

void PlayerSession::mount_damaged(Clock::time_point now) {
    if (!mounts_ || !abilities_) return;
    mounts_->on_damage(ability_seconds(now));
}

void PlayerSession::run_mounts(Connection& conn, Clock::time_point now) {
    if (!mounts_ || !abilities_) return;
    // stage 4, modelled: a mount left farther than MountDesummonDistanceCentimeters from its owner is desummoned (the Blueprints turn bEnableMountDistanceDesummon on; no data
    // or capture has the distance).  A server-side move of the player (a respawn: the player died, which desummons anyway) shows up the same way.
    if (!mounted() && mounts_->phase() == MountController::Phase::Out) {
        if (const world::WorldActor* m = mounts_->mount()) {
            const GoodPosition at = checker_.good();
            if (std::hypot(m->x - at.x, m->y - at.y) > desummon_distance_) {
                ++distance_desummons_;
                spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the mount is {:.0f} cm from its owner (more than {:.0f}): it is desummoned", name(), std::hypot(m->x - at.x, m->y - at.y),
                            desummon_distance_);
                mounts_->desummon(ability_seconds(now));
            }
        }
    }
    MountController::Effects fx = mounts_->advance(ability_seconds(now));
    if (!fx.empty()) apply_mount_effects(conn, std::move(fx));
}

void PlayerSession::apply_mount_effects(Connection& conn, MountController::Effects&& fx) {
    // the pawn's ability item first (flushed with the next ability update), then what the PlayerController is told: retail's order in one packet
    if (!fx.ability.empty()) {
        if (pb_.apply_ability_change(fx.ability)) {
            abilities_dirty_ = true;
        } else {
            spdlog::warn("[session] {}: the Summon_Mount ability item cannot be applied to the pawn ({})", name(), pb_.error().empty() ? "no reason" : pb_.error());
        }
    }
    for (const int32_t id : fx.ends) {
        if (pending_ends_.size() < 64) pending_ends_.push_back(id);
    }
    flush_abilities(conn);
    if (fx.pet_changed && world_ && room_for(1)) {
        // The pawn's InteractInfo.MountablePet names the summoned mount (and 0 once it has closed), as retail's does in the packet of the Summon_Mount item [R: 871 packet 210,
        // 898 packets 158 and 601].  The client's bank-18 Mount slot, the dash, is resolved through it (exe: GetActionbarInstigator 0x6aba180 -> GetMountablePet): without it the
        // client sends nothing when the dash is pressed (stage 3 fix 3: a player's report "the dash can't be used", worldd's log with no dash trigger at all).
        if (!interact_info_) interact_info_ = world_->mint_guid();
        const protocol::emit::FIntrepidNetworkGUID info = *interact_info_;
        const protocol::emit::FIntrepidNetworkGUID pet = fx.pet;
        spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: the pawn's mountable pet {} (InteractInfo.MountablePet: what the client's dash button needs)", name(),
                    pet.ObjectId != 0 ? "is the mount (actor " + std::to_string(pet.ObjectId) + ")" : std::string("is none: the mount is gone"));
        queue("the pawn's mountable pet", [info, pet](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.ensure_interact_info(info)) return std::nullopt;
            return pb.pawn_mountable_pet(pet);
        }, conn, /*must_build=*/true);
    }
    if (fx.bar && room_for(1)) {
        const int64_t dash = fx.bar_ability;
        const int32_t slot = fx.bar_slot;
        queue("the mount action bar", [dash, slot](bootstrap::PlayerBootstrap& pb) { return pb.mount_bar(dash, slot); }, conn);
    }
    if (fx.persist) apply_persist(&conn, *fx.persist);
    for (const MountController::Callout& c : fx.callouts) {
        if (!room_for(1)) break;
        const int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const int64_t ticks = items::ticks_of_ms(unix_ms);
        const std::string text = c.text;
        const uint64_t key = c.key;
        queue("a callout", [text, ticks, key](bootstrap::PlayerBootstrap& pb) {
            return pb.callout_add(text, MountController::kCalloutType, MountController::kCalloutPointSize, ticks, key);
        }, conn);
    }
    for (const uint64_t key : fx.callouts_removed) {
        if (!room_for(1)) break;
        queue("a callout is taken off", [key](bootstrap::PlayerBootstrap& pb) { return pb.callout_remove(key); }, conn);
    }
}
// CoA x C:Y:B

// ============================================================================================================================
//  riding (the mounts milestone, stage 3: session/riding.h has the rules and the numbers)
// ============================================================================================================================

void PlayerSession::on_mount_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now) {
    if (bunch.close || bunch.bits == 0 || !registry_ || !ride_) return;
    const world::WorldActor* m = registry_->find(ride_->actor_id());
    if (!m || !m->actor.obj.schema) return;
    const repl::Actor* actor = &m->actor;
    const auto resolve = [actor](const FIntrepidNetworkGUID& g) -> const schema::ClassSchema* {
        const repl::Subobject* sub = actor->find(g);
        return sub ? sub->obj.schema : nullptr;
    };
    repl::CodecOptions opts;
    opts.inline_objects = false;   // a rider's trigger names no target by path (the dash's Targets are the default)
    const auto parsed = repl::read_client_bunch(*m->actor.obj.schema, resolve, bunch.data.data(), bunch.bits, opts);
    if (!parsed) {
        if (++malformed_bunches_ <= 3) {
            spdlog::warn("[session] {}: a bunch of {} bits on the mount's channel {} does not read: ignored", name(), bunch.bits, bunch.channel);
        }
        return;
    }
    for (const repl::ClientRpc& rpc : parsed->rpcs) dispatch_mount(conn, rpc, bunch, now);
}

void PlayerSession::dispatch_mount(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now) {
    const std::string_view n = rpc.call.rpc->name;
    if (rpc.on_actor) {
        if (n == "ServerMovePacked") return on_move(conn, rpc, bunch, now, /*on_mount=*/true);
    } else if (rpc.object == mount_ability_) {
        if (n == "ServerTriggerAbility") return on_ability_trigger(conn, rpc, bunch, now, /*on_mount=*/true);
        if (n == "RequestHitVerification_Server") {
            // the dash's hit: its confirmation went out with its effects, the verification has nothing more to ask [R: 906]
            ++hit_requests_;
            return;
        }
        if (n == "ServerNotifyHeldAbilityCancelled" || n == "ServerEndAbility") return;
    }
    if (unhandled_logged_.insert("mount:" + std::string(n)).second) spdlog::debug("[session] {}: ignoring the client's {} (the mount)", name(), n);
}

void PlayerSession::on_mount_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const auto refuse = [&](const std::string& why) {
        ++mount_refused_;
        spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: getting on the mount is refused: {}", name(), why);
    };
    if (!mounts_ || !registry_ || !view_) return refuse("worldd has no mounts");
    if (a.owner == 0 || a.owner != static_cast<uint64_t>(pb_.setup().player_id)) return refuse("it is not this player's mount");
    if (mounts_->actor_id() != a.id) return refuse("it is not the mount the player has out");
    if (mounts_->phase() != MountController::Phase::Out) return refuse("the mount is not ready (it is being summoned, dissolves or died)");
    if (ride_ && ride_->actor_id() == a.id && ride_->dead()) return refuse("the mount is dead");
    if (view_->state_of(a.id) != world::ClientState::Awake) return refuse("the client does not have the mount open");
    if (!within_budget(ride_budget_, "a mount interaction", now)) return;
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a.x - at.x, a.y - at.y);
    if (distance > Ride::kInteractRangeCm || std::fabs(a.z - at.z) > 2.0 * Ride::kInteractRangeCm) {
        return refuse("the mount is " + std::to_string(static_cast<int>(distance)) + " cm away (at most " + std::to_string(static_cast<int>(Ride::kInteractRangeCm)) + ")");
    }
    if (!room_for(8)) return refuse("the connection is too busy");
    mount_up(conn, a, now);
}
// -- Cyb & C | O | A --

bool PlayerSession::mount_up(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const content::MountRow* row = mounts_ ? mounts_->book().find(mounts_->item_record()) : nullptr;
    if (!row || !world_ || !registry_ || !view_) return false;
    if (!ability_runtime(now)) return false;
    cancel_quick_make(&conn, "the player mounts");   // Crafting, step 3: a craft needs standing still
    cancel_gather(&conn, "the player mounts", Interrupt::Mounted);   // Gathering stage 2: a rider harvests nothing
    const double t = ability_seconds(now);
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const spawns::MountBook& book = mounts_->book();
    const spawns::DashBar dash = book.dash_bar(*row);
    const repl::Subobject* ability_sub = spawns::sub_of(a, "UAoCAbilityComponent");
    if (!ability_sub || a.npc.collision_cylinder == 0) {
        ++mount_refused_;
        spdlog::warn("[session] {}: the mount has no AbilityComponent or collision cylinder to ride on", name());
        return false;
    }
    // the mount's guids: its actor, its cylinder (the same server id and randomizer: minted in its own block), its AbilityComponent
    const FIntrepidNetworkGUID mount_guid = a.actor.guid;
    FIntrepidNetworkGUID cylinder = a.actor.guid;
    cylinder.ObjectId = a.npc.collision_cylinder;
    if (!ride_ || ride_->actor_id() != a.id) {
        Ride::Setup s;
        s.registry = registry_;
        s.data = &book.data();
        s.row = row;
        s.actor = a.id;
        s.run_speed = mounts_->run_speed(*row);
        Ride::fill_speeds(s, book, *row);
        s.world_time_at_zero = pb_.setup().world_time;
        s.roads = world_->roads().empty() ? nullptr : &world_->roads();
        s.dash_ability = dash.ability;
        s.dash_ready_at = dash_ready_at_;   // review A I3: the dash's 30 s belong to the rider, not to the mount actor of the moment
        const auto& cg = pb_.setup().character_guid;
        s.owner = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
        ride_.emplace(std::move(s));
        mount_mesh_ = world_->mint_guid();
    }
    mount_guid_ = mount_guid;
    mount_cylinder_ = cylinder;
    mount_ability_ = ability_sub->guid;
    // the mount's own side: its owner, its rider's stats (the HUD of a possessed pawn reads them), its dash
    spawns::Possession p;
    p.player_controller = g.player_controller;
    p.player_state = g.player_state;
    p.rider_pawn = g.pawn;
    p.dash_ability = dash.ability;
    if (const repl::Subobject* stats = pb_.pawn().find(g.stats)) {
        const auto copy = [&](const char* field, std::vector<repl::ElementState>& out) {
            const schema::NetField* f = stats->obj.schema->field(field);
            const repl::FastArrayState* arr = f ? stats->obj.arrays.find(f->index) : nullptr;
            if (!arr) return;
            for (const repl::FastArrayItem& it : arr->items()) out.push_back(it.values);
        };
        copy("StatRepInt32Owner", p.owner_stats);
        copy("StatRepInt32OwnerProxy", p.owner_proxy_stats);
    }
    // the rider's pawn: it can name its mount (MountPtr: the InteractInfo component) and knows the dismount ability from now on [R: 910 packet 310]
    if (!interact_info_) interact_info_ = world_->mint_guid();
    if (!pb_.ensure_interact_info(*interact_info_)) {
        ++mount_refused_;
        spdlog::warn("[session] {}: the pawn has no InteractInfo to hold its mount: getting on is refused", name());
        return false;
    }
    if (const content::MountAbilityRow* dismount_ability = book.data().ability_with_role("dismount_elevated")) {
        if (std::find(known_.begin(), known_.end(), dismount_ability->ability) == known_.end()) {
            known_.push_back(dismount_ability->ability);
            if (std::find(records_.begin(), records_.end(), dismount_ability->ability) == records_.end()) records_.push_back(dismount_ability->ability);
            push_abilities();
        }
    }
    std::string error;
    ride_->set_gm_multiplier(gm_speed_, t);   // the GM travel tools: the mount's MoveSpeedMult starts at the GM's multiplier
    if (!ride_->begin(p, t, error)) {
        ++mount_refused_;
        spdlog::warn("[session] {}: getting on the mount failed: {}", name(), error);
        return false;
    }
    // the controlled actor is the mount now: its own checker, from where it stands (the client's TimeStamp restarts near 0)
    GoodPosition start;
    start.x = a.x;
    start.y = a.y;
    start.z = a.z;
    start.yaw_degrees = a.yaw_degrees;
    start.movement_mode = repl::kMoveWalking;
    checker_.rebase(ride_->limits(base_limits_, t), start);   // (review A I1: the pace bank and the credits are the session's, a new checker per possession refilled them)
    ride_->moved(a.x, a.y, a.z, a.yaw_degrees, t);   // a mount that stands on a road starts on it
    pending_fix_.reset();
    dismounted_valid_ = false;
    ++mount_ups_;
    // a mounted rider has no Sprint and no Stamina use (stage 3 fix 2: a play test of 2026-10-08 sprinted on through the mount-up and drained to 0 mounted): a running Sprint ends
    // the way a Sprint ends (ClientEndAbility, MoveSpeedMult 1.0, the StaminaRegeneration item) before the possession goes out
    abilities_->set_mounted(true, t);
    take_ability_events();
    flush_abilities(conn);
    // retail's three packets within a millisecond [R: 910 packets 306, 307, 310]: the PlayerController, the mount, the pawn and the PlayerController again
    const int64_t dash_ability = dash.ability;
    const int32_t dash_slot = dash.slot;
    const FIntrepidNetworkGUID mesh = mount_mesh_;
    queue("the mount is possessed", [mount_guid, dash_ability, dash_slot](bootstrap::PlayerBootstrap& pb) { return pb.possess_mount(mount_guid, dash_ability, dash_slot); }, conn,
          /*must_build=*/true);
    {
        // the mount's InteractableComponent has the rider's client play the mounting [R: 910 packet 306, with the PlayerController's chain]
        const uint32_t channel = view_->channel_of(a.id);
        const FIntrepidNetworkGUID interactable = component_guid_of(a, a.npc.interact_component);
        const uint64_t id = a.id;
        queue("the mount's interaction animation", [this, id, interactable, channel](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            const world::WorldActor* m = registry_ ? registry_->find(id) : nullptr;
            if (!m || channel == 0) return std::nullopt;
            auto s = pb.interaction_animation_on(m->actor, interactable, static_cast<uint16_t>(spawns::kMountInput));
            if (s) s->channel = channel;
            return s;
        }, conn);
    }
    view_->update_now(conn, a.id);
    queue("the rider is attached to the mount", [mount_guid, mesh](bootstrap::PlayerBootstrap& pb) { return pb.pawn_mounted(mount_guid, mesh, 100); }, conn, /*must_build=*/true);
    queue("the controller follows the mount", [mount_guid, cylinder](bootstrap::PlayerBootstrap& pb) { return pb.controller_attach(mount_guid, cylinder); }, conn, /*must_build=*/true);
    spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: gets on {} (actor {}): its speed is {:.0f} cm/s at MoveSpeedMult 1, the move checker's limit {:.0f} cm/s", name(), row->item_name, a.id,
                mounts_ ? mounts_->run_speed(*row) : 0.0, ride_->speed_limit(1, t));
    return true;
}
// Cyb / C.O.A

std::optional<double> PlayerSession::ground_below(double x, double y, double z) const {
    const nav::AreaNav* nav = world_ ? world_->nav() : nullptr;
    double ground = 0;
    std::optional<double> below;
    // a flyer can be a hundred metres up: the search reaches far below, and 150 cm above (the world's own tolerance)
    if (nav && nav->ground_height(x, y, z, &ground, 60000.0, 150.0)) below = ground;
    // over a city node's town the baked ground is the pit under it (world/town_floor.h, about 50 m down): the town floor, when the flyer is above it, is the ground.  That is the floor under
    // the flyer: a terrace, the floor of a building or a roof when it is over one (as it is, a covered floor has no room for a slack), else the street plus the slack.
    if (world_) {
        if (const auto town = world_->town_ground_at(x, y, z)) {
            const double floor = town->z + (town->raised ? 0.0 : PlayerWorld::kBowlSlackCm);
            if (floor <= z + 150.0 && (!below || floor > *below)) below = floor;
        }
    }
    return below;
}

void PlayerSession::dismount(Connection& conn, Clock::time_point now, DismountWhy why) {
    if (!mounted()) return;
    const double t = ability_seconds(now);
    const GoodPosition g = checker_.good();
    const Ride::Place place = Ride::dismount_place(g.x, g.y, g.z, g.yaw_degrees);
    std::string error;
    // fix 2 (retail's rule): a dismounted mount STAYS where its rider left it, in the air or in the water, in its movement mode (5 flying, 4 swimming): it hovers or floats
    // and only the rider falls (its moves start in falling mode, below) or swims; an earlier draft let a flyer settle on the ground and an aquatic mount sink
    const bool in_the_air = g.movement_mode == repl::kMoveFlying;
    if (!ride_->end(t, g.x, g.y, g.z, g.yaw_degrees, g.movement_mode, error)) spdlog::warn("[session] {}: the mount does not let its rider go cleanly: {}", name(), error);
    if (in_the_air) {
        ++air_dismounts_;
        spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the rider leaves its flying mount in the air: it falls from the detachment point, the mount hovers where it was", name());
    }
    // the pawn is the controlled actor again: its own checker, from where it lands (its clock restarts near 0 as well)
    GoodPosition start;
    start.x = place.x;
    start.y = place.y;
    start.z = place.z;
    start.yaw_degrees = place.yaw_degrees;
    start.movement_mode = repl::kMoveFalling;   // the rider leaps off [R: 912: its moves start in mode 3]
    checker_.rebase(base_limits_, start);
    pending_fix_.reset();
    dismounted_at_ = now;
    dismounted_valid_ = true;
    dismount_extra_sent_ = false;
    ++dismounts_;
    if (abilities_) {   // on foot again: the pawn's Sprint, Dodge and Rest are its own again
        abilities_->set_mounted(false, t);
        take_ability_events();
        flush_abilities(conn);
    }
    static const char* const kWhy[] = {"asked to", "its mount is desummoned", "another mount is summoned", "an ability needs it", "it died", "its mount died", "its mount is gone", "a GM teleport"};
    spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: gets off its mount ({}): at ({:.0f}, {:.0f}, {:.0f}) cm", name(), kWhy[static_cast<size_t>(why)], place.x, place.y, place.z);
    // retail's packet [R: 912 packet 139]: the PlayerController (restarted on its own pawn, attached to it), the pawn (detached beside the mount), the mount (released)
    const FIntrepidNetworkGUID pawn = pb_.setup().guids.pawn;
    const FIntrepidNetworkGUID cylinder = pb_.setup().guids.collision_cylinder;
    const repl::Vec3d at{place.x, place.y, place.z};
    const double yaw = place.yaw_degrees;
    if (why != DismountWhy::PlayerDied) {   // (a death's own frame restarts the pawn)
        queue("ClientRestart (the own pawn) and camera mode", [](bootstrap::PlayerBootstrap& pb) { return pb.restart_own_pawn(1); }, conn, /*must_build=*/true);
    }
    queue("the controller follows its own pawn", [pawn, cylinder](bootstrap::PlayerBootstrap& pb) { return pb.controller_attach(pawn, cylinder); }, conn, /*must_build=*/true);
    queue("the rider is detached from its mount", [at, yaw](bootstrap::PlayerBootstrap& pb) { return pb.pawn_dismounted(at, yaw); }, conn, /*must_build=*/true);
    if (view_ && ride_) view_->update_now(conn, ride_->actor_id());
}

void PlayerSession::on_dismount_request(Connection& conn, Clock::time_point now) {
    if (!within_budget(ride_budget_, "ServerDismountInteraction", now)) return;
    if (mounted()) return dismount(conn, now, DismountWhy::Request);
    // the second request of a dismount and a desummon in one datagram: retail's answer has two ClientRestarts [R: 893 packet 142]
    if (dismounted_valid_ && dismounted_at_ == now && !dismount_extra_sent_ && room_for(1)) {
        dismount_extra_sent_ = true;
        queue("ClientRestart (the own pawn) again", [](bootstrap::PlayerBootstrap& pb) { return pb.restart_own_pawn(1); }, conn, /*must_build=*/true);
        return;
    }
    spdlog::debug("[session] {}: ServerDismountInteraction while on foot: ignored", name());
}

void PlayerSession::run_ride(Connection& conn, Clock::time_point now) {
    if (!ride_ || !abilities_) return;
    if (!ride_->mount()) {
        // the mount left the world (desummoned and gone, the session's registry removed it): nothing is ridden, nothing is kept
        if (ride_->riding()) dismount(conn, now, DismountWhy::MountGone);
        ride_.reset();
        return;
    }
    if (!ride_->riding() && (!mounts_ || mounts_->actor_id() != ride_->actor_id())) {
        ride_.reset();   // the controller let the mount go (a desummon in its last moments): the ride is over
        return;
    }
    const Ride::Events ev = ride_->advance(ability_seconds(now));
    if (ev.dash_applied) {
        // the dash's reply [R: 906 packet 67]: the mount's Cooldowns, MoveSpeedMult and CurrentEffects go out with the PlayerController's hit confirmation
        if (view_) view_->update_now(conn, ride_->actor_id());
        bootstrap::HitConfirm h;
        h.record_guid = Ride::kDashHitRecord;
        h.source_type = content::kSourceTypeAbility;
        h.source_guid = ride_->dash_ability();
        h.source_is_actor = true;
        h.predicted_autonomous = true;
        h.instigator = mount_guid_;
        bootstrap::HitTargetOut target;
        target.component = mount_guid_;
        target.cylinder = mount_cylinder_;
        h.targets.push_back(std::move(target));
        if (pending_confirms_.size() >= kMaxPendingConfirms) pending_confirms_.pop_front();
        pending_confirms_.push_back(std::move(h));
        flush_abilities(conn);
    }
}
// C_Y_B and CoA

bool PlayerSession::mounted_trigger(Connection& conn, int64_t ability, int32_t instance, bool on_mount, Clock::time_point now) {
    const double t = ability_seconds(now);
    switch (ride_->trigger_dash(ability, t)) {
        case Ride::Dash::Started:
            ++dashes_;
            dash_ready_at_ = ride_->dash_ready_at();
            spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the dash: its effects come in {:.0f} ms ({} s of MoveSpeedMult x{:.2f})", name(), Ride::kDashReplyDelay * 1000,
                        mounts_ && mounts_->book().data().dash() ? mounts_->book().data().dash()->duration : 0.0,
                        1.0 + (mounts_ && mounts_->book().data().dash() ? mounts_->book().data().dash()->value : 0.0));
            return false;
        case Ride::Dash::OnCooldown:
            ++dashes_refused_;
            spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the dash is refused: it is cooling down (ready in {:.1f} s)", name(), std::max(0.0, ride_->dash_ready_at() - t));
            end_instance(conn, instance, on_mount);
            return false;
        case Ride::Dash::NotTheDash: break;
    }
    // what else a rider may do: the data's flags (the exe's bUsableWhileMounted and bAutoDismount)
    const content::MountAbilityRow* row = mounts_ ? mounts_->book().data().ability(ability) : nullptr;
    if (row && row->role == "summon") return true;   // Summon_Mount while riding: the mount is desummoned (on_summon_trigger gets the rider off first)
    if (row && row->auto_dismount) {
        dismount(conn, now, DismountWhy::Ability);
        return true;
    }
    if (row && row->usable_while_mounted && row->role != "dash") return true;
    ++mount_triggers_refused_;
    spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: ability {} is refused while mounted (bUsableWhileMounted is off)", name(), ability);
    end_instance(conn, instance, on_mount);
    return false;
}

void PlayerSession::end_instance(Connection& conn, int32_t instance, bool on_mount) {
    if (!on_mount) {
        if (pending_ends_.size() < 64) pending_ends_.push_back(instance);
        flush_abilities(conn);
        return;
    }
    if (!registry_ || !ride_) return;
    const uint32_t channel = mount_channel();
    if (channel == 0) return;
    const uint64_t id = ride_->actor_id();
    const FIntrepidNetworkGUID ability = mount_ability_;
    queue("ClientEndAbility (the mount)", [this, id, ability, instance, channel](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        const world::WorldActor* m = registry_ ? registry_->find(id) : nullptr;
        if (!m) return std::nullopt;
        auto s = pb.end_ability_on(m->actor, ability, instance);
        if (s) s->channel = channel;
        return s;
    }, conn);
}

void PlayerSession::hit_on_mount(Connection& conn, const combat::IncomingHit& h, Clock::time_point now) {
    if (!ride_ || !pipeline_) return;
    ++mount_hits_;
    double damage = 0;
    if ((h.flags & stats::kHitMiss) == 0) {
        for (const combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0 && std::isfinite(m.value)) damage += -m.value;
        }
    }
    const Ride::Hurt r = ride_->hurt(damage);
    pipeline_->confirm_hit_on(h, ability_seconds(now), mount_guid_, mount_cylinder_, r.taken, r.died);
    if (r.died) mount_died(conn, now);
}

void PlayerSession::mount_died(Connection& conn, Clock::time_point now) {
    ++mount_deaths_;
    spdlog::info("[session] {}: its mount died under it: the rider is dismounted, the mount closes and its item is locked for {} s", name(), MountController::kLockSeconds);
    dismount(conn, now, DismountWhy::MountDied);
    if (mounts_ && abilities_) {
        const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        mounts_->die(ability_seconds(now), now_unix);
        run_mounts(conn, now);
        // review A M4: the lock and the Health 0 go to the saver now, like the player's own death's inventory: a worldd that stops before the next periodic save must not bring the dead mount back whole
        if (inventory_dirty_) hand_over_inventory("a mount's death", /*periodic=*/false);
    }
}

void PlayerSession::apply_persist(Connection* conn, const MountController::Persist& p) {
    const std::string guid = items::guid_hex(p.item_guid);
    const items::Outcome o = inventory_.update_mount(guid, p.health, p.lock_expiration, /*events=*/conn != nullptr && p.died);
    if (!o.ok) {
        spdlog::debug("[session] {}: the mount's item keeps nothing: {}", name(), o.refusal);
        return;
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    if (!conn || o.events.empty() || !room_for(2)) return;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(o);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent (the mount's item)", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, *conn, /*must_build=*/true);
    }
}

std::optional<bootstrap::Send> PlayerSession::mount_ack(float time_stamp) {
    if (!ride_ || !registry_) return std::nullopt;
    const world::WorldActor* m = registry_->find(ride_->actor_id());
    const uint32_t channel = mount_channel();
    if (!m || channel == 0) return std::nullopt;
    auto s = bootstrap::PlayerBootstrap::move_ack_on(m->actor, time_stamp);
    if (s) s->channel = channel;
    return s;
}

std::optional<bootstrap::Send> PlayerSession::mount_correction(float time_stamp, const GoodPosition& good) {
    if (!ride_ || !registry_) return std::nullopt;
    const world::WorldActor* m = registry_->find(ride_->actor_id());
    const uint32_t channel = mount_channel();
    if (!m || channel == 0) return std::nullopt;
    const uint8_t mode = good.movement_mode == 0 ? repl::kMoveWalking : good.movement_mode;
    auto s = pb_.correction_on(m->actor, time_stamp, repl::Vec3d{good.x, good.y, good.z}, repl::Vec3d{}, mode);
    if (s) s->channel = channel;
    return s;
}

/// The client calls of summoned mounts: desummon and dismount (session/rpc_table.h).
void PlayerSession::register_mounts_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerDesummonMount", [](PlayerSession& s, const RpcContext& c) { s.on_desummon_mount(c.conn, c.now); });
    t.add(RpcScope::PlayerController, "ServerDismountInteraction", [](PlayerSession& s, const RpcContext& c) { s.on_dismount_request(c.conn, c.now); });
}

}}  // namespace aoc::worldd
