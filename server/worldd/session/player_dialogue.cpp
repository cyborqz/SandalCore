// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_dialogue.cpp
//
//  The talking window: the conversation's facts, opening it, choosing a line and closing it.
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

// -- quests: the talking window --------------------------------------------------------------------------------------------------------------------

namespace {

/// What a conversation's conditions ask of the character who talks: the bag, the level, where the player and the NPC stand.
class TalkFacts final : public dialogue::Facts {
public:
    std::vector<svc::InventoryItem> items;
    int                             level = 1;
    double                          player_x = 0, player_y = 0, npc_x = 0, npc_y = 0;
    bool                            has_npc = false;
    int64_t                         citizen_node = 0;
    bool                            citizenship_cooldown = false;   // the account renounced lately and may not join yet
    const world::Elections*         elections = nullptr;            // node citizenship, stage 2 (null: no elections)
    int64_t                         account = 0;
    const world::CityNodes*         nodes = nullptr;
    int64_t                         now_ms = 0;         // the wall clock (buy orders run out by it)
    const quest::QuestLog*          quests = nullptr;   // quests: the character's log (null: it has none)

    std::optional<int64_t> quest_count(int64_t quest, const std::string& query) const override { return quests ? quests->quest_count(quest, query) : std::nullopt; }
    std::optional<int64_t> objective_count(int64_t quest, int64_t objective, const std::string& query) const override {
        return quests ? quests->objective_count(quest, objective, query) : std::nullopt;
    }
    int64_t player_variable(const std::string& name) const override { return quests ? quests->variable(name) : 0; }
    int64_t quest_variable(int64_t quest, const std::string& name) const override { return quests ? quests->quest_variable(quest, name) : 0; }

    int64_t item_count(const std::vector<int64_t>& records, const std::vector<std::string>& /*tags*/) const override {
        if (records.empty()) return 0;   // a count by tag needs the items' tags, which the bag does not hold
        int64_t n = 0;
        for (const svc::InventoryItem& it : items) {
            if (std::find(records.begin(), records.end(), it.item_record_id) != records.end()) n += it.quantity;
        }
        return n;
    }

