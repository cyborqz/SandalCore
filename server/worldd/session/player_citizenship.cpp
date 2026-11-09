// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_citizenship.cpp
//
//  Node citizenship, stage 1: an account joins one city node at its Node Administrator, pays its dues and can renounce.  The client's side, read from its code:
//
//    * the administrator's line runs the action response AddNodeCitizenship: the server opens the join dialog, ClientShowAddNodeCitizenshipDialog(NodeGuid, CitizenshipCost,
//      CitizenshipDuesDueTime, InteractionNPCGuid, VendorDefRecordGuid).  The dialog checks CitizenshipCost against the dues currency the player holds (the TaxReceipts, the
//      TaxVoucher items: UAddNodeCitizenshipDialog::HandleCurrencyUpdated); its confirm sends ServerSendCitizenshipApplication(NodeGuid, NumDuesToFulfill)
//      (UDialogueWidget::HandleAddNodeCitizenshipConfirmed), its purchase button ServerOpenVendor(InteractionNPCGuid, VendorDefRecordGuid),
//      the town hall's shop of the vouchers;
//    * the answer is ClientCitizenshipApplicationSuccessful(NumDuesFulfilled) or ClientCitizenshipApplicationRejection(), then ClientShowCitizenshipToast(NodeGuid, false);
//      the account's node is OwnerInfo's CitizenNodeGuid and every CharactersNodeData entry;
//    * RemoveNodeCitizenship opens ClientShowCitizenshipRenouncementWarning(); its accept sends ServerRemoveNodeCitizen(NodeGuid, PlayerName) with the player's own name
//      (AAoCHUDBase::HandleCitizenshipRenouncementAccepted), answered with ClientShowCitizenshipToast(NodeGuid, true);
//    * the dues: ServerFulfillCitizenshipDues(DuesRequired) -> ClientCitizenshipDuesFulfillmentSuccessful(NumFulfilled) or ...Rejection();
//    * the window: ServerSetEssentialCitizenshipWindowData() (AAoCPlayerController::RequestCitizenshipWindowData, at most once per the window's update frequency)
//      -> ClientPopulateCitizenshipData(FEssentialCitizenshipWindowData) (protocol/repl/citizenship_codec.h).
//
//  The election's mechanics in the window: its MayorElectionStateChanged notification (Created the phase's start, Timestamp its end) is what the node window opens
//  the candidacy modal on (RegistrationPeriod) or the voting modal (ActiveElection), and the int32 after the two maps is the phase's seconds left.  The window
//  goes again to every citizen online when the phase changes or a candidate registers (ClientPopulateCitizenshipData only stores the data and updates an open
//  window), the results come with ClientFinishMayorVote, and the mayor's message comes at the login.
//
//  Modelled: the window's rank, the parts of the window data that are not read yet (sent empty), and the offer's lifetime.  Times on the wire are Unix seconds (the window compares them with the seconds since 1970
//  the client keeps at its receipt).
// ============================================================================
#include "session/player_session_util.h"

#include "items/currency.h"
#include "items/inventory.h"
#include "protocol/repl/citizenship_codec.h"
#include "world/citizenship.h"
#include "world/elections.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

/// An integer parameter of a decoded call: 0 for one left at its default (not on the wire); nullopt for an undecoded call or another type.
std::optional<int64_t> int_param(const repl::ClientRpc& rpc, std::string_view path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return std::nullopt;
}

std::optional<std::string> str_param(const repl::ClientRpc& rpc, std::string_view path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    if (!v) return std::string();
    if (const std::string* s = v->get_if<std::string>()) return *s;
    return std::nullopt;
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::pair<int64_t, int> PlayerSession::citizenship_node_at(double x, double y) const {
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!nodes) return {0, -1};
    const world::CityNodeInfo* n = nodes->by_rank(nodes->node_at(x, y).node_rank);
    if (!n) return {0, -1};
    return {n->guid, nodes->level_of(n->guid)};
}

void PlayerSession::send_citizenship_call(Connection& conn, const char* rpc, std::function<bool(const schema::RpcSchema&, repl::RpcParams&)> fill) {
    if (!room_for(1)) return;
    queue(
        rpc,
        [rpc, fill = std::move(fill)](bootstrap::PlayerBootstrap& pb) {
            return pb.citizenship_call(rpc, [&](const schema::RpcSchema& s) -> std::optional<repl::RpcParams> {
                repl::RpcParams p;
                if (!fill(s, p)) return std::nullopt;
                return p;
            });
        },
        conn, /*must_build=*/true);
}

