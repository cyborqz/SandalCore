// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_gather.cpp  (Gathering stage 2, part "Harvesting")
//
//  The player's side of a harvest of a resource node: the checks of the start, the channel's conditions, and the frame of the result.  The node's shared state is
//  gather/node_board.h's, the data and the rolls gather/gathering.h's, the requirements gather/rules.h's; this file applies them to the session's items, professions and
//  experience and queues what the client is told, in the order of the retail captures [R: 1427 packets 673 and 674, 1470 packets 781 to 784, 1461 packets 897 and 904]:
//
//    the start        the pawn's InteractInfo.ChannelingData (the node, the start on the synchronized clock, the channel's length, the tool: the client plays the gather animation,
//                     shows the bar and sends its own cancel from it) and the node's update (the board's): who channels, the status counter [R: 1427 packet 67, the pawn's block first]
//    every ~114 ms    the node's heartbeat (the board's)
//    the result       ClientUpdateItemDurability (the tool's slot, one less), a ClientReceiveStorageEvent for each stack, the pawn's ChannelingData cleared, the XPTracker (the
//                     profession's XP and Adventuring's, as debt removal first) and BroadcastPlayLevelUpEffects for a level; the node is Gathered in the same step [R: 1427 packet 674:
//                     the node's done update, the pawn's clear, then the PlayerController's XPTracker maps]
//    a cancel         the pawn's ChannelingData cleared in the same form [inferred: no capture shows a cancel]
//    a full bag       the stacks that do not fit are not lost: an APlayerLootPile where the player stands, 100 ms after the experience frame, and a callout "Unable to loot, not
//                     enough inventory space." (3 s) in that frame [R: 1461]
//
//  Every refusal and every cancellation is logged and counted; the first few of each kind at info.  The log lines the user's test reads start with "[gather]".
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "gather/rules.h"
#include "items/acquired.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
// -- Cyb x C O A --

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The player may stray this far (cm) beyond the range a harvest starts from before the channel ends: one step of a client that fidgets is not leaving.
constexpr double kChannelSlackCm = 100.0;
/// A channel needs standing still [modelled: no capture shows who ends it when the player walks; the client does with ServerInterruptChannel once it believes it is channeling
/// (the pawn's ChannelingData), and this is the server's safety net for a client that does not]: the channel ends, with no reward, when the player's good position is more than
/// this far (cm, horizontally) from where it was when the channel began.  The captured harvesters stood still (0 cm in 8 of 10; 1469's 32 cm is a vertical settle after the result);
/// 60 cm is a fidget's step, not a walk.
constexpr double kChannelDriftCm = 60.0;
/// [R] the pile of a full bag opens 100 ms after the experience frame (1461 packets 897 and 904).
constexpr std::chrono::milliseconds kPileDelay{100};
/// [R: the mounts' refusal callout lives 3 s] the "no room" callout.
constexpr std::chrono::seconds kCalloutLife{3};
constexpr const char* kNoRoomText = "Unable to loot, not enough inventory space.";   // [R: 1461 packet 897]
constexpr int32_t kCalloutType = 1, kCalloutPointSize = 32;   // [R: the mounts' refusal callout: type 1, point size 32; 1461's own is checked by test_worldd_gather]

/// [R: ten harvests] the flags word of a channel that runs (6) and of one that is over (0x106); [inferred] what the bits mean.
constexpr uint32_t kChannelingFlags = 6, kChannelingOverFlags = 0x106;

/// The pawn's ChannelingData once the channel is over: the node, the "over" flags and nothing else [R: 305 bits in all ten captures].
repl::ChannelingData channeling_over(const protocol::emit::FIntrepidNetworkGUID& target) {
    repl::ChannelingData d;
    d.target = target;
    d.flags = kChannelingOverFlags;
    return d;
}

std::string join_names(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ", ") + s;
    return out;
}

}  // namespace

