// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/area_session_citizenship_tests.inc  (listed in tests/area_session_registered.inc)
//
//  Node citizenship through a session (session/player_citizenship.cpp): the Node Administrator's line opens the join dialog for its node with the cost, the dues' date
//  and the shop of the tax receipts; an application for another node, or before any offer, is refused with nothing paid; the application takes the cost in the
//  dues currency, makes the account a citizen and saves it with the progress; the window's data names the player as the primary character; a second offer is
//  refused; dues paid ahead are refused, overdue dues cost the late fee too and move the date on; a renouncement in another's name is refused, one in the player's
//  own ends the citizenship.  The world's content.db here has no city nodes or currencies: the test gives the world one node over the zone of influence where the
//  player stands, and a tax receipt currency carried as the character's potions.
// ============================================================================
#include "items/currency.h"
#include "protocol/repl/citizenship_codec.h"
#include "world/citizenship.h"
#include "world/elections.h"
#include "world/node_control.h"
#include "world/node_reputation.h"
#include "world/zoi_surface.h"

namespace {

constexpr int64_t kTestCitizenNode = 7770001;
constexpr int64_t kTestReceiptTier = 7770002;
constexpr int64_t kTestReceiptDef = 7770003;
constexpr int64_t kTestReceiptShop = 7770004;

struct CitizenHarness : Harness {
    uint64_t admin = 0;
    std::unique_ptr<world::CityNodes> nodes;
    bool zoned = false;

    static svc::Character citizen_record(const std::function<void(svc::Character&)>& tweak) {
        svc::Character c = cyb_record();
        c.level = 12;
        if (tweak) tweak(c);
        return c;
    }
    explicit CitizenHarness(const std::function<void(svc::Character&)>& tweak = {}) : Harness(citizen_record(tweak)) {
        // one node over the zone of influence at the player's spot, at Town (4)
        const uint64_t zoi = world::zoi_at(kCx, kCy);
        zoned = zoi != 0;
        world::CityNodeInfo n;
        n.rank = 0;
        n.guid = kTestCitizenNode;
        n.name = "Verra_Test_Node";
        n.display_name = "Testhold";
        n.x = kCx;
        n.y = kCy;
        n.zoi_guid = static_cast<int64_t>(zoi);
        world::CityZoneInfo z;
        z.guid = static_cast<int64_t>(zoi);
        z.name = "ZOI_Test";
        z.node_guid = kTestCitizenNode;
        z.node_rank = 0;
        nodes = std::make_unique<world::CityNodes>(std::vector<world::CityNodeInfo>{n}, std::vector<world::CityZoneInfo>{z});
        nodes->apply_change(world::LiveNode{kTestCitizenNode, 4, 0, 0});
        fx.world->set_city_nodes(nodes.get());
        world::CitizenshipRules rules;
        rules.dues_tier = kTestReceiptTier;
        rules.join_cost = 1;   // (the join dialog's CitizenshipCost)
        rules.dues_cost = 1;
        rules.late_fee = 1;
        rules.level_threshold = 1000;   // (the flow's dues stay at 1: the level impact of the data's formula is tested in test_worldd_elections)
        rules.cooldown_secs = {{3, 0}, {4, 3600}};
        fx.world->set_citizenship_rules(rules);
        content::CurrencyData data;
        data.tiers.push_back({kTestReceiptTier, "TaxReceipts", 1000000});
        data.defs.push_back({kTestReceiptDef, "TaxReceiptCurrencyDef", kTestReceiptTier, 0, fi::kPotion, 20});
        data.tiers.push_back({world::kNodeReputationTier, "Reputation", 0});   // (node reputation: the data's tier, no item)
        data.defs.push_back({world::kNodeReputationCurrency, "NodeToPlayerReputationCurrency", world::kNodeReputationTier, 0, 0, 0});
        std::string why;
        if (auto table = items::CurrencyTable::from(data, why)) fx.world->set_currencies_for_test(std::move(*table));
        else std::printf("  [FAIL] the test's currency does not build: %s\n", why.c_str());
        const spawns::OpenTemplate* t = fx.npc_template();
        if (!t) {
            std::printf("  [FAIL] no NPC template\n");
            std::exit(2);
        }
        admin = fx.add(*t, kCx + 300, kCy, false, 0, "Node Administrator");
        start();
    }
    const world::WorldActor& npc() const { return *fx.world->registry().find(admin); }
    template <class F>
    void pc_call(const char* rpc, F fill) {
        const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
        const schema::RpcSchema* r = pc.rpc(rpc);
        if (!r) {
            std::printf("  [FAIL] no RPC %s\n", rpc);
            return;
        }
        repl::RpcParams p;
        fill(*r, p);
        client_request(actor_rpc(pc, rpc, p), pc_ch);
    }
    void apply(int64_t node, int64_t dues) {
        pc_call("ServerSendCitizenshipApplication", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(node));
            repl::set_rpc_param(r, p, "NumDuesToFulfill", repl::Value::i64(dues));
        });
    }
};

int64_t call_int(const repl::RpcCall& c, const char* path) { return repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0).value_or(-1); }

/// The last call named `name` among `calls` (null for none).
const repl::RpcCall* last_call(const std::vector<repl::RpcCall>& calls, const char* name) {
    const repl::RpcCall* found = nullptr;
    for (const repl::RpcCall& c : calls) {
        if (std::string(c.rpc->name) == name) found = &c;
    }
    return found;
}

const repl::CitizenshipWindowData* window_of(const repl::RpcCall* c) {
    if (!c) return nullptr;
    const repl::Value* v = repl::rpcf::value_of(*c->rpc, c->params, "EssentialCitizenshipData");
    return v ? v->get_if<repl::CitizenshipWindowData>() : nullptr;
}