bool PlayerSession::pay_tax_receipts(Connection& conn, int32_t cost, std::string& why) {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const items::CurrencyTier* tier = nullptr;
    if (world_ && world_->currencies() && rules) {
        for (const items::CurrencyTier& t : world_->currencies()->tiers()) {
            if (t.guid == rules->dues_tier) tier = &t;
        }
    }
    // the TaxReceipts tier has one kind, the TaxVoucher item (TaxReceiptCurrencyDef); a tier of several kinds would need change, which this does not make
    if (!tier || tier->kinds.size() != 1 || tier->kinds[0].item == 0) {
        why = "content.db has no TaxReceipts currency of one item kind";
        return false;
    }
    if (cost <= 0) return true;
    const int64_t voucher = tier->kinds[0].item;
    const int64_t held = inventory_.count_of(voucher);
    if (held < cost) {
        why = "the bags hold " + std::to_string(held) + " Tax Receipt(s) and " + std::to_string(cost) + " are asked";
        return false;
    }
    const items::Inventory kept = inventory_;
    const items::Outcome paid = inventory_.take(voucher, cost);
    if (!paid.ok) {
        inventory_ = kept;
        why = paid.refusal;
        return false;
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(paid);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    hand_over_inventory("the citizenship's Tax Receipts", /*periodic=*/false);
    return true;
}

void PlayerSession::set_citizenship(Connection& conn, const svc::Citizenship& c, const char* why) {
    citizenship_ = c;
    citizenship_changed_ = true;
    if (world_) world_->set_account_citizenship(account_id_, c);
    if (world::Elections* el = world_ ? world_->elections() : nullptr) {   // stage 2: the node's citizens (who may stand and vote; the mayor loses the office with it)
        if (c.node_guid != 0) el->set_citizen(c.node_guid, citizen_person());
        else el->remove_citizen(account_id_);
        world_->save_elections();
    }
    if (pb_.set_citizen_node(c.node_guid) && room_for(1)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn);
    hand_over_progress(why);
}

void PlayerSession::open_citizenship_dialog(Connection& conn, const world::WorldActor& npc, bool join, int64_t vendor, Clock::time_point now) {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const auto [node, level] = citizenship_node_at(npc.x, npc.y);
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        citizenship_offer_.reset();
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {}'s citizenship line: {}", name(), npc.npc.name, why);
        if (join) send_citizenship_call(conn, "ClientCitizenshipApplicationRejection", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
    };
    if (!rules) return refuse("this world has no citizenship rules (content.db)");
    if (node == 0) return refuse("the NPC stands in no node's zone of influence");
    const int64_t now_ms = wall_ms();
    if (join) {
        if (citizenship_.node_guid != 0) return refuse("the account is a citizen of node " + std::to_string(citizenship_.node_guid) + " already");
        if (citizenship_.renounce_cooldown_until_ms > now_ms) return refuse("the account renounced lately: it may join again in " + std::to_string((citizenship_.renounce_cooldown_until_ms - now_ms) / 1000) + " s");
        if (!rules->takes_citizens(level)) return refuse("node " + std::to_string(node) + " is at level " + std::to_string(level) + ", which takes no citizens");
        if (progress_.level < rules->min_level) return refuse("the character is level " + std::to_string(progress_.level) + " (" + std::to_string(rules->min_level) + " is asked)");
        citizenship_offer_ = CitizenshipOffer{npc.id, node, vendor, rules->join_cost, true, now};
        ++citizenship_dialogs_;
        const int32_t cost = rules->join_cost;
        const world::Elections* el = world_->elections();
        const int64_t anchor = el && el->dues_anchor(node) != 0 ? el->dues_anchor(node) : now_ms;   // (a node with no citizen yet: its schedule starts with this one)
        const int64_t due_s = rules->next_dues_ms(anchor, now_ms) / 1000;
        const FIntrepidNetworkGUID g = npc.actor.guid;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {} offers the citizenship of node {} for {} Tax Receipts (dues next on {} s)", name(), npc.npc.name, node, cost, due_s);
        send_citizenship_call(conn, "ClientShowAddNodeCitizenshipDialog", [node, cost, due_s, g, vendor](const schema::RpcSchema& s, repl::RpcParams& p) {
            return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(s, p, "CitizenshipCost", repl::Value::i64(cost)) &&
                   repl::set_rpc_param(s, p, "CitizenshipDuesDueTime", repl::Value::i64(due_s)) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.A", repl::Value::i64(static_cast<int32_t>(static_cast<uint32_t>(g.ObjectId)))) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.B", repl::Value::i64(static_cast<int32_t>(static_cast<uint32_t>(g.ObjectId >> 32)))) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.C", repl::Value::i64(static_cast<int32_t>(g.ServerId))) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.D", repl::Value::i64(static_cast<int32_t>(g.Randomizer))) &&
                   repl::set_rpc_param(s, p, "VendorDefRecordGuid", repl::Value::i64(vendor));
        });
        return;
    }
    if (citizenship_.node_guid != node) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {}'s renounce line: the account is not a citizen of node {}", name(), npc.npc.name, node);
        return;
    }
    citizenship_offer_ = CitizenshipOffer{npc.id, node, 0, 0, false, now};
    ++citizenship_dialogs_;
    send_citizenship_call(conn, "ClientShowCitizenshipRenouncementWarning", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
}

const world::WorldActor* PlayerSession::citizenship_offer_npc(int64_t vendor, Clock::time_point now) const {
    if (!citizenship_offer_ || !citizenship_offer_->join || vendor == 0 || citizenship_offer_->vendor != vendor || now - citizenship_offer_->at > kCitizenshipOfferFor) return nullptr;
    const world::WorldActor* npc = registry_ ? registry_->find(citizenship_offer_->npc) : nullptr;
    if (!npc) return nullptr;
    const GoodPosition at = checker_.good();
    return std::hypot(npc->x - at.x, npc->y - at.y) <= kDialogueRangeCm ? npc : nullptr;
}

