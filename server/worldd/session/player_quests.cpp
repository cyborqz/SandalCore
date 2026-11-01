// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_quests.cpp
//
//  The quest log: taking, answering, abandoning and ending quests, the game master commands and the once-a-second quest work.
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

/// ServerSetQuestBlacklisted: the Destiny window's toggle: accepted, nothing to toggle yet
void PlayerSession::on_quest_blacklisted() {
    if (quest_told_.insert("ServerSetQuestBlacklisted").second) spdlog::info("[session] {}: ServerSetQuestBlacklisted is accepted and ignored (there is no Destiny track yet; told once)", name());
    return;
}

// -- quests: take a quest ------------------------------------------------------------------------------------------------------------------------

void PlayerSession::init_quests(PlayerWorld& world, const svc::Character& character) {
    quest_worker_ = world.quest_worker();
    const quest::QuestBook* book = world.quests();
    if (!quest_worker_ || !book || !character.quest_log) {
        spdlog::info("[session] {}: quests are off for this character ({})", pb_.setup().name,
                     !book ? "this content.db has no quest tables" : !quest_worker_ ? "worldd has no quest service" : "its quest log did not load at the login");
        return;
    }
    const double now = pb_.setup().world_time;
    const size_t repaired = quest_log_.load(*character.quest_log, book->data(), now);
    quests_on_ = true;
    std::string err;
    repl::FastArrays* arrays = pb_.quest_arrays();
    const spawns::AreaReport* area = world.area_report();
    quest::RegistryPoints points(world.registry(), area ? area->data.get() : nullptr);   // (the map pins of the objectives that are active now: the client draws them from the join)
    quest::MarkerContext markers;
    markers.book = book;
    markers.points = &points;
    if (!arrays || !quest::sync(*arrays, quest_log_, &err, &markers)) {
        spdlog::warn("[session] {}: the quest log does not fit the quest component, so quests are off for this character: {}", pb_.setup().name, err);
        quests_on_ = false;
        return;
    }
    log_quest_markers(markers);
    quest_worker_->watch(character_id_);
    spdlog::info("[session] {}: quest log: {} quest(s) taken, {} finished, {} variable(s){}", pb_.setup().name, quest_log_.instances().size(), quest_log_.completed().size(),
                 quest_log_.variables().size(), repaired ? " (" + std::to_string(repaired) + " repaired or dropped: the log is saved again)" : "");
}

void PlayerSession::quest_sync(Connection& conn, const char* why) {
    quests_touched_ = true;
    givers_dirty_ = true;   // (what the NPCs around offer depends on the log)
    repl::FastArrays* arrays = pb_.quest_arrays();
    std::string err;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    std::optional<quest::RegistryPoints> points;
    quest::MarkerContext markers;
    if (qb && registry_) {   // the map pins of the objectives that became active
        const spawns::AreaReport* area = world_ ? world_->area_report() : nullptr;
        points.emplace(*registry_, area ? area->data.get() : nullptr);
        markers.book = qb;
        markers.points = &*points;
    }
    if (!arrays || !quest::sync(*arrays, quest_log_, &err, markers.points ? &markers : nullptr)) {
        spdlog::warn("[session] {}: the quest component is not brought in line with the log ({}): {}", name(), why, err);
        return;
    }
    log_quest_markers(markers);
    queue("QuestConsumerComponent", [](bootstrap::PlayerBootstrap& pb) { return pb.quest_update(); }, conn, /*must_build=*/false);
}

void PlayerSession::start_quest(Connection& conn, const content::QuestRow& def, int64_t source, const char* why, Clock::time_point now) {
    ++quests_taken_;
    spdlog::log(level_for(quests_logged_, 100), "[session] {}: took quest {} ({}), {} objective(s) ({})", name(), def.name, def.guid, def.objectives.size(), why);
    // quests: the quest engine takes the quest (all of its start actions run and the objectives settle) and the client is told in one update
    quest_engine(conn, now, "a quest was taken", [&](quest::Engine& eng) { eng.start(def, source); });
    if (!quest_log_.find(def.guid)) {   // (a world with no conversations' book has no engine: the quest joins the log as it did in the first slice)
        quest_log_.start(def, quest_clock(now), source);
        quest_sync(conn, "a quest was taken");
    }
}