std::unique_ptr<stats::CharacterStats::Evaluation> PlayerSession::gather_run(std::string& why) {
    if (!stats_ || !stats_->started()) {
        why = "the character has no stats";
        return nullptr;
    }
    return stats_->evaluation(bootstrap::worn_items_of(inventory_), why);
}

void PlayerSession::on_gather_interact(Connection& conn, const world::WorldActor& node, Clock::time_point now) {
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (!g) {
        if (!gather_off_logged_) {
            gather_off_logged_ = true;
            spdlog::info("[gather] {}: an interaction with a resource node is ignored: this world does not harvest (content.db has no gather tables, or --no-gathering)", name());
        }
        ++interact_ignored_;
        return;
    }
    const content::GatherDefRow* def = g->def_of(node);
    const std::string what = (def ? def->name : std::string("a node of no gather def")) + " (node " + std::to_string(node.id) + ")";
    const auto refuse = [&](const char* kind, const std::string& why) {
        ++harvests_refused_;
        ++harvest_refusals_[kind];
        spdlog::log(level_for(gather_refused_logged_, 12), "[gather] {}: the harvest of {} is refused ({}): {}", name(), what, kind, why);
    };
    if (!def) return refuse("no gather row", "the node's GatherableActorDef has no row in gather_def");
    // the client has the node open (a prompt it never saw cannot be answered)
    if (view_->state_of(node.id) != world::ClientState::Awake) return refuse("not open", "the client does not have that node open");
    begin_channel(conn, node, *def, what, "gather", "harvest", /*hunt=*/false, /*gives_nothing=*/false, now);
}

void PlayerSession::on_hunt_interact(Connection& conn, const world::WorldActor& corpse, Clock::time_point now) {
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    const gather::Gathering::HuntKind* kind = g ? g->hunt_kind(corpse.npc.interact_def) : nullptr;
    if (!kind) {
        ++interact_ignored_;
        return;
    }
    const std::string what = kind->name + " (" + (corpse.npc.name.empty() ? std::string("creature") : corpse.npc.name) + " " + std::to_string(corpse.id) + ")";
    const auto refuse = [&](const char* why_kind, const std::string& why) {
        ++harvests_refused_;
        ++harvest_refusals_[why_kind];
        spdlog::log(level_for(gather_refused_logged_, 12), "[hunt] {}: the hunt of {} is refused ({}): {}", name(), what, why_kind, why);
    };
    // a huntable is hunted ALIVE, as it stands [R: captures 1456 1457 1458 1178 1179 start at the client's interact with no kill before it; the user, 2026-10-12: "while its standing still, you
    // interact with it"]; one whose hunt is done (it dies with the done update) is not hunted twice
    combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const combat::Corpse* body = after ? after->corpse_of(corpse.id) : nullptr;
    if (body && body->hunted) return refuse("corpse hunted", "the creature was hunted already");
    begin_channel(conn, corpse, *kind->row, what, "hunt", "hunt", /*hunt=*/true, kind->gives_nothing, now);
}
// [CoA - C:Y:B]