void PlayerSession::on_citizenship_application(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerSendCitizenshipApplication", now)) return;
    const auto node = int_param(rpc, "NodeGuid");
    const auto dues = int_param(rpc, "NumDuesToFulfill");
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: the citizenship application is refused: {}", name(), why);
        send_citizenship_call(conn, "ClientCitizenshipApplicationRejection", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
    };
    if (!node || !dues) return refuse("its parameters do not read");
    if (dead_) return refuse("the character is dead");
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules) return refuse("this world has no citizenship rules");
    if (!citizenship_offer_ || !citizenship_offer_->join || now - citizenship_offer_->at > kCitizenshipOfferFor) return refuse("no Node Administrator offered a citizenship lately");
    if (*node != citizenship_offer_->node) return refuse("it names node " + std::to_string(*node) + ", the offer was node " + std::to_string(citizenship_offer_->node));
    const world::WorldActor* npc = registry_ ? registry_->find(citizenship_offer_->npc) : nullptr;
    const GoodPosition at = checker_.good();
    if (!npc || std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return refuse("the Node Administrator is gone or out of reach");
    const int64_t now_ms = wall_ms();
    if (citizenship_.node_guid != 0) return refuse("the account is a citizen of node " + std::to_string(citizenship_.node_guid) + " already");
    if (citizenship_.renounce_cooldown_until_ms > now_ms) return refuse("the account renounced lately");
    const auto [here, level] = citizenship_node_at(npc->x, npc->y);
    if (here != *node || !rules->takes_citizens(level)) return refuse("node " + std::to_string(*node) + " takes no citizens at its level now (" + std::to_string(level) + ")");
    if (progress_.level < rules->min_level) return refuse("the character is below level " + std::to_string(rules->min_level));
    const int32_t cost = citizenship_offer_->cost;
    if (*dues != cost) spdlog::log(level_for(citizenship_log_), "[nodes] {}: the application names {} dues, the offer asked {}: the offer's are taken", name(), *dues, cost);
    std::string why;
    if (!pay_tax_receipts(conn, cost, why)) return refuse(why);
    svc::Citizenship c;
    c.node_guid = *node;
    c.primary_character_id = character_id_;
    c.joined_ms = now_ms;
    start_dues_schedule(c.node_guid, now_ms);
    {
        const world::Elections* el = world_->elections();
        c.dues_due_ms = rules->next_dues_ms(el && el->dues_anchor(c.node_guid) != 0 ? el->dues_anchor(c.node_guid) : now_ms, now_ms);   // the node's next date
    }
    citizenship_offer_.reset();
    ++citizenship_joins_;
    spdlog::info("[nodes] {}: the account becomes a citizen of node {} ({} Tax Receipts paid; dues next due at {} ms)", name(), c.node_guid, cost, c.dues_due_ms);
    set_citizenship(conn, c, "the citizenship");
    send_citizenship_call(conn, "ClientCitizenshipApplicationSuccessful", [cost](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NumDuesFulfilled", repl::Value::i64(cost));
    });
    const int64_t joined = c.node_guid;
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [joined](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(joined)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(false));
    });
}

void PlayerSession::on_citizenship_renounce(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerRemoveNodeCitizen", now)) return;
    const auto node = int_param(rpc, "NodeGuid");
    const auto who = str_param(rpc, "PlayerName");
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerRemoveNodeCitizen is refused: {}", name(), why);
    };
    if (!node || !who) return refuse("its parameters do not read");
    // the player's own renouncement names the player; another name is the mayor's or a game master's removal of a citizen, which is not built
    if (!same_name(*who, name())) return refuse("it names " + *who + ", not the player (removing another citizen is not built)");
    if (citizenship_.node_guid == 0 || citizenship_.node_guid != *node) return refuse("the account is not a citizen of node " + std::to_string(*node));
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const int level = nodes ? nodes->level_of(*node) : -1;
    svc::Citizenship c;
    c.renounce_cooldown_until_ms = rules && rules->cooldown_ms(level) > 0 ? wall_ms() + rules->cooldown_ms(level) : 0;
    citizenship_offer_.reset();
    ++citizenship_renounces_;
    spdlog::info("[nodes] {}: the account renounces its citizenship of node {}{}", name(), *node,
                 c.renounce_cooldown_until_ms > 0 ? " (it may join again in " + std::to_string(rules->cooldown_ms(level) / 1000) + " s)" : "");
    set_citizenship(conn, c, "the citizenship's renouncement");
    const int64_t left = *node;
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [left](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(left)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(true));
    });
}

void PlayerSession::on_citizenship_dues(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerFulfillCitizenshipDues", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: the citizenship dues are refused: {}", name(), why);
        send_citizenship_call(conn, "ClientCitizenshipDuesFulfillmentRejection", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
    };
    const auto asked = int_param(rpc, "DuesRequired");
    if (!asked) return refuse("its parameters do not read");
    if (dead_) return refuse("the character is dead");
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules) return refuse("this world has no citizenship rules");
    if (citizenship_.node_guid == 0) return refuse("the account is a citizen of no node");
    if (const world::Elections* el = world_->elections(); el && el->dues_disabled(citizenship_.node_guid)) return refuse("the node's dues are switched off");
    const int64_t now_ms = wall_ms();
    if (dues_paid_ahead(now_ms)) return refuse("the dues for the node's coming date (" + std::to_string(node_dues_date(now_ms)) + " ms) are paid already");
    const int32_t owed = citizenship_dues(now_ms);
    if (*asked != owed) spdlog::log(level_for(citizenship_log_), "[nodes] {}: the client asks to pay {} dues, {} are owed: what is owed is taken", name(), *asked, owed);
    std::string why;
    if (!pay_tax_receipts(conn, owed, why)) return refuse(why);
    svc::Citizenship c = citizenship_;
    // paid for the node's coming date (an overdue payment too): the next unpaid date is the one after it
    c.dues_due_ms = node_dues_date(now_ms) + rules->period_ms();
    ++citizenship_dues_paid_;
    spdlog::info("[nodes] {}: pays {} Tax Receipt(s) of citizenship dues for node {}: next due at {} ms", name(), owed, c.node_guid, c.dues_due_ms);
    set_citizenship(conn, c, "the citizenship dues");
    send_citizenship_call(conn, "ClientCitizenshipDuesFulfillmentSuccessful", [owed](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NumFulfilled", repl::Value::i64(owed));
    });
}