void test_citizenship_flow() {
    std::printf("node citizenship through a session\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");   // (the store that keeps the node's dues schedule)
    CHECK(h.zoned, "the player stands in a zone of influence (the test's node is over it)");
    CHECK(h.s->inventory().count_of(fi::kPotion) == 5, "the character carries 5 potions, the test's tax receipts");

    // the window before joining: no citizenship
    size_t from = h.got.size();
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* before = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    CHECK(before && before->status.primary.name.empty() && before->dues_due_time == 0, "the window's data is answered: no citizenship yet");

    // an application before any offer: refused, nothing paid
    from = h.got.size();
    h.apply(kTestCitizenNode, 3);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientCitizenshipApplicationRejection") == 1 && h.s->citizenship().node_guid == 0 && h.s->inventory().count_of(fi::kPotion) == 5,
          "an application no administrator offered is refused, with nothing paid");

    // the administrator's line: the join dialog
    from = h.got.size();
    h.s->open_citizenship_dialog_for_test(h.conn, h.npc(), /*join=*/true, kTestReceiptShop, h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* dialog = last_call(calls, "ClientShowAddNodeCitizenshipDialog");
    CHECK(dialog && call_int(*dialog, "NodeGuid") == kTestCitizenNode && call_int(*dialog, "CitizenshipCost") == 1 && call_int(*dialog, "VendorDefRecordGuid") == kTestReceiptShop,
          "the line opens the join dialog: the node, a cost of 1 and the shop of the tax receipts");
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(dialog && call_int(*dialog, "CitizenshipDuesDueTime") >= now_s + 14 * 86400 && call_int(*dialog, "CitizenshipDuesDueTime") < now_s + 21 * 86400,
          "... with the first dues due on a due day two to three weeks away (Unix seconds)");

    // an application for another node: refused
    from = h.got.size();
    h.apply(kTestCitizenNode + 1, 3);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientCitizenshipApplicationRejection") == 1 && h.s->citizenship().node_guid == 0, "an application for another node is refused");

    // the application
    const size_t saves = h.sink.progress.size();
    from = h.got.size();
    h.apply(kTestCitizenNode, 1);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* ok = last_call(calls, "ClientCitizenshipApplicationSuccessful");
    const repl::RpcCall* toast = last_call(calls, "ClientShowCitizenshipToast");
    CHECK(ok && call_int(*ok, "NumDuesFulfilled") == 1, "the application succeeds: 1 due fulfilled (CitizenshipCost 1)");
    CHECK(toast && call_int(*toast, "NodeGuid") == kTestCitizenNode, "... with the citizenship toast for the node");
    CHECK(count_calls(calls, "ClientReceiveStorageEvent") >= 1 && h.s->inventory().count_of(fi::kPotion) == 4, "the tax receipt leaves the bags (5 -> 4), told as storage events");
    CHECK(h.s->citizenship().node_guid == kTestCitizenNode && h.s->citizenship().primary_character_id == kCyb && h.s->citizenship().joined_ms > 0 &&
              h.s->citizenship().dues_due_ms > h.s->citizenship().joined_ms,
          "the account is a citizen of the node, the player its primary character, with dues due later");
    bool saved = false;
    for (size_t i = saves; i < h.sink.progress.size(); ++i) saved = saved || (h.sink.progress[i].replace_citizenship && h.sink.progress[i].citizenship.node_guid == kTestCitizenNode);
    CHECK(saved, "the citizenship is saved with the progress (replace_citizenship)");
    const svc::Citizenship* shared = h.fx.world->account_citizenship(h.record.account_id);
    CHECK(shared && shared->node_guid == kTestCitizenNode, "the world keeps the account's citizenship for its other logins");

    // the window: the player as the primary character
    from = h.got.size();
    h.t += milliseconds(500);   // (the request budget)
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* after = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    CHECK(after && after->status.primary.name == "cyb" && after->status.dues_required == 1 && after->status.player_level == 12 && after->dues_state == 0 &&
              after->dues_due_time == h.s->citizenship().dues_due_ms / 1000 && after->status.property_state == 2,
          "the window names the player as the primary character, 1 Tax Receipt of dues, awaiting the due date");
    const world::Elections* dues_el = h.fx.world->elections();
    CHECK(dues_el && dues_el->dues_anchor(kTestCitizenNode) == h.s->citizenship().joined_ms &&
              h.s->citizenship().dues_due_ms == h.s->citizenship().joined_ms + 14LL * 86400000,
          "the node's first citizen starts its dues schedule: the first date a period after the join");
    CHECK(after && after->status.dues_paid == 0, "a new citizen's dues for the coming date are unpaid: DuesPaid 0");

    // a second offer: the account is a citizen already
    from = h.got.size();
    h.s->open_citizenship_dialog_for_test(h.conn, h.npc(), true, kTestReceiptShop, h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientShowAddNodeCitizenshipDialog") == 0 && count_calls(calls, "ClientCitizenshipApplicationRejection") == 1, "a second offer is refused: a citizen already");

    // the dues, one rule for the window and the payment: unpaid, they are taken; paid, the window shows them paid and a payment is refused
    const int64_t coming = h.s->citizenship().dues_due_ms;
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerFulfillCitizenshipDues", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "DuesRequired", repl::Value::i64(1)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(last_call(calls, "ClientCitizenshipDuesFulfillmentSuccessful") && h.s->inventory().count_of(fi::kPotion) == 3 &&
              h.s->citizenship().dues_due_ms == coming + 14LL * 86400000,
          "the unpaid dues of the coming date are taken (4 -> 3); the next unpaid date is the one after it");
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    const repl::CitizenshipWindowData* paid_window = window_of(last_call(pc_calls_since(h, from), "ClientPopulateCitizenshipData"));
    CHECK(paid_window && paid_window->status.dues_paid == 1 && paid_window->dues_due_time == coming / 1000,
          "... the window shows them paid, and still the node's coming date");
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerFulfillCitizenshipDues", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "DuesRequired", repl::Value::i64(1)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientCitizenshipDuesFulfillmentRejection") == 1 && h.s->inventory().count_of(fi::kPotion) == 3, "paid dues are not paid again");
    svc::Citizenship overdue = h.s->citizenship();
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    overdue.dues_due_ms = now_ms - 10LL * 86400000;   // ten days ago: past the 5 overdue days
    h.s->set_citizenship_for_test(overdue);
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerFulfillCitizenshipDues", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "DuesRequired", repl::Value::i64(2)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* paid = last_call(calls, "ClientCitizenshipDuesFulfillmentSuccessful");
    CHECK(paid && call_int(*paid, "NumFulfilled") == 2 && h.s->inventory().count_of(fi::kPotion) == 1, "overdue dues cost the dues and the late fee: 2 Tax Receipts");
    CHECK(h.s->citizenship().dues_due_ms > now_ms && (h.s->citizenship().dues_due_ms - dues_el->dues_anchor(kTestCitizenNode)) % (14LL * 86400000) == 0,
          "... and the next unpaid date is after now, one of the node's");

    // the renouncement: another's name is refused; the player's own ends it
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerRemoveNodeCitizen", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlayerName", repl::Value::str("SomebodyElse"));
    });
    h.run(milliseconds(300));
    CHECK(h.s->citizenship().node_guid == kTestCitizenNode && h.s->citizenship_renounces() == 0, "a removal in another player's name is refused");
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerRemoveNodeCitizen", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlayerName", repl::Value::str("Cyb"));
    });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* left = last_call(calls, "ClientShowCitizenshipToast");
    CHECK(left && call_int(*left, "NodeGuid") == kTestCitizenNode && h.s->citizenship().node_guid == 0 && h.s->citizenship_renounces() == 1,
          "the player's own renouncement (the name in any case) ends the citizenship, with the toast");
    CHECK(h.s->citizenship().renounce_cooldown_until_ms > now_ms, "... and the account waits the node level's cooldown before it may join again");
    from = h.got.size();
    h.s->open_citizenship_dialog_for_test(h.conn, h.npc(), true, kTestReceiptShop, h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientShowAddNodeCitizenshipDialog") == 0, "no join dialog during the cooldown");

    // another session of the account changes it: this one follows, without saving it again
    svc::Citizenship other;
    other.node_guid = kTestCitizenNode;
    other.primary_character_id = "0123456789abcdef0123456789abcdef";
    other.joined_ms = now_ms;
    other.dues_due_ms = now_ms - 20LL * 86400000;   // twenty days ago: past the 5 overdue days and the 7 at risk
    h.fx.world->set_account_citizenship(h.record.account_id, other);
    h.s->set_citizenship_for_test(svc::Citizenship{});
    // (the lapse is judged right after the adoption: first check the adoption with dues that are fine)
    svc::Citizenship fine = other;
    fine.dues_due_ms = now_ms + 86400000;
    h.fx.world->set_account_citizenship(h.record.account_id, fine);
    h.s->run_citizenship_for_test(h.conn);
    CHECK(h.s->citizenship() == fine && h.s->citizenship_adopted() == 1, "a change another session of the account made is followed");
    // dues unpaid far too long: the citizenship lapses
    h.fx.world->set_account_citizenship(h.record.account_id, other);
    from = h.got.size();
    h.s->run_citizenship_for_test(h.conn);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(h.s->citizenship_lapses() == 1 && h.s->citizenship().node_guid == 0 && count_calls(calls, "ClientShowCitizenshipToast") == 1,
          "dues unpaid past the overdue date and the time at risk: the citizenship lapses, with the toast");
    const svc::Citizenship* after_lapse = h.fx.world->account_citizenship(h.record.account_id);
    CHECK(after_lapse && after_lapse->node_guid == 0, "... for the whole account");
    world::CitizenshipRules r;
    CHECK(r.lapse_ms(1, 1000) == 1000 + 5LL * 86400000 + 7LL * 86400000 && r.lapse_ms(1, 0) == 0, "the lapse: the due date, 5 overdue days, 7 at risk; none without a due date");
}

