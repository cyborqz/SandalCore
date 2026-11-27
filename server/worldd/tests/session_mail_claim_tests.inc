// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_mail_claim_tests.inc: what a mail claim does to the bags when the store refuses it, does not answer it, answers it late, or the player logs out
//  and in again meanwhile.  Included after session_mail_tests.inc (its fake store, helpers and PartyWorld are used here).
//
//  The rules these tests hold:
//    * a refused claim undoes only its own placement: a bag change the player made meanwhile stays and is still saved, and the client's bags are sent again;
//    * while a claim of the character is in flight no bag save is made (the periodic save, the logout save and the others wait for the answer);
//    * a claim with no answer is never settled by a guess: the item stays, no bag is written, and the worker asks the same request again at a growing pace until the
//      service answers; a true repeat of a stored claim is answered Ok by the service itself, and a "claimed already" is a refusal like any other;
//    * a logout during a claim leaves the bags the answer decides to the mail worker, and a log-in of the character waits (on its own thread) until the claim is answered
//      and its bags are saved, then loads them: the item is in the bags exactly once and its mail says so.
// ============================================================================
#include "login/login_service.h"

#include <future>

namespace {

/// A character with one mail of one chest attachment; `store` holds the mail.
struct ClaimCase {
    std::string id, item;
    explicit ClaimCase(TestMailStore& store, uint32_t mail, uint32_t guid, int64_t record = content_fixture::item::kChest) : id(test_mail_hex(mail)), item(test_mail_hex(guid)) {
        store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item, record)}));
    }
};

/// How many bag rows hold the instance.
static size_t test_claim_rows_with(const PlayerSession& s, const std::string& guid) {
    size_t n = 0;
    for (const svc::InventoryItem& item : s.inventory().items()) n += item.instance_guid == guid ? 1 : 0;
    return n;
}

/// Joins the character; `carried` are the bags a login takes over from the session it replaces (the endpoint's carry), else the record has none.
static bool test_claim_join(PartyWorld& w, size_t who, TestMailStore& store, const char* name, uint32_t guid_word, const std::vector<svc::InventoryItem>* carried = nullptr) {
    w.fx.world->set_position_sink(&store);
    w.fx.world->set_mail_job_config(MailJobs::Config{std::chrono::milliseconds(1), std::chrono::milliseconds(4)});
    svc::Character character = test_mail_record(name, {guid_word, 0, 0, 1}, 1);
    if (carried) character.items = *carried;
    return w.enter(who, character, identity());
}

/// How many ClientReceiveStoragePayload calls the pawn's channel carried among the chains `got[from..]`.
static size_t test_claim_pawn_payloads(const PartyWorld& w, size_t who, size_t from) {
    size_t n = 0;
    if (!w.p[who].s) return n;
    const uint32_t pawn = w.p[who].s->channel_of(bs::Role::Pawn);
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& d = w.p[who].got[i];
        if (d.channel != pawn || d.open || d.close) continue;
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            const auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), d.content.data(), b.payload_pos + b.payload_bits, pos);
            if (!calls) continue;
            for (const repl::RpcCall& call : *calls) n += call.rpc && std::string(call.rpc->name) == "ClientReceiveStoragePayload" ? 1 : 0;
        }
    }
    return n;
}

/// The log-in tests' gateway: every ticket is the character's own.
struct ClaimLoginGateway : svc::GatewayApi {
    svc::CallStatus status = svc::CallStatus::Ok;   // what the gateway answers to the ticket
    std::atomic<int> redeems{0};
    svc::RedeemResult redeem_ticket(const std::string&, const std::string& character_id, uint32_t) override {
        ++redeems;
        svc::RedeemResult result;
        result.status = status;
        result.account_id = 3;
        result.character_id = character_id;
        return result;
    }
};

/// The log-in tests' character service: the record it answers carries the bags the mail store holds at that moment, and every load is counted.
struct ClaimLoginCharacters : svc::CharacterApi {
    TestMailStore&   store;
    svc::Character   record;
    std::atomic<int> loads{0};
    ClaimLoginCharacters(TestMailStore& s, svc::Character base) : store(s), record(std::move(base)) {}
    svc::CharacterResult get_character(const std::string&) override {
        ++loads;
        svc::CharacterResult result;
        result.status = svc::CallStatus::Ok;
        result.character = record;
        result.character.account_id = 3;
        std::lock_guard<std::mutex> lock(store.store_mu);
        result.character.items = store.bag_items;
        return result;
    }
    svc::CallStatus save_position(const std::string&, const svc::Position&) override { return svc::CallStatus::Ok; }
};

/// A log-in of one character on its own thread, as the log-in worker runs it, with the world's barrier.
struct ClaimLogin {
    ClaimLoginGateway     gateway;
    ClaimLoginCharacters  characters;
    LoginService          service;
    std::string           id;
    std::future<LoginResult> result;
    ClaimLogin(PartyWorld& w, TestMailStore& store, svc::Character record, std::chrono::milliseconds bound = std::chrono::seconds(10), uint64_t mark_session = 0)
        : characters(store, record), service(gateway, characters), id(record.id) {
        LoginService::Barrier barrier;
        PlayerWorld* world = &*w.fx.world;
        barrier.writes_pending = [world](const std::string& who) { return world->character_writes_pending(who); };
        if (mark_session != 0) barrier.redeemed = [world, mark_session](uint64_t, const std::string& who) { world->login_began(who, mark_session); };
        barrier.bound = bound;
        barrier.poll = std::chrono::milliseconds(2);
        service.set_barrier(std::move(barrier));
    }
    void start() {
        result = std::async(std::launch::async, [this] { return service.login(LoginRequest{id, std::string(32, 'a')}, 7); });
    }
    bool done(std::chrono::milliseconds wait) { return result.wait_for(wait) == std::future_status::ready; }
};

/// How many rows of the loaded bags hold the instance.
static size_t test_claim_loaded_rows_with(const LoginResult& r, const std::string& guid) {
    size_t n = 0;
    for (const svc::InventoryItem& item : r.character.items) n += item.instance_guid == guid ? 1 : 0;
    return n;
}

/// How many rows of the latest bags a hand-over gave the store hold the instance.
static size_t test_claim_saved_rows_with(TestMailStore& store, const std::string& guid) {
    std::lock_guard<std::mutex> lock(store.store_mu);
    size_t n = 0;
    for (const svc::InventoryItem& item : store.saved_bags.empty() ? std::vector<svc::InventoryItem>() : store.saved_bags.back()) n += item.instance_guid == guid ? 1 : 0;
    return n;
}

/// A sink for the worker alone: a claim is answered Unavailable until `answer` is set (then Ok), every ask is timed, and the loads are counted per character.
struct AskingSink : PositionSink {
    std::mutex                                         mu;
    std::vector<std::chrono::steady_clock::time_point> asks;
    std::vector<std::string>                           loaded;
    std::atomic<bool>                                  answer{false};
    void save(const std::string&, const svc::Position&) override {}
    svc::MailsResult load_mails(const std::string& character_id) override {
        std::lock_guard<std::mutex> lock(mu);
        loaded.push_back(character_id);
        svc::MailsResult result;
        result.status = svc::CallStatus::Ok;
        return result;
    }
    svc::MailClaimResult claim_mail_items(const std::string&, const std::vector<svc::MailClaim>&, std::vector<svc::InventoryItem>, std::vector<svc::WalletEntry>,
                                          std::vector<svc::ProcessingJob>, std::vector<int64_t>, std::vector<svc::WarehouseItem>) override {
        std::lock_guard<std::mutex> lock(mu);
        asks.push_back(std::chrono::steady_clock::now());
        svc::MailClaimResult result;
        result.status = answer ? svc::CallStatus::Ok : svc::CallStatus::Unavailable;
        return result;
    }
    size_t ask_count() {
        std::lock_guard<std::mutex> lock(mu);
        return asks.size();
    }
    size_t loads_of(const std::string& character_id) {
        std::lock_guard<std::mutex> lock(mu);
        return static_cast<size_t>(std::count(loaded.begin(), loaded.end(), character_id));
    }
};