void PlayerSession::on_quest_offer(Connection& conn, uint8_t response, Clock::time_point now) {
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (dead_ || !book || !qb || !talk_.open() || !talk_.sub() || !room_for(2)) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: an answer to a quest offer is refused: {}", name(), dead_ ? "the player is dead" : !talk_.open() ? "no conversation is open" : !talk_.sub() ? "no offer is open" : "the world has no quests");
        return;
    }
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return end_dialogue(conn, "the player is too far from the NPC");
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    const dialogue::Talk::Sub sub = *talk_.sub();
    bool taken = false, back = false;
    if (sub.type == 4) {   // quests: a confirmation (the path a gatewatcher's welcome asks the player to confirm): a yes goes on, a no goes back to the line's node
        taken = response == static_cast<uint8_t>(repl::SubDialogueResponse::Accept);
        back = response == static_cast<uint8_t>(repl::SubDialogueResponse::Decline);
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: the player {} the confirmation of node {}", name(), taken ? "confirmed" : "declined", sub.node);
    } else if (response == static_cast<uint8_t>(repl::SubDialogueResponse::Accept)) {
        const content::QuestRow* def = qb->quest(sub.quest);
        if (!quests_on_) {
            ++quests_refused_;
            spdlog::warn("[session] {}: the offer of quest {} is not taken: quests are off for this character (the quest service did not answer at the login)", name(), sub.quest);
        } else if (const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false); c != quest::Create::Ok) {
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: the offer of quest {} is refused: {}", name(), sub.quest, quest::create_name(c));
        } else {
            start_quest(conn, *def, npc->npc.instance_guid, "the player took the offer", now);
            taken = true;
        }
    } else {
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: the player declined the offer of quest {}", name(), sub.quest);
    }
    const dialogue::Result r = talk_.finish_sub(*book, ev, taken, back);
    run_talk_actions(conn, r, *npc, now);   // quests: what entering the node after the answer does
    if (r.outcome == dialogue::Outcome::Changed) {
        ++dialogue_steps_;
        if (!talk_.open()) ++dialogue_closes_;
        send_dialogue(conn, "a quest offer was answered", /*exhaust=*/true);
    }
}

bool PlayerSession::end_quest(Connection& conn, int64_t record, const char* kind, Clock::time_point now) {
    const std::string k = kind;
    const bool ok = k == "finish" ? quest_log_.finish(record) : quest_log_.remove(record);
    if (!ok) return false;
    if (k == "abandon") ++quests_abandoned_;
    remove_quest_items(conn, record, now);
    quest_sync(conn, kind);
    return true;
}