void PlayerSession::begin_channel(Connection& conn, const world::WorldActor& node, const content::GatherDefRow& def_row, const std::string& what, const char* tag, const char* noun, bool hunt,
                                  bool gives_nothing, Clock::time_point now) {
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (!g) return;
    const content::GatherDefRow* def = &def_row;
    const auto refuse = [&](const char* kind, const std::string& why) {
        ++harvests_refused_;
        ++harvest_refusals_[kind];
        spdlog::log(level_for(gather_refused_logged_, 12), "[{}] {}: the {} of {} is refused ({}): {}", tag, name(), noun, what, kind, why);
    };
    // in range
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(node.x - at.x, node.y - at.y);
    const double reach = gather::reach_cm(*def);
    if (distance > reach) return refuse("out of range", std::to_string(static_cast<int>(distance)) + " cm away, the interaction reaches " + std::to_string(static_cast<int>(reach)) + " cm");
    // not already at it (a repeated or flooded interact changes nothing)
    if (gather_.node != 0 || foliage_.row != kNoInstance) return refuse("already channeling", gather_.node == node.id ? "this player is harvesting it already" : "this player is harvesting something already");
    // the node
    gather::NodeBoard& board = g->board();
    switch (board.state_of(node.id)) {
        case gather::NodeState::Channeling: return refuse(hunt ? "corpse busy" : "node busy", "another player is harvesting it");
        case gather::NodeState::Gathered: return refuse(hunt ? "corpse hunted" : "node gathered", hunt ? "it was hunted already" : "it was gathered and has not returned yet");
        case gather::NodeState::Available: break;
    }
    // the requirements: certification, level, the tool, combat
    gather::ToolInfo tool;
    const svc::InventoryItem* worn = inventory_.at(svc::ItemContainer::Equipment, def->tool_slot);
    if (worn) {
        tool.worn = true;
        tool.tool = g->tool(worn->item_record_id);
        tool.durability = worn->durability;
    }
    const bool in_combat = stats_ && stats_->in_combat();
    const gather::Verdict verdict = gather::check_requirements(*def, professions_, tool, in_combat);
    if (!verdict.ok()) return refuse(gather::refusal_name(verdict.why), verdict.text);
    // the channel's length: the data's Speed equation for this player
    std::string why;
    auto run = gather_run(why);
    if (!run) return refuse("no stats", why);
    const auto seconds = g->channel_seconds(*def, *run, why);
    if (!seconds) return refuse("no duration", why);
    gather::Channeler by;
    by.player_id = static_cast<uint64_t>(pb_.setup().player_id);
    const auto& cg = pb_.setup().character_guid;
    by.character = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
    by.pawn = pb_.setup().guids.pawn;
    if (!board.begin(node.id, by, now, std::chrono::duration<double>(*seconds), why)) return refuse("node refused", why);
    gather_.node = node.id;
    gather_.target = node.actor.guid;
    gather_.def = def;
    gather_.tool_slot = def->tool_slot;
    gather_.tool_guid = worn->instance_guid;
    gather_.started = now;
    gather_.seconds = *seconds;
    gather_.start_x = at.x;
    gather_.start_y = at.y;
    gather_.hunt = hunt;
    gather_.gives_nothing = gives_nothing;
    gather_.what = what;
    ++harvests_started_;
    if (hunt) {   // the corpse stays for the hunt, and a few seconds after it
        if (combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr) after->hunt_started(node.id, npc_world_->world_clock(), *seconds);
    }
    const professions::Profession* profession = professions_.valid() ? professions_.book()->find(def->profession_guid) : nullptr;
    spdlog::log(level_for(gather_logged_, 20), "[{}] {}: starts {} {}: a {} {} {}, with {}, a channel of {:.2f} s (base {:.2f} s), {:.0f} cm from it{}", tag, name(), hunt ? "hunting" : "harvesting", what,
                professions_.valid() ? professions_.book()->tier(def->cert_tier).name : std::string("?"), profession ? profession->name : std::string("?"), hunt ? "kill" : "node",
                tool.tool ? tool.tool->item_name : std::string("?"), *seconds, def->base_duration, distance, gives_nothing ? "; this kind gives no item (RETIRED_ or empty table): the experience only" : "");
    // the pawn's ChannelingData first, then the node [R: 1427 packet 67: the pawn's block precedes the node's open]: what the client plays the animation and the bar from
    repl::ChannelingData channeling;
    channeling.target = node.actor.guid;
    channeling.flags = kChannelingFlags;
    channeling.active = true;
    channeling.start_time = std::floor(bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now)));   // whole seconds on the effects' clock [R: all ten captures]
    channeling.timed = true;
    channeling.duration = static_cast<float>(*seconds);
    channeling.tool_record = worn->item_record_id;
    if (const auto guid = items::guid_of(worn->instance_guid)) channeling.tool_guid = *guid;
    send_channeling(conn, channeling);
    view_->update_now(conn, node.id);   // the harvester hears of the start at once (retail: 87 ms after the interact)
}