/// Stage 2: a citizen stands, votes, is elected mayor (OwnerInfo.MayorNode, ClientFinishMayorVote), sets the mayor's message, lists the citizens, and loses the office with the
/// citizenship; the window's data carries the state, the candidates and the player's ballot.
void test_mayor_flow() {
    std::printf("node citizenship, stage 2: the mayor's election through a session\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    h.s->set_citizenship_for_test(svc::Citizenship{kTestCitizenNode, kCyb, now_ms, now_ms + 14LL * 86400000, 0});
    h.s->run_citizenship_for_test(h.conn);
    const world::NodeElection* e = el ? el->node(kTestCitizenNode) : nullptr;
    CHECK(e && (e->state == world::ElectionState::AwaitingFirst || (e->state == world::ElectionState::Registration && e->first)) && e->citizens.count(h.record.account_id) == 1,
          "the citizen's node starts its first election with its first citizen, the account among its citizens");
    el->skip(kTestCitizenNode, now_ms);
    // standing
    size_t from = h.got.size();
    h.pc_call("ServerAddNodeMayorCandidate", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* w = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    const auto own = items::guid_of(kCyb);
    CHECK(h.s->mayor_candidacies() == 1 && el->is_candidate(kTestCitizenNode, h.record.account_id), "the citizen stands in the registration period");
    CHECK(w && w->state == 1 && w->names.size() == 1 && own && w->names[0].first == *own && w->names[0].second == "cyb", "the window lists the candidate, its guid and name, in RegistrationPeriod");
    CHECK(w && !w->notifications.empty() && w->notifications[0].type == 7 && w->notifications[0].election_state == 1, "... with the election's notification");
    // voting
    el->skip(kTestCitizenNode, now_ms);
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerRankedChoiceVoteForMayor", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::ArrayValue list;
        repl::ElementState g;
        g.values[1] = repl::Value::i64(static_cast<int32_t>(own->a));
        g.values[2] = repl::Value::i64(static_cast<int32_t>(own->b));
        g.values[3] = repl::Value::i64(static_cast<int32_t>(own->c));
        g.values[4] = repl::Value::i64(static_cast<int32_t>(own->d));
        list.elements.push_back(std::move(g));
        repl::set_rpc_param(r, p, "RankedCandidateGuids", repl::Value(std::move(list)));
    });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    w = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    CHECK(h.s->mayor_votes() == 1 && el->node(kTestCitizenNode)->votes.count(h.record.account_id) == 1, "the citizen's ranked vote is taken in the election");
    CHECK(w && w->state == 2 && w->named_guids.size() == 1 && w->named_guids[0].key == *own && w->named_guids[0].guids.size() == 1 && w->named_guids[0].guids[0] == *own,
          "the window carries the player's ballot under the player's guid (the voting window marks it chosen)");
    // the results
    el->skip(kTestCitizenNode, now_ms);
    el->skip(kTestCitizenNode, now_ms);
    from = h.got.size();
    h.s->run_citizenship_for_test(h.conn);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* finish = last_call(calls, "ClientFinishMayorVote");
    const repl::Value* winner = finish ? repl::rpcf::value_of(*finish->rpc, finish->params, "WinnerName") : nullptr;
    CHECK(winner && winner->get_if<std::string>() && *winner->get_if<std::string>() == "cyb" && h.s->mayor_results_seen() == 1, "the results reach the citizen: ClientFinishMayorVote names the winner");
    CHECK(h.s->mayor_node() == kTestCitizenNode, "the winner's OwnerInfo.MayorNode is the node");
    // the mayor's tools
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerSetMayorMessage", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "Message", repl::Value::str("Welcome to Testhold")); });
    h.pc_call("ServerShowMayorCitizens", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(el->node(kTestCitizenNode)->mayor_message == "Welcome to Testhold" && count_calls(calls, "ClientMayorMessageCallout") == 1, "the mayor sets the node's message");
    const repl::RpcCall* citizens = last_call(calls, "ClientShowMayorCitizens");
    const repl::Value* list = citizens ? repl::rpcf::value_of(*citizens->rpc, citizens->params, "Citizens") : nullptr;
    CHECK(list && list->get_if<repl::ArrayValue>() && list->get_if<repl::ArrayValue>()->elements.size() == 1, "the mayor's list of citizens: one");
    // the game master's console: ServerGMSetElectionStateAndDuration, answered only with --gm-commands (off here)
    h.t += milliseconds(500);
    h.pc_call("ServerGMSetElectionStateAndDuration", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", repl::Value::str("Testhold"));
        repl::set_rpc_param(r, p, "ElectionState", repl::Value::u64(1));
        repl::set_rpc_param(r, p, "DurationInSeconds", repl::Value::i64(60));
    });
    h.run(milliseconds(300));
    CHECK(el->node(kTestCitizenNode)->state == world::ElectionState::Post, "the console's election call is not answered without --gm-commands");
    CHECK(h.s->gm_set_election_for_test(h.conn, "Nowhere", 1, 60).find("no node") != std::string::npos, "a game master's change must name a node there is");
    CHECK(h.s->gm_set_election_for_test(h.conn, "Testhold", 9, 60).find("no such") != std::string::npos, "... and a state there is");
    const std::string said = h.s->gm_set_election_for_test(h.conn, "Testhold", 1, 60);
    const world::NodeElection* moved = el->node(kTestCitizenNode);
    CHECK(said.find("RegistrationPeriod") != std::string::npos && moved->state == world::ElectionState::Registration &&
              moved->phase_ends_ms - moved->phase_started_ms == 60000 && moved->mayor.account == h.record.account_id,
          "a game master moves the node's election to the registration for 60 s; the mayor keeps the office until the next results");
    // the office goes with the citizenship
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerRemoveNodeCitizen", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlayerName", repl::Value::str("cyb"));
    });
    h.run(milliseconds(300));
    h.s->run_citizenship_for_test(h.conn);
    CHECK(el->node(kTestCitizenNode)->mayor.account == 0 && h.s->mayor_node() == 0, "renouncing the citizenship gives up the office");
}

