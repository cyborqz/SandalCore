// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_requests.cpp
//
//  The client's loads and quiet requests: the settings echo, the load, the targets, the hover, the PvP flag, a closed loot window, the map marker and the tutorials.
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

void PlayerSession::on_combat_settings(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    // every value the request carries goes back as the PlayerController's own: the client's options page and its combat code read that block,
    // and nothing but this update fills it (the paths are built here, so the lambda below owns them together with the values)
    std::vector<std::string> paths;
    std::vector<repl::Value> held;
    if (rpc.call.decoded) {
        for (const schema::RepCommand& c : rpc.call.rpc->commands) {
            const auto it = rpc.call.params.values.find(c.handle);
            if (it == rpc.call.params.values.end()) continue;
            paths.push_back(std::string(kCombatEchoPrefix) + c.path);
            held.push_back(it->second);
        }
    }
    const size_t total = rpc.call.rpc ? rpc.call.rpc->commands.size() : 0;
    if (paths.empty()) {
        spdlog::log(level_for(combat_empty_logged_), "[session] {}: the client's combat settings carry no value worldd can echo", name());
        return;
    }
    if (!within_budget(combat_budget_, "ServerUpdateAccountCombatSettings", now) || !room_for(1)) return;
    spdlog::log(level_for(combat_logged_), "[session] {}: the client's combat settings: echoing {} of the {} values", name(), paths.size(), total);
    queue("combat settings echo",
          [paths = std::move(paths), held = std::move(held)](bootstrap::PlayerBootstrap& pb) {
              std::vector<bootstrap::PropertyValue> values;
              values.reserve(paths.size());
              for (size_t i = 0; i < paths.size(); ++i) values.push_back({paths[i], held[i]});
              return pb.combat_settings_echo(values);
          },
          conn);
}

// -- missing systems, step 0: tutorials stay done -------------------------------------------------------------------------------------------------------

void PlayerSession::on_remove_ftues(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now) {
    ++ftue_calls_;
    if (!rpc.call.decoded) {
        if (!ftue_unreadable_logged_) {
            ftue_unreadable_logged_ = true;
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs arrived and its parameters do not decode: nothing is recorded (further ones are not logged)", name());
        }
        return;
    }
    const protocol::repl::Value* v = protocol::repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "FTUEsToRemove");
    const protocol::repl::ArrayValue* list = v ? v->get_if<protocol::repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty()) return;   // (an empty list is the default: not on the wire)
    // the ids that would join the list: 0 and below are ignored, a repeat adds nothing, and the whole call is refused when the list would go over its cap
    if (list->elements.size() > bootstrap::kMaxCompletedFtues) {
        if (++ftue_refused_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs names {} tutorials, more than a character can have finished ({}): refused", name(), list->elements.size(),
                         bootstrap::kMaxCompletedFtues);
        }
        return;
    }
    std::vector<int64_t> fresh;
    for (const protocol::repl::ElementState& e : list->elements) {
        const auto it = e.values.find(1);
        const int64_t* id = it != e.values.end() ? it->second.get_if<int64_t>() : nullptr;
        if (!id || *id <= 0) {   // (0 is the id of no tutorial; an element that is not on the wire is 0)
            ++ftue_ignored_;
            if (++ftue_ignored_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: Server_RemoveLocaleFTUEs names tutorial {}: ignored", name(), id ? *id : 0);
            continue;
        }
        if (std::find(completed_ftues_.begin(), completed_ftues_.end(), *id) != completed_ftues_.end() || std::find(fresh.begin(), fresh.end(), *id) != fresh.end()) {
            ++ftue_repeats_;
            continue;
        }
        fresh.push_back(*id);
    }
    if (fresh.empty()) {
        spdlog::debug("[session] {}: Server_RemoveLocaleFTUEs: nothing new ({} calls, {} repeats, {} ignored so far)", name(), ftue_calls_, ftue_repeats_, ftue_ignored_);
        return;
    }
    if (completed_ftues_.size() + fresh.size() > bootstrap::kMaxCompletedFtues) {
        if (++ftue_refused_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs would take the finished tutorials over {}: refused, nothing changes", name(), bootstrap::kMaxCompletedFtues);
        }
        return;
    }
    for (const int64_t id : fresh) {
        completed_ftues_.push_back(id);
        spdlog::log(level_for(ftue_logged_, 20), "[session] {}: finished tutorial {} ({} finished in all)", name(), id, completed_ftues_.size());
    }
    progress_changed_ = true;
    hand_over_progress("a tutorial was finished");
    // the list goes back to the client a moment later, as the retail server's quiet replies do: the client takes the finished ones off its running list when it arrives
    if (pb_.set_completed_ftues(completed_ftues_)) {
        queue_later("FTUE Component", [](bootstrap::PlayerBootstrap& pb) { return pb.ftue_update(); }, conn, now, /*must_build=*/false);
    } else {
        spdlog::warn("[session] {}: the tutorial component does not take the {} finished tutorials: the client is not told", name(), completed_ftues_.size());
    }
}