void PlayerSession::remove_quest_items(Connection& conn, int64_t quest, Clock::time_point /*now*/) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    items::Outcome all;
    all.ok = true;
    for (const svc::InventoryItem& it : inventory_.items()) {
        if (it.container != svc::ItemContainer::Quest) continue;   // only the quest bag is cleared here
        const auto* links = qb->item_links(it.item_record_id);
        if (!links) continue;
        bool bound = false;
        for (const content::QuestItemLink& l : *links) bound = bound || (l.quest == quest && (l.kind == "delete_all" || l.kind == "delete_one"));
        if (!bound) continue;
        items::Outcome o = inventory_.consume(it.container, it.slot, it.instance_guid, it.quantity);
        if (!o.ok) continue;
        all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (all.events.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::info("[session] {}: {} quest item event(s) left the quest bag with quest {}", name(), events.size(), quest);
}

void PlayerSession::on_abandon_quests(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const repl::Value* guids = quest_rpc_value(rpc, "QuestGuids");
    const repl::ArrayValue* list = guids ? guids->get_if<repl::ArrayValue>() : nullptr;
    if (!rpc.call.decoded || !list || !qb || !quests_on_) {
        ++quests_refused_;
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: ServerAbandonQuests is ignored: {}", name(), !quests_on_ ? "quests are off" : !rpc.call.decoded ? "its parameters do not decode" : "it names no quest");
        return;
    }
    if (list->elements.size() > 64) {
        ++quests_refused_;
        return;
    }
    for (const repl::ElementState& e : list->elements) {
        quest::Guid128 g;
        uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
        for (uint16_t i = 0; i < 4; ++i) {
            const auto it = e.values.find(static_cast<uint16_t>(i + 1));
            const auto* v = it == e.values.end() ? nullptr : it->second.get_if<int64_t>();
            *words[i] = v ? static_cast<uint32_t>(*v) : 0;
        }
        const quest::QuestInstance* q = quest_log_.find_guid(g);
        if (!q) {
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: abandon of quest instance {}: it is not in the log", name(), g.hex());
            continue;
        }
        const content::QuestRow* def = qb->quest(q->record);
        if (def && !def->can_abandon) {   // the client drops these itself; the server refuses them too
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: quest {} cannot be abandoned (its record says so)", name(), def->name);
            continue;
        }
        const int64_t record = q->record;
        end_quest(conn, record, "abandon", now);
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: abandoned quest {}", name(), def ? def->name : std::to_string(record));
    }
}

void PlayerSession::apply_quest_command(Connection& conn, const svc::QuestCommand& cmd, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb || !quests_on_) return;
    const content::QuestRow* def = cmd.quest.empty() ? nullptr : qb->find(cmd.quest);
    if (cmd.kind != "clear" && cmd.kind != "set-var" && !def) {
        spdlog::warn("[session] {}: GM command {} {}: no such quest", name(), cmd.kind, cmd.quest);
        return;
    }
    if (cmd.kind == "give") {
        const auto facts = dialogue_facts(nullptr);
        const dialogue::Book* book = world_->dialogue();
        if (!book) return;
        dialogue::Evaluator ev(*book, *facts);
        const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/true);
        if (c != quest::Create::Ok) {
            spdlog::warn("[session] {}: GM give {}: refused, {}", name(), def->name, quest::create_name(c));
            return;
        }
        start_quest(conn, *def, 0, "a GM gave it", now);
    } else if (cmd.kind == "finish" || cmd.kind == "fail" || cmd.kind == "abandon") {
        if (!end_quest(conn, def->guid, cmd.kind.c_str(), now)) spdlog::warn("[session] {}: GM {} {}: the quest is not in the log", name(), cmd.kind, def->name);
        else spdlog::info("[session] {}: GM {} {}", name(), cmd.kind, def->name);
    } else if (cmd.kind == "clear") {
        quest_log_.clear(def ? def->guid : 0);
        quests_touched_ = true;
        spdlog::info("[session] {}: GM clear {}", name(), def ? def->name : "(the finished quests and the variables)");
    } else if (cmd.kind == "set-var") {
        quest_log_.set_variable(cmd.name, cmd.value);
        quests_touched_ = true;
        spdlog::info("[session] {}: GM set-var {} = {}", name(), cmd.name, cmd.value);
    }
}