void PlayerSession::run_citizenship(Connection& conn) {
    const int64_t now_ms = wall_ms();
    if (now_ms - citizenship_checked_ms_ < 5000) return;
    citizenship_checked_ms_ = now_ms;
    // another session of the account changed it (the world keeps the latest): this one follows, and leaves the saving to that one
    if (const svc::Citizenship* shared = world_ ? world_->account_citizenship(account_id_) : nullptr; shared && *shared != citizenship_) {
        citizenship_ = *shared;
        citizenship_offer_.reset();
        ++citizenship_adopted_;
        spdlog::info("[nodes] {}: the account's citizenship changed in another of its sessions: node {}", name(), citizenship_.node_guid);
        if (pb_.set_citizen_node(citizenship_.node_guid) && room_for(1)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn);
    }
    run_elections(conn, now_ms);
    // unpaid dues: at risk once overdue, lapsed after the node's time at risk
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules || citizenship_.node_guid == 0) return;
    if (const world::Elections* el = world_->elections(); el && el->dues_disabled(citizenship_.node_guid)) return;   // dues off: nothing lapses
    const int64_t lapse = rules->lapse_ms(citizenship_.node_guid, citizenship_.dues_due_ms);
    if (lapse == 0 || now_ms < lapse) return;
    const int64_t node = citizenship_.node_guid;
    ++citizenship_lapses_;
    spdlog::info("[nodes] {}: the account's citizenship of node {} lapses: its dues were due at {} ms and stayed unpaid past {} ms", name(), node, citizenship_.dues_due_ms, lapse);
    set_citizenship(conn, svc::Citizenship{}, "the citizenship's lapse");
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [node](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(true));
    });
}

repl::CitizenshipWindowData PlayerSession::citizenship_window(int64_t now_ms) const {
    repl::CitizenshipWindowData d;
    if (citizenship_.node_guid == 0) return d;   // no citizenship: the window shows the non-citizen's panel
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    repl::CitizenshipStatus& s = d.status;
    const std::string primary = upper(citizenship_.primary_character_id.empty() ? character_id_ : citizenship_.primary_character_id);
    for (const bootstrap::AccountCharacterNode& c : pb_.setup().account_characters) {
        const auto g = items::guid_of(c.guid_hex);
        if (!g) continue;
        if (c.guid_hex == primary) s.primary = {*g, c.name};
        else if (s.alternates.size() < repl::kMaxCitizenshipAlternates) s.alternates.push_back({*g, c.name});
    }
    if (s.primary.name.empty()) {   // (the account's list is not known: the player is the primary character)
        if (const auto g = items::guid_of(character_id_)) s.primary = {*g, name()};
    }
    const int64_t due = citizenship_.dues_due_ms;
    world::CitizenshipRules::DuesFactors factors;
    int rank = 1;
    s.dues_required = citizenship_dues(now_ms, &factors, &rank);
    s.dues_paid = dues_paid_ahead(now_ms) ? s.dues_required : 0;   // (the payment's rule: 0 until the coming date is paid)
    s.dues_level_impact = factors.level;        // DuesLevelImpact
    s.dues_rank_impact = factors.rank;          // DuesCitizenshipRankImpact
    s.dues_non_owner_impact = factors.non_owner;   // DuesNonPropertyOwnerImpact
    s.dues_overdue = due > 0 && now_ms >= due;
    if (rules && due > 0 && now_ms >= rules->overdue_ms(due)) s.at_risk_start = static_cast<double>(rules->overdue_ms(due)) / 1000.0;   // CitizenshipAtRiskStartTime (Unix seconds)
    s.rank = rank;                                                            // CitizenshipRank: the place among the node's citizens by when they joined
    s.player_level = progress_.level;
    s.highest_level = account_highest_level();
    s.property_state = 2;                                                     // EPropertyOwnershipState's count: neither owner nor renter (no housing)
    // the date shown: the player's unpaid one while it is ahead and not past the node's coming date, else the node's coming date (after a date passes, the next one)
    const int64_t coming = node_dues_date(now_ms);
    const int64_t shown = due > now_ms ? std::min(due, coming) : coming;
    d.dues_due_time = shown / 1000;
    d.late_fees_due_time = rules && due > 0 ? rules->overdue_ms(due > now_ms ? shown : due) / 1000 : 0;
    d.dues_state = due > 0 && now_ms >= due ? 1 : 0;                          // AwaitingOverdueDate once due, else AwaitingDueDate
    // stage 2: the node's election.  The second map is the candidates (UVotingWindow::Populate makes an item of each, its guid and name), the first the ballots by
    // the voter's character guid (the window looks the player's up by PrimaryCharacter, then AlternateCharacters, and marks the first of its guids chosen); the byte is the state.
    const world::Elections* el = world_ ? world_->elections() : nullptr;
    if (const world::NodeElection* e = el ? el->node(citizenship_.node_guid) : nullptr) {
        d.state = static_cast<uint8_t>(e->state);
        d.phase_seconds_left = static_cast<int32_t>(std::clamp<int64_t>((e->phase_ends_ms - now_ms) / 1000, 0, INT32_MAX));   // the phase's countdown
        if (e->state == world::ElectionState::Registration || e->state == world::ElectionState::Active || e->state == world::ElectionState::AwaitingResults) {
            for (const world::ElectionPerson& c : e->candidates) {
                if (const auto g = items::guid_of(c.character_id); g && d.names.size() < repl::kMaxCitizenshipEntries) d.names.emplace_back(*g, c.name);
            }
        }
        if (const auto v = e->votes.find(account_id_); v != e->votes.end()) {
            repl::CitizenshipNamedGuids ballot;
            ballot.key = s.primary.guid;
            ballot.name = s.primary.name;
            for (const std::string& id : v->second) {
                if (const auto g = items::guid_of(id)) ballot.guids.push_back(*g);
            }
            d.named_guids.push_back(std::move(ballot));
        }
        repl::NodeNotification n;   // the election's state, the notification the window shows for it
        n.type = 7;                 // ENodeSystemNotificationType::MayorElectionStateChanged: the node window opens the candidacy (Registration) or the vote (Active) on it
        n.created = e->phase_started_ms / 1000;   // the phase's start
        n.timestamp = std::max(e->phase_ends_ms, e->phase_started_ms) / 1000;   // and its end
        n.election_type = el->rules().election_type;
        n.election_state = static_cast<uint8_t>(e->state);
        d.notifications.push_back(std::move(n));
        if (e->mayor.account != 0) {
            repl::NodeNotification m;
            m.type = 6;             // MayorElected
            m.created = m.timestamp = e->mayor.at_ms / 1000;
            m.mayor = e->mayor.name;
            m.election_type = el->rules().election_type;
            m.election_state = static_cast<uint8_t>(world::ElectionState::Post);
            d.notifications.push_back(std::move(m));
        }
        for (auto it = e->news.rbegin(); it != e->news.rend() && d.notifications.size() + 1 < repl::kMaxCitizenshipNotifications; ++it) {
            repl::NodeNotification n2;   // the node's feed: taxes, commissions, buildings
            n2.type = it->type;
            n2.created = n2.timestamp = it->at_ms / 1000;
            n2.mayor = it->mayor;
            n2.plot = it->plot;
            n2.building = it->building;
            n2.building_state = it->building_state;
            n2.commission = it->commission;
            d.notifications.push_back(std::move(n2));
        }
    }
    if (citizenship_.dues_due_ms > 0 && now_ms >= citizenship_.dues_due_ms && d.notifications.size() < repl::kMaxCitizenshipNotifications) {
        repl::NodeNotification due;   // the player's own: the dues are due (ENodeSystemNotificationType::CitizenshipDuesDue)
        due.type = 11;
        due.created = due.timestamp = citizenship_.dues_due_ms / 1000;
        d.notifications.push_back(std::move(due));
    }
    return d;
}