/// The game master's console calls of citizenship and the elections (session/player_citizenship_gm.cpp), on the game master's own account; and the mayor's tools
/// refusing a player who is no mayor with ClientShowMustBeMayorCallout.
void test_citizenship_gm() {
    std::printf("node citizenship: the game master's console\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    const auto str = [](const char* v) { return repl::Value::str(v); };
    const auto add_citizen = [&](const char* node) {
        h.t += milliseconds(500);
        h.pc_call("ServerGMAddNodeCitizen", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "NodeName", str(node));
            repl::set_rpc_param(r, p, "PlayerName", str(""));
        });
        h.run(milliseconds(300));
    };
    // without GM mode the calls do nothing
    add_citizen("Testhold");
    CHECK(h.s->citizenship().node_guid == 0, "a game master's call does nothing without --gm-commands");
    h.fx.world->set_gm_commands(true);
    const size_t potions = h.s->inventory().count_of(fi::kPotion);
    add_citizen("Nowhere");
    CHECK(h.s->citizenship().node_guid == 0, "a node name that names no node does nothing");
    add_citizen("Testhold");
    CHECK(h.s->citizenship().node_guid == kTestCitizenNode && h.s->inventory().count_of(fi::kPotion) == potions && h.s->citizenship().dues_due_ms > 0,
          "ServerGMAddNodeCitizen makes the account a citizen of the named node, for nothing, its dues due a period on");
    h.s->run_citizenship_for_test(h.conn);
    // the dues: off and on again, a date from now
    h.t += milliseconds(500);
    h.pc_call("ServerGMDisableCitizenshipDues", [&](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "NodeName", str("Testhold")); });
    h.run(milliseconds(300));
    size_t from = h.got.size();
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* w = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    CHECK(el->dues_disabled(kTestCitizenNode) && w && w->status.dues_required == 0, "ServerGMDisableCitizenshipDues: the node's dues are off, the window asks for none");
    h.t += milliseconds(500);
    h.pc_call("ServerGMTryEnableCitizenshipDues", [&](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "NodeName", str("Testhold")); });
    h.run(milliseconds(300));
    CHECK(!el->dues_disabled(kTestCitizenNode), "ServerGMTryEnableCitizenshipDues: on again");
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    h.t += milliseconds(500);
    h.pc_call("ServerGMSetCitizenshipDuesDueFromNow", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "SecondsDueFromNow", repl::Value::i64(60));
    });
    h.run(milliseconds(300));
    const int64_t due = h.s->citizenship().dues_due_ms;
    CHECK(due >= now_ms + 55000 && due <= now_ms + 70000, "ServerGMSetCitizenshipDuesDueFromNow: the dues are due in 60 s");
    // the election: a candidate, a ballot, the mayor, the message
    el->skip(kTestCitizenNode, now_ms);   // (the first registration)
    if (el->node(kTestCitizenNode)->state != world::ElectionState::Registration) el->skip(kTestCitizenNode, now_ms);
    const auto candidate = [&](bool adding) {
        h.t += milliseconds(500);
        h.pc_call("ServerGMSetMayorCandidate", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "NodeName", str("Testhold"));
            repl::set_rpc_param(r, p, "CandidateName", str("CYB"));
            repl::set_rpc_param(r, p, "bAdding", repl::Value::boolean(adding));
        });
        h.run(milliseconds(300));
    };
    candidate(true);
    CHECK(el->is_candidate(kTestCitizenNode, h.record.account_id), "ServerGMSetMayorCandidate adds the citizen named (any case)");
    candidate(false);
    CHECK(!el->is_candidate(kTestCitizenNode, h.record.account_id), "... and takes it off again");
    candidate(true);
    el->skip(kTestCitizenNode, now_ms);   // (voting)
    h.t += milliseconds(500);
    h.pc_call("ServerGMRankedChoiceVoteForMayor", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", str("Testhold"));
        repl::ArrayValue names;
        repl::ElementState e;
        e.values[1] = str("cyb");
        names.elements.push_back(std::move(e));
        repl::set_rpc_param(r, p, "RankedCandidateNames", repl::Value(std::move(names)));
    });
    h.run(milliseconds(300));
    CHECK(el->node(kTestCitizenNode)->state == world::ElectionState::Active && el->node(kTestCitizenNode)->votes.count(h.record.account_id) == 1,
          "ServerGMRankedChoiceVoteForMayor casts the game master's ballot by the candidates' names");
    // not yet the mayor: the mayor's tools refuse with the callout
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerShowMayorCitizens", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientShowMustBeMayorCallout") == 1 && count_calls(calls, "ClientShowMayorCitizens") == 0, "a player who is no mayor gets ClientShowMustBeMayorCallout");
    h.t += milliseconds(500);
    h.pc_call("ServerGMSetMayor", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", str("Testhold"));
        repl::set_rpc_param(r, p, "CharacterName", str("cyb"));
    });
    h.run(milliseconds(300));
    CHECK(el->mayor_node(h.record.account_id) == kTestCitizenNode, "ServerGMSetMayor makes the citizen named the node's mayor");
    h.t += milliseconds(500);
    h.pc_call("ServerGMSetMayorMessage", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "Message", str("Set by a game master"));
    });
    h.run(milliseconds(300));
    CHECK(el->node(kTestCitizenNode)->mayor_message == "Set by a game master", "ServerGMSetMayorMessage sets the mayor's message");
    h.t += milliseconds(500);
    h.pc_call("ServerGMSetMayor", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", str("Testhold"));
        repl::set_rpc_param(r, p, "CharacterName", str(""));
    });
    h.run(milliseconds(300));
    CHECK(el->node(kTestCitizenNode)->mayor.account == 0, "ServerGMSetMayor with no name leaves the node without a mayor");
    // the citizenship ends, and the cooldown
    h.t += milliseconds(500);
    h.pc_call("ServerGMRemoveNodeCitizen", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", str("Testhold"));
        repl::set_rpc_param(r, p, "PlayerName", str("cyb"));
    });
    h.run(milliseconds(300));
    CHECK(h.s->citizenship().node_guid == 0 && h.s->citizenship().renounce_cooldown_until_ms == 0 && el->node(kTestCitizenNode)->citizens.count(h.record.account_id) == 0,
          "ServerGMRemoveNodeCitizen ends the citizenship, with no cooldown");
    h.s->set_citizenship_for_test(svc::Citizenship{0, "", 0, 0, now_ms + 3600000});
    h.t += milliseconds(500);
    h.pc_call("ServerGMEndCitizenshipRenounceCooldown", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    CHECK(h.s->citizenship().renounce_cooldown_until_ms == 0, "ServerGMEndCitizenshipRenounceCooldown ends the renounce cooldown");
}

/// The mayor's taxes through a session (session/player_node_taxes.cpp): a rate within the node's limits is taken and the table goes back with it, one outside them is
/// refused, a player who is no mayor gets the callout; the mayor's view of the treasury.
void test_node_taxes() {
    std::printf("node citizenship: the mayor's taxes and the node's treasury\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    world::CitizenshipRules rules = *h.fx.world->citizenship_rules();
    constexpr int64_t kTable = 7770099;
    rules.node_tax_table[kTestCitizenNode] = kTable;
    rules.tax_limits[kTable]["NodeTax"] = world::TaxLimit{0.5f, 0.1f, 1.0f, 0, 0, 0};
    h.fx.world->set_citizenship_rules(rules);
    const auto send_rate = [&](float percent) {
        h.t += milliseconds(500);
        h.pc_call("ServerSetTaxOverride", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::NodeTaxDetails d;
            d.node = kTestCitizenNode;
            repl::NodeTaxEntry e;
            e.tag = "NodeTax.ArtisanTax";
            e.tax_percent = percent;
            d.entries.push_back(e);
            repl::set_rpc_param(r, p, "TaxUpdates", repl::Value(std::move(d)));
        });
        h.run(milliseconds(300));
    };
    const auto table_rate = [&](const std::vector<repl::RpcCall>& calls) -> float {
        const repl::RpcCall* c = last_call(calls, "ClientSetNodeTaxTable");
        const repl::Value* v = c ? repl::rpcf::value_of(*c->rpc, c->params, "NodeTaxDetails") : nullptr;
        const repl::NodeTaxDetails* d = v ? v->get_if<repl::NodeTaxDetails>() : nullptr;
        if (!d) return -1.0f;
        for (const repl::NodeTaxEntry& e : d->entries) {
            if (e.tag == "NodeTax.ArtisanTax") return e.flag_a && e.flag_b ? e.tax_percent : -2.0f;
        }
        return -3.0f;
    };
    // no mayor yet: the callout
    size_t from = h.got.size();
    send_rate(0.25f);
    auto calls = pc_calls_since(h, from);
    CHECK(!el->taxes(kTestCitizenNode) && count_calls(calls, "ClientShowMustBeMayorCallout") == 1 && h.s->tax_overrides_refused() == 1,
          "a player who is no mayor of the node sets no tax: ClientShowMustBeMayorCallout");
    // the mayor
    el->set_citizen(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1});
    el->set_mayor(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1}, 1);
    from = h.got.size();
    send_rate(0.25f);
    calls = pc_calls_since(h, from);
    CHECK(el->taxes(kTestCitizenNode) && el->taxes(kTestCitizenNode)->at("NodeTax.ArtisanTax").percent == 0.25f && h.s->tax_overrides_taken() == 1,
          "the mayor's rate within the node's limits (10% to 100%) is taken");
    CHECK(table_rate(calls) == 0.25f, "... and the node's table goes back with it, marked as changed");
    h.s->set_citizenship_for_test(svc::Citizenship{kTestCitizenNode, kCyb, 1, 2, 0});   // (a citizen of the node, its dues long due)
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* w = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    bool taxes_news = false, dues_news = false;
    for (const repl::NodeNotification& n : w ? w->notifications : std::vector<repl::NodeNotification>{}) {
        taxes_news = taxes_news || (n.type == 10 && n.mayor == "cyb");
        dues_news = dues_news || n.type == 11;
    }
    CHECK(taxes_news, "the citizenship window's feed tells of the mayor's new taxes (Taxes, by the mayor)");
    CHECK(dues_news, "... and of the player's dues being due (CitizenshipDuesDue)");
    from = h.got.size();
    send_rate(0.05f);
    calls = pc_calls_since(h, from);
    CHECK(el->taxes(kTestCitizenNode)->at("NodeTax.ArtisanTax").percent == 0.25f && h.s->tax_overrides_refused() == 2 && table_rate(calls) == 0.25f,
          "a rate below the node's minimum is refused; the table keeps the rate before");
    // the treasury
    el->add_treasury(kTestCitizenNode, 1234);
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerShowMayorInventory", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* inv = last_call(calls, "ClientShowMayorInventory");
    const repl::Value* gold = inv ? repl::rpcf::value_of(*inv->rpc, inv->params, "InventoryGold") : nullptr;
    const repl::Value* desc = inv ? repl::rpcf::value_of(*inv->rpc, inv->params, "NodeDescription") : nullptr;
    CHECK(gold && gold->get_if<int64_t>() && *gold->get_if<int64_t>() == 1234 && desc && desc->get_if<std::string>() && *desc->get_if<std::string>() == "Testhold",
          "the mayor's view of the treasury: the node's name and its 1234 copper");
    // a game master's rate past the limits, and gold for the treasury
    h.fx.world->set_gm_commands(true);
    h.t += milliseconds(500);
    h.pc_call("ServerGMSetTaxOverride", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", repl::Value::str("Testhold"));
        repl::set_rpc_param(r, p, "TaxTag", repl::Value::str("NodeTax.GoodsTax"));
        repl::set_rpc_param(r, p, "NewTaxPercent", repl::Value::f32(0.02f));
        repl::set_rpc_param(r, p, "NewFlatTaxAmount", repl::Value::i64(3));
    });
    h.t += milliseconds(500);
    h.pc_call("ServerGMAddNodeGold", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "Amount", repl::Value::i64(66));
    });
    h.run(milliseconds(300));
    const auto* taxes = el->taxes(kTestCitizenNode);
    CHECK(taxes && taxes->count("NodeTax.GoodsTax") && taxes->at("NodeTax.GoodsTax").percent == 0.02f && taxes->at("NodeTax.GoodsTax").flat == 3,
          "ServerGMSetTaxOverride sets a rate past the node's limits");
    CHECK(el->treasury(kTestCitizenNode) == 1234 + 66, "ServerGMAddNodeGold adds to the treasury");
}