    std::optional<dialogue::ExprValue> call(const std::string& name, const std::vector<dialogue::ExprValue>& args) const override {
        using dialogue::ExprValue;
        constexpr int64_t kPlayer = 1, kNpc = 2;   // the objects an expression passes around
        if (name == "GetPlayer") return ExprValue::of(kPlayer);
        if (name == "GetInteractionTarget") return ExprValue::of(has_npc ? kNpc : 0);
        if (name == "GetCurrentNode") return ExprValue::of(node_at(player_x, player_y));
        if (name == "Node_GetActorCityNode") {
            if (args.size() != 1) return std::nullopt;
            return ExprValue::of(args[0].i == kNpc && has_npc ? node_at(npc_x, npc_y) : node_at(player_x, player_y));
        }
        if (name == "GetCharacterLevel" || name == "GetPlayerLevel") return ExprValue::of(level);
        if (name == "Node_IsPlayerCitizen") return ExprValue::of(citizen_node != 0 ? 1 : 0);
        if (name == "Node_IsPlayerCitizenOfNode") {
            if (args.size() != 2) return std::nullopt;
            return ExprValue::of(citizen_node != 0 && citizen_node == args[1].i ? 1 : 0);
        }
        // the world around: worldd runs no constructions or expansions yet (the elections and the buy orders are below)
        if (name == "Node_HasPlayerCitizenshipCooldown") return ExprValue::of(citizenship_cooldown ? 1 : 0);
        // the elections: the registration is the client's "prevote"
        const auto election_state = [&](int64_t node) -> int {
            const world::NodeElection* e = elections ? elections->node(node) : nullptr;
            return e ? static_cast<int>(e->state) : -1;
        };
        if (name == "Node_IsNodeMayorPrevoteActive" || name == "Node_IsNodeMayorElectionActive") {
            if (args.size() != 1) return std::nullopt;
            const int want = name == "Node_IsNodeMayorPrevoteActive" ? static_cast<int>(world::ElectionState::Registration) : static_cast<int>(world::ElectionState::Active);
            return ExprValue::of(election_state(args[0].i) == want ? 1 : 0);
        }
        if (name == "Node_IsPlayerMayorCandidate") {
            if (args.size() != 2) return std::nullopt;
            return ExprValue::of(elections && elections->is_candidate(args[1].i, account) ? 1 : 0);
        }
        if (name == "Node_IsPlayerMayorCurrentNode") return ExprValue::of(elections && account != 0 && elections->mayor_node(account) == node_at(player_x, player_y) && node_at(player_x, player_y) != 0 ? 1 : 0);
        if (name == "Node_CanPlayerBecomeCitizen") return ExprValue::of(citizen_node == 0 && !citizenship_cooldown ? 1 : 0);
        // buy orders (world/buy_orders.h): the node's orders that still run and still need units
        const auto live_orders = [&](int64_t node, int64_t commodity) {
            int64_t n = 0;
            if (const world::NodeBuyOrders* b = elections ? elections->buy_orders(node) : nullptr) {
                for (const world::NodeBuyOrder& o : b->active) n += o.expires_ms > now_ms && o.fulfilled < o.requested && (commodity == 0 || o.commodity == commodity) ? 1 : 0;
            }
            return n;
        };
        if (name == "Node_IsBuyOrderActive" || name == "Node_GetNumBuyOrders") {
            if (args.size() != 1) return std::nullopt;
            const int64_t n = live_orders(args[0].i, 0);
            return ExprValue::of(name == "Node_IsBuyOrderActive" ? (n > 0 ? 1 : 0) : n);
        }
        if (name == "Node_IsSpecificBuyOrderActive") {
            if (args.size() != 2) return std::nullopt;
            return ExprValue::of(args[1].i != 0 && live_orders(args[0].i, args[1].i) > 0 ? 1 : 0);
        }
        static const char* const kNodeActivity[] = {"Node_NumOfServiceBuildingsInState", "Node_NumOfServiceBuilding", "Node_IsExpansionInGivenState"};
        for (const char* f : kNodeActivity) {
            if (name == f) return ExprValue::of(0);
        }
        return std::nullopt;
    }

private:
    int64_t node_at(double x, double y) const {
        if (!nodes) return 0;
        const world::NodeSite site = nodes->node_at(x, y);
        const world::CityNodeInfo* n = nodes->by_rank(site.node_rank);
        return n ? n->guid : 0;
    }
};

}  // namespace

std::unique_ptr<dialogue::Facts> PlayerSession::dialogue_facts(const world::WorldActor* npc) const {
    auto f = std::make_unique<TalkFacts>();
    f->items = inventory_.items();
    f->level = progress_.level;
    const GoodPosition at = checker_.good();
    f->player_x = at.x;
    f->player_y = at.y;
    if (npc) {
        f->has_npc = true;
        f->npc_x = npc->x;
        f->npc_y = npc->y;
    }
    f->citizen_node = pb_.setup().citizen_node;
    f->citizenship_cooldown = citizenship_.node_guid == 0 && citizenship_.renounce_cooldown_until_ms > wall_ms();
    f->elections = world_ ? world_->elections() : nullptr;
    f->account = account_id_;
    f->nodes = world_ ? world_->city_nodes() : nullptr;
    f->now_ms = wall_ms();
    if (quests_on_) f->quests = &quest_log_;
    return f;
}