void PlayerSession::send_citizenship_window(Connection& conn) {
    const repl::CitizenshipWindowData d = citizenship_window(wall_ms());
    ++citizenship_windows_;
    election_seen_candidates_ = d.names.size();   // (this window lists the candidates already: run_elections need not send it again for them)
    send_citizenship_call(conn, "ClientPopulateCitizenshipData", [d](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "EssentialCitizenshipData", repl::Value(d));
    });
}

void PlayerSession::on_citizenship_window_request() {
    ++citizenship_requests_;   // answered by the RPC table's entry below, which has the connection
}

// ============================================================================================================================
//  stage 2: the mayors
// ============================================================================================================================

void PlayerWorld::set_elections(world::ElectionRules rules, std::string file) {
    elections_.emplace(rules);
    elections_file_ = std::move(file);
    if (elections_file_.empty()) {
        spdlog::warn("[nodes] no --elections-file: the nodes' elections are lost at a restart");
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(elections_file_, ec)) {
        spdlog::info("[nodes] the elections are kept in {} (none yet)", elections_file_);
        return;
    }
    std::ifstream in(elections_file_, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    if (!elections_->from_json(text, error)) {
        // moved aside before anything writes the elections again (the next change, a tick of the calendar, saves them), so nothing it holds is lost
        in.close();
        const int64_t unix_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string aside = elections_file_ + ".unreadable-" + std::to_string(unix_s);
        std::error_code moved;
        std::filesystem::rename(elections_file_, aside, moved);
        if (moved) {
            spdlog::error("[nodes] the elections file {} does not read ({}) and cannot be moved aside ({}): the elections start afresh, and the file is written over at their next change",
                          elections_file_, error, moved.message());
        } else {
            spdlog::error("[nodes] the elections file {} does not read ({}): it is kept as {}, and the elections start afresh", elections_file_, error, aside);
        }
        return;
    }
    elections_saved_ = elections_->version();
    spdlog::info("[nodes] the elections of {} node(s) read from {}", elections_->all().size(), elections_file_);
}

void PlayerWorld::save_elections() const {
    if (!elections_ || elections_file_.empty() || elections_->version() == elections_saved_) return;
    const std::filesystem::path path(elections_file_), tmp(elections_file_ + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << elections_->to_json();
        if (!out) {
            spdlog::error("[nodes] the elections file {} cannot be written", tmp.string());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        spdlog::error("[nodes] the elections file {} cannot be replaced: {}", path.string(), ec.message());
        return;
    }
    elections_saved_ = elections_->version();
}

std::vector<world::Elections::Change> PlayerWorld::tick_elections(int64_t now_ms, bool force) const {
    if (!elections_ || !city_nodes_ || !citizenship_rules_) return {};
    if (!force && now_ms - elections_ticked_ms_ < 5000) return {};
    elections_ticked_ms_ = now_ms;
    std::vector<int64_t> nodes;
    for (const world::CityNodeInfo& n : city_nodes_->nodes()) nodes.push_back(n.guid);
    const world::CitizenshipRules* rules = &*citizenship_rules_;
    const world::CityNodes* registry = city_nodes_;
    auto changes = elections_->advance(now_ms, [rules, registry](int64_t node) { return rules->takes_citizens(registry->level_of(node)); }, nodes);
    for (const world::Elections::Change& c : changes) {
        const world::CityNodeInfo* n = city_nodes_->by_guid(c.node);
        spdlog::info("[nodes] the election of {} enters {}{}", n ? n->display_name : std::to_string(c.node), world::election_state_name(c.state),
                     c.results ? (c.winner.empty() ? ": no candidate, no mayor" : ": " + c.winner + " is the mayor") : "");
    }
    save_elections();
    return changes;
}

world::ElectionPerson PlayerSession::election_person() const { return world::ElectionPerson{account_id_, character_id_, name(), 0}; }

int32_t PlayerSession::citizenship_dues(int64_t now_ms, world::CitizenshipRules::DuesFactors* factors, int* rank) const {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules || citizenship_.node_guid == 0) return 1;
    const world::Elections* el = world_->elections();
    if (el && el->dues_disabled(citizenship_.node_guid)) return 0;   // a game master switched the node's dues off
    const int r = el ? std::max(1, el->citizen_rank(citizenship_.node_guid, account_id_)) : 1;
    const int node_level = world_->city_nodes() ? world_->city_nodes()->level_of(citizenship_.node_guid) : 0;
    const world::CitizenshipRules::DuesFactors f = rules->dues_factors(r, account_highest_level(), node_level);
    if (factors) *factors = f;
    if (rank) *rank = r;
    return rules->dues_owed(citizenship_.dues_due_ms, now_ms, rules->dues_amount(f), node_level);
}

int64_t PlayerSession::node_dues_date(int64_t now_ms) const {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules || citizenship_.node_guid == 0) return 0;
    const world::Elections* el = world_->elections();
    int64_t anchor = el ? el->dues_anchor(citizenship_.node_guid) : 0;
    if (anchor == 0) anchor = citizenship_.joined_ms > 0 ? citizenship_.joined_ms : now_ms;
    return rules->next_dues_ms(anchor, now_ms);
}