void PlayerSession::send_channeling(Connection& conn, const repl::ChannelingData& data) {
    if (!world_) return;
    if (!interact_info_) interact_info_ = world_->mint_guid();
    const protocol::emit::FIntrepidNetworkGUID info = *interact_info_;
    queue("the pawn's channeling data", [info, data](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.ensure_interact_info(info)) return std::nullopt;
        return pb.pawn_channeling(data);
    }, conn, /*must_build=*/true);
}

void PlayerSession::on_interrupt_channel(Connection& conn, Clock::time_point) {
    if (foliage_.row != kNoInstance) {   // Gathering stage 3: the client ends the cutting of a tree or plant the same way [modelled: no capture shows the client's cancel of a foliage channel]
        cancel_foliage(&conn, "the client interrupted the channel (ServerInterruptChannel)");
        return;
    }
    if (gather_.node == 0) {
        spdlog::debug("[gather] {}: ServerInterruptChannel with no harvest in progress: ignored", name());
        return;
    }
    cancel_gather(&conn, "the client interrupted the channel (ServerInterruptChannel)");
}

void PlayerSession::cancel_gather(Connection* conn, const std::string& why, Interrupt interrupt) {
    cancel_foliage(conn, why, interrupt);   // (a player cuts one thing at a time: the tree's channel ends where the node's would)
    if (gather_.node == 0) return;
    const uint64_t node = gather_.node;
    const std::string def = gather_.def ? gather_.def->name : std::string("?");
    const protocol::emit::FIntrepidNetworkGUID target = gather_.target;
    const bool hunt = gather_.hunt;
    const std::string what = gather_.what;
    gather_ = GatherRun{};
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    const gather::Channel* ch = g ? g->board().channel_of(node) : nullptr;
    const bool mine = ch && ch->player_id == static_cast<uint64_t>(pb_.setup().player_id);
    if (mine) g->board().cancel(node, why);
    if (hunt && mine) {   // a living creature is alive again, as it was
        if (combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr) after->hunt_cancelled(node);
    }
    ++harvests_cancelled_;
    if (hunt) spdlog::log(level_for(gather_logged_, 20), "[hunt] {}: the hunt of {} ends with no reward: {}", name(), what.empty() ? def : what, why);
    else spdlog::log(level_for(gather_logged_, 20), "[gather] {}: the harvest of {} (node {}) ends with no reward: {}", name(), def, node, why);
    if (conn) send_channeling(*conn, channeling_over(target));   // the client's animation and bar end with the channel [inferred: the result's clear, no capture shows a cancel]
    if (conn && interrupt != Interrupt::None) send_interrupt_callout(*conn, interrupt, last_now_);   // Gathering stage 3 (optional): worldd's own reasons are told, as a tree's are [modelled]
    if (conn && view_ && mine) view_->update_now(*conn, node);
}
// == C-Y-B | CoA ==