size_t PlayerSession::gm_reset_tutorials(Connection& conn, const std::string& by) {
    const size_t n = completed_ftues_.size();
    if (n == 0) return 0;
    completed_ftues_.clear();
    progress_changed_ = true;
    hand_over_progress("the tutorials were put back by a game master");
    if (pb_.set_completed_ftues(completed_ftues_)) queue("FTUE Component", [](bootstrap::PlayerBootstrap& pb) { return pb.ftue_update(); }, conn, /*must_build=*/false);
    spdlog::info("[session] {}: had {} finished tutorial(s) put back by {}", name(), n, by);
    return n;
}

// -- missing systems, step 1: the client's quiet requests ------------------------------------------------------------------------------------------------------
// A target, a hover, the PvP flag, a closed loot window, a marker and a batch of bar slots: the client sends them and waits for nothing, but the game's own server keeps
// them (and answers the first three with a property update a tenth of a second later).  Nothing here is saved: the flag, the targets and the marker last as long as the session.

void PlayerSession::on_set_active_target(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++target_calls_;
    std::optional<uint64_t> slot;
    const repl::Value* named = nullptr;
    if (rpc.call.decoded) {
        slot = repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Slot"), 0);   // (a slot at 0 is not on the wire)
        named = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "InActiveTarget");               // (no object is not on the wire either)
    }
    const FIntrepidNetworkGUID* object = named ? named->get_if<FIntrepidNetworkGUID>() : nullptr;
    if (!rpc.call.decoded || !slot || (named && !object)) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetActiveTarget does not read ({} parameter bits): the target is not kept (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetActiveTarget", now) || !room_for(1)) return;
    if (*slot < 1 || *slot > 2) {
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: ServerSetActiveTarget names target slot {}: only the slots 1 and 2 exist, ignored", name(), *slot);
        return;
    }
    // the target is an actor of the world or the player's own pawn; any other object is no target
    FIntrepidNetworkGUID target;
    if (object && object->is_valid()) {
        const FIntrepidNetworkGUID& own = pb_.setup().guids.pawn;
        const world::WorldActor* actor = registry_ ? registry_->find(object->ObjectId) : nullptr;
        if (object->ObjectId == own.ObjectId) {
            target = own;
        } else if (actor) {
            target = actor->actor.guid;
        } else {
            ++target_unknown_;
            if (++quiet_refused_logged_ <= kLoggedPerKind) {
                spdlog::info("[session] {}: ServerSetActiveTarget names object {}, which is no actor the world knows: the slot is left empty", name(), object->ObjectId);
            }
        }
    }
    FIntrepidNetworkGUID& held = active_targets_[static_cast<size_t>(*slot - 1)];
    if (held == target) return;   // (the same target again: the client is told what it was told, so nothing goes out)
    held = target;
    ++target_changes_;
    if (!pb_.set_active_targets(active_targets_)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.ActiveTargets: the client is not told", name());
        return;
    }
    // the answer is the property update of the game's own server, about 84 ms after the call
    queue_later("CombatInfo (target)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("active target"); }, conn, now, /*must_build=*/false);
    spdlog::log(level_for(target_logged_, 20), "[session] {}: target slot {} is {} now", name(), *slot, target.is_valid() ? "object " + std::to_string(target.ObjectId) : std::string("empty"));
}

void PlayerSession::on_set_hover_target(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++hover_calls_;
    std::array<uint32_t, 4> words{};
    bool ok = rpc.call.decoded;
    constexpr const char* kWords[4] = {"InHoverTargetGuid.A", "InHoverTargetGuid.B", "InHoverTargetGuid.C", "InHoverTargetGuid.D"};
    for (size_t i = 0; ok && i < 4; ++i) {
        const auto w = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, kWords[i]), 0);   // (a word at 0 is not on the wire)
        ok = w && *w >= std::numeric_limits<int32_t>::min() && *w <= std::numeric_limits<uint32_t>::max();
        if (ok) words[i] = static_cast<uint32_t>(*w);
    }
    if (!ok) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetHoverTargetGuid does not read ({} parameter bits): the hover target is not kept (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetHoverTargetGuid", now) || !room_for(1)) return;
    if (words == hover_words_) return;
    hover_words_ = words;
    ++hover_changes_;
    if (!pb_.set_hover_target(words)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.HoverTargetGuid: the client is not told", name());
        return;
    }
    queue_later("CombatInfo (hover)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("hover target"); }, conn, now, /*must_build=*/false);
    const bool nothing = words == std::array<uint32_t, 4>{};
    spdlog::log(level_for(hover_logged_, 20), "[session] {}: points at {}", name(), nothing ? std::string("nothing") : fmt::format("{:08x}-{:08x}-{:08x}-{:08x}", words[0], words[1], words[2], words[3]));
}

void PlayerSession::on_set_pvp_flag(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++flag_calls_;
    const auto flag = rpc.call.decoded ? repl::rpcf::bool_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Flag"), false) : std::nullopt;
    if (!flag) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetPvpFlag does not read ({} parameter bits): the flag is not changed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetPvpFlag", now) || !room_for(1)) return;
    if (*flag == pvp_flag_) return;   // (a flag that is the flag: the game's server marks it and sends nothing, as nothing changed)
    pvp_flag_ = *flag;
    ++flag_changes_;
    if (!pb_.set_pvp_force_flag(pvp_flag_)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.bPvPForceFlag: the client is not told", name());
        return;
    }
    queue_later("CombatInfo (PvP flag)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("PvP flag"); }, conn, now, /*must_build=*/false);
    spdlog::log(level_for(flag_logged_, 20), "[session] {}: the PvP flag is {} (kept until the session ends)", name(), pvp_flag_ ? "on" : "off");
}

void PlayerSession::on_close_loot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    const repl::Value* named = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "LootContainer") : nullptr;
    const FIntrepidNetworkGUID* container = named ? named->get_if<FIntrepidNetworkGUID>() : nullptr;
    if (!rpc.call.decoded || (named && !container)) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerCloseLoot does not read ({} parameter bits): the loot window is not closed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerCloseLoot", now)) return;
    if (loot_actor_ == 0) return;   // (the window is closed already: the corpse was emptied, left behind or looted out)
    combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const combat::Corpse* corpse = after ? after->corpse_of(loot_actor_) : nullptr;
    // the call names the container the window shows (the game's server ignores a close that names another one)
    if (!container || (corpse && corpse->parts && container->ObjectId != corpse->parts->container)) {
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: ServerCloseLoot names another container than the open loot window's: the window stays open", name());
        return;
    }
    const std::string closed = corpse ? corpse->name : std::string("a corpse that is gone");
    loot_actor_ = 0;
    ++loot_closes_;
    spdlog::log(level_for(close_logged_, 20), "[session] {}: closes the loot window of {}", name(), closed);
}