/// What the client sees of an election a game master moves on with the console's ServerGMSetElectionStateAndDuration: at every phase the citizen's window comes again unasked (the node window opens
/// the candidacy or the vote from its MayorElectionStateChanged notification), with the phase's start and end and its countdown; the results with ClientFinishMayorVote;
/// the mayor's message at the next login.
void test_election_in_game() {
    std::printf("node citizenship: an election a game master moves on, as the client sees it\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    h.fx.world->set_gm_commands(true);
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    h.s->set_citizenship_for_test(svc::Citizenship{kTestCitizenNode, kCyb, now_ms, now_ms + 14LL * 86400000, 0});
    h.s->run_citizenship_for_test(h.conn);
    // the window as the client gets it unasked, and its election notification
    const auto pushed = [&](size_t from) -> std::optional<repl::CitizenshipWindowData> {   // (a copy: the calls are a temporary list)
        const auto calls = pc_calls_since(h, from);
        const repl::CitizenshipWindowData* w = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
        return w ? std::optional<repl::CitizenshipWindowData>(*w) : std::nullopt;
    };
    const auto phase_ok = [&](const std::optional<repl::CitizenshipWindowData>& w, uint8_t state) {
        const world::NodeElection* e = el->node(kTestCitizenNode);
        if (!w || !e || w->state != state || w->notifications.empty()) return false;
        const repl::NodeNotification& n = w->notifications[0];
        const int64_t left = (e->phase_ends_ms - now_ms) / 1000;
        return n.type == 7 && n.election_state == state && n.created == e->phase_started_ms / 1000 && n.timestamp == e->phase_ends_ms / 1000 &&
               std::abs(w->phase_seconds_left - left) <= 120;
    };
    const auto next = [&](world::ElectionState to) {   // the console's call, as the client sends it
        h.t += milliseconds(500);
        h.pc_call("ServerGMSetElectionStateAndDuration", [to](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "NodeName", repl::Value::str("Testhold"));
            repl::set_rpc_param(r, p, "ElectionState", repl::Value::u64(static_cast<uint64_t>(to)));
            repl::set_rpc_param(r, p, "DurationInSeconds", repl::Value::i64(0));
        });
        h.run(milliseconds(300));
    };
    // the first registration: the citizen stands (the window answers the candidacy)
    if (el->node(kTestCitizenNode)->state == world::ElectionState::AwaitingFirst) next(world::ElectionState::Registration);
    CHECK(el->node(kTestCitizenNode)->state == world::ElectionState::Registration, "the citizen's node is in its first registration");
    size_t from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerAddNodeMayorCandidate", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    CHECK(phase_ok(pushed(from), 1) && count_calls(pc_calls_since(h, from), "ClientPopulateCitizenshipData") == 1,
          "RegistrationPeriod: one window with the notification the candidacy modal opens on, the phase's start, end and countdown");
    // the console moves it to the vote: the vote opens on every citizen's client
    const uint32_t pushes = h.s->election_windows_pushed();
    from = h.got.size();
    next(world::ElectionState::Active);
    CHECK(el->node(kTestCitizenNode)->state == world::ElectionState::Active && h.s->election_windows_pushed() == pushes + 1,
          "the console's ActiveElection: the window goes to the citizen");
    auto w = pushed(from);
    CHECK(phase_ok(w, 2), "... its notification in ActiveElection (the voting modal's), the phase's start and end, the countdown to its end");
    CHECK(w && w->names.size() == 1 && w->names[0].second == "cyb", "... with the candidates the voting modal lists");
    // the vote, then the results
    const auto own = items::guid_of(kCyb);
    h.t += milliseconds(500);
    h.pc_call("ServerRankedChoiceVoteForMayor", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::ArrayValue list;
        repl::ElementState g;
        g.values[1] = repl::Value::i64(static_cast<int32_t>(own->a));
        g.values[2] = repl::Value::i64(static_cast<int32_t>(own->b));
        g.values[3] = repl::Value::i64(static_cast<int32_t>(own->c));
        g.values[4] = repl::Value::i64(static_cast<int32_t>(own->d));
        list.elements.push_back(std::move(g));
        repl::set_rpc_param(r, p, "RankedCandidateGuids", repl::Value(std::move(list)));
    });
    h.run(milliseconds(300));
    from = h.got.size();
    next(world::ElectionState::AwaitingResults);
    CHECK(phase_ok(pushed(from), 3), "the console's AwaitingElectionResults reaches the client");
    from = h.got.size();
    next(world::ElectionState::Post);
    auto calls = pc_calls_since(h, from);
    const repl::RpcCall* finish = last_call(calls, "ClientFinishMayorVote");
    const repl::Value* winner = finish ? repl::rpcf::value_of(*finish->rpc, finish->params, "WinnerName") : nullptr;
    w = pushed(from);
    CHECK(winner && winner->get_if<std::string>() && *winner->get_if<std::string>() == "cyb" && h.s->mayor_node() == kTestCitizenNode,
          "the console's PostElection: the results, ClientFinishMayorVote naming the mayor, OwnerInfo.MayorNode");
    bool elected = false;
    if (w) {
        for (const repl::NodeNotification& n : w->notifications) elected = elected || (n.type == 6 && n.mayor == "cyb");
    }
    CHECK(phase_ok(w, 4) && elected, "... and the window in PostElection with the MayorElected notification");
    // the mayor's message: at the next login the citizen is told it
    h.t += milliseconds(500);
    h.pc_call("ServerSetMayorMessage", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "Message", repl::Value::str("Taxes are due")); });
    h.run(milliseconds(300));
    from = h.got.size();
    h.s->relogin_elections_for_test();
    h.s->run_citizenship_for_test(h.conn);
    h.run(milliseconds(300));
    const auto login_calls = pc_calls_since(h, from);
    const repl::RpcCall* callout = last_call(login_calls, "ClientMayorMessageCallout");
    const repl::Value* text = callout ? repl::rpcf::value_of(*callout->rpc, callout->params, "Message") : nullptr;
    CHECK(text && text->get_if<std::string>() && *text->get_if<std::string>() == "Taxes are due", "a citizen's login brings the mayor's message");
    CHECK(count_calls(login_calls, "ClientFinishMayorVote") == 0, "... and not the results again");
}

}  // namespace