void PlayerSession::on_quest_debug_rpc(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string_view n = rpc.call.rpc->name;
    char params[160];
    std::snprintf(params, sizeof params, "%s", rpc.call.decoded ? "" : "the parameters did not decode");
    if (!gm_rpc_admitted(rpc.call.rpc->name, params, now) || !rpc.call.decoded) return;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb || !quests_on_) return;
    if (n == "ServerDebugStartQuest") {
        const int64_t id = quest_int_value(quest_rpc_value(rpc, "Guid"));
        const repl::Value* b = quest_rpc_value(rpc, "bBypassActivationRules");
        const bool bypass = b && b->get_if<bool>() && *b->get_if<bool>();
        const content::QuestRow* def = qb->quest(id);
        const dialogue::Book* book = world_->dialogue();
        if (!def || !book) {
            spdlog::warn("[gm] {}: ServerDebugStartQuest {}: no such quest", name(), id);
            return;
        }
        const auto facts = dialogue_facts(nullptr);
        dialogue::Evaluator ev(*book, *facts);
        const quest::Create c = quest_log_.can_start(def, ev, bypass);
        if (c != quest::Create::Ok) {
            spdlog::warn("[gm] {}: ServerDebugStartQuest {} ({}): refused, {}", name(), def->name, bypass ? "rules bypassed" : "rules asked", quest::create_name(c));
            return;
        }
        ++gm_rpcs_answered_;
        start_quest(conn, *def, 0, "ServerDebugStartQuest", now);
    } else if (n == "ServerDebugEndQuest") {
        const int64_t id = quest_int_value(quest_rpc_value(rpc, "Guid"));
        if (end_quest(conn, id, "finish", now)) ++gm_rpcs_answered_;
        else spdlog::warn("[gm] {}: ServerDebugEndQuest {}: the quest is not in the log", name(), id);
    } else if (n == "ServerDebugFailQuestByInstance") {
        quest::Guid128 g;
        uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
        const char* const letters[4] = {"Guid.A", "Guid.B", "Guid.C", "Guid.D"};
        for (int i = 0; i < 4; ++i) *words[i] = static_cast<uint32_t>(quest_int_value(quest_rpc_value(rpc, letters[i])));
        const quest::QuestInstance* q = quest_log_.find_guid(g);
        if (q && end_quest(conn, q->record, "fail", now)) ++gm_rpcs_answered_;
        else spdlog::warn("[gm] {}: ServerDebugFailQuestByInstance {}: the quest is not in the log", name(), g.hex());
    } else if (n == "ServerClearCompletedQuests") {
        quest_log_.clear(0);
        quests_touched_ = true;
        ++gm_rpcs_answered_;
        spdlog::info("[gm] {}: ServerClearCompletedQuests: the finished quests and the variables are forgotten", name());
    }
}

void PlayerSession::run_quests(Connection& conn, Clock::time_point now) {
    if (!quests_on_ || !quest_worker_ || dead_) return;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    // the GM commands that arrived
    for (const svc::QuestCommand& cmd : quest_worker_->take(character_id_)) apply_quest_command(conn, cmd, now);
    // a zone of influence that starts a quest for a character who stands in it (once a second): the starting areas' first quests
    if (phase_in_done_ && now - zone_checked_ >= std::chrono::seconds(1)) {
        zone_checked_ = now;
        const GoodPosition at = checker_.good();
        const int64_t zoi = static_cast<int64_t>(world::zoi_at(at.x, at.y));
        if (zoi != zoi_seen_) {
            zoi_seen_ = zoi;
            for (const int64_t q : qb->zone_quests(zoi)) {
                const content::QuestRow* def = qb->quest(q);
                const dialogue::Book* book = world_->dialogue();
                if (!def || !book) continue;
                const auto facts = dialogue_facts(nullptr);
                dialogue::Evaluator ev(*book, *facts);
                const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false);
                if (c == quest::Create::Ok) start_quest(conn, *def, 0, "the character stands in the zone that starts it", now);
                else spdlog::log(level_for(quests_logged_, 100), "[session] {}: the zone {} would start quest {}: not taken, {}", name(), zoi, def->name, quest::create_name(c));
            }
        }
    }
    // quests: the places, the items carried, the rules; the rewards that waited
    tick_quests(conn, now);
    // the save
    if (quest_log_.changed()) {
        quest_worker_->save(character_id_, quest_log_.to_saved());
        quest_log_.clear_changed();
    }
}

/// The client calls of the quest book (session/rpc_table.h).
void PlayerSession::register_quests_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerAbandonQuests", [](PlayerSession& s, const RpcContext& c) { s.on_abandon_quests(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetQuestBlacklisted", [](PlayerSession& s, const RpcContext&) { s.on_quest_blacklisted(); });
    t.add(RpcScope::PlayerController, "ServerDebugStartQuest", [](PlayerSession& s, const RpcContext& c) { s.on_quest_debug_rpc(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerDebugEndQuest", [](PlayerSession& s, const RpcContext& c) { s.on_quest_debug_rpc(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerDebugFailQuestByInstance", [](PlayerSession& s, const RpcContext& c) { s.on_quest_debug_rpc(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerClearCompletedQuests", [](PlayerSession& s, const RpcContext& c) { s.on_quest_debug_rpc(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