void PlayerSession::on_set_map_marker(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    ++marker_calls_;
    const repl::Value* named = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Location") : nullptr;
    const repl::Vec3d* at = named ? named->get_if<repl::Vec3d>() : nullptr;
    if (!rpc.call.decoded || (named && !at)) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetMapMarkerLocation does not read ({} parameter bits): the marker is not changed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    const repl::Vec3d where = at ? *at : repl::Vec3d{};   // (a location of 0, 0, 0 is not on the wire: it is the call that takes the marker away)
    if (!std::isfinite(where.x) || !std::isfinite(where.y) || !std::isfinite(where.z)) {
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: ServerSetMapMarkerLocation names a location that is not a number: the marker is not changed", name());
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetMapMarkerLocation", now)) return;
    if (where.x == 0 && where.y == 0 && where.z == 0) {
        if (!map_marker_) return;
        map_marker_.reset();
        spdlog::log(level_for(marker_logged_, 20), "[session] {}: the map marker is taken away", name());
        return;
    }
    if (map_marker_ && *map_marker_ == where) return;
    map_marker_ = where;
    spdlog::log(level_for(marker_logged_, 20), "[session] {}: the map marker is at ({:.0f}, {:.0f}, {:.0f}) (kept until the session ends)", name(), where.x, where.y, where.z);
}

void PlayerSession::on_client_load(Connection& conn, Clock::time_point now) {
    if (!within_budget(load_budget_, "ServerRequestClientLoad", now) || !room_for(8)) return;
    spdlog::log(level_for(load_logged_), "[session] {}: the client asks for its load: storage payloads, group and action bar", name());
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment, bootstrap::StorageKind::QuestStorage};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));   // Gathering stage 3: the container of every worn bag
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::PlayerController); },
              conn);
    }
    queue("ClientUpdateGroup", [](bootstrap::PlayerBootstrap& pb) { return pb.empty_group(); }, conn);
    if (book_) {
        // plan D1, stage 2: the bar the character arranged, or the default (the weapon bank and the class's skills), once, as retail's
        const repl::ActionbarPayload payload = bar_payload();
        queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn);
    } else {
        queue("ClientReceiveActionbarSlotPayload", [](bootstrap::PlayerBootstrap& pb) { return pb.default_action_bar(); }, conn);
    }
}

/// The client calls of the settings echo, loads, the quiet requests (a flag, a closed loot window, a batch of bar slots, a marker) and the tutorials (session/rpc_table.h).
void PlayerSession::register_requests_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestClientLoad", [](PlayerSession& s, const RpcContext& c) { s.on_client_load(c.conn, c.now); });
    t.add(RpcScope::PlayerController, "ServerUpdateAccountCombatSettings", [](PlayerSession& s, const RpcContext& c) { s.on_combat_settings(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetPvpFlag", [](PlayerSession& s, const RpcContext& c) { s.on_set_pvp_flag(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCloseLoot", [](PlayerSession& s, const RpcContext& c) { s.on_close_loot(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetMapMarkerLocation", [](PlayerSession& s, const RpcContext& c) { s.on_set_map_marker(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerSetActiveTarget", [](PlayerSession& s, const RpcContext& c) { s.on_set_active_target(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerSetHoverTargetGuid", [](PlayerSession& s, const RpcContext& c) { s.on_set_hover_target(c.conn, c.rpc, c.now); });
    t.add(RpcScope::FtueComponent, "Server_RemoveLocaleFTUEs", [](PlayerSession& s, const RpcContext& c) { s.on_remove_ftues(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