void PlayerSession::run_gather(Connection& conn, Clock::time_point now) {
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (g) g->board().advance(now);   // any session's tick brings the returned nodes back and frees the stuck channels
    // what waits for its time: the pile of a full bag, the callout's end
    while (!gather_piles_.empty() && now >= gather_piles_.front().due) {
        GatherPile pile = std::move(gather_piles_.front());
        gather_piles_.pop_front();
        std::string error;
        uint64_t actor = 0;
        if (npc_world_) {
            const auto& cg = pb_.setup().character_guid;
            actor = npc_world_->drop_harvest_pile(pile.x, pile.y, pile.z, repl::GuidValue{cg[0], cg[1], cg[2], cg[3]}, pb_.setup().name, pile.stacks, error);
        } else {
            error = "the session has no world";
        }
        if (actor != 0) {
            ++harvest_piles_;
            spdlog::log(level_for(gather_done_logged_, 20), "[gather] {}: the bag was full: {} stack(s) of the harvest of {} are in a pile where it stands", name(), pile.stacks.size(), pile.node_name);
        } else {
            spdlog::warn("[gather] {}: the pile for {} stack(s) of the harvest of {} is not made ({}): they are lost", name(), pile.stacks.size(), pile.node_name, error);
        }
    }
    while (!gather_callouts_.empty() && now >= gather_callouts_.front().due) {
        if (!room_for(1)) break;
        const uint64_t key = gather_callouts_.front().key;
        gather_callouts_.pop_front();
        queue("a callout is taken off", [key](bootstrap::PlayerBootstrap& pb) { return pb.callout_remove(key); }, conn);
    }
    if (gather_.node == 0 || !g) return;

    // the channel's conditions: the node is this player's, in the world, the player alive and on foot, the same tool in the slot, still in range
    const uint64_t me = static_cast<uint64_t>(pb_.setup().player_id);
    const gather::Channel* ch = g->board().channel_of(gather_.node);
    if (!ch || ch->player_id != me) return cancel_gather(&conn, "the node is not this player's channel any more");
    const world::WorldActor* node = registry_ ? registry_->find(gather_.node) : nullptr;
    if (!node) return cancel_gather(&conn, "the node left the world");
    if (dead_) return cancel_gather(&conn, "the player is dead");
    if (mounted()) return cancel_gather(&conn, "the player is on a mount");
    const svc::InventoryItem* tool = inventory_.at(svc::ItemContainer::Equipment, gather_.tool_slot);
    if (!tool || tool->instance_guid != gather_.tool_guid) return cancel_gather(&conn, tool ? "the tool was swapped for another" : "the tool was taken off");
    if (!(tool->durability > 0)) return cancel_gather(&conn, "the tool broke");
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(node->x - at.x, node->y - at.y);
    if (distance > gather::reach_cm(*gather_.def) + kChannelSlackCm) {
        return cancel_gather(&conn, "the player moved away (" + std::to_string(static_cast<int>(distance)) + " cm from the node, the interaction reaches " +
                                        std::to_string(static_cast<int>(gather::reach_cm(*gather_.def))) + " cm)", Interrupt::Movement);
    }
    // standing still: measured from where the channel began, so a slow walk adds up
    const double drift = std::hypot(at.x - gather_.start_x, at.y - gather_.start_y);
    if (drift > kChannelDriftCm) {
        return cancel_gather(&conn, "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) + " cm since the channel began (a harvest needs standing still; at most " +
                                        std::to_string(static_cast<int>(kChannelDriftCm)) + " cm)", Interrupt::Movement);
    }
    // the heartbeat: the harvester's own client hears of it at once
    if (g->board().beat(gather_.node, now)) view_->update_now(conn, gather_.node);
    if (now >= ch->completes) complete_gather(conn, now);
}

bool PlayerSession::complete_gather(Connection& conn, Clock::time_point now) {
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!g || !db || !gather_.def) return true;
    const content::GatherDefRow& def = *gather_.def;
    const uint64_t node_id = gather_.node;
    const size_t grants = world_ ? world_->auto_grants().size() : 0;
    if (!room_for(14 + grants)) return false;   // the frame waits for room in the reply queue; the channel's clock is over
    const world::WorldActor* node = registry_ ? registry_->find(node_id) : nullptr;
    if (!node) {
        cancel_gather(&conn, "the node left the world");
        return true;
    }
    std::string why;
    auto run = gather_run(why);
    if (!run) {
        cancel_gather(&conn, "no stat run for the rolls: " + why);
        return true;
    }
    const world::CityNodes* nodes = world_->city_nodes();
    const world::NodeSite site = nodes ? nodes->node_at(node->x, node->y) : world::NodeSite{};
    auto harvest = g->harvest(def, *run, progress_.level, site, nodes, g->rng(), why);
    if (!harvest) {
        cancel_gather(&conn, "the rolls do not run: " + why);
        return true;
    }
    const bool hunt = gather_.hunt;
    if (hunt && gather_.gives_nothing) harvest->items.clear();   // a kind whose items are RETIRED_ or whose table is empty: the experience only (logged at the start)
    // the node is gathered in the step the result is made (a second completion of the same channel is not possible); a hunted corpse is never "returned": it is removed from the world a few
    // seconds after the hunt, and the board forgets it with it
    const std::chrono::duration<double> respawn = hunt ? std::chrono::duration<double>(3600.0) : g->board().respawn_of(def.override_respawn, def.empty_duration);
    if (!g->board().complete(node_id, now, respawn, why)) {
        cancel_gather(&conn, (hunt ? "the corpse cannot be hunted: " : "the node cannot be gathered: ") + why);
        return true;
    }
    const std::string node_name = hunt ? gather_.what : def.name + " (node " + std::to_string(node_id) + ")";
    const protocol::emit::FIntrepidNetworkGUID node_target = gather_.target;
    HarvestFrame frame;
    frame.what = node_name;
    if (hunt) frame.tag = "hunt";
    frame.profession = def.profession_guid;
    frame.harvest = &*harvest;
    frame.tool_guid = gather_.tool_guid;
    frame.tool_slot = gather_.tool_slot;
    gather_ = GatherRun{};
    ++harvests_completed_;
    // [R: 1427 packet 674] the pawn's ChannelingData is cleared between the storage events and the XPTracker
    frame.after_items = [this, &conn, node_target] { send_channeling(conn, channeling_over(node_target)); };
    if (hunt) {
        combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
        if (after) after->hunt_done(node_id, npc_world_->world_clock());
        frame.log_tail = "; the corpse goes in " + std::to_string(static_cast<int>(after ? after->config().hunt_done_seconds : 4.0)) + " s";
    } else {
        frame.log_tail = "; the node returns in " + std::to_string(static_cast<int>(respawn.count())) + " s";
    }
    deliver_harvest(conn, now, frame);
    return true;
}
// Cyb with C.O.A