/// An elections file that does not read is moved aside before anything writes the elections again (session/player_citizenship.cpp PlayerWorld::set_elections).
void test_elections_unreadable_file() {
    std::printf("node citizenship: an elections file that does not read\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    const std::filesystem::path bad =
        std::filesystem::temp_directory_path() / ("aoc_elections_unreadable_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    const std::string text = "{ this is not the file's format";
    {
        std::ofstream out(bad, std::ios::binary | std::ios::trunc);
        out << text;
    }
    h.fx.world->set_elections(world::ElectionRules{}, bad.string());
    h.fx.world->elections()->set_citizen(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1});
    h.fx.world->save_elections();
    std::string aside_text;
    std::vector<std::filesystem::path> asides;
    for (const auto& e : std::filesystem::directory_iterator(bad.parent_path())) {
        if (e.path().filename().string().rfind(bad.filename().string() + ".unreadable-", 0) == 0) asides.push_back(e.path());
    }
    if (asides.size() == 1) {
        std::ifstream in(asides[0], std::ios::binary);
        aside_text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::string now_text;
    {
        std::ifstream in(bad, std::ios::binary);
        now_text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    world::Elections fresh{world::ElectionRules{}};
    std::string err;
    const bool reads = fresh.from_json(now_text, err);
    std::error_code ec;
    for (const auto& a : asides) std::filesystem::remove(a, ec);
    std::filesystem::remove(bad, ec);
    CHECK(asides.size() == 1 && aside_text == text, "an elections file that does not read is moved aside with what it held (FILE.unreadable-<time>)");
    CHECK(reads && fresh.citizen_count(kTestCitizenNode) == 1, "... and the next change writes a new file at the old name");
}

/// The node's stores through a session (session/player_node_inventory.cpp): the start the world gives the data's nodes, ClientSendNodeInventoryData, and the mayor's
/// ClientShowMayorInventory listing the goods.
void test_node_inventory() {
    std::printf("node citizenship: the node's stores\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    constexpr int64_t kHalcyon = 108646610851452ll;
    CHECK(el->treasury(kHalcyon) == 1525050 && el->stored(kHalcyon) && el->stored(kHalcyon)->size() == 3 && el->node(kHalcyon) && el->node(kHalcyon)->stocked,
          "the world gives each node what its record starts it with (Halcyon: 1,525,050 copper and three kinds of goods)");
    el->change_node(kTestCitizenNode, [](world::NodeElection& e) {
        e.stored[77] = 5;
        e.treasury = 900;
        return true;
    });
    size_t at = h.got.size();
    h.s->send_node_inventory_for_test(h.conn, kTestCitizenNode);
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, at);
    const repl::RpcCall* c = last_call(calls, "ClientSendNodeInventoryData");
    const repl::Value* data = c ? repl::rpcf::value_of(*c->rpc, c->params, "Data") : nullptr;
    const repl::NodeInventoryData* d = data ? data->get_if<repl::NodeInventoryData>() : nullptr;
    CHECK(d && d->node == kTestCitizenNode && d->items.size() == 1 && d->items[0].first == 77 && d->items[0].second == 5 && d->money == 900,
          "ClientSendNodeInventoryData: the node, its goods and its money");
    // the mayor's view of the node lists its goods
    el->set_citizen(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1});
    el->set_mayor(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1}, 1);
    at = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerShowMayorInventory", [&](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, at);
    c = last_call(calls, "ClientShowMayorInventory");
    const repl::Value* items = c ? repl::rpcf::value_of(*c->rpc, c->params, "InventoryItems") : nullptr;
    const repl::ArrayValue* list = items ? items->get_if<repl::ArrayValue>() : nullptr;
    const repl::Value* first = list && list->elements.size() == 1 && list->elements[0].values.count(1) ? &list->elements[0].values.at(1) : nullptr;
    const repl::ItemInstance* item = first ? first->get_if<repl::ItemInstance>() : nullptr;
    const repl::Value* gold = c ? repl::rpcf::value_of(*c->rpc, c->params, "InventoryGold") : nullptr;
    CHECK(item && item->has_item && item->record_id == 77 && item->quantity == 5 && gold && *gold->get_if<int64_t>() == 900,
          "ClientShowMayorInventory lists the node's goods as items, with its gold");
}

/// The node's buy orders through a session (session/player_buy_orders.cpp): the mayor places one (ServerAddBuyOrder: NodeCost from the treasury, the client's ContributionLimit
/// of 300 brought into the recipe's range), the window's data (ClientUpdateBuyOrderData), and a delivery at the Buy Order Manager (ServerFulfillActiveBuyOrder) that takes the
/// named stack, counts the units, fills the node's stores and answers ClientBuyOrderFulfilled; a unit over the character's limit is refused with nothing taken.
void test_buy_orders_session() {
    std::printf("node citizenship: the buy orders\n");
    namespace bo = world::buy_orders;
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    el->set_citizen(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1});
    el->set_mayor(kTestCitizenNode, world::ElectionPerson{h.record.account_id, kCyb, "cyb", 1}, 1);
    el->change_node(kTestCitizenNode, [](world::NodeElection& e) {
        e.treasury = 100000;
        return true;
    });
    h.s->set_buy_order_slots_for_test(2);
    const bo::Recipe* r = nullptr;
    for (const bo::Recipe& x : bo::kRecipes) {
        if (std::string(x.name) == "Commodity_RVR_Spirits_T1_1") r = &x;
    }
    const bo::Commodity* commodity = r ? bo::commodity(r->commodity) : nullptr;
    CHECK(r && commodity && commodity->item != 0, "(the data's Commodity_RVR_Spirits_T1_1 and the item its commodity makes)");
    if (!r || !commodity) return;
    const int64_t m0 = bo::kOptions[bo::kRequirements[r->first].first], m1 = bo::kOptions[bo::kRequirements[r->first + 1].first];

    // the mayor places an order
    size_t at = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerAddBuyOrder", [&](const schema::RpcSchema& rs, repl::RpcParams& p) {
        repl::ArrayValue mats;
        for (const int64_t m : {m0, m1}) {
            repl::ElementState e;
            e.values[1] = repl::Value::i64(m);
            e.values[2] = repl::Value::u64(10);
            mats.elements.push_back(std::move(e));
        }
        // (a struct parameter goes whole: every member is set)
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.TimeCreated", repl::Value::i64(0));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.RewardId", repl::Value::i64(r->reward_table));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.NumTimesLimitIncreased", repl::Value::u64(0));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.VisiblyFulfilled", repl::Value::i64(0));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.ExpirationTimeInSeconds", repl::Value::i64(0));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.CommodityId", repl::Value::i64(r->commodity));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.CommodityRecipeId", repl::Value::i64(r->recipe));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.Materials", repl::Value(mats));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.ContributionLimit", repl::Value::i64(300));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.Requested", repl::Value::i64(100));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.NodeCost", repl::Value::i64(20000));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.RewardQuantity", repl::Value::i64(1));
        repl::set_rpc_param(rs, p, "RequestedBuyOrder.TimeRemainingSeconds", repl::Value::i64(3 * 86400));
    });
    h.run(milliseconds(300));
    const world::NodeBuyOrders* orders = el->buy_orders(kTestCitizenNode);
    CHECK(h.s->buy_orders_added() == 1 && orders && orders->active.size() == 1 && el->treasury(kTestCitizenNode) == 80000 && orders->active[0].contribution_limit == 100 &&
              orders->active[0].requested == 100 && orders->active[0].materials.size() == 2,
          "the mayor's ServerAddBuyOrder places the order: 20,000 copper from the treasury, the client's ContributionLimit of 300 brought to the recipe's 100");
    auto calls = pc_calls_since(h, at);
    const repl::RpcCall* c = last_call(calls, "ClientUpdateBuyOrderData");
    const repl::Value* dv = c ? repl::rpcf::value_of(*c->rpc, c->params, "BuyOrderData") : nullptr;
    const repl::ClientBuyOrderData* d = dv ? dv->get_if<repl::ClientBuyOrderData>() : nullptr;
    CHECK(d && d->active.size() == 1 && d->active[0].second.recipe == r->recipe && d->active[0].second.requested == 100 && d->max_slots == 2 && !d->possible.empty(),
          "ClientUpdateBuyOrderData: the running order, the node's slots and, for the node's mayor, what it may order");
    CHECK(el->node(kTestCitizenNode) && !el->node(kTestCitizenNode)->news.empty() && el->node(kTestCitizenNode)->news.back().type == 13, "the node's feed: NewBuyOrder");
    at = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerGetPossibleAndActiveBuyOrders", [&](const schema::RpcSchema& rs, repl::RpcParams& p) { repl::set_rpc_param(rs, p, "NodeGuid", repl::Value::i64(kTestCitizenNode)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, at);
    CHECK(last_call(calls, "ClientUpdateBuyOrderData") != nullptr, "ServerGetPossibleAndActiveBuyOrders is answered with the node's data");

    // a delivery: an order that takes the character's potions, one a unit, two units a character a day
    const world::MayorGuid g{1, 2, 3, 4};
    const int64_t wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    el->change_node(kTestCitizenNode, [&](world::NodeElection& e) {
        world::NodeBuyOrder o;
        o.guid = g;
        o.commodity = r->commodity;
        o.recipe = r->recipe;
        o.created_ms = wall;
        o.expires_ms = wall + 86400000;
        o.materials = {{fi::kPotion, 1}};
        o.contribution_limit = 2;
        o.requested = 3;
        e.buy_orders.active.push_back(o);
        return true;
    });
    std::optional<svc::InventoryItem> potions;
    for (const svc::InventoryItem& it : h.s->inventory().items()) {
        if (it.item_record_id == fi::kPotion && it.quantity >= 3) potions = it;
    }
    const auto words = potions ? items::guid_of(potions->instance_guid) : std::nullopt;
    CHECK(potions && words, "(the character carries a stack of potions)");
    if (!potions || !words) return;
    const int64_t had = h.s->inventory().count_of(fi::kPotion);
    const auto deliver = [&](int64_t units, int64_t quantity) {
        h.t += milliseconds(500);
        h.pc_call("ServerFulfillActiveBuyOrder", [&](const schema::RpcSchema& rs, repl::RpcParams& p) {
            repl::ArrayValue combo;
            repl::ElementState e;
            e.values[1] = repl::Value::i64(static_cast<int32_t>(words->a));
            e.values[2] = repl::Value::i64(static_cast<int32_t>(words->b));
            e.values[3] = repl::Value::i64(static_cast<int32_t>(words->c));
            e.values[4] = repl::Value::i64(static_cast<int32_t>(words->d));
            e.values[5] = repl::Value::i64(quantity);
            e.values[6] = repl::Value::u64(static_cast<uint64_t>(repl::ItemLocation::Backpack));
            combo.elements.push_back(std::move(e));
            repl::set_rpc_param(rs, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
            repl::set_rpc_param(rs, p, "RequestedBuyOrderGuid.A", repl::Value::i64(1));
            repl::set_rpc_param(rs, p, "RequestedBuyOrderGuid.B", repl::Value::i64(2));
            repl::set_rpc_param(rs, p, "RequestedBuyOrderGuid.C", repl::Value::i64(3));
            repl::set_rpc_param(rs, p, "RequestedBuyOrderGuid.D", repl::Value::i64(4));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.ItemCombination", repl::Value(combo));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.OriginalNumToFulfill", repl::Value::i64(units));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.AdjustedNumToFulfill", repl::Value::i64(units));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.TransactionId.A", repl::Value::i64(7));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.TransactionId.B", repl::Value::i64(8));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.TransactionId.C", repl::Value::i64(9));
            repl::set_rpc_param(rs, p, "FulfillmentCombo.TransactionId.D", repl::Value::i64(10));
        });
        h.run(milliseconds(300));
    };
    at = h.got.size();
    deliver(2, 2);
    CHECK(h.s->buy_order_deliveries() == 0 && h.s->buy_order_deliveries_refused() == 1 && h.s->inventory().count_of(fi::kPotion) == had,
          "with no Buy Order Manager's window open, a delivery is refused and nothing is taken");
    h.s->open_buy_orders_for_test(kTestCitizenNode);
    at = h.got.size();
    deliver(2, 2);
    calls = pc_calls_since(h, at);
    const repl::RpcCall* ok = last_call(calls, "ClientBuyOrderFulfilled");
    CHECK(ok && call_int(*ok, "FulfilledBuyOrder.VisiblyFulfilled") == 2 && call_int(*ok, "FulfilledBuyOrder.Requested") == 3 &&
              call_int(*ok, "FulfillmentCombo.AdjustedNumToFulfill") == 2 && call_int(*ok, "FulfillmentCombo.TransactionId.A") == 7,
          "ClientBuyOrderFulfilled: the order with its two units, and the combo back");
    const world::NodeBuyOrders* now_orders = el->buy_orders(kTestCitizenNode);
    const world::NodeBuyOrder* order = now_orders ? bo::find(*now_orders, g) : nullptr;
    CHECK(h.s->buy_order_deliveries() == 1 && h.s->inventory().count_of(fi::kPotion) == had - 2 && order && order->fulfilled == 2 && el->stored(kTestCitizenNode) &&
              el->stored(kTestCitizenNode)->count(commodity->item) && el->stored(kTestCitizenNode)->at(commodity->item) == 2,
          "the delivery takes the two potions, counts two units and puts two of the commodity's item into the node's stores");
    at = h.got.size();
    deliver(1, 1);
    calls = pc_calls_since(h, at);
    CHECK(last_call(calls, "ClientBuyOrderFulfillmentRejection") && !last_call(calls, "ClientBuyOrderFulfilled") && h.s->inventory().count_of(fi::kPotion) == had - 2 &&
              h.s->buy_order_deliveries_refused() == 2,
          "a third unit over the character's limit of two is refused (ClientBuyOrderFulfillmentRejection) and nothing is taken");
}