static MailJobRequest test_claim_job(MailJobKind kind, const std::string& character_id) {
    MailJobRequest request;
    request.kind = kind;
    request.character_id = character_id;
    if (kind == MailJobKind::Claim) request.claims.push_back(svc::MailClaim{test_mail_hex(1), test_mail_hex(2)});
    return request;
}

}  // namespace

static void test_mail_refused_claim_gives_the_item_back_and_resyncs_the_client() {
    std::printf("a claim the store refuses gives the item back, sends the client its bags again and reads the mails again\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase c(store, 60, 7401);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d40)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t loads = store.loads;
    store.claim_script = {{svc::CallStatus::Refused, false}};
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
    w.settle();
    CHECK(test_claim_rows_with(*w[0].s, c.item) == 0, "the item is not in the bags");
    CHECK(store.by_id(c.id) && !store.by_id(c.id)->items[0].claimed, "the mail stays unclaimed");
    CHECK(test_claim_pawn_payloads(w, 0, from) >= 2, "the client is sent its storages again");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "it is not told the claim was made");
    CHECK(store.loads == loads + 1, "the mails are read again");
    CHECK(!w[0].s->mail_claim_pending(), "no claim is in flight");
}

static void test_mail_rollback_keeps_a_bag_change_made_while_the_claim_was_in_flight() {
    std::printf("a bag change made while a claim is in flight is still saved after the claim is refused, and the claim's item is not\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase c(store, 61, 7402);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d41)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.claim_script = {{svc::CallStatus::Refused, false}};
    store.block(2);
    CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
    CHECK(test_mail_wait_for(store.claims, 1), "the worker took the claim");
    const PlayerSession::GmItemResult given = w[0].s->gm_give_item(w[0].conn, content_fixture::item::kLegs, 1, w.t, "the test");
    CHECK(given.ok, "a bag change is made while the claim is in flight");
    CHECK(w[0].s->inventory_dirty() && w[0].s->inventory_changed(), "the change is waiting to be saved");
    const size_t handovers = store.handovers;
    store.release();
    w.settle();
    CHECK(test_claim_rows_with(*w[0].s, c.item) == 0, "the refused claim's item is not in the bags");
    CHECK(w[0].s->inventory().count(svc::ItemContainer::Backpack) == 1, "the bag change the player made is still there");
    CHECK(w[0].s->inventory_dirty() && w[0].s->inventory_changed(), "the refusal left the change marked unsaved");
    w.t += seconds(61);
    w.step();
    CHECK(store.handovers == handovers + 1, "the next periodic save saves it");
    bool legs_saved = false, claimed_saved = false;
    for (const svc::InventoryItem& item : store.saved_bags.empty() ? std::vector<svc::InventoryItem>() : store.saved_bags.back()) {
        legs_saved = legs_saved || item.item_record_id == content_fixture::item::kLegs;
        claimed_saved = claimed_saved || item.instance_guid == c.item;
    }
    CHECK(legs_saved && !claimed_saved, "it saves the player's change and not the item of the refused claim");
    CHECK(store.by_id(c.id) && !store.by_id(c.id)->items[0].claimed, "the mail stays unclaimed");
}

static void test_mail_claim_in_flight_holds_the_periodic_save() {
    std::printf("the periodic save waits for a claim in flight and then saves the bags that hold its item\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase c(store, 62, 7403);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d42)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t handovers = store.handovers;
    store.block(2);
    CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
    CHECK(test_mail_wait_for(store.claims, 1), "the worker took the claim");
    w.t += seconds(61);
    w.step();
    CHECK(store.handovers == handovers, "the periodic save made no save while the claim is in flight");
    CHECK(w[0].s->mail_claim_pending(), "the claim is in flight");
    store.release();
    w.settle();
    CHECK(store.by_id(c.id) && store.by_id(c.id)->items[0].claimed && store.bag_items_with(c.item) == 1, "the claim stored the item and the mark together");
    CHECK(store.handovers == handovers + 1, "the save that waited is made when the claim is answered");
    size_t rows = 0;
    for (const svc::InventoryItem& item : store.saved_bags.empty() ? std::vector<svc::InventoryItem>() : store.saved_bags.back()) rows += item.instance_guid == c.item ? 1 : 0;
    CHECK(rows == 1, "and the saved bags hold the item once");
}

static void test_mail_second_claim_waits_for_the_one_in_flight() {
    std::printf("a claim made while another is in flight waits for its answer and is made then\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase a(store, 63, 7404);
    const ClaimCase b(store, 64, 7405, content_fixture::item::kLegs);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d43)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.block(2);
    CHECK(test_mail_claim(w, 0, a.id, 0, true), "the first claim is sent");
    CHECK(test_mail_wait_for(store.claims, 1), "the worker took the first claim");
    CHECK(test_mail_claim(w, 0, b.id, 0, true), "the second mail's claim is sent behind it");
    CHECK(store.claims == 1 && w[0].s->mail_claims_waiting() == 1, "it is not sent to the store: it waits");
    CHECK(test_claim_rows_with(*w[0].s, b.item) == 0, "and nothing of it is placed yet");
    store.release();
    w.settle();
    CHECK(store.claims == 2 && w[0].s->mail_claims_waiting() == 0, "the second claim is made when the first is answered");
    CHECK(store.by_id(a.id)->items[0].claimed && store.by_id(b.id)->items[0].claimed, "both marks are stored");
    CHECK(store.bag_items_with(a.item) == 1 && store.bag_items_with(b.item) == 1, "the stored bags hold both items once");
    CHECK(test_claim_rows_with(*w[0].s, a.item) == 1 && test_claim_rows_with(*w[0].s, b.item) == 1, "so do the session's bags");
}

static void test_mail_claim_with_a_lost_answer_keeps_the_item() {
    std::printf("a claim the store stored but did not answer is asked again, the repeat is answered Ok, and the item stays once\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase c(store, 65, 7406);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d44)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.claim_script = {{svc::CallStatus::Unavailable, true}};
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent and its answer is lost");
    w.settle();
    CHECK(store.claims == 2, "the worker asked again");
    CHECK(test_claim_rows_with(*w[0].s, c.item) == 1 && store.bag_items_with(c.item) == 1, "the item is in the bags once, and in the stored bags once");
    CHECK(store.by_id(c.id) && store.by_id(c.id)->items[0].claimed, "the mail is claimed");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1 && test_guild_storage_events(test_mail_pc_calls(w, 0, from)).size() == 1,
          "the client is told once");
}

static void test_login_waits_for_the_claim_then_loads_the_stored_bags() {
    std::printf("a log-in while a claim is unanswered waits on its own thread, and loads the bags and mail the answer decided: the item once\n");
    std::fflush(stdout);
    // 0: the session ended, the claim is stored.  1: the session ended, the claim is refused.  2: the session is still playing, the claim is stored.
    for (int variant = 0; variant < 3; ++variant) {
        const bool stored = variant != 1, live = variant == 2;
        TestMailStore store;
        const ClaimCase c(store, 80 + static_cast<uint32_t>(variant), 7420 + static_cast<uint32_t>(variant));
        PartyWorld w;
        if (!test_claim_join(w, 0, store, "mailer", 0x4d50 + static_cast<uint32_t>(variant))) {
            CHECK(false, "the character joins");
            return;
        }
        CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
        w.settle();
        const svc::Character record = test_mail_record("mailer", {0x4d50 + static_cast<uint32_t>(variant), 0, 0, 1}, 1);
        if (!stored) store.claim_script = {{svc::CallStatus::Refused, false}};
        store.save_delay_ms = 60;   // the bags the answer decides reach the store slowly: a log-in that does not wait for them reads the old ones
        store.block(2);
        CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
        CHECK(test_mail_wait_for(store.claims, 1), "the worker has the claim at the store");
        if (!live) w[0].s->finish("the player logs out");
        CHECK(w.fx.world->character_writes_pending(record.id), "the character has a claim without an answer");
        CHECK(!w.fx.world->character_writes_pending(test_mail_hex(99)), "another character has none");
        ClaimLogin login(w, store, record);
        login.start();
        CHECK(!login.done(std::chrono::milliseconds(150)), "the log-in waits while the claim is unanswered");
        CHECK(login.characters.loads == 0, "and loads nothing meanwhile");
        store.release();
        bool finished = false;
        for (int i = 0; i < 600 && !finished; ++i) {
            if (live) w.step();   // (a playing session applies the answer on its own tick)
            finished = login.done(std::chrono::milliseconds(live ? 1 : 10));
        }
        CHECK(finished, "the log-in goes on once the claim is answered and its bags are saved");
        if (!finished) return;
        const LoginResult r = login.result.get();
        CHECK(r.accepted && login.characters.loads == 1, "it is accepted and loaded once");
        CHECK(test_claim_loaded_rows_with(r, c.item) == (stored ? 1u : 0u), stored ? "the loaded bags hold the item once" : "the loaded bags hold no item of the refused claim");
        CHECK(store.by_id(c.id) && store.by_id(c.id)->items[0].claimed == stored, "the mail says the same");
        if (live) continue;
        // the new session starts from what the log-in loaded
        w[0].s.reset();
        svc::Character loaded = r.character;
        loaded.unread_mail_count = 1;
        if (!w.enter(1, loaded, identity())) {
            CHECK(false, "the second session joins");
            return;
        }
        CHECK(test_mail_fetch(w, 1), "the new session's list is sent");
        w.settle();
        CHECK(test_claim_rows_with(*w[1].s, c.item) == (stored ? 1u : 0u), "the new session's bags hold what the answer decided");
        CHECK(w[1].s->mails().size() == 1 && w[1].s->mails()[0].items[0].claimed == stored, "and so do its mails");
        if (!stored) {
            CHECK(test_mail_claim(w, 1, c.id, 0, true), "the claim is made again");
            w.settle();
            CHECK(test_claim_rows_with(*w[1].s, c.item) == 1 && store.bag_items_with(c.item) == 1 && store.by_id(c.id)->items[0].claimed, "and gives the item once");
        }
    }
}

static void test_login_after_a_refused_top_up_and_a_relog_finds_the_stack_as_it_was() {
    std::printf("a refused claim that topped up a stack, a logout and an immediate log-in: the log-in loads the stack at its old quantity\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(81), held_guid = test_mail_hex(7430), attachment_guid = test_mail_hex(7431);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(attachment_guid, content_fixture::item::kPotion, 2)}));
    std::vector<svc::InventoryItem> held = {test_mail_item(held_guid, content_fixture::item::kPotion, svc::ItemContainer::Quest, 0, 1)};
    PartyWorld w(test_mail_quest_db().string());
    if (!test_claim_join(w, 0, store, "mailer", 0x4d54, &held)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    svc::Character record = test_mail_record("mailer", {0x4d54, 0, 0, 1}, 1);
    store.claim_script = {{svc::CallStatus::Refused, false}};
    store.block(2);
    CHECK(test_mail_claim(w, 0, id, 0, true), "the potion's claim is sent");
    CHECK(test_mail_wait_for(store.claims, 1), "the worker has the claim at the store");
    CHECK(w[0].s->inventory().find_guid(held_guid) && w[0].s->inventory().find_guid(held_guid)->quantity == 3, "the held stack grew by the attachment's two while the claim is in flight");
    w[0].s->finish("the player logs out");
    ClaimLogin login(w, store, record);
    login.start();
    CHECK(!login.done(std::chrono::milliseconds(100)), "the log-in waits for the claim");
    store.release();
    CHECK(login.done(std::chrono::seconds(5)), "the log-in goes on once the refusal is answered and the bags are saved");
    const LoginResult r = login.result.get();
    size_t quantity = 0;
    for (const svc::InventoryItem& item : r.character.items) quantity += item.instance_guid == held_guid ? static_cast<size_t>(item.quantity) : 0;
    CHECK(r.accepted && quantity == 1, "the loaded stack has its old quantity: the refused top-up is not in it");
    CHECK(test_claim_loaded_rows_with(r, attachment_guid) == 0, "and the attachment is not a row of its own");
    w[0].s.reset();
    if (!w.enter(1, r.character, identity())) {
        CHECK(false, "the second session joins");
        return;
    }
    const svc::InventoryItem* stack = w[1].s->inventory().find_guid(held_guid);
    CHECK(stack != nullptr && stack->quantity == 1, "the new session's stack holds its own one");
    CHECK(test_mail_fetch(w, 1), "the new session's list is sent");
    w.settle();
    CHECK(test_mail_claim(w, 1, id, 0, true), "the claim is made again");
    w.settle();
    CHECK(w[1].s->inventory().find_guid(held_guid) && w[1].s->inventory().find_guid(held_guid)->quantity == 3 && store.by_id(id)->items[0].claimed, "and tops the stack up once");
}

static void test_login_bound_refuses_the_login_and_loads_nothing() {
    std::printf("a log-in that still has an unanswered claim of its character after the bound is refused with the try-again text and loads nothing\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase c(store, 82, 7432);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d55)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const svc::Character record = test_mail_record("mailer", {0x4d55, 0, 0, 1}, 1);
    store.block(2);
    CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
    CHECK(test_mail_wait_for(store.claims, 1), "the worker has the claim at the store");
    w[0].s->finish("the player logs out");
    ClaimLogin login(w, store, record, std::chrono::milliseconds(120));
    const auto began = std::chrono::steady_clock::now();
    login.start();
    CHECK(login.done(std::chrono::seconds(5)), "the log-in ends at its bound");
    const LoginResult r = login.result.get();
    CHECK(!r.accepted && r.error == login_text::kServiceUnavailable, "it is refused with the try-again text");
    CHECK(std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(100), "after waiting for the bound");
    CHECK(login.characters.loads == 0, "and nothing was loaded");
    store.release();
    CHECK(test_mail_wait_for(store.handovers, 1), "the claim is answered later and its bags are saved all the same");
}

static void test_a_claim_waits_while_a_login_of_the_character_is_pending() {
    std::printf("a claim made while a log-in of the character waits to be applied is not started: it is made when the log-in is applied or refused\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase a(store, 83, 7433);
    const ClaimCase b(store, 84, 7434, content_fixture::item::kLegs);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d56)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const std::string id = w[0].s->character_id();
    w.fx.world->login_began(id, 901);
    CHECK(w.fx.world->login_pending(id) && !w.fx.world->login_pending(test_mail_hex(99)), "the character has a log-in pending");
    CHECK(test_mail_claim(w, 0, a.id, 0, true), "a claim is sent");
    CHECK(test_mail_claim_all(w, 0, {b.id}), "a claim-all is sent");
    for (int i = 0; i < 5; ++i) w.step();
    CHECK(store.claims == 0 && w[0].s->mail_claims_waiting() == 2, "neither is started: both wait");
    CHECK(test_claim_rows_with(*w[0].s, a.item) == 0 && test_claim_rows_with(*w[0].s, b.item) == 0, "and nothing of them is placed in the bags");
    w.fx.world->login_ended(901);
    w.settle();
    CHECK(store.claims == 2 && w[0].s->mail_claims_waiting() == 0, "once the log-in is over they are made, one after the other");
    CHECK(store.by_id(a.id)->items[0].claimed && store.by_id(b.id)->items[0].claimed, "both marks are stored");
    CHECK(test_claim_rows_with(*w[0].s, a.item) == 1 && test_claim_rows_with(*w[0].s, b.item) == 1, "and both items are in the bags once");
}

static void test_a_refused_ticket_marks_nothing_and_a_redeemed_login_holds_a_new_claim() {
    std::printf("a log-in whose ticket the gateway refuses marks nothing and its character's claim starts at once; a redeemed log-in holds a new claim until its result is applied\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase a(store, 88, 7438);
    const ClaimCase b(store, 89, 7439, content_fixture::item::kLegs);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d5b)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const svc::Character record = test_mail_record("mailer", {0x4d5b, 0, 0, 1}, 1);
    {
        ClaimLogin login(w, store, record, std::chrono::seconds(10), 55);
        login.gateway.status = svc::CallStatus::Refused;
        login.start();
        CHECK(login.done(std::chrono::seconds(5)), "the log-in ends");
        const LoginResult r = login.result.get();
        CHECK(!r.accepted && r.error == login_text::kTicketRefused && login.gateway.redeems == 1 && login.characters.loads == 0, "the refused ticket refuses the log-in and loads nothing");
        CHECK(!w.fx.world->login_pending(record.id), "and marks nothing");
        CHECK(test_mail_claim(w, 0, a.id, 0, true), "a claim of the character is sent");
        w.settle();
        CHECK(store.claims == 1 && store.by_id(a.id)->items[0].claimed && test_claim_rows_with(*w[0].s, a.item) == 1, "it starts at once and is stored");
    }
    {
        ClaimLogin login(w, store, record, std::chrono::seconds(10), 56);
        login.start();
        CHECK(login.done(std::chrono::seconds(5)) && login.result.get().accepted, "a log-in with a redeemed ticket is accepted");
        CHECK(w.fx.world->login_pending(record.id) && !w.fx.world->login_pending(test_mail_hex(99)), "it marks its character until its result is applied");
        CHECK(test_mail_claim(w, 0, b.id, 0, true), "a claim of the character is sent");
        for (int i = 0; i < 5; ++i) w.step();
        CHECK(store.claims == 1 && w[0].s->mail_claims_waiting() == 1 && test_claim_rows_with(*w[0].s, b.item) == 0, "it waits: nothing is asked and nothing is placed");
        w.fx.world->login_ended(56);
        w.settle();
        CHECK(store.claims == 2 && store.by_id(b.id)->items[0].claimed && test_claim_rows_with(*w[0].s, b.item) == 1, "when the result is applied the claim is made");
    }
}

static void test_a_claim_the_worker_refuses_at_submit_is_undone_and_made_once_when_the_mark_ends() {
    std::printf("a claim that passes the session's check and is refused by the worker at submit (a log-in was marked in between) is undone, asks nothing, waits first in line, and is made once when the mark ends\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id_a = test_mail_hex(90), held_guid = test_mail_hex(7440), attachment_guid = test_mail_hex(7441);
    store.mails.push_back(test_mail_mail(id_a, "Sender", "Subject", "Body", 0, {test_mail_attachment(attachment_guid, content_fixture::item::kPotion, 2)}));
    const ClaimCase b(store, 92, 7442, content_fixture::item::kLegs);
    std::vector<svc::InventoryItem> held = {test_mail_item(held_guid, content_fixture::item::kPotion, svc::ItemContainer::Quest, 0, 1)};
    PartyWorld w(test_mail_quest_db().string());
    if (!test_claim_join(w, 0, store, "mailer", 0x4d5c, &held)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const std::string id = w[0].s->character_id();
    const size_t handovers = store.handovers;
    bool marked = false;
    // the log-in's thread marks the character between the session's check and the worker's registration of the claim
    w.fx.world->set_mail_submit_hook([&] {
        if (marked) return;
        marked = true;
        w.fx.world->login_began(id, 91);
    });
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id_a, 0, true), "the potion's claim is sent");
    CHECK(marked && w.fx.world->login_pending(id), "a log-in of the character was marked after the check and before the submit");
    CHECK(store.claims == 0 && !w[0].s->mail_claim_pending(), "the worker refused it: no claim call was made and none is in flight");
    const svc::InventoryItem* stack = w[0].s->inventory().find_guid(held_guid);
    CHECK(stack != nullptr && stack->quantity == 1, "the top-up was undone: the stack has its old quantity");
    CHECK(w[0].s->inventory().find_guid(attachment_guid) == nullptr, "and the attachment is not a row of its own");
    CHECK(w[0].s->mail_claims_waiting() == 1, "the claim waits");
    CHECK(store.handovers == handovers, "no bag was written");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty() && test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0,
          "the client was told nothing");
    CHECK(test_mail_claim(w, 0, b.id, 0, true), "a second claim is sent while the mark stands");
    CHECK(w[0].s->mail_claims_waiting() == 2 && test_claim_rows_with(*w[0].s, b.item) == 0, "it waits behind the first and places nothing");
    for (int i = 0; i < 5; ++i) w.step();
    stack = w[0].s->inventory().find_guid(held_guid);
    CHECK(store.claims == 0 && w[0].s->mail_claims_waiting() == 2 && stack != nullptr && stack->quantity == 1 && store.handovers == handovers, "both keep waiting while the mark stands");
    w.fx.world->set_mail_submit_hook(nullptr);
    store.block(2);
    w.fx.world->login_ended(91);
    for (int i = 0; i < 400 && store.claims < 1; ++i) {
        w.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(store.claims == 1, "when the mark ends the first claim is asked");
    stack = w[0].s->inventory().find_guid(held_guid);
    CHECK(stack != nullptr && stack->quantity == 3 && test_claim_rows_with(*w[0].s, b.item) == 0 && w[0].s->mail_claims_waiting() == 1,
          "it is the claim that waited first in line (its item is placed once, the second claim's is not placed yet)");
    store.release();
    w.settle();
    {
        std::lock_guard<std::mutex> lock(store.store_mu);
        CHECK(store.last_claims.size() == 1 && store.last_claims[0].mail_id == b.id, "the second claim is asked after it");
    }
    stack = w[0].s->inventory().find_guid(held_guid);
    CHECK(store.claims == 2 && store.by_id(id_a)->items[0].claimed && store.by_id(b.id)->items[0].claimed, "both claims are made, once each, one after the other");
    CHECK(stack != nullptr && stack->quantity == 3 && test_claim_rows_with(*w[0].s, b.item) == 1 && store.bag_items_with(b.item) == 1, "each gives its item once");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).size() == 2 && test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 2,
          "and the client is sent one Add and one answer for each");
}

static void test_mail_jobs_refuse_a_new_claim_of_a_character_with_a_login_pending() {
    std::printf("the worker refuses a new claim of a character whose log-in is marked, in the one step that registers it, and holds nothing of other characters or other jobs\n");
    std::fflush(stdout);
    AskingSink sink;
    sink.answer = true;
    MailJobs jobs(MailJobs::Config{std::chrono::milliseconds(1), std::chrono::milliseconds(4)});
    jobs.login_began("char-a", 7);
    CHECK(jobs.login_pending("char-a") && !jobs.login_pending("char-b"), "the character is marked");
    CHECK(jobs.submit(test_claim_job(MailJobKind::Claim, "char-a"), &sink) == 0 && !jobs.claim_open("char-a") && sink.ask_count() == 0, "its new claim is refused: nothing is registered or asked");
    const uint64_t other = jobs.submit(test_claim_job(MailJobKind::Claim, "char-b"), &sink);
    const uint64_t load = jobs.submit(test_claim_job(MailJobKind::Load, "char-a"), &sink);
    CHECK(other != 0 && load != 0, "another character's claim and the same character's other jobs are taken");
    MailJobAnswer answer;
    bool got_other = false, got_load = false;
    for (int i = 0; i < 500 && !(got_other && got_load); ++i) {
        got_other = got_other || jobs.take(other, answer);
        got_load = got_load || jobs.take(load, answer);
        if (!(got_other && got_load)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(got_other && got_load, "and both are answered");
    jobs.claim_closed(other);
    jobs.login_ended(7);
    CHECK(!jobs.login_pending("char-a"), "the mark ends with its log-in");
    const uint64_t again = jobs.submit(test_claim_job(MailJobKind::Claim, "char-a"), &sink);
    CHECK(again != 0 && jobs.claim_open("char-a"), "the claim is taken then");
    for (int i = 0; i < 500 && !jobs.take(again, answer); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    jobs.claim_closed(again);
}

static void test_mail_claim_with_every_answer_lost_keeps_the_item_until_the_service_answers() {
    std::printf("a stored claim whose answers are all lost keeps its item, writes no bag and waits; when the service answers the item exists once and the next save holds it\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase c(store, 72, 7413);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d4a)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.claim_script = {{svc::CallStatus::Unavailable, true}};   // the first ask is stored and its answer is lost
    store.claims_down = true;                                       // the service then answers nothing
    const size_t handovers = store.handovers;
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
    CHECK(test_mail_wait_for(store.claims, 6), "the worker asks again and again");
    for (int i = 0; i < 20; ++i) w.step();
    CHECK(w[0].s->mail_claim_pending(), "the claim is still in flight");
    CHECK(test_claim_rows_with(*w[0].s, c.item) == 1, "the item stays in the bags: no answer never gives it back");
    CHECK(store.handovers == handovers, "no bag was written");
    w.t += seconds(61);
    w.step();
    CHECK(store.handovers == handovers && w[0].s->inventory_dirty(), "not even the periodic save: the change waits, marked unsaved");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "the client is told nothing yet");
    store.claims_down = false;
    w.settle();
    CHECK(!w[0].s->mail_claim_pending(), "the service answered: the claim is no longer in flight");
    CHECK(test_claim_rows_with(*w[0].s, c.item) == 1 && store.bag_items_with(c.item) == 1, "the item is in the bags once, and in the stored bags once");
    CHECK(store.by_id(c.id) && store.by_id(c.id)->items[0].claimed, "the mail is claimed");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1 && test_guild_storage_events(test_mail_pc_calls(w, 0, from)).size() == 1,
          "the client is told once");
    CHECK(store.handovers == handovers + 1 && test_claim_saved_rows_with(store, c.item) == 1, "the save that waited is made and holds the item once");
}

static void test_mail_service_down_for_many_asks_makes_no_bag_write_and_the_tick_goes_on() {
    std::printf("a service that is down for many asks: no bag write, the tick goes on, a bag change is kept and other claims wait; then everything is stored once\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase a(store, 75, 7417);
    const ClaimCase b(store, 76, 7418, content_fixture::item::kLegs);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d4d)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.claims_down = true;
    const size_t handovers = store.handovers;
    CHECK(test_mail_claim(w, 0, a.id, 0, true), "the first claim is sent");
    CHECK(test_mail_wait_for(store.claims, 8), "the worker has asked eight times");
    CHECK(test_mail_claim(w, 0, b.id, 0, true), "a second claim is sent");
    CHECK(w[0].s->mail_claims_waiting() == 1 && test_claim_rows_with(*w[0].s, b.item) == 0, "it waits for the first and places nothing");
    const auto began = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) w.step();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(5), "the tick goes on while the claim is unanswered");
    const PlayerSession::GmItemResult given = w[0].s->gm_give_item(w[0].conn, content_fixture::item::kFeet, 1, w.t, "the test");
    CHECK(given.ok && w[0].s->inventory_dirty(), "a bag change is made and marked unsaved");
    w.t += seconds(61);
    w.step();
    CHECK(store.handovers == handovers, "no bag was written in all that time");
    CHECK(test_claim_rows_with(*w[0].s, a.item) == 1 && w[0].s->mail_claim_pending(), "the first item stays in the bags and its claim in flight");
    const size_t asked = store.claims;
    store.claims_down = false;
    w.settle();
    CHECK(store.claims >= asked + 2 && store.by_id(a.id)->items[0].claimed && store.by_id(b.id)->items[0].claimed, "the service came back: both claims are stored");
    CHECK(test_claim_rows_with(*w[0].s, a.item) == 1 && test_claim_rows_with(*w[0].s, b.item) == 1 && store.bag_items_with(a.item) == 1 && store.bag_items_with(b.item) == 1,
          "each item is in the bags once, and in the stored bags once");
    CHECK(w[0].s->inventory().count(svc::ItemContainer::Backpack) == 3, "and the bag change the player made is kept");
    w.t += seconds(61);
    w.step();
    bool feet_saved = false;
    {
        std::lock_guard<std::mutex> lock(store.store_mu);
        for (const svc::InventoryItem& item : store.saved_bags.empty() ? std::vector<svc::InventoryItem>() : store.saved_bags.back()) feet_saved = feet_saved || item.item_record_id == content_fixture::item::kFeet;
    }
    CHECK(store.handovers > handovers && feet_saved, "and it is saved");
}

/// The quantity of the stack with `guid` in the latest bags a hand-over gave the store (0: none).
static int32_t test_claim_saved_quantity(TestMailStore& store, const std::string& guid) {
    std::lock_guard<std::mutex> lock(store.store_mu);
    if (store.saved_bags.empty()) return 0;
    for (const svc::InventoryItem& item : store.saved_bags.back()) {
        if (item.instance_guid == guid) return item.quantity;
    }
    return 0;
}

static void test_mail_claimed_already_on_the_repeat_after_a_lost_answer_is_a_refusal() {
    std::printf("'claimed already' to the repeat of a claim whose first ask had no answer is a refusal: the items go back, the bags are saved without them, and the attachments stay claimable\n");
    std::fflush(stdout);
    // 0: one claim, session stays.  1: a claim-all of two mails, session stays.  2: one claim, logout in between.  3: a claim-all, logout in between.
    for (int variant = 0; variant < 4; ++variant) {
        const bool all = (variant & 1) != 0, with_logout = (variant & 2) != 0;
        const uint32_t n = static_cast<uint32_t>(variant);
        TestMailStore store;
        const std::string id_a = test_mail_hex(100 + n * 2), id_b = test_mail_hex(101 + n * 2);
        const std::string held_guid = test_mail_hex(7500 + n * 3), att_a = test_mail_hex(7501 + n * 3), att_b = test_mail_hex(7502 + n * 3);
        store.mails.push_back(test_mail_mail(id_a, "Sender", "Subject", "Body", 0, {test_mail_attachment(att_a, content_fixture::item::kPotion, 2)}));
        if (all) store.mails.push_back(test_mail_mail(id_b, "Sender", "Subject", "Body", 0, {test_mail_attachment(att_b, content_fixture::item::kLegs)}));
        std::vector<svc::InventoryItem> held = {test_mail_item(held_guid, content_fixture::item::kPotion, svc::ItemContainer::Quest, 0, 1)};
        PartyWorld w(test_mail_quest_db().string());
        if (!test_claim_join(w, 0, store, "mailer", 0x4d70 + n, &held)) {
            CHECK(false, "the character joins");
            return;
        }
        CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
        w.settle();
        const svc::Character record = test_mail_record("mailer", {0x4d70 + n, 0, 0, 1}, 1);
        const std::string id = record.id;
        store.claim_script = {{svc::CallStatus::Unavailable, false}};   // the first ask never arrived (nothing was stored), and the service then answers nothing
        store.claims_down = true;
        const size_t loads = store.loads;
        const size_t handovers = store.handovers;
        const size_t from = w[0].got.size();
        CHECK(all ? test_mail_claim_all(w, 0, {id_a, id_b}) : test_mail_claim(w, 0, id_a, 0, true), "the claim is sent");
        CHECK(test_mail_wait_for(store.claims, 3), "the worker asks again");
        if (with_logout) {
            w[0].s->finish("the player logs out");
            CHECK(store.handovers == handovers, "the end of the session saves no bags next to the claim");
        }
        {
            std::lock_guard<std::mutex> lock(store.store_mu);
            store.claim_script.push_back({svc::CallStatus::Refused, false, true});   // the repeat is answered "claimed already", and it wrote nothing
        }
        store.claims_down = false;
        if (with_logout) {
            CHECK(test_mail_wait_for(store.handovers, handovers + 1), "the worker saves the bags when it has that answer");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (w.fx.world->character_writes_pending(id) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(test_claim_saved_rows_with(store, att_a) == 0 && test_claim_saved_rows_with(store, att_b) == 0 && test_claim_saved_quantity(store, held_guid) == 1,
                  "the saved bags hold none of the items and the stack has its old quantity");
        } else {
            w.settle();
            CHECK(test_claim_rows_with(*w[0].s, att_a) == 0 && test_claim_rows_with(*w[0].s, att_b) == 0, "the items went back");
            const svc::InventoryItem* stack = w[0].s->inventory().find_guid(held_guid);
            CHECK(stack != nullptr && stack->quantity == 1, "the stack has its old quantity");
            CHECK(!w[0].s->mail_claim_pending(), "the claim is no longer in flight");
            CHECK(test_claim_pawn_payloads(w, 0, from) >= 2, "the client is sent its storages again");
            CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0 && test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(),
                  "it is told nothing of a claim");
            CHECK(store.loads == loads + 1, "the mails are read again");
            w.t += seconds(61);
            w.step();
            CHECK(store.handovers == handovers + 1 && test_claim_saved_rows_with(store, att_a) == 0 && test_claim_saved_rows_with(store, att_b) == 0 && test_claim_saved_quantity(store, held_guid) == 1,
                  "the next save holds none of the items and the stack at its old quantity");
        }
        CHECK(store.by_id(id_a) && !store.by_id(id_a)->items[0].claimed && (!all || !store.by_id(id_b)->items[0].claimed), "the attachments stay unclaimed in the store");
        // a later claim gives each item once
        size_t who = 0;
        if (with_logout) {
            ClaimLogin login(w, store, record);
            login.start();
            CHECK(login.done(std::chrono::seconds(5)), "the log-in is not held");
            const LoginResult r = login.result.get();
            CHECK(r.accepted && test_claim_loaded_rows_with(r, att_a) == 0, "it loads the stored bags: no item of the claim");
            w[0].s.reset();
            who = 1;
            if (!w.enter(who, r.character, identity())) {
                CHECK(false, "the second session joins");
                return;
            }
            CHECK(test_mail_fetch(w, who), "the new session's list is sent");
            w.settle();
        }
        CHECK(all ? test_mail_claim_all(w, who, {id_a, id_b}) : test_mail_claim(w, who, id_a, 0, true), "the claim is made again");
        w.settle();
        const svc::InventoryItem* stack = w[who].s->inventory().find_guid(held_guid);
        CHECK(stack != nullptr && stack->quantity == 3 && store.by_id(id_a)->items[0].claimed, "it tops the stack up once and marks the mail");
        CHECK(!all || (test_claim_rows_with(*w[who].s, att_b) == 1 && store.bag_items_with(att_b) == 1 && store.by_id(id_b)->items[0].claimed), "and gives the other item once");
    }
}

static void test_mail_claimed_already_without_a_lost_answer_and_not_found_are_refusals() {
    std::printf("'claimed already' to a first ask, and NotFound to a repeat after a lost answer, are refusals: the item goes back\n");
    std::fflush(stdout);
    for (const bool first_ask : {true, false}) {
        TestMailStore store;
        const ClaimCase c(store, first_ask ? 77 : 78, first_ask ? 7419 : 7421);
        PartyWorld w;
        if (!test_claim_join(w, 0, store, "mailer", first_ask ? 0x4d4e : 0x4d4f)) {
            CHECK(false, "the character joins");
            return;
        }
        CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
        w.settle();
        // a first ask answered "claimed already" was claimed by something else; a repeat after an ask that never arrived and answered NotFound has no mail any more
        if (first_ask) store.claim_script = {{svc::CallStatus::Refused, false, true}};
        else store.claim_script = {{svc::CallStatus::Unavailable, false}, {svc::CallStatus::NotFound, false}};
        const size_t from = w[0].got.size();
        CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
        w.settle();
        CHECK(test_claim_rows_with(*w[0].s, c.item) == 0, "the item went back");
        CHECK(store.by_id(c.id) && !store.by_id(c.id)->items[0].claimed, "the mail stays unclaimed");
        CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "no claim answer is sent");
        CHECK(!w[0].s->mail_claim_pending(), "the claim is no longer in flight");
    }
}

static void test_mail_logout_with_an_unanswered_claim_the_worker_keeps_asking_and_saves_the_answer() {
    std::printf("a logout while a claim has no answer: the worker keeps asking, writes no bag meanwhile, and saves the bags the answer decides\n");
    std::fflush(stdout);
    for (const bool stored : {true, false}) {
        TestMailStore store;
        const ClaimCase c(store, stored ? 79 : 85, stored ? 7422 : 7435);
        PartyWorld w;
        if (!test_claim_join(w, 0, store, "mailer", stored ? 0x4d57 : 0x4d58)) {
            CHECK(false, "the character joins");
            return;
        }
        CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
        w.settle();
        const std::string id = w[0].s->character_id();
        store.claim_script = {{svc::CallStatus::Unavailable, stored}};   // stored and lost, or never arrived
        store.claims_down = true;
        CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
        CHECK(test_mail_wait_for(store.claims, 3), "the worker asks again");
        const size_t handovers = store.handovers;
        w[0].s->finish("the player logs out");
        CHECK(store.handovers == handovers, "the end of the session saves no bags");
        const size_t asked = store.claims;
        CHECK(test_mail_wait_for(store.claims, asked + 3), "the worker goes on asking after the session ended");
        CHECK(store.handovers == handovers, "and writes no bag while no answer is known");
        CHECK(w.fx.world->character_writes_pending(id), "the character has an unanswered claim");
        if (!stored) {
            std::lock_guard<std::mutex> lock(store.store_mu);
            store.claim_script.push_back({svc::CallStatus::Refused, false});
        }
        store.claims_down = false;
        CHECK(test_mail_wait_for(store.handovers, handovers + 1), "the worker saves the bags when the answer comes");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (w.fx.world->character_writes_pending(id) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(!w.fx.world->character_writes_pending(id), "and the claim is closed after that save");
        CHECK(test_claim_saved_rows_with(store, c.item) == (stored ? 1u : 0u), stored ? "the saved bags hold the item once and the mail is claimed" : "the saved bags hold no item and the mail is not claimed");
        CHECK(store.by_id(c.id) && store.by_id(c.id)->items[0].claimed == stored && store.bag_items_with(c.item) == (stored ? 1u : 0u), "the store's bags and mail agree");
    }
}

static void test_mail_a_claim_settled_at_logout_stays_open_until_its_bags_are_queued() {
    std::printf("a claim the session settles itself at logout stays open while its bags are handed over, and is closed after them\n");
    std::fflush(stdout);
    for (const bool answered : {true, false}) {
        TestMailStore store;
        const ClaimCase c(store, answered ? 86 : 87, answered ? 7436 : 7437, answered ? content_fixture::item::kChest : content_fixture::item::kLegs);
        PartyWorld w;
        if (!test_claim_join(w, 0, store, "mailer", answered ? 0x4d59 : 0x4d5a)) {
            CHECK(false, "the character joins");
            return;
        }
        CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
        w.settle();
        const std::string id = w[0].s->character_id();
        std::atomic<int> open_during_save{-1};
        store.on_save = [&] { open_during_save = w.fx.world->character_writes_pending(id) ? 1 : 0; };
        if (answered) {
            // the worker has the answer, and the session (which does not tick) has not applied it
            CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
            CHECK(test_mail_wait_for(store.claims, 1), "the worker asked");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            // the claim is still queued behind a load: the worker never asks for it
            store.block(1);
            CHECK(test_mail_refresh(w, 0), "a refresh keeps the worker busy");
            CHECK(test_mail_wait_for(store.loads, 2), "the worker took the load");
            CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent behind it");
        }
        const size_t handovers = store.handovers;
        w[0].s->finish("the player logs out");
        CHECK(store.handovers == handovers + 1, "the end of the session saves the bags");
        CHECK(open_during_save == 1, "the claim was still open while the bags were handed over");
        CHECK(!w.fx.world->character_writes_pending(id), "and it is closed after them");
        if (!answered) store.release();
        store.on_save = nullptr;
    }
}

static void test_mail_an_unanswered_claim_is_warned_about_at_most_once_in_the_interval() {
    std::printf("a claim that stays unanswered is warned about when it first gets no answer and then at most once in the interval, naming the character and the mail\n");
    std::fflush(stdout);
    std::ostringstream out;
    auto logger = std::make_shared<spdlog::logger>("claim_warn", std::make_shared<spdlog::sinks::ostream_sink_mt>(out));
    logger->set_level(spdlog::level::trace);
    const auto previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);
    AskingSink sink;
    MailJobs jobs(MailJobs::Config{std::chrono::milliseconds(1), std::chrono::milliseconds(4), std::chrono::milliseconds(200)});
    const uint64_t ticket = jobs.submit(test_claim_job(MailJobKind::Claim, "char-a"), &sink);
    const auto began = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds(700)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const size_t asks = sink.ask_count();
    logger->flush();
    const std::string text = out.str();
    const std::string phrase = "bags are not saved until the service answers";
    size_t lines = 0;
    for (size_t at = text.find(phrase); at != std::string::npos; at = text.find(phrase, at + 1)) ++lines;
    CHECK(asks >= 40, "the claim was asked many times in that time");
    CHECK(lines >= 2 && lines <= 5, "and the warning came when it first had no answer and then about once in the interval, not once per ask");
    CHECK(text.find("char-a") != std::string::npos && text.find(test_mail_hex(1)) != std::string::npos, "it names the character and the mail");
    CHECK(text.find("warning") != std::string::npos, "at the warning level");
    sink.answer = true;
    MailJobAnswer answer;
    bool got = false;
    for (int i = 0; i < 500 && !got; ++i) {
        got = jobs.take(ticket, answer);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    jobs.claim_closed(ticket);
    logger->flush();
    size_t after = 0;
    const std::string text2 = out.str();
    for (size_t at = text2.find(phrase); at != std::string::npos; at = text2.find(phrase, at + 1)) ++after;
    CHECK(got && after <= lines + 1, "once the service answers the warnings stop");
    spdlog::set_default_logger(previous);
}

static void test_mail_worker_asks_again_at_a_growing_capped_pace_and_serves_other_characters() {
    std::printf("the worker asks the same claim again at a growing, capped pace; another character's job is served meanwhile and the claiming character's own later job waits\n");
    std::fflush(stdout);
    {
        AskingSink sink;
        MailJobs jobs(MailJobs::Config{std::chrono::milliseconds(20), std::chrono::milliseconds(60)});
        const uint64_t ticket = jobs.submit(test_claim_job(MailJobKind::Claim, "char-a"), &sink);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sink.ask_count() < 6 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(sink.ask_count() >= 6, "the claim is asked again and again while it has no answer");
        std::vector<std::chrono::steady_clock::time_point> asks;
        {
            std::lock_guard<std::mutex> lock(sink.mu);
            asks = sink.asks;
        }
        std::vector<long long> gaps;
        for (size_t i = 1; i < asks.size() && i < 6; ++i) gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(asks[i] - asks[i - 1]).count());
        CHECK(gaps.size() == 5 && gaps[0] >= 15 && gaps[1] >= 30 && gaps[2] >= 45, "the wait grows: about 20, 40 and 60 milliseconds");
        CHECK(gaps.size() == 5 && gaps[3] >= 45 && gaps[4] >= 45 && gaps[3] < 400 && gaps[4] < 400, "and stops growing at the cap");
        MailJobAnswer answer;
        CHECK(!jobs.take(ticket, answer), "no answer is handed out while the service has none");
        CHECK(jobs.claim_open("char-a"), "the claim is open");
        sink.answer = true;
        bool got = false;
        for (int i = 0; i < 500 && !got; ++i) {
            got = jobs.take(ticket, answer);
            if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(got && answer.claim.status == svc::CallStatus::Ok, "the answer comes when the service gives one");
        CHECK(jobs.claim_open("char-a"), "the claim stays open until the session has applied the answer");
        jobs.claim_closed(ticket);
        CHECK(!jobs.claim_open("char-a"), "and is closed then");
    }
    {
        AskingSink sink;
        MailJobs jobs(MailJobs::Config{std::chrono::milliseconds(400), std::chrono::milliseconds(400)});
        const uint64_t claim = jobs.submit(test_claim_job(MailJobKind::Claim, "char-a"), &sink);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sink.ask_count() < 1 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(sink.ask_count() == 1, "the claim was asked once");
        const uint64_t own_load = jobs.submit(test_claim_job(MailJobKind::Load, "char-a"), &sink);
        const uint64_t other_load = jobs.submit(test_claim_job(MailJobKind::Load, "char-b"), &sink);
        MailJobAnswer answer;
        bool other_done = false;
        for (int i = 0; i < 100 && !other_done; ++i) {
            other_done = jobs.take(other_load, answer);
            if (!other_done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(other_done && sink.ask_count() == 1, "another character's job is served while the claim waits to be asked again: the worker does not sleep");
        CHECK(!jobs.take(own_load, answer) && sink.loads_of("char-a") == 0, "the claiming character's own later job waits for the claim");
        CHECK(!jobs.cancel(claim), "a claim that was asked once cannot be taken back");
        sink.answer = true;
        bool own_done = false;
        for (int i = 0; i < 400 && !own_done; ++i) {
            own_done = jobs.take(own_load, answer);
            if (!own_done) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(own_done && sink.loads_of("char-a") == 1, "it runs after the claim is answered");
    }
}

static void test_mail_logout_during_a_claim_saves_the_bags_the_answer_decides() {
    std::printf("a logout during a claim with no login after it: the worker saves the bags with the item when the claim is stored, and without it when it is not\n");
    std::fflush(stdout);
    for (const bool stored : {true, false}) {
        TestMailStore store;
        const ClaimCase c(store, stored ? 68 : 69, stored ? 7409 : 7410);
        PartyWorld w;
        if (!test_claim_join(w, 0, store, "mailer", stored ? 0x4d47 : 0x4d48)) {
            CHECK(false, "the character joins");
            return;
        }
        CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
        w.settle();
        if (!stored) store.claim_script = {{svc::CallStatus::Refused, false}};
        store.block(2);
        CHECK(test_mail_claim(w, 0, c.id, 0, true), "the claim is sent");
        CHECK(test_mail_wait_for(store.claims, 1), "the worker has the claim at the store");
        const size_t handovers = store.handovers;
        w[0].s->finish("the player logs out");
        CHECK(store.handovers == handovers, "the end of the session saves no bags next to the claim");
        store.release();
        CHECK(test_mail_wait_for(store.handovers, handovers + 1), "the worker saves the bags when the claim is answered");
        size_t rows = 0;
        {
            std::lock_guard<std::mutex> lock(store.store_mu);
            for (const svc::InventoryItem& item : store.saved_bags.empty() ? std::vector<svc::InventoryItem>() : store.saved_bags.back()) rows += item.instance_guid == c.item ? 1 : 0;
        }
        CHECK(rows == (stored ? 1u : 0u), stored ? "the saved bags hold the item once and the mail is claimed" : "the saved bags hold no item and the mail is not claimed");
        CHECK(store.by_id(c.id) && store.by_id(c.id)->items[0].claimed == stored, "the mail and the bags say the same");
        CHECK(store.bag_items_with(c.item) == (stored ? 1u : 0u), "the store's bags and the store's mail agree");
    }
}

static void test_mail_a_logout_before_the_worker_took_the_claim_cancels_it() {
    std::printf("a logout while the claim waits in the worker's queue cancels it: nothing is stored and the saved bags have no item\n");
    std::fflush(stdout);
    TestMailStore store;
    const ClaimCase queued(store, 71, 7412, content_fixture::item::kLegs);
    PartyWorld w;
    if (!test_claim_join(w, 0, store, "mailer", 0x4d49)) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.block(1);   // the worker is busy with a load, so the claim behind it has not started
    CHECK(test_mail_refresh(w, 0), "a refresh keeps the worker busy");
    CHECK(test_mail_wait_for(store.loads, 2), "the worker took the load");
    CHECK(test_mail_claim(w, 0, queued.id, 0, true), "the claim is sent behind it");
    w[0].s->finish("the player logs out");
    store.release();
    w.settle();
    CHECK(store.claims == 0, "the claim never reached the store");
    CHECK(store.by_id(queued.id) && !store.by_id(queued.id)->items[0].claimed, "the mail is not claimed");
    bool saved_with_item = false;
    for (const std::vector<svc::InventoryItem>& bag : store.saved_bags) {
        for (const svc::InventoryItem& item : bag) saved_with_item = saved_with_item || item.instance_guid == queued.item;
    }
    CHECK(!store.saved_bags.empty() && !saved_with_item, "the bags the end of the session saved hold no item of it");
}

AOC_REGISTER_TEST(test_mail_refused_claim_gives_the_item_back_and_resyncs_the_client);
AOC_REGISTER_TEST(test_mail_rollback_keeps_a_bag_change_made_while_the_claim_was_in_flight);
AOC_REGISTER_TEST(test_mail_claim_in_flight_holds_the_periodic_save);
AOC_REGISTER_TEST(test_mail_second_claim_waits_for_the_one_in_flight);
AOC_REGISTER_TEST(test_mail_claim_with_a_lost_answer_keeps_the_item);
AOC_REGISTER_TEST(test_login_waits_for_the_claim_then_loads_the_stored_bags);
AOC_REGISTER_TEST(test_login_after_a_refused_top_up_and_a_relog_finds_the_stack_as_it_was);
AOC_REGISTER_TEST(test_login_bound_refuses_the_login_and_loads_nothing);
AOC_REGISTER_TEST(test_a_claim_waits_while_a_login_of_the_character_is_pending);
AOC_REGISTER_TEST(test_a_refused_ticket_marks_nothing_and_a_redeemed_login_holds_a_new_claim);
AOC_REGISTER_TEST(test_a_claim_the_worker_refuses_at_submit_is_undone_and_made_once_when_the_mark_ends);
AOC_REGISTER_TEST(test_mail_jobs_refuse_a_new_claim_of_a_character_with_a_login_pending);
AOC_REGISTER_TEST(test_mail_claim_with_every_answer_lost_keeps_the_item_until_the_service_answers);
AOC_REGISTER_TEST(test_mail_service_down_for_many_asks_makes_no_bag_write_and_the_tick_goes_on);
AOC_REGISTER_TEST(test_mail_claimed_already_on_the_repeat_after_a_lost_answer_is_a_refusal);
AOC_REGISTER_TEST(test_mail_claimed_already_without_a_lost_answer_and_not_found_are_refusals);
AOC_REGISTER_TEST(test_mail_logout_with_an_unanswered_claim_the_worker_keeps_asking_and_saves_the_answer);
AOC_REGISTER_TEST(test_mail_a_claim_settled_at_logout_stays_open_until_its_bags_are_queued);
AOC_REGISTER_TEST(test_mail_an_unanswered_claim_is_warned_about_at_most_once_in_the_interval);
AOC_REGISTER_TEST(test_mail_worker_asks_again_at_a_growing_capped_pace_and_serves_other_characters);
AOC_REGISTER_TEST(test_mail_logout_during_a_claim_saves_the_bags_the_answer_decides);
AOC_REGISTER_TEST(test_mail_a_logout_before_the_worker_took_the_claim_cancels_it);