void PlayerSession::deliver_harvest(Connection& conn, Clock::time_point now, const HarvestFrame& frame) {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const gather::Harvest& harvest = *frame.harvest;
    const std::string& node_name = frame.what;
    const std::string& tool_guid = frame.tool_guid;
    const int tool_slot = frame.tool_slot;

    // the tool's wear
    const svc::InventoryItem* tool = inventory_.at(svc::ItemContainer::Equipment, tool_slot);
    float tool_before = 0, tool_after = 0;
    bool wore = false;
    if (tool && tool->instance_guid == tool_guid) {
        tool_before = tool->durability;
        tool_after = static_cast<float>(std::max(0.0, static_cast<double>(tool_before) - harvest.durability_loss));
        wore = inventory_.set_durability(svc::ItemContainer::Equipment, tool_slot, tool_guid, tool_after);
    }

    // the stacks: the rolled items cut to the record's stack size
    std::vector<combat::LootStack> stacks;
    std::map<int64_t, std::string> shown_name;   // each rolled record's name for the pick-up popup (the record's ItemName; its internal name in a content.db older than contentc 16)
    for (const combat::RolledItem& it : harvest.items) {
        std::string err;
        const auto rec = db ? db->item(it.item, err) : std::nullopt;
        if (rec && !shown_name.count(it.item)) shown_name[it.item] = rec->display_name.empty() ? rec->name : rec->display_name;
        const int64_t max_stack = rec ? std::max(1, rec->max_stack_size) : 1;
        const int quality = it.quality > 0 ? it.quality : (rec ? std::max(1, rec->default_quality) : 1);
        for (int64_t left = it.quantity; left > 0;) {
            const int64_t take = std::min(left, max_stack);
            combat::LootStack s;
            s.item = it.item;
            s.variation = it.variation;
            s.quantity = take;
            s.quality = quality;
            stacks.push_back(s);
            left -= take;
        }
    }
    // the inventory takes what fits and routes it by kind (Gathering stage 3: a gathered item is a resource: it tops up the stacks of its record and quality in the resource containers and
    // starts new ones at the first free rectangle of the first container with room, the Backpack is not touched); the rest is a pile
    std::vector<svc::InventoryItem> fresh;
    std::vector<combat::LootStack> overflow;
    std::map<std::string, int64_t> variation_of;   // the rolled variation of each fresh stack, for the stacks that go to the pile
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (size_t i = 0; i < stacks.size(); ++i) {
        std::string err;
        const auto rec = db ? db->item(stacks[i].item, err) : std::nullopt;
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = stacks[i].item;
        it.instance_guid = fresh_guid();
        for (const svc::InventoryItem& other : fresh) {
            if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();   // (fresh_guid looks at the inventory only)
        }
        it.quantity = static_cast<int32_t>(std::min<int64_t>(stacks[i].quantity, 1 << 20));
        it.quality = stacks[i].quality;
        it.durability = rec ? static_cast<float>(std::floor(rec->max_durability + 0.5)) : 0.0f;
        it.created_at_ms = created;
        variation_of[it.instance_guid] = stacks[i].variation;
        fresh.push_back(std::move(it));
    }
    const auto pile_stack = [&](const svc::InventoryItem& it) {
        combat::LootStack s;
        s.item = it.item_record_id;
        s.variation = variation_of.count(it.instance_guid) ? variation_of[it.instance_guid] : 0;
        s.quantity = it.quantity;
        s.quality = it.quality;
        return s;
    };
    items::Outcome granted;
    granted.ok = true;
    if (!fresh.empty()) {
        items::GrantResult placed = inventory_.grant_what_fits(fresh, item_catalog());
        if (!placed.outcome.ok) {   // a record the table refuses: nothing was placed
            spdlog::error("[gather] {}: the harvest's items are not granted ({}): they go to a pile", name(), placed.outcome.refusal);
            for (const svc::InventoryItem& it : fresh) overflow.push_back(pile_stack(it));
        } else {
            granted = std::move(placed.outcome);
            for (const svc::InventoryItem& it : placed.rest) {
                if (it.quantity > 0) overflow.push_back(pile_stack(it));
            }
        }
    }
    if (wore || !granted.events.empty()) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
    }

    // the experience: the profession's, then Adventuring's (the debt first)
    professions::Professions after = professions_;
    if (professions_.valid() && harvest.profession_xp > 0) after.add_xp(frame.profession, harvest.profession_xp);
    const std::vector<professions::Change> changes = professions_.valid() ? set_professions(after) : std::vector<professions::Change>();
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    const std::span<const content::AutoGrant> grant_list = world_ ? world_->auto_grants() : std::span<const content::AutoGrant>();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && harvest.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grant_list, progress_, harvest.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);
// C O A - C | Y | B

    // what the client is told, in the order of the retail frame: the tool's wear, the items, the experience
    if (wore) {
        const int32_t slot = tool_slot;
        const float value = tool_after;
        queue("ClientUpdateItemDurability", [slot, value](bootstrap::PlayerBootstrap& pb) { return pb.item_durability(bootstrap::ItemStorage::Equipment, slot, value); }, conn, /*must_build=*/true);
    }
    if (!granted.events.empty()) {
        std::vector<bootstrap::ItemEvent> events = item_events_for(granted);
        // the pick-up popup: the retail server sends an FCalloutQueueInfo with the Add of every harvested item (items/acquired.h; the 24 captured events), and the client shows the popup with
        // the item's icon from it.  A stack that was topped up arrives as the Remove of the old stack and the Add of the new total: the popup says what was added (inferred, no capture has one)
        const int64_t made = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        for (size_t i = 0; i < events.size(); ++i) {
            bootstrap::ItemEvent& ev = events[i];
            if (ev.type != repl::StorageEventType::Add || !ev.item.has_item) continue;
            repl::ItemInstance shown = ev.item;
            if (i > 0 && events[i - 1].type == repl::StorageEventType::Remove && events[i - 1].storage == ev.storage && events[i - 1].slot == ev.slot &&
                events[i - 1].item.has_item && events[i - 1].item.quantity < ev.item.quantity) {
                shown.quantity = ev.item.quantity - events[i - 1].item.quantity;
            }
            const auto name = shown_name.find(static_cast<int64_t>(shown.record_id));
            ev.callout = items::acquired_callout(shown, name != shown_name.end() ? std::string_view(name->second) : std::string_view("Unknown item"), made);
        }
        for (size_t at2 = 0; at2 < events.size(); at2 += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at2),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at2 + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    if (!overflow.empty()) {
        const int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const int64_t ticks = items::ticks_of_ms(unix_ms);
        const uint64_t key = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
        queue("a callout", [ticks, key](bootstrap::PlayerBootstrap& pb) { return pb.callout_add(kNoRoomText, kCalloutType, kCalloutPointSize, ticks, key); }, conn);
        gather_callouts_.push_back({now + kCalloutLife, key});
    }
    if (frame.after_items) frame.after_items();
    if (xp.changed() || leveled || !changes.empty()) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(changes);
        queue("XPTracker", [total, debt, wire](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
    }
    if (!changes.empty()) queue_profession_level_ups(conn, changes);
    if (leveled) queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }
    if (frame.after_experience) frame.after_experience();
    if (!overflow.empty()) {
        GatherPile pile;
        pile.due = now + kPileDelay;
        pile.stacks = std::move(overflow);
        const GoodPosition at = checker_.good();
        pile.x = at.x;
        pile.y = at.y;
        pile.z = at.z;
        pile.node_name = node_name;
        gather_piles_.push_back(std::move(pile));
    }

    // the log: what the user's test reads
    std::vector<std::string> got;
    for (const combat::RolledItem& it : harvest.items) {
        got.push_back((world_ ? world_->item_name(it.item) : std::to_string(it.item)) + " x" + std::to_string(it.quantity) + " q" + std::to_string(it.quality > 0 ? it.quality : 1));
    }
    std::string line = "harvested " + node_name + ": " + (got.empty() ? std::string("nothing") : join_names(got));
    if (!harvest.items.empty()) line += " (the first item's rarity " + std::to_string(harvest.primary_rarity) + ")";
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (pf) line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " + std::to_string(c.after.level) + ")";
    }
    if (harvest.adventuring_xp > 0) line += "; Adventuring +" + std::to_string(harvest.adventuring_xp) + " XP offered (debt " + std::to_string(xp.before.debt) + " -> " + std::to_string(xp.after.debt) + ", total " +
                                            std::to_string(xp.after.xp) + (leveled ? ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level) : std::string()) + ")";
    if (wore) line += "; the tool's durability " + std::to_string(static_cast<int>(tool_before)) + " -> " + std::to_string(static_cast<int>(tool_after));
    if (!gather_piles_.empty()) line += "; the bag is full: the rest goes to a pile";
    line += frame.log_tail;
    spdlog::log(level_for(gather_done_logged_, 40), "[{}] {}: {}", frame.tag, name(), line);
    if (!changes.empty() || xp.changed()) hand_over_progress(leveled ? "a level-up" : "a harvest");
    if (inventory_dirty_) hand_over_inventory("a harvest", /*periodic=*/false);
}

}}  // namespace aoc::worldd