/// Node reputation through a session (session/player_reputation.cpp): the saved reputations come decayed to now; the rank counts only at the node the account is a
/// citizen of and scales its shops' taxes; the game master's grants (one player, or the node's citizens) stay within the node's range; every change is saved whole
/// with the progress.
void test_node_reputation_session() {
    std::printf("node citizenship: node reputation\n");
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    constexpr int64_t kOther = kTestCitizenNode + 9;
    CitizenHarness h([&](svc::Character& c) {
        svc::Citizenship cz;
        cz.node_guid = kTestCitizenNode;
        cz.primary_character_id = kCyb;
        cz.joined_ms = now_ms - 86400000;
        cz.dues_due_ms = now_ms + 10LL * 86400000;
        c.citizenship = cz;
        c.node_reputation = {svc::NodeReputation{kTestCitizenNode, 10500, now_ms - 3 * 3600000 - 60000}, svc::NodeReputation{kOther, 50, 0}};
    });
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    const auto near = [](double a, double b) { return std::fabs(a - b) < 0.01; };
    CHECK(h.s->citizenship().node_guid == kTestCitizenNode, "(the account is a citizen of the test's node)");
    CHECK(near(h.s->node_reputation(kTestCitizenNode), 10500 - 3) && near(h.s->node_reputation(kOther), 50),
          "at the join: 10,500 decayed by three hours (1 an hour); another node's 50 kept");
    CHECK(h.s->reputation_rank_at(kTestCitizenNode) == 1 && h.s->reputation_rank_at(kOther) == world::kNoReputationRank,
          "the rank: Commoner at the citizen node, none at a node the account is no citizen of");
    const auto& reps = h.s->bootstrap().setup().node_reputation;
    CHECK(reps.size() == 2 && std::any_of(reps.begin(), reps.end(), [&](const bootstrap::NodeReputationEntry& r) { return r.node == kTestCitizenNode && near(r.reputation, 10497); }),
          "OwnerInfo.NodeToPlayerReputation carries both nodes");
    const vendor::TaxRate base{10.0f, 100, true};
    const vendor::TaxRate commoner = h.s->reputation_taxed(base, kTestCitizenNode), other = h.s->reputation_taxed(base, kOther);
    CHECK(near(commoner.percent, 9.5) && commoner.flat == 95 && near(other.percent, 10) && other.flat == 100, "a Commoner's tax at the node is 95% of it; elsewhere it is the same");

    // the game master's grants
    const auto grant = [&](const char* player, const char* node, float amount) {
        h.t += milliseconds(500);
        h.pc_call("ServerGrantNodeToPlayerReputation", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "PlayerName", repl::Value::str(player));
            repl::set_rpc_param(r, p, "NodeName", repl::Value::str(node));
            repl::set_rpc_param(r, p, "ReputationModifier", repl::Value::f32(amount));
        });
        h.run(milliseconds(300));
    };
    grant("", "Testhold", 20000);
    CHECK(near(h.s->node_reputation(kTestCitizenNode), 10497), "a game master's grant does nothing without --gm-commands");
    h.fx.world->set_gm_commands(true);
    const size_t saves = h.sink.progress.size();
    const size_t from = h.got.size();
    grant("cyb", "Testhold", 20000);
    CHECK(near(h.s->node_reputation(kTestCitizenNode), 30497) && h.s->reputation_rank_at(kTestCitizenNode) == 2, "ServerGrantNodeToPlayerReputation: +20,000, Liege now");
    CHECK(count_calls(pc_calls_since(h, from), "ClientShowCurrencyCallout") == 1, "... with the Reputation tier's callout");
    CHECK(count_calls(pc_calls_since(h, from), "ClientReceiveChatMessages") >= 1, "... the new rank is told in the chat");
    bool saved = false;
    for (size_t i = saves; i < h.sink.progress.size(); ++i) {
        const svc::Progress& p = h.sink.progress[i];
        saved = saved || (p.replace_node_reputation && p.node_reputation.size() == 2 &&
                          std::any_of(p.node_reputation.begin(), p.node_reputation.end(), [&](const svc::NodeReputation& r) { return r.node_guid == kTestCitizenNode && near(r.reputation, 30497); }));
    }
    CHECK(saved, "... saved with the progress, the whole list (replace_node_reputation)");
    h.t += milliseconds(500);
    h.pc_call("ServerGrantCitizensNodeToPlayerReputation", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", repl::Value::str(""));
        repl::set_rpc_param(r, p, "ReputationModifier", repl::Value::f32(-100));
    });
    h.run(milliseconds(300));
    CHECK(near(h.s->node_reputation(kTestCitizenNode), 30397), "ServerGrantCitizensNodeToPlayerReputation (the node the game master stands in): -100 to its citizens");
    grant("", "Testhold", 1000000);
    CHECK(near(h.s->node_reputation(kTestCitizenNode), 100000) && h.s->reputation_rank_at(kTestCitizenNode) == 4, "a grant past the node's range stops at its 100,000: Aristocrat");
    const vendor::TaxRate aristocrat = h.s->reputation_taxed(base, kTestCitizenNode);
    CHECK(near(aristocrat.percent, 8) && aristocrat.flat == 80, "an Aristocrat pays 80% of the node's tax");
    grant("", "Testhold", 2000000);
    grant("", "No_Such_Node", 5);
    grant("nobody", "Testhold", 5);
    CHECK(near(h.s->node_reputation(kTestCitizenNode), 100000), "a grant over a million, for no node or for no player in the world does nothing");
    const std::string text = h.s->reputation_text();
    CHECK(text.find("Testhold: 100000.0 (Aristocrat)") != std::string::npos && text.find("not a citizen") != std::string::npos, "the reputation text lists the nodes, the rank at the citizen node");

    // no longer a citizen: no rank, the full tax
    h.t += milliseconds(500);
    h.pc_call("ServerGMRemoveNodeCitizen", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeName", repl::Value::str("Testhold"));
        repl::set_rpc_param(r, p, "PlayerName", repl::Value::str(""));
    });
    h.run(milliseconds(300));
    const vendor::TaxRate gone = h.s->reputation_taxed(base, kTestCitizenNode);
    CHECK(h.s->citizenship().node_guid == 0 && h.s->reputation_rank_at(kTestCitizenNode) == world::kNoReputationRank && near(gone.percent, 10) &&
              near(h.s->node_reputation(kTestCitizenNode), 100000),
          "a character of no node has no rank and its full tax (the old reputation stays)");
}