void PlayerSession::start_dues_schedule(int64_t node, int64_t at_ms) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || node == 0 || el->dues_anchor(node) != 0) return;
    el->set_dues_anchor(node, at_ms);
    world_->save_elections();
    spdlog::info("[nodes] the dues schedule of node {} starts at {} ms", node, at_ms);
}

bool PlayerSession::dues_paid_ahead(int64_t now_ms) const {
    if (citizenship_.node_guid == 0) return false;
    if (const world::Elections* el = world_ ? world_->elections() : nullptr; el && el->dues_disabled(citizenship_.node_guid)) return false;
    return citizenship_.dues_due_ms > node_dues_date(now_ms);
}

int PlayerSession::account_highest_level() const {
    int best = progress_.level;
    const std::string own = upper(character_id_);
    for (const bootstrap::AccountCharacterNode& c : pb_.setup().account_characters) {
        if (c.guid_hex != own) best = std::max<int>(best, c.level);   // (this character's own is its live level)
    }
    return best;
}

world::ElectionPerson PlayerSession::citizen_person() const {
    world::ElectionPerson p = election_person();
    p.at_ms = citizenship_.joined_ms;   // the rank among the node's citizens is by when they joined
    if (!citizenship_.primary_character_id.empty() && citizenship_.primary_character_id != character_id_) {
        p.character_id = citizenship_.primary_character_id;
        const std::string want = upper(citizenship_.primary_character_id);
        for (const bootstrap::AccountCharacterNode& c : pb_.setup().account_characters) {
            if (c.guid_hex == want) p.name = c.name;
        }
    }
    return p;
}