bool PlayerSession::open_dialogue(Connection& conn, const world::WorldActor& npc, Clock::time_point now) {
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (!book) return false;   // a content.db without the conversations: the NPC answers as it did
    if (talk_.open()) {
        // the window is open already (the client does not click through its own window): a second click on the same NPC changes nothing, on another is ignored
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a click on {} while the talking window is open: ignored", name(), npc.npc.name);
        return true;
    }
    if (!room_for(1)) return true;
    const auto facts = dialogue_facts(&npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    // quests: an NPC that offers a quest the character can take starts its talk at the node the offer names; otherwise at its root
    int64_t offered = 0;
    if (const quest::QuestBook* qb = world_->quests(); qb && quests_on_) {
        for (const content::QuestOfferRow& offer : qb->offers(npc.npc.instance_guid, npc.npc.asset_guid)) {
            if (offer.node != 0 && quest_log_.can_start(qb->quest(offer.quest), ev, /*bypass_rules=*/false) == quest::Create::Ok) {
                offered = offer.node;
                break;
            }
        }
    }
    // quests: an active Dialogue objective that names this NPC and a node of its conversation starts the talk at that node
    if (const quest::QuestBook* qb2 = world_->quests(); qb2 && quests_on_) {
        if (const int64_t entry = quest::dialogue_entry(quest_log_, *qb2, npc.npc.instance_guid)) offered = entry;
    }
    if (npc.npc.dialogue_root == 0 && offered == 0) return false;   // nothing on offer and no conversation
    const dialogue::Result r = talk_.begin(*book, ev, npc.id, npc.npc.dialogue_root, offered);
    for (const std::string& s : r.skipped) {
        if (dialogue_told_.insert("skipped " + s).second) spdlog::info("[session] {}: a conversation action is not done yet: {} (told once)", name(), s);
    }
    if (r.outcome != dialogue::Outcome::Changed) {
        if (r.silent) {   // quests: a placeholder with nothing to show whose entering did its work: the click is answered, no window opens
            spdlog::log(level_for(dialogue_logged_), "[session] {}: talks to {}: {}", name(), npc.npc.name, r.why);
            run_talk_actions(conn, r, npc, now);
            return true;
        }
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: {} has a conversation that cannot be shown ({}): it answers as before", name(), npc.npc.name, r.why);
        return false;
    }
    ++dialogue_opens_;
    const dialogue::Talk::State& s = talk_.state();
    spdlog::log(level_for(dialogue_logged_), "[session] {}: talks to {} (node {}, {} line(s) shown)", name(), npc.npc.name, s.shown_node, s.lines.size());
    run_talk_actions(conn, r, npc, now);   // quests: what entering the greeting's nodes does
    send_dialogue(conn, "greeting");
    if (r.open_buy_orders) open_buy_order_window(conn, npc, now);   // the Buy Order Manager's greeting is its window (session/player_buy_orders.cpp)
    if (r.open_contribution) open_contribution_window(conn, npc, now);   // the Construction Foreman's greeting (session/player_node_buildings.cpp)
    return true;
}

bootstrap::PlayerBootstrap::DialogueView PlayerSession::dialogue_view(bool exhaust) const {
    const dialogue::Talk::State s = talk_.state();
    bootstrap::PlayerBootstrap::DialogueView v;
    v.exhaust = exhaust;
    if (s.sub) {
        bootstrap::PlayerBootstrap::DialogueView::SubDialogue sd;
        sd.type = s.sub->type;   // a quest offer (2) or a confirmation (4)
        sd.pending_next = s.sub->pending_next;
        sd.previous_node = s.sub->previous_node;
        sd.node = s.sub->node;
        sd.quest = s.sub->quest;
        sd.objectives = s.sub->objectives;
        // the objectives the quest starts with: the ones that are awake or that a start action activates (a quest offer's preview; the hand-in panel has none)
        if (const quest::QuestBook* qb = world_ && s.sub->type == 2 ? world_->quests() : nullptr) {
            if (const content::QuestRow* def = qb->quest(s.sub->quest)) {
                for (const content::QuestObjectiveRow& o : def->objectives) {
                    bool starts = o.awake;
                    for (const content::QuestStartAction& a : def->start_actions) starts = starts || (a.action == "ActivateObjective" && a.objective == o.guid);
                    if (starts) sd.initial_objectives.push_back(o.guid);
                }
            }
        }
        v.sub = std::move(sd);
    }
    v.previous_node = s.previous_node;
    v.previous_choice_node = s.previous_choice_node;
    v.previous_choice_index = s.previous_choice_index;
    v.current_node = s.current_node;
    v.shown_node = s.shown_node;
    v.greeting = s.greeting;
    for (const dialogue::Talk::Line& l : s.lines) v.lines.push_back({l.node, l.index});
    if (s.source != 0 && registry_) {
        if (const world::WorldActor* a = registry_->find(s.source)) v.source = a->actor.guid;
    }
    return v;
}

void PlayerSession::send_dialogue(Connection& conn, const char* /*why*/, bool exhaust) {
    const bootstrap::PlayerBootstrap::DialogueView v = dialogue_view(exhaust);   // a copy: the reply is built when the connection has room for it
    queue("dialogue", [v](bootstrap::PlayerBootstrap& pb) { return pb.dialogue_update(v); }, conn);
}

void PlayerSession::end_dialogue(Connection& conn, const char* why) {
    const world::WorldActor* npc = talk_.open() && registry_ ? registry_->find(talk_.state().source) : nullptr;
    const dialogue::Result r = talk_.close(world_ ? world_->dialogue() : nullptr);
    if (r.outcome != dialogue::Outcome::Changed) return;
    ++dialogue_closes_;
    spdlog::log(level_for(dialogue_logged_), "[session] {}: the conversation ends: {}", name(), why);
    if (npc && !r.actions.empty()) run_talk_actions(conn, r, *npc, quest_last_now_);   // quests: leaving the node it showed
    send_dialogue(conn, why, /*exhaust=*/r.offer_answered);
}

bool PlayerSession::npc_offers_quests(const world::WorldActor& npc) const {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    return qb && quests_on_ && !qb->offers(npc.npc.instance_guid, npc.npc.asset_guid).empty();
}

void PlayerSession::on_dialogue_choice(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto response = repl::parse_dialogue_response(rpc.call);
    if (!response) {
        if (dialogue_told_.insert("unreadable").second) {
            spdlog::warn("[session] {}: a ServerSelectDialogueChoice does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    using repl::DialogueResponseType;
    using repl::SubDialogueResponse;
    if (response->response_type == static_cast<uint8_t>(DialogueResponseType::SubDialogue) && response->quest_response == static_cast<uint8_t>(SubDialogueResponse::Close)) {
        // the window closes: the client says it twice and then names the window it opened; the first clears, the others find nothing to clear.  (A close is not held back by the
        // budget: a lost close would leave the conversation open on the server for good.)
        end_dialogue(conn, "the client closed the window");
        return;
    }
    if (!within_budget(dialogue_budget_, "ServerSelectDialogueChoice", now)) return;
    if (response->response_type == static_cast<uint8_t>(DialogueResponseType::SubDialogue) &&
        (response->quest_response == static_cast<uint8_t>(SubDialogueResponse::Accept) || response->quest_response == static_cast<uint8_t>(SubDialogueResponse::Decline))) {
        return on_quest_offer(conn, response->quest_response, now);
    }
    if (response->response_type == static_cast<uint8_t>(DialogueResponseType::ActionWindow)) return;   // a window a line opened was closed: no window opens yet
    if (response->response_type != static_cast<uint8_t>(DialogueResponseType::Choice)) {
        if (dialogue_told_.insert("response " + std::to_string(response->response_type) + "/" + std::to_string(response->quest_response)).second) {
            spdlog::info("[session] {}: a talking-window response of type {} (quest response {}) is not answered yet (told once)", name(), response->response_type, response->quest_response);
        }
        return;
    }
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (dead_ || !book || !talk_.open() || !room_for(1)) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a talking-window choice is refused: {}", name(), dead_ ? "the player is dead" : !talk_.open() ? "no conversation is open" : "the world has no conversations");
        return;
    }
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) {
        end_dialogue(conn, "the NPC is gone");
        return;
    }
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) {
        end_dialogue(conn, "the player is too far from the NPC");
        return;
    }
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    const dialogue::Result r = talk_.choose(*book, ev, *response);
    for (const std::string& s : r.skipped) {
        if (dialogue_told_.insert("skipped " + s).second) spdlog::info("[session] {}: a conversation action is not done yet: {} (told once)", name(), s);
    }
    // storage clerks, step 0: a line that opens the warehouse ends the conversation (it clears below) and the window opens in its place
    bool warehouse_opened = false;
    if (r.open_warehouse != 0) {
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: it opens the storage (access {})", name(), response->choice_index, response->node, r.open_warehouse);
        warehouse_opened = open_warehouse(conn, *npc, r.open_warehouse, now);
    }
    // vendors, step 2: a line that opens a shop opens its window; the conversation is left as it is (the client closes its own window as the shop's opens)
    if (r.open_vendor != 0) {
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: it opens the shop {}", name(), response->choice_index, response->node, r.open_vendor);
        open_talk_shop(conn, *npc, r.open_vendor, now);
    }
    if (r.outcome == dialogue::Outcome::Changed) run_talk_actions(conn, r, *npc, now);   // quests: the line's actions, the node left and the nodes entered
    // node citizenship: the Node Administrator's lines open the join dialog or the renounce warning (session/player_citizenship.cpp)
    if (r.citizenship == 3 && r.outcome == dialogue::Outcome::Changed) {
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: the character stands for mayor", name(), response->choice_index, response->node);
        stand_for_mayor(conn, "the Sheriff's line");
    } else if (r.citizenship != 0 && r.outcome == dialogue::Outcome::Changed) {
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: it opens the {}", name(), response->choice_index, response->node,
                    r.citizenship == 1 ? "citizenship dialog" : "renounce warning");
        open_citizenship_dialog(conn, *npc, r.citizenship == 1, r.citizenship_vendor, now);
    }
    switch (r.outcome) {
        case dialogue::Outcome::Changed:
            ++dialogue_steps_;
            if (!talk_.open()) ++dialogue_closes_;
            spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: {}", name(), response->choice_index, response->node,
                        talk_.open() ? "on to node " + std::to_string(talk_.state().shown_node) : std::string("the conversation ends"));
            if (!warehouse_opened) send_dialogue(conn, "a line was chosen");   // (the warehouse's second half sends the clear after its window call)
            if (r.open_buy_orders && talk_.open()) open_buy_order_window(conn, *npc, now);   // a line that leads to the buy order panel (session/player_buy_orders.cpp)
            if (r.open_contribution) open_contribution_window(conn, *npc, now);   // the Construction Foreman's lines (session/player_node_buildings.cpp)
            break;
        case dialogue::Outcome::Nothing:
            if (r.open_vendor == 0) spdlog::log(level_for(dialogue_logged_), "[session] {}: line {} of node {} changes nothing: {}", name(), response->choice_index, response->node, r.why);
            break;
        case dialogue::Outcome::Refused:
            ++dialogue_refused_;
            spdlog::log(level_for(dialogue_logged_), "[session] {}: a talking-window choice is refused: {}", name(), r.why);
            break;
    }
    if (warehouse_opened) finish_open_warehouse(conn, now, /*clear=*/r.outcome == dialogue::Outcome::Changed);   // (retail's packet: the window call, the clear, then the lists)
}

void PlayerSession::run_dialogue(Connection& conn, Clock::time_point /*now*/) {
    if (!talk_.open()) return;
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) end_dialogue(conn, "the player walked away");
}

/// The client calls of the talking window (session/rpc_table.h).
void PlayerSession::register_dialogue_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSelectDialogueChoice", [](PlayerSession& s, const RpcContext& c) { s.on_dialogue_choice(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