/// Nodes 2 through a session (session/player_node_xp.cpp): the base XP a player earns in a node that grows goes to citynode times the node rate, gathered and sent
/// every 10 s (once per node); a node of no growth takes none; a node's level-up countdown is shown to the players in its zone once (ClientShowNodeLevelUpCountdownToast).
struct NodeXpControl : world::NodeControl {
    std::vector<std::pair<int64_t, int64_t>> gains;
    void set_level(const std::string&, int, bool, const std::string&) override {}
    void add_experience(const std::string&, int64_t, const std::string&) override {}
    void node_data(const std::string&, const std::string&) override {}
    void clock(const std::string&) override {}
    void advance_clock(int64_t, const std::string&) override {}
    void gain_experience(int64_t guid, int64_t amount) override { gains.emplace_back(guid, amount); }
};

void test_node_xp_session() {
    std::printf("node citizenship: node XP and the level-up countdown\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    // the test's node, as a node that grows
    world::CityNodeInfo n = *h.nodes->by_guid(kTestCitizenNode);
    n.levelable = true;
    world::CityZoneInfo z = *h.nodes->zone_by_guid(n.zoi_guid);
    world::CityNodes growing(std::vector<world::CityNodeInfo>{n}, std::vector<world::CityZoneInfo>{z});
    growing.apply_change(world::LiveNode{kTestCitizenNode, 2, 0, 0});
    h.fx.world->set_city_nodes(&growing);
    NodeXpControl control;
    h.fx.world->set_node_control(&control);
    h.fx.world->set_node_experience_rate(100);
    h.s->earn_node_experience(120, "a test kill");
    h.s->earn_node_experience(30, "a test harvest");
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    h.fx.world->flush_node_experience(now_ms, /*force=*/true);
    CHECK(control.gains.size() == 1 && control.gains[0].first == kTestCitizenNode && control.gains[0].second == 15000 && h.s->node_xp_gains() == 2,
          "two gains in the node's zone go to citynode together: (120 + 30) x 100 = 15,000 XP, one call");
    h.fx.world->flush_node_experience(now_ms + 20000, true);
    CHECK(control.gains.size() == 1, "nothing gathered: nothing sent");
    h.fx.world->set_city_nodes(h.nodes.get());   // (the harness's node does not grow)
    h.s->earn_node_experience(500, "a test kill");
    h.fx.world->flush_node_experience(now_ms + 40000, true);
    CHECK(control.gains.size() == 1, "a node that does not grow takes no XP");

    // the countdown
    h.fx.world->set_city_nodes(&growing);
    size_t from = h.got.size();
    growing.apply_change(world::LiveNode{kTestCitizenNode, 2, 31643207, 0, now_ms + 5000});
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, from);
    const repl::RpcCall* toast = last_call(calls, "ClientShowNodeLevelUpCountdownToast");
    CHECK(toast && call_int(*toast, "NodeGuid") == kTestCitizenNode && h.s->level_up_toasts_sent() == 1, "a countdown of the player's node: the toast, with the node");
    from = h.got.size();
    growing.apply_change(world::LiveNode{kTestCitizenNode, 2, 31643208, 0, now_ms + 5000});
    h.run(milliseconds(300));
    CHECK(count_calls(pc_calls_since(h, from), "ClientShowNodeLevelUpCountdownToast") == 0, "... once for that countdown");
    growing.apply_change(world::LiveNode{kTestCitizenNode, 3, 31643208, 0, 0});
    h.run(milliseconds(300));
    h.fx.world->set_city_nodes(h.nodes.get());
    h.fx.world->set_node_control(nullptr);
}

AOC_REGISTER_TEST(test_buy_orders_session);
AOC_REGISTER_TEST(test_node_xp_session);
AOC_REGISTER_TEST(test_node_reputation_session);
AOC_REGISTER_TEST(test_citizenship_flow);
AOC_REGISTER_TEST(test_mayor_flow);
AOC_REGISTER_TEST(test_citizenship_gm);
AOC_REGISTER_TEST(test_node_taxes);
AOC_REGISTER_TEST(test_elections_unreadable_file);
AOC_REGISTER_TEST(test_node_inventory);
AOC_REGISTER_TEST(test_election_in_game);