void PlayerSession::run_elections(Connection& conn, int64_t now_ms) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el) return;
    if (citizenship_.node_guid != 0) {   // (a citizen from before the elections, or one that joined on another worldd: the store learns of it)
        const world::NodeElection* e = el->node(citizenship_.node_guid);
        if (!e || e->citizens.count(account_id_) == 0) {
            el->set_citizen(citizenship_.node_guid, citizen_person());
            world_->save_elections();
        }
        // a node with citizens from before its dues schedule: it starts at this citizen's date, so that date is one of the node's
        if (el->dues_anchor(citizenship_.node_guid) == 0) start_dues_schedule(citizenship_.node_guid, citizenship_.dues_due_ms > 0 ? citizenship_.dues_due_ms : now_ms);
    }
    world_->tick_elections(now_ms);
    if (el->version() == election_seen_version_) return;
    election_seen_version_ = el->version();
    const int64_t mayor = el->mayor_node(account_id_);
    if (mayor != mayor_node_) {
        mayor_node_ = mayor;
        spdlog::info("[nodes] {}: {}", name(), mayor != 0 ? "the account is the mayor of node " + std::to_string(mayor) : std::string("the account is no node's mayor"));
        if (pb_.set_mayor_node(mayor) && room_for(1)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn);
    }
    const world::NodeElection* e = citizenship_.node_guid != 0 ? el->node(citizenship_.node_guid) : nullptr;
    const int state = e ? static_cast<int>(e->state) : -1;
    const size_t candidates = e ? e->candidates.size() : 0;
    // the login's first look: the mayor's message goes to the node's citizens
    if (e && election_seen_state_ < 0 && e->mayor.account != 0 && !e->mayor_message.empty()) {
        const std::string mayor = e->mayor.name, text = e->mayor_message;
        send_citizenship_call(conn, "ClientMayorMessageCallout", [mayor, text](const schema::RpcSchema& s, repl::RpcParams& p) {
            return repl::set_rpc_param(s, p, "MayorName", repl::Value::str(mayor)) && repl::set_rpc_param(s, p, "Message", repl::Value::str(text));
        });
    }
    // a phase change or a new candidate while this session watched: the window again, so its notification opens the candidacy or the vote as it stands now
    if (e && election_seen_state_ >= 0 && (state != election_seen_state_ || candidates != election_seen_candidates_)) {
        ++election_windows_pushed_;
        send_citizenship_window(conn);
    }
    election_seen_candidates_ = candidates;
    // the results: the node's election came to its term while this session watched (not at its first look: a login after the results is not told them again)
    if (e && state == static_cast<int>(world::ElectionState::Post) && election_seen_state_ >= 0 && election_seen_state_ != static_cast<int>(world::ElectionState::Post)) {
        ++mayor_results_seen_;
        const std::string winner = e->mayor.name;
        send_citizenship_call(conn, "ClientFinishMayorVote", [winner](const schema::RpcSchema& s, repl::RpcParams& p) { return repl::set_rpc_param(s, p, "WinnerName", repl::Value::str(winner)); });
    }
    election_seen_state_ = state;
}

void PlayerSession::stand_for_mayor(Connection& conn, const char* by) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why;
    if (!el) why = "this world holds no elections";
    else if (citizenship_.node_guid == 0) why = "the account is a citizen of no node";
    else if (el->register_candidate(citizenship_.node_guid, election_person(), wall_ms(), why)) {
        ++mayor_candidacies_;
        world_->save_elections();
        spdlog::info("[nodes] {}: stands for mayor of node {} ({})", name(), citizenship_.node_guid, by);
        send_citizenship_window(conn);
        return;
    }
    ++citizenship_refused_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: does not stand for mayor ({}): {}", name(), by, why);
}

void PlayerSession::on_mayor_candidate(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerAddNodeMayorCandidate", now)) return;
    stand_for_mayor(conn, "ServerAddNodeMayorCandidate");
}

void PlayerSession::on_mayor_ranked_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerRankedChoiceVoteForMayor", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::vector<std::string> ranked;
    if (rpc.call.decoded) {
        if (const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "RankedCandidateGuids")) {
            if (const auto* arr = v->get_if<repl::ArrayValue>()) {
                for (const repl::ElementState& e : arr->elements) {
                    repl::GuidValue g;
                    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
                    for (uint16_t i = 0; i < 4; ++i) {
                        const auto it = e.values.find(static_cast<uint16_t>(i + 1));
                        const int64_t* n = it == e.values.end() ? nullptr : it->second.get_if<int64_t>();
                        if (n) *words[i] = static_cast<uint32_t>(*n);
                    }
                    ranked.push_back(items::guid_hex(g));
                }
            }
        }
    }
    std::string why;
    if (!el) why = "this world holds no elections";
    else if (citizenship_.node_guid == 0) why = "the account is a citizen of no node";
    else if (el->vote(citizenship_.node_guid, election_person(), ranked, wall_ms(), why)) {
        ++mayor_votes_;
        world_->save_elections();
        spdlog::info("[nodes] {}: votes in the election of node {} ({} candidate(s) ranked)", name(), citizenship_.node_guid, ranked.size());
        send_citizenship_window(conn);
        return;
    }
    ++citizenship_refused_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the vote is refused: {}", name(), why);
}

void PlayerSession::on_mayor_named_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerRegisterMayorVote", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const auto node = int_param(rpc, "NodeGuid");
    const auto chosen = str_param(rpc, "SelectedName");
    std::string why;
    const world::NodeElection* e = el && node ? el->node(*node) : nullptr;
    std::vector<std::string> ranked;
    if (e && chosen) {
        for (const world::ElectionPerson& c : e->candidates) {
            if (same_name(c.name, *chosen)) ranked.push_back(c.character_id);
        }
    }
    if (!el) why = "this world holds no elections";
    else if (!node || !chosen) why = "its parameters do not read";
    else if (*node != citizenship_.node_guid) why = "the account is not a citizen of node " + std::to_string(*node);
    else if (el->vote(*node, election_person(), ranked, wall_ms(), why)) {
        ++mayor_votes_;
        world_->save_elections();
        spdlog::info("[nodes] {}: votes for {} in the election of node {}", name(), *chosen, *node);
        send_citizenship_window(conn);
        return;
    }
    ++citizenship_refused_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the vote is refused: {}", name(), why);
}

void PlayerSession::on_mayor_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerSetMayorMessage", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const auto message = str_param(rpc, "Message");
    std::string why;
    const int64_t node = el ? el->mayor_node(account_id_) : 0;
    if (!el || !message) why = "no elections, or the message does not read";
    else if (el->set_message(node, account_id_, *message, why)) {
        world_->save_elections();
        const std::string mayor = name(), text = *message;
        send_citizenship_call(conn, "ClientMayorMessageCallout", [mayor, text](const schema::RpcSchema& s, repl::RpcParams& p) {
            return repl::set_rpc_param(s, p, "MayorName", repl::Value::str(mayor)) && repl::set_rpc_param(s, p, "Message", repl::Value::str(text));
        });
        return;
    }
    ++citizenship_refused_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the mayor's message is refused: {}", name(), why);
    if (el && node == 0) send_must_be_mayor(conn);
}

void PlayerSession::on_mayor_citizens(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerShowMayorCitizens", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const int64_t node = el ? el->mayor_node(account_id_) : 0;
    const world::NodeElection* e = node ? el->node(node) : nullptr;
    if (!e) {
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerShowMayorCitizens is not answered: the account is no node's mayor", name());
        if (el) send_must_be_mayor(conn);
        return;
    }
    (void)rpc;
    const world::CityNodeInfo* info = world_->city_nodes() ? world_->city_nodes()->by_guid(node) : nullptr;
    const std::string description = info ? info->display_name : std::string();
    std::vector<std::string> names;
    for (const auto& [account, p] : e->citizens) {
        if (names.size() < 1000) names.push_back(p.name);
    }
    send_citizenship_call(conn, "ClientShowMayorCitizens", [description, names](const schema::RpcSchema& s, repl::RpcParams& p) {
        repl::ArrayValue list;
        for (const std::string& n : names) {
            repl::ElementState el;
            el.values[1] = repl::Value::str(n);
            list.elements.push_back(std::move(el));
        }
        return repl::set_rpc_param(s, p, "NodeDescription", repl::Value::str(description)) && repl::set_rpc_param(s, p, "Citizens", repl::Value(std::move(list)));
    });
}

void PlayerSession::on_gm_set_election(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_param(rpc, "NodeName").value_or("");
    const int64_t state = int_param(rpc, "ElectionState").value_or(-1), secs = int_param(rpc, "DurationInSeconds").value_or(0);
    char params[192];
    std::snprintf(params, sizeof params, "NodeName=\"%.100s\" ElectionState=%lld DurationInSeconds=%lld%s", node_name.c_str(), static_cast<long long>(state),
                  static_cast<long long>(secs), rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerGMSetElectionStateAndDuration", params, now) || !rpc.call.decoded) return;
    const std::string said = gm_set_election(conn, node_name, static_cast<int>(state), secs);
    spdlog::info("[gm] {}: ServerGMSetElectionStateAndDuration: {}", name(), said);
}

std::string PlayerSession::gm_set_election(Connection& conn, const std::string& node_name, int state, int64_t duration_secs) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el) return "this world holds no elections";
    if (state < 0 || state > static_cast<int>(world::ElectionState::Post)) return "no such election state (" + std::to_string(state) + ")";
    if (duration_secs < 0) return "a negative duration";
    const world::CityNodes* nodes = world_->city_nodes();
    int64_t node = 0;
    if (!node_name.empty()) {
        const world::CityNodeInfo* named = nodes ? nodes->by_name(node_name) : nullptr;
        if (!named) return "no node is named \"" + node_name + "\"";
        node = named->guid;
    } else {
        const GoodPosition at = checker_.good();
        node = citizenship_node_at(at.x, at.y).first;
        if (node == 0) return "no node name, and the player stands in no node";
    }
    const world::CityNodeInfo* info = nodes ? nodes->by_guid(node) : nullptr;
    const std::string shown = info ? info->display_name : std::to_string(node);
    world_->tick_elections(wall_ms(), /*force=*/true);
    const world::Elections::Change c = el->set_state(node, static_cast<world::ElectionState>(state), wall_ms(), duration_secs);
    world_->save_elections();
    run_elections(conn, wall_ms());
    return "the election of " + shown + " is now in " + world::election_state_name(c.state) +
           (c.results ? (c.winner.empty() ? ": no mayor" : ": " + c.winner + " is the mayor") : "");
}

/// The client calls of node citizenship (session/rpc_table.h).
void PlayerSession::register_citizenship_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSetEssentialCitizenshipWindowData", [](PlayerSession& s, const RpcContext& c) {
        s.on_citizenship_window_request();
        if (s.within_budget(s.citizenship_budget_, "ServerSetEssentialCitizenshipWindowData", c.now)) s.send_citizenship_window(c.conn);
    });
    t.add(RpcScope::PlayerController, "ServerSendCitizenshipApplication", [](PlayerSession& s, const RpcContext& c) { s.on_citizenship_application(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRemoveNodeCitizen", [](PlayerSession& s, const RpcContext& c) { s.on_citizenship_renounce(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerFulfillCitizenshipDues", [](PlayerSession& s, const RpcContext& c) { s.on_citizenship_dues(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerAddNodeMayorCandidate", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_candidate(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRankedChoiceVoteForMayor", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_ranked_vote(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRegisterMayorVote", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_named_vote(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetMayorMessage", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_message(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerShowMayorCitizens", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_citizens(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMSetElectionStateAndDuration", [](PlayerSession& s, const RpcContext& c) { s.on_gm_set_election(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
