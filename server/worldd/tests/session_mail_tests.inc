// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_mail_tests.inc: the mail window's nine calls, the session's cache, the claim order and the unread count.
//
//  A fake mail store (PositionSink) keeps the character's mails the way the character service does: live mails newest first, a whole mail
//  written at once, and the item's instance guid on every attachment.  The tests send the client's calls through the fake client and read the
//  PlayerController's chains (the headers, the storage events and the status answers) and the social component's blocks (the detail, the claim
//  answers and the unread count).
// ============================================================================
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

static constexpr size_t kMailTestPage = 20;

static const schema::ClassSchema& test_mail_social_cls() { return class_named("UIntrepidSocialComponent"); }

static const schema::ClassSchema& test_mail_pc_cls() { return class_named("AoCPlayerControllerBP_C"); }

/// The 32 hex digits of a number: a mail's id or an item's instance guid.
static std::string test_mail_hex(uint32_t n) {
    char text[33];
    std::snprintf(text, sizeof text, "%032x", n);
    return text;
}

/// One item as the character service stores it: the mail's attachment and the bag's copy carry the same fields.
static svc::InventoryItem test_mail_item(const std::string& guid, int64_t record, svc::ItemContainer container = svc::ItemContainer::Backpack,
                                         int slot = 0, int32_t quantity = 1, float durability = 0.0f) {
    svc::InventoryItem item;
    item.container = container;
    item.slot = slot;
    item.item_record_id = record;
    item.instance_guid = guid;
    item.quantity = quantity;
    item.quality = 1;
    item.durability = durability;
    item.created_at_ms = 1'700'000'000'000ll;
    return item;
}

static svc::MailItem test_mail_attachment(const std::string& guid, int64_t record, int32_t quantity = 1, bool claimed = false,
                                          float durability = 0.0f) {
    svc::MailItem item;
    item.item = test_mail_item(guid, record, svc::ItemContainer::Backpack, 0, quantity, durability);
    item.claimed = claimed;
    return item;
}

static svc::Mail test_mail_mail(const std::string& id, const char* sender, const char* subject, const char* body, int32_t status,
                                std::vector<svc::MailItem> items) {
    svc::Mail mail;
    mail.id = id;
    mail.sender = sender;
    mail.subject = subject;
    mail.body = body;
    mail.status = status;
    mail.issued_at_ms = 1'700'000'000'000ll;
    mail.expires_at_ms = 1'700'003'600'000ll;
    mail.items = std::move(items);
    return mail;
}

static svc::Character test_mail_record(const char* name, const std::array<uint32_t, 4>& words, int32_t unread) {
    svc::Character character = party_record(name, words);
    character.unread_mail_count = unread;
    return character;
}

/// A character service's mail store: the mails, the last bag the session saved, and the answer each call gives.  A stale save or a service
/// that is down is one answer pushed before the call.
struct TestMailStore : PositionSink {
    void save(const std::string&, const svc::Position&) override {}
    std::vector<svc::Mail>          mails;      // newest first, as load_mails answers
    std::vector<svc::InventoryItem> bag_items;  // the last save_inventory_now
    std::vector<svc::WalletEntry>   bag_wallet;
    std::vector<svc::ProcessingJob> bag_jobs;
    std::vector<int64_t>            bag_recipes;
    std::vector<svc::WarehouseItem> bag_warehouse;
    std::vector<std::string>        ops;        // "load", "bag", "mail <id>", "create", in call order
    svc::NewMail                    last_created;
    std::deque<svc::CallStatus>     bag_answers;
    std::deque<svc::CallStatus>     mail_answers;
    svc::CallStatus                 load_status = svc::CallStatus::Ok;
    svc::CallStatus                 create_status = svc::CallStatus::Ok;
    std::string                     create_reason = "the store refused the mail";
    std::atomic<size_t>             loads{0}, bag_saves{0}, mail_saves{0}, creates{0};
    uint32_t                        next_mail = 500, next_item = 5'000;
    int                             gate_bits = 0;
    std::atomic<bool>               gate_open{true};
    std::mutex                      gate_mu;
    std::condition_variable         gate_cv;

    // A call may be held at the worker (the test then sees the tick go on while the store is busy): bit 1 load, 2 bags, 4 mail, 8 create.
    void block(int bits) {
        gate_bits = bits;
        gate_open = false;
    }
    void release() {
        gate_open = true;
        gate_cv.notify_all();
    }
    void wait_gate(int bit) {
        if ((gate_bits & bit) == 0) return;
        std::unique_lock<std::mutex> lock(gate_mu);
        gate_cv.wait(lock, [this] { return gate_open.load(); });
    }

    svc::MailsResult load_mails(const std::string&) override {
        ++loads;
        ops.push_back("load");
        wait_gate(1);
        svc::MailsResult result;
        if (load_status != svc::CallStatus::Ok) {
            result.status = load_status;
            result.reason = "the store is down";
            return result;
        }
        result.status = svc::CallStatus::Ok;
        result.mails = mails;
        return result;
    }

    svc::MailSaveResult save_mail(const std::string&, const svc::Mail& mail) override {
        ++mail_saves;
        ops.push_back("mail " + mail.id);
        wait_gate(4);
        svc::MailSaveResult result;
        result.status = mail_answers.empty() ? svc::CallStatus::Ok : mail_answers.front();
        if (!mail_answers.empty()) mail_answers.pop_front();
        if (result.status != svc::CallStatus::Ok) {
            result.reason = "the mail did not save";
            return result;
        }
        for (svc::Mail& stored : mails) {
            if (stored.id != mail.id) continue;
            stored = mail;
            return result;
        }
        result.status = svc::CallStatus::NotFound;
        result.reason = "no such mail";
        return result;
    }

    svc::MailResult create_mail(const std::string&, const svc::NewMail& mail) override {
        ++creates;
        ops.push_back("create");
        wait_gate(8);
        last_created = mail;
        svc::MailResult result;
        if (create_status != svc::CallStatus::Ok) {
            result.status = create_status;
            result.reason = create_reason;
            return result;
        }
        svc::Mail made;
        made.id = test_mail_hex(next_mail++);
        made.sender = mail.sender;
        made.subject = mail.subject;
        made.body = mail.body;
        made.issued_at_ms = 1'700'000'000'000ll;
        made.expires_at_ms = mail.expires_in_ms == 0 ? 0 : made.issued_at_ms + mail.expires_in_ms;
        for (svc::MailItem item : mail.items) {
            item.item.instance_guid = test_mail_hex(next_item++);
            item.item.created_at_ms = 1'700'000'000'000ll;
            made.items.push_back(std::move(item));
        }
        mails.insert(mails.begin(), made);
        result.status = svc::CallStatus::Ok;
        result.mail = made;
        return result;
    }

    svc::CallStatus save_inventory_now(const std::string&, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet,
                                       std::vector<svc::ProcessingJob> jobs, std::vector<int64_t> known_recipes,
                                       std::vector<svc::WarehouseItem> warehouse) override {
        ++bag_saves;
        ops.push_back("bag");
        wait_gate(2);
        bag_items = std::move(items);
        bag_wallet = std::move(wallet);
        bag_jobs = std::move(jobs);
        bag_recipes = std::move(known_recipes);
        bag_warehouse = std::move(warehouse);
        const svc::CallStatus answer = bag_answers.empty() ? svc::CallStatus::Ok : bag_answers.front();
        if (!bag_answers.empty()) bag_answers.pop_front();
        return answer;
    }

    const svc::Mail* by_id(const std::string& id) const {
        for (const svc::Mail& mail : mails) {
            if (mail.id == id) return &mail;
        }
        return nullptr;
    }

    size_t bag_items_with(const std::string& guid) const {
        size_t n = 0;
        for (const svc::InventoryItem& item : bag_items) n += item.instance_guid == guid ? 1 : 0;
        return n;
    }
};

/// A copy of the small content.db whose potions are Quest items: a claim routes them through grant(), which tops up a held
/// stack and splits an over-sized one, the two placements whose own events the client must be sent.
static const std::filesystem::path& test_mail_quest_db() {
    static const std::filesystem::path dir = content_fixture::make_temp_dir("worldd_mail_quest_");
    static const std::filesystem::path db = dir / "content.db";
    static const bool made = [] {
        if (!content_fixture::make_db(db)) return false;
        sqlite3* handle = nullptr;
        if (sqlite3_open_v2(db.string().c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            return false;
        }
        char* error = nullptr;
        const int rc = sqlite3_exec(handle, "CREATE TABLE quest_item (item_guid INTEGER PRIMARY KEY, major INTEGER NOT NULL); "
                                            "INSERT INTO quest_item VALUES (6064631187700000007, 0);",
                                    nullptr, nullptr, &error);
        if (error) sqlite3_free(error);
        sqlite3_close(handle);
        return rc == SQLITE_OK;
    }();
    if (!made) std::printf("  [FAIL] the quest-item content.db cannot be made\n");
    return db;
}

/// The four words of a GUID on a call's parameters (`MailGuid.A` to `.D`, or `MailboxGuid.A` to `.D`).
static bool test_mail_put_guid(const schema::RpcSchema& rpc, repl::RpcParams& params, const char* prefix, const repl::GuidValue& guid) {
    const uint32_t words[4] = {guid.a, guid.b, guid.c, guid.d};
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        ok = repl::set_rpc_param(rpc, params, std::string(prefix) + "." + "ABCD"[i], repl::Value::i64(static_cast<int32_t>(words[i]))) && ok;
    }
    return ok;
}

/// The client's mailbox GUID of the tests: any GUID does, because the server shows the mailbox window it names nowhere.
static const repl::GuidValue kMailTestMailbox{5100u, 0u, 0u, 0u};

static bool test_mail_send_social(PartyWorld& w, size_t who, const char* name, const repl::RpcParams& params) {
    return test_social_send(w, who, name, params);
}

static bool test_mail_fetch(PartyWorld& w, size_t who) { return party_send(w, who, "ServerFetchMail", {}); }

static bool test_mail_fetch_num(PartyWorld& w, size_t who) { return party_send(w, who, "ServerFetchMailNum", {}); }

static bool test_mail_refresh(PartyWorld& w, size_t who) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerRefreshMailHeaders");
    repl::RpcParams params;
    if (!rpc || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox)) return false;
    return test_mail_send_social(w, who, "ServerRefreshMailHeaders", params);
}

static bool test_mail_detail(PartyWorld& w, size_t who, const std::string& mail_id) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerGetMailData");
    const auto guid = items::guid_of(mail_id);
    repl::RpcParams params;
    if (!rpc || !guid || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox) ||
        !test_mail_put_guid(*rpc, params, "MailGuid", *guid)) {
        return false;
    }
    return test_mail_send_social(w, who, "ServerGetMailData", params);
}

/// `with_index` false leaves MailItemIndex at its default on the wire (a default value is absent on the wire).
static bool test_mail_claim(PartyWorld& w, size_t who, const std::string& mail_id, int32_t index, bool with_index) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerClaimMailItem");
    const auto guid = items::guid_of(mail_id);
    repl::RpcParams params;
    if (!rpc || !guid || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox) ||
        !test_mail_put_guid(*rpc, params, "MailGuid", *guid)) {
        return false;
    }
    if (with_index && !repl::set_rpc_param(*rpc, params, "MailItemIndex", repl::Value::i64(index))) return false;
    return test_mail_send_social(w, who, "ServerClaimMailItem", params);
}

static bool test_mail_currency(PartyWorld& w, size_t who, const std::string& mail_id) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerClaimMailCurrency");
    const auto guid = items::guid_of(mail_id);
    repl::RpcParams params;
    if (!rpc || !guid || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox) ||
        !test_mail_put_guid(*rpc, params, "MailGuid", *guid)) {
        return false;
    }
    return test_mail_send_social(w, who, "ServerClaimMailCurrency", params);
}

static bool test_mail_claim_all(PartyWorld& w, size_t who, const std::vector<std::string>& ids) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerClaimAllMail");
    repl::RpcParams params;
    if (!rpc || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox)) return false;
    repl::ArrayValue list;
    for (const std::string& id : ids) {
        const auto guid = items::guid_of(id);
        if (!guid) return false;
        repl::ElementState element;
        element.values[1] = repl::Value::i64(static_cast<int64_t>(guid->a));
        element.values[2] = repl::Value::i64(static_cast<int64_t>(guid->b));
        element.values[3] = repl::Value::i64(static_cast<int64_t>(guid->c));
        element.values[4] = repl::Value::i64(static_cast<int64_t>(guid->d));
        list.elements.push_back(std::move(element));
    }
    if (!repl::set_rpc_param(*rpc, params, "MailGuids", repl::Value(std::move(list)))) return false;
    return test_mail_send_social(w, who, "ServerClaimAllMail", params);
}

static bool test_mail_bulk_status(PartyWorld& w, size_t who, const std::vector<std::string>& ids, int32_t status) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerBulkUpdateMailStatus");
    repl::RpcParams params;
    if (!rpc || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox)) return false;
    repl::ArrayValue list;
    for (const std::string& id : ids) {
        const auto guid = items::guid_of(id);
        if (!guid) return false;
        repl::ElementState element;
        element.values[1] = repl::Value::i64(static_cast<int64_t>(guid->a));
        element.values[2] = repl::Value::i64(static_cast<int64_t>(guid->b));
        element.values[3] = repl::Value::i64(static_cast<int64_t>(guid->c));
        element.values[4] = repl::Value::i64(static_cast<int64_t>(guid->d));
        list.elements.push_back(std::move(element));
    }
    if (!repl::set_rpc_param(*rpc, params, "MailGuids", repl::Value(std::move(list))) ||
        !repl::set_rpc_param(*rpc, params, "Status", repl::Value::u64(static_cast<uint64_t>(status)))) {
        return false;
    }
    return test_mail_send_social(w, who, "ServerBulkUpdateMailStatus", params);
}

/// `with_status` false leaves Status at its default on the wire.
static bool test_mail_status(PartyWorld& w, size_t who, const std::string& mail_id, int32_t status, bool with_status) {
    const schema::RpcSchema* rpc = test_mail_social_cls().rpc("ServerUpdateMailStatus");
    const auto guid = items::guid_of(mail_id);
    repl::RpcParams params;
    if (!rpc || !guid || !test_mail_put_guid(*rpc, params, "MailboxGuid", kMailTestMailbox) ||
        !test_mail_put_guid(*rpc, params, "MailGuid", *guid)) {
        return false;
    }
    if (with_status && !repl::set_rpc_param(*rpc, params, "Status", repl::Value::u64(static_cast<uint64_t>(status)))) return false;
    return test_mail_send_social(w, who, "ServerUpdateMailStatus", params);
}

static std::vector<repl::RpcCall> test_mail_pc_calls(const PartyWorld& w, size_t who, size_t from) { return test_guild_pc_calls(w, who, from); }

static std::vector<repl::RpcCall> test_mail_social_calls(const PartyWorld& w, size_t who, size_t from) {
    std::vector<repl::RpcCall> out;
    if (!w.p[who].s) return out;
    const auto social = w.p[who].s->bootstrap().setup().guids.social;
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& given = w.p[who].got[i];
        const auto blocks = repl::read_content_blocks(given.content.data(), given.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& block : *blocks) {
            if (block.header.is_actor || block.header.has_rep_layout || !(block.header.object == social)) continue;
            size_t pos = block.payload_pos;
            auto calls = repl::read_rpc_fields(test_mail_social_cls(), given.content.data(), block.payload_pos + block.payload_bits, pos);
            if (!calls) continue;
            for (repl::RpcCall& call : *calls) out.push_back(std::move(call));
        }
    }
    return out;
}

static size_t test_mail_count_named(const std::vector<repl::RpcCall>& calls, const char* name) {
    size_t n = 0;
    for (const repl::RpcCall& call : calls) n += call.rpc && std::string(call.rpc->name) == name ? 1 : 0;
    return n;
}

static const repl::RpcCall* test_mail_first_named(const std::vector<repl::RpcCall>& calls, const char* name) {
    for (const repl::RpcCall& call : calls) {
        if (call.rpc && std::string(call.rpc->name) == name) return &call;
    }
    return nullptr;
}

/// The four words of a GUID parameter (`MailGuid.A` to `.D`), or nullopt when the call does not carry them.
static std::optional<repl::GuidValue> test_mail_guid_param(const repl::RpcCall& call, const char* prefix) {
    uint32_t words[4]{};
    for (int i = 0; i < 4; ++i) {
        const repl::Value* value = repl::rpcf::value_of(*call.rpc, call.params, std::string(prefix) + "." + "ABCD"[i]);
        const auto* number = value ? value->get_if<int64_t>() : nullptr;
        if (!number) return std::nullopt;
        words[i] = static_cast<uint32_t>(*number);
    }
    return repl::GuidValue{words[0], words[1], words[2], words[3]};
}

/// A page of ClientUpdateMailHeaders: its headers (none for the empty list), its version and bComplete.
static bool test_mail_page(const repl::RpcCall& call, std::vector<repl::MailHeader>& headers, bool& has_version, uint64_t& version, bool& complete) {
    headers.clear();
    const repl::Value* field = repl::rpcf::value_of(*call.rpc, call.params, "Headers");
    const auto* list = field ? field->get_if<repl::ArrayValue>() : nullptr;
    if (field && !list) return false;
    if (list) {
        for (const repl::ElementState& element : list->elements) {
            const repl::Value* value = repl::rpcf::element_value(element, 1);
            const auto* header = value ? value->get_if<repl::MailHeader>() : nullptr;
            if (!header) return false;
            headers.push_back(*header);
        }
    }
    const repl::Value* version_value = repl::rpcf::value_of(*call.rpc, call.params, "Version");
    const auto* number = version_value ? version_value->get_if<uint64_t>() : nullptr;
    has_version = number != nullptr;
    version = number ? *number : 0;
    const repl::Value* complete_value = repl::rpcf::value_of(*call.rpc, call.params, "bComplete");
    const auto* flag = complete_value ? complete_value->get_if<bool>() : nullptr;
    if (!flag) return false;
    complete = *flag;
    return true;
}

/// The SocialComponent's NumUnreadMail as the bootstrap holds it (what friends_update sends to the client).
static bool test_mail_unread_state(PartyWorld& w, size_t who, int32_t& out) {
    if (!w[who].s) return false;
    const auto social = w[who].s->bootstrap().setup().guids.social;
    const repl::Subobject* subobject = w[who].s->bootstrap().player_controller().find(social);
    if (!subobject || !subobject->obj.schema) return false;
    const repl::Value* value = subobject->obj.state.get(*subobject->obj.schema, "NumUnreadMail");
    const auto* number = value ? value->get_if<int64_t>() : nullptr;
    if (!number) { out = 0; return true; }   // (not sent yet: the client's state is the schema's default, 0)
    out = static_cast<int32_t>(*number);
    return true;
}

/// True when a chain from `from` on carries a block on the player's social component (the unread count's delta).
static bool test_mail_social_delta(const PartyWorld& w, size_t who, size_t from) {
    if (!w.p[who].s) return false;
    const auto social = w.p[who].s->bootstrap().setup().guids.social;
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& given = w.p[who].got[i];
        const auto blocks = repl::read_content_blocks(given.content.data(), given.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& block : *blocks) {
            if (!block.header.is_actor && block.header.object == social) return true;
        }
    }
    return false;
}

static size_t test_mail_log_count(const std::string& text) {
    const std::string log = g_log.str();
    size_t n = 0;
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) ++n;
    return n;
}

/// True when `call` is ClientClaimMailItem for `mail_id` and item `index`.
static bool test_mail_claim_answer(const repl::RpcCall& call, const std::string& mail_id, int32_t index) {
    const auto guid = test_mail_guid_param(call, "MailGuid");
    const repl::Value* value = repl::rpcf::value_of(*call.rpc, call.params, "MailItemIndex");
    const auto* number = value ? value->get_if<int64_t>() : nullptr;
    const int32_t got = number ? static_cast<int32_t>(*number) : 0;   // (the first index is the wire's default)
    const auto expected = items::guid_of(mail_id);
    return expected && guid && *guid == *expected && got == index;
}

/// True when `call` is ClientUpdateMailStatus for `mail_id` with `status`.
static bool test_mail_status_answer(const repl::RpcCall& call, const std::string& mail_id, int32_t status) {
    const auto guid = test_mail_guid_param(call, "MailGuid");
    const repl::Value* value = repl::rpcf::value_of(*call.rpc, call.params, "Status");
    const auto* number = value ? value->get_if<uint64_t>() : nullptr;
    const auto expected = items::guid_of(mail_id);
    return expected && guid && *guid == *expected && number && *number == static_cast<uint64_t>(status);
}

/// The count of ClientUpdateMailStatus calls in each delivered chain that carries any, from `from` on (the chains the status answer is built in).
static std::vector<size_t> test_mail_status_chain_sizes(const PartyWorld& w, size_t who, size_t from) {
    std::vector<size_t> out;
    if (!w.p[who].s) return out;
    const auto social = w.p[who].s->bootstrap().setup().guids.social;
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& given = w.p[who].got[i];
        const auto blocks = repl::read_content_blocks(given.content.data(), given.content.bit_pos(), 0);
        if (!blocks) continue;
        size_t n = 0;
        for (const repl::BlockView& block : *blocks) {
            if (block.header.is_actor || block.header.has_rep_layout || !(block.header.object == social)) continue;
            size_t pos = block.payload_pos;
            auto calls = repl::read_rpc_fields(test_mail_social_cls(), given.content.data(), block.payload_pos + block.payload_bits, pos);
            if (!calls) continue;
            for (const repl::RpcCall& call : *calls) n += call.rpc && std::string(call.rpc->name) == "ClientUpdateMailStatus" ? 1 : 0;
        }
        if (n != 0) out.push_back(n);
    }
    return out;
}

/// Waits up to 5 s of real time for the worker to reach `want` of a counter (it is off the test's thread).
static bool test_mail_wait_for(const std::atomic<size_t>& n, size_t want) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (n.load() < want && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return n.load() >= want;
}

}  // namespace

static void test_mail_load_does_not_hold_the_tick() {
    std::printf("a load runs on the worker: the tick goes on and the list is sent when the store answers\n");
    std::fflush(stdout);
    TestMailStore store;
    store.mails.push_back(test_mail_mail(test_mail_hex(40), "Sender", "Subject", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d20, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    const size_t from = w[0].got.size();
    store.block(1);
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    CHECK(test_mail_wait_for(store.loads, 1), "the worker took the load");
    w.step();
    CHECK(w[0].s->mail_pending() == 1, "the tick ended with the load still pending");
    CHECK(test_mail_count_named(test_mail_pc_calls(w, 0, from), "ClientUpdateMailHeaders") == 0, "no list is sent before the store answers");
    store.release();
    w.settle();
    CHECK(test_mail_count_named(test_mail_pc_calls(w, 0, from), "ClientUpdateMailHeaders") == 1, "one list is sent after the answer");
    CHECK(w[0].s->mail_pending() == 0, "the load landed");
}

static void test_mail_claim_does_not_hold_the_tick() {
    std::printf("a claim's bag save runs on the worker: the tick goes on and the claim answers when the store answers\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(41), item_guid = test_mail_hex(7301);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d21, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    store.block(2);
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is sent");
    CHECK(test_mail_wait_for(store.bag_saves, 1), "the worker took the bag save");
    w.step();
    CHECK(w[0].s->mail_pending() == 1, "the tick ended with the claim still pending");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "no claim answer is sent before the store answers");
    store.release();
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) != nullptr, "the item is in the bags after the answer");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1, "one claim answer is sent");
    CHECK(store.bag_saves == 1 && store.mail_saves == 1, "one bag save and one mail save");
}

static void test_mail_bulk_status_is_one_job() {
    std::printf("a bulk status of three mails is one worker job: the tick goes on while its first save waits\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(42), b = test_mail_hex(43), c = test_mail_hex(44);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(c, "Sender", "C", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d22, 0, 0, 1}, 3), identity())) {
        CHECK(false, "the character with three mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t saves_before = store.mail_saves;
    const size_t from = w[0].got.size();
    store.block(4);
    CHECK(test_mail_bulk_status(w, 0, {a, b, c}, svc::kMailStatusRead), "ServerBulkUpdateMailStatus names all three");
    CHECK(test_mail_wait_for(store.mail_saves, saves_before + 1), "the worker took the first mail save");
    w.step();
    CHECK(w[0].s->mail_pending() == 1, "the tick ended with the one bulk job pending");
    store.release();
    w.settle();
    CHECK(store.mail_saves == saves_before + 3, "one job wrote all three mails");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientUpdateMailStatus") == 3, "all three are answered");
    CHECK(store.by_id(a) && store.by_id(a)->status == svc::kMailStatusRead && store.by_id(b) && store.by_id(b)->status == svc::kMailStatusRead &&
              store.by_id(c) && store.by_id(c)->status == svc::kMailStatusRead,
          "the store keeps all three read marks");
}

static void test_mail_failed_load_backs_off() {
    std::printf("a failed load is not tried again within ten seconds and its second failure waits twenty\n");
    std::fflush(stdout);
    TestMailStore store;
    store.load_status = svc::CallStatus::Failed;
    store.mails.push_back(test_mail_mail(test_mail_hex(45), "Sender", "Subject", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d23, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    CHECK(store.loads == 1, "the load was tried once");
    CHECK(test_mail_log_count("the next load waits 10 s") == 1, "the failure is logged once with the ten-second step");
    CHECK(!w[0].s->mail_loaded(), "a failed load leaves the cache unloaded");
    CHECK(test_mail_fetch(w, 0), "a second fetch is sent at once");
    w.settle();
    CHECK(store.loads == 1, "the second fetch reads nothing inside the back-off");
    w.t += seconds(9);
    CHECK(test_mail_fetch_num(w, 0), "a count call is sent at nine seconds");
    w.settle();
    CHECK(store.loads == 1, "the count call does not load");
    w.t += seconds(2);
    CHECK(test_mail_fetch(w, 0), "a fetch after ten seconds is sent");
    w.settle();
    CHECK(store.loads == 2, "the load is tried again after the first step");
    CHECK(test_mail_log_count("the next load waits 20 s") == 1, "the second failure backs off twenty seconds");
}

static void test_mail_double_claim_while_in_flight_grants_once() {
    std::printf("a second claim of the same attachment while the first is in flight is ignored: one grant, one answer\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(46), item_guid = test_mail_hex(7302);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d24, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.block(2);
    CHECK(test_mail_claim(w, 0, id, 0, true), "the first claim is sent");
    CHECK(test_mail_wait_for(store.bag_saves, 1), "the worker took the first claim's bags");
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the second claim is sent while the first is in flight");
    CHECK(store.bag_saves == 1, "the second claim writes no bags");
    CHECK(test_mail_log_count("item 0 of " + id + " is already claimed: ignored") == 1, "the second claim is logged as already claimed");
    store.release();
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) != nullptr && store.bag_items_with(item_guid) == 1, "the item is granted once");
    CHECK(store.mail_saves == 1 && store.by_id(id) && store.by_id(id)->items[0].claimed, "the mail is saved claimed once");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1, "one claim answer is sent");
}

static void test_mail_noop_status_makes_no_save() {
    std::printf("a status mark whose bits are already there makes no save and still answers once\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(47);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", svc::kMailStatusRead, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d25, 0, 0, 1}, 0), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t saves_before = store.mail_saves;
    const size_t from = w[0].got.size();
    CHECK(test_mail_status(w, 0, id, svc::kMailStatusRead, true), "the read mark is sent again");
    w.settle();
    CHECK(store.mail_saves == saves_before, "the same bits save nothing");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientUpdateMailStatus") == 1, "the mark is answered once");
    CHECK(store.by_id(id) && store.by_id(id)->status == svc::kMailStatusRead, "the store's mark is unchanged");
}

static void test_mail_session_removed_with_a_job_in_flight() {
    std::printf("a session dropped with a load in flight takes no result and does not crash\n");
    std::fflush(stdout);
    TestMailStore store;
    store.mails.push_back(test_mail_mail(test_mail_hex(48), "Sender", "Subject", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d26, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    store.block(1);
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    CHECK(test_mail_wait_for(store.loads, 1), "the worker took the load");
    w[0].s.reset();
    CHECK(!w[0].s, "the session is gone");
    store.release();
    w.settle();   // (the worker's answer is dropped, never written into the session)
    CHECK(true, "the world went on with no session");
}

static void test_mail_claim_sends_the_top_up_event() {
    std::printf("a claim that tops up a held stack sends the placement's Add, not a bag row with the attachment's guid\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(49), held_guid = test_mail_hex(7303), attachment_guid = test_mail_hex(7304);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(attachment_guid, content_fixture::item::kPotion, 2)}));
    svc::Character character = test_mail_record("mailer", {0x4d27, 0, 0, 1}, 1);
    character.items.push_back(test_mail_item(held_guid, content_fixture::item::kPotion, svc::ItemContainer::Quest, 0, 1));
    PartyWorld w(test_mail_quest_db().string());
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, character, identity())) {
        CHECK(false, "the character with the held potion joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the potion's claim is sent");
    w.settle();
    const svc::InventoryItem* held = w[0].s->inventory().find_guid(held_guid);
    CHECK(held != nullptr && held->quantity == 3, "the held stack grew by the attachment's two");
    CHECK(w[0].s->inventory().find_guid(attachment_guid) == nullptr, "the attachment's guid is not a bag row");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_mail_pc_calls(w, 0, from));
    CHECK(events.size() == 1 && events[0].type == 0, "one Add is sent");
    CHECK(!events.empty() && events[0].item.guid == *items::guid_of(held_guid) && events[0].item.quantity == 2,
          "the Add names the held stack and the amount added");
}

static void test_mail_claim_sends_both_events_of_a_split() {
    std::printf("a claim split over two slots sends both of the placement's Adds\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(50), attachment_guid = test_mail_hex(7305);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(attachment_guid, content_fixture::item::kPotion, 25)}));
    PartyWorld w(test_mail_quest_db().string());
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d28, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the potion's claim is sent");
    w.settle();
    CHECK(store.by_id(id) && store.by_id(id)->items[0].claimed, "the mail is claimed");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_mail_pc_calls(w, 0, from));
    CHECK(events.size() == 2, "both Adds of the placement are sent");
    size_t twenty = 0, five = 0;
    for (const repl::ItemStorageEvent& event : events) {
        twenty += event.item.quantity == 20 ? 1 : 0;
        five += event.item.quantity == 5 ? 1 : 0;
    }
    CHECK(twenty == 1 && five == 1, "the two Adds are the twenty and the five");
}

static void test_mail_retry_after_a_failed_mail_save_keeps_the_events() {
    std::printf("a retry after a failed mail save grants nothing more and sends the first placement's event\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(51), held_guid = test_mail_hex(7306), attachment_guid = test_mail_hex(7307);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(attachment_guid, content_fixture::item::kPotion, 2)}));
    svc::Character character = test_mail_record("mailer", {0x4d29, 0, 0, 1}, 1);
    character.items.push_back(test_mail_item(held_guid, content_fixture::item::kPotion, svc::ItemContainer::Quest, 0, 1));
    PartyWorld w(test_mail_quest_db().string());
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, character, identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.mail_answers = {svc::CallStatus::Refused};
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is sent with a refused mail save");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(held_guid) && w[0].s->inventory().find_guid(held_guid)->quantity == 3, "the top-up is kept");
    CHECK(store.by_id(id) && !store.by_id(id)->items[0].claimed, "the mail stays unclaimed");
    const size_t bag_saves = store.bag_saves;
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is retried");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(held_guid) && w[0].s->inventory().find_guid(held_guid)->quantity == 3, "the retry grants nothing more");
    CHECK(store.bag_saves == bag_saves + 1 && store.by_id(id) && store.by_id(id)->items[0].claimed, "the retry saves the bags and the mail");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_mail_pc_calls(w, 0, from));
    CHECK(events.size() == 1 && events[0].item.guid == *items::guid_of(held_guid) && events[0].item.quantity == 2,
          "the first placement's Add is sent with the retry");
}

static void test_mail_calls_are_registered() {
    std::printf("the nine mail calls are registered on the player controller and the social component\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(1);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(test_mail_hex(7001), content_fixture::item::kChest)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d01, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character with one mail joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    CHECK(test_mail_fetch_num(w, 0), "ServerFetchMailNum is sent");
    CHECK(test_mail_detail(w, 0, id), "ServerGetMailData is sent");
    CHECK(test_mail_refresh(w, 0), "ServerRefreshMailHeaders is sent");
    CHECK(test_mail_claim(w, 0, id, -1, true), "ServerClaimMailItem is sent");
    CHECK(test_mail_currency(w, 0, id), "ServerClaimMailCurrency is sent");
    CHECK(test_mail_claim_all(w, 0, {id}), "ServerClaimAllMail is sent");
    CHECK(test_mail_status(w, 0, id, svc::kMailStatusRead, true), "ServerUpdateMailStatus is sent");
    CHECK(test_mail_bulk_status(w, 0, {id}, svc::kMailStatusRead), "ServerBulkUpdateMailStatus is sent");
    w.settle();
    const char* const names[] = {"ServerFetchMail",       "ServerFetchMailNum",       "ServerGetMailData",        "ServerRefreshMailHeaders",
                                 "ServerClaimMailItem",   "ServerClaimMailCurrency",  "ServerClaimAllMail",       "ServerUpdateMailStatus",
                                 "ServerBulkUpdateMailStatus"};
    for (const char* name : names) {
        CHECK(test_mail_log_count(std::string("ignoring the client's ") + name) == 0, "no mail call is left unanswered by the dispatch");
    }
    CHECK(store.loads >= 1, "the list read the store");
    CHECK(test_mail_social_calls(w, 0, 0).size() >= 1, "a mail answer came back on the social component");
}

static void test_mail_list_shows_the_headers_the_store_holds() {
    std::printf("the list carries each mail's sender, subject, times, attachment flag and status in the store's order\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string first = test_mail_hex(2), second = test_mail_hex(3);
    store.mails.push_back(test_mail_mail(first, "Sender One", "Subject One", "Body One", 0, {test_mail_attachment(test_mail_hex(7011), content_fixture::item::kChest)}));
    store.mails.push_back(test_mail_mail(second, "Sender Two", "Subject Two", "Body Two", svc::kMailStatusRead,
                                         {test_mail_attachment(test_mail_hex(7012), content_fixture::item::kChest, 2, true)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d02, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character with two mails joins");
        return;
    }
    const size_t from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const std::vector<repl::RpcCall> calls = test_mail_pc_calls(w, 0, from);
    CHECK(test_mail_count_named(calls, "ClientUpdateMailHeaders") == 1, "the list answers one page of two headers");
    const repl::RpcCall* page = test_mail_first_named(calls, "ClientUpdateMailHeaders");
    std::vector<repl::MailHeader> headers;
    bool has_version = false, complete = false;
    uint64_t version = 0;
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete), "the page reads back");
    CHECK(headers.size() == 2, "the page holds both mails");
    if (headers.size() != 2) return;
    CHECK(has_version && version != 0 && complete, "the page names a version and says it is complete");
    const auto first_guid = items::guid_of(first);
    const auto second_guid = items::guid_of(second);
    CHECK(first_guid && headers[0].guid == *first_guid, "the first header is the first mail");
    CHECK(std::string(headers[0].sender) == "Sender One" && std::string(headers[0].subject) == "Subject One", "its sender and subject are the store's");
    CHECK(headers[0].status == 0 && headers[0].expiration_time == 1'700'003'600'000ll && headers[0].issue_time == 1'700'000'000'000ll,
          "its status, expiry and issue time are the store's");
    CHECK(headers[0].has_attachments, "an unclaimed attachment marks the header");
    CHECK(second_guid && headers[1].guid == *second_guid, "the second header is the second mail");
    CHECK(headers[1].status == svc::kMailStatusRead, "the read mark is the store's");
    CHECK(!headers[1].has_attachments, "a mail whose only attachment is claimed marks no attachment");
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 1, "the window's count is the one unread mail");
}

static void test_mail_login_unread_count_comes_from_the_character_load() {
    std::printf("the unread count comes from the character load, and only the first mail call sends it\n");
    std::fflush(stdout);
    TestMailStore store;
    store.mails.push_back(test_mail_mail(test_mail_hex(4), "Sender", "Subject", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(test_mail_hex(5), "Sender", "Subject", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d03, 0, 0, 1}, 2), identity())) {
        CHECK(false, "the character with two unread mails joins");
        return;
    }
    CHECK(w[0].s->unread_mail_count() == 2, "the session keeps the two the character load carries");
    CHECK(store.loads == 0, "the login does not read the mail store");
    CHECK(!w[0].s->mail_loaded(), "the cache is not loaded at the login");
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 0, "the social component does not show the count before a mail call");
    const size_t from = w[0].got.size();
    CHECK(test_mail_fetch_num(w, 0), "ServerFetchMailNum is sent");
    w.settle();
    CHECK(test_mail_unread_state(w, 0, social) && social == 2, "the count reaches the social component");
    CHECK(test_mail_social_delta(w, 0, from), "the changed count's delta is sent");
    CHECK(store.loads == 0, "the count call does not read the mail store");
    CHECK(!w[0].s->mail_loaded(), "the count call does not load the cache");
    CHECK(test_mail_count_named(test_mail_pc_calls(w, 0, from), "ClientUpdateMailHeaders") == 0, "the count call sends no headers");
}

static void test_mail_login_count_reaches_the_client_at_the_first_call() {
    std::printf("the login's unread count reaches the client at the first mail call, the list\n");
    std::fflush(stdout);
    TestMailStore store;
    store.mails.push_back(test_mail_mail(test_mail_hex(6), "Sender", "Subject", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(test_mail_hex(7), "Sender", "Subject", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d04, 0, 0, 1}, 2), identity())) {
        CHECK(false, "the character with two unread mails joins");
        return;
    }
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 0, "the login itself sends no count");
    const size_t from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is the first mail call");
    w.settle();
    CHECK(test_mail_unread_state(w, 0, social) && social == 2, "the list sets the two the character load carried");
    CHECK(test_mail_social_delta(w, 0, from), "the count's delta is sent with the list");
    CHECK(w[0].s->mail_loaded(), "the list loaded the cache");
}

static void test_mail_detail() {
    std::printf("a mail's detail carries its message, its items and each item's claimed flag\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(8);
    store.mails.push_back(test_mail_mail(id, "Detail Sender", "Detail Subject", "Detail body", 0,
                                         {test_mail_attachment(test_mail_hex(7021), content_fixture::item::kChest, 1, false),
                                          test_mail_attachment(test_mail_hex(7022), content_fixture::item::kLegs, 3, true)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d05, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character with one mail joins");
        return;
    }
    const size_t from = w[0].got.size();
    CHECK(test_mail_detail(w, 0, id), "ServerGetMailData is sent");
    w.settle();
    const std::vector<repl::RpcCall> detail_calls = test_mail_social_calls(w, 0, from);
    const repl::RpcCall* call = test_mail_first_named(detail_calls, "ClientGetMailData");
    CHECK(call != nullptr, "the detail comes back on the social component");
    if (!call) return;
    const repl::Value* value = repl::rpcf::value_of(*call->rpc, call->params, "MailData");
    const auto* detail = value ? value->get_if<repl::MailPersistenceData>() : nullptr;
    CHECK(detail != nullptr, "the detail reads as its wire struct");
    if (!detail) return;
    const auto guid = items::guid_of(id);
    CHECK(guid && detail->guid == *guid, "it is the mail that was asked for");
    CHECK(std::string(detail->subject) == "Detail Subject" && std::string(detail->sender) == "Detail Sender", "its subject and sender are the store's");
    CHECK(detail->expiration_time == 1'700'003'600'000ll, "its expiry is the store's");
    CHECK(std::string(detail->message.title) == "Detail Subject" && std::string(detail->message.body) == "Detail body", "its message is the mail's subject and body");
    CHECK(detail->status == 0, "its status is the store's");
    CHECK(detail->items.size() == 2, "it carries both attachments");
    if (detail->items.size() != 2) return;
    const auto first_guid = items::guid_of(test_mail_hex(7021));
    const auto second_guid = items::guid_of(test_mail_hex(7022));
    CHECK(first_guid && detail->items[0].item.guid == *first_guid && detail->items[0].item.record_id == content_fixture::item::kChest, "the first item is the stored one");
    CHECK(!detail->items[0].claimed, "the first item is unclaimed");
    CHECK(second_guid && detail->items[1].item.guid == *second_guid && detail->items[1].item.record_id == content_fixture::item::kLegs, "the second item is the stored one");
    CHECK(detail->items[1].item.quantity == 3 && detail->items[1].claimed, "the second item keeps its count and its claimed flag");
    CHECK(detail->currency.entries.empty() && !detail->currency_claimed, "the mail carries no money");
}

static void test_mail_refresh_reads_the_store_again() {
    std::printf("a refresh reads the store again and shows a mail that arrived since the list opened\n");
    std::fflush(stdout);
    TestMailStore store;
    store.mails.push_back(test_mail_mail(test_mail_hex(9), "Sender", "First", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d06, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    CHECK(store.loads == 1, "the list read the store once");
    store.mails.insert(store.mails.begin(), test_mail_mail(test_mail_hex(10), "Other", "Second", "Body", 0, {}));
    const size_t from = w[0].got.size();
    CHECK(test_mail_refresh(w, 0), "ServerRefreshMailHeaders is sent");
    w.settle();
    CHECK(store.loads == 2, "the refresh read the store again");
    const std::vector<repl::RpcCall> refresh_calls = test_mail_pc_calls(w, 0, from);
    const repl::RpcCall* page = test_mail_first_named(refresh_calls, "ClientUpdateMailHeaders");
    std::vector<repl::MailHeader> headers;
    bool has_version = false, complete = false;
    uint64_t version = 0;
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete), "the refresh's page reads back");
    CHECK(headers.size() == 2 && std::string(headers[0].subject) == "Second", "the new mail is at the front of the new list");
}

static void test_mail_refresh_budget() {
    std::printf("a second refresh inside ten seconds reads nothing; after ten seconds it reads again\n");
    std::fflush(stdout);
    TestMailStore store;
    store.mails.push_back(test_mail_mail(test_mail_hex(11), "Sender", "Subject", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d07, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    CHECK(test_mail_refresh(w, 0), "the first refresh is sent");
    w.settle();
    CHECK(store.loads == 2, "the first refresh read the store");
    const size_t from = w[0].got.size();
    CHECK(test_mail_refresh(w, 0), "a second refresh at once is sent");
    w.settle();
    CHECK(store.loads == 2, "the second refresh inside ten seconds reads nothing");
    CHECK(test_mail_count_named(test_mail_pc_calls(w, 0, from), "ClientUpdateMailHeaders") == 0, "and sends no headers");
    w.t += seconds(10);
    CHECK(test_mail_refresh(w, 0), "a refresh after ten seconds is sent");
    w.settle();
    CHECK(store.loads == 3, "it reads the store again");
    CHECK(test_mail_count_named(test_mail_pc_calls(w, 0, from), "ClientUpdateMailHeaders") == 1, "and sends the new list");
}

static void test_mail_claim_one_item() {
    std::printf("one item's claim moves it into a free bag slot, saves the bags first, then marks the mail claimed\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(12), item_guid = test_mail_hex(7031);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest, 1, false)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d08, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.ops.clear();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, false), "ServerClaimMailItem leaves MailItemIndex at its default");
    w.settle();
    const svc::InventoryItem* held = w[0].s->inventory().find_guid(item_guid);
    CHECK(held != nullptr, "the item is in the bags");
    CHECK(held && held->item_record_id == content_fixture::item::kChest && held->quantity == 1, "it is the mail's item");
    CHECK(held && held->container == svc::ItemContainer::Backpack, "it is in the item bag");
    const svc::Mail* stored = store.by_id(id);
    CHECK(stored && stored->items.size() == 1 && stored->items[0].claimed, "the store's mail holds the claimed flag");
    CHECK(store.bag_saves == 1 && store.mail_saves == 1, "one bag save and one mail save");
    CHECK(store.ops.size() >= 2 && store.ops[0] == "bag" && store.ops[1] == "mail " + id, "the bags are saved before the mail");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_mail_pc_calls(w, 0, from));
    CHECK(events.size() == 1 && events[0].type == 0, "one Add storage event is sent");
    CHECK(!events.empty() && events[0].item.record_id == content_fixture::item::kChest, "the event names the mail's item");
    const std::vector<repl::RpcCall> social = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social, "ClientClaimMailItem") == 1, "one claim answer is sent");
    const repl::RpcCall* answer = test_mail_first_named(social, "ClientClaimMailItem");
    const auto answered = answer ? test_mail_guid_param(*answer, "MailGuid") : std::nullopt;
    const auto mail_guid = items::guid_of(id);
    CHECK(mail_guid && answered && *answered == *mail_guid, "the answer names the mail");
    const size_t items_before = w[0].s->inventory().items().size();
    const size_t got_before = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, false), "the same claim is sent again");
    w.settle();
    CHECK(w[0].s->inventory().items().size() == items_before, "the second claim moves nothing");
    CHECK(store.bag_saves == 1 && store.mail_saves == 1, "the second claim saves nothing");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, got_before), "ClientClaimMailItem") == 0, "the second claim answers nothing");
}

static void test_mail_claim_all_items() {
    std::printf("claim all moves every unclaimed item of the listed mails with one bag save and one mail save per mail\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(13), b = test_mail_hex(14);
    const std::string a0 = test_mail_hex(7041), a1 = test_mail_hex(7042), b0 = test_mail_hex(7043);
    store.mails.push_back(test_mail_mail(a, "Sender A", "Subject A", "Body A", 0,
                                         {test_mail_attachment(a0, content_fixture::item::kChest), test_mail_attachment(a1, content_fixture::item::kScepter)}));
    store.mails.push_back(test_mail_mail(b, "Sender B", "Subject B", "Body B", 0, {test_mail_attachment(b0, content_fixture::item::kLegs)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d09, 0, 0, 1}, 2), identity())) {
        CHECK(false, "the character with two mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.ops.clear();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim_all(w, 0, {a, b}), "ServerClaimAllMail names both mails");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(a0) != nullptr && w[0].s->inventory().find_guid(a1) != nullptr &&
              w[0].s->inventory().find_guid(b0) != nullptr,
          "all three items are in the bags under their own guids");
    CHECK(store.bag_saves == 1, "one bag save carries all three");
    CHECK(store.mail_saves == 2, "each mail is saved once");
    CHECK(store.ops.size() == 3 && store.ops[0] == "bag" && store.ops[1] == "mail " + a && store.ops[2] == "mail " + b,
          "the bag is saved before either mail, and the mails in the request's order");
    const svc::Mail* stored_a = store.by_id(a);
    const svc::Mail* stored_b = store.by_id(b);
    CHECK(stored_a && stored_a->items.size() == 2 && stored_a->items[0].claimed && stored_a->items[1].claimed, "the first mail's two items are claimed");
    CHECK(stored_b && stored_b->items.size() == 1 && stored_b->items[0].claimed, "the second mail's item is claimed");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_mail_pc_calls(w, 0, from));
    CHECK(events.size() == 3, "three Add storage events are sent");
    const std::vector<repl::RpcCall> social = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social, "ClientClaimMailItem") == 3, "each item gets its claim answer");
    size_t matched = 0;
    for (const repl::RpcCall& call : social) {
        if (std::string(call.rpc ? call.rpc->name : "") != "ClientClaimMailItem") continue;
        matched += test_mail_claim_answer(call, a, 0) ? 1 : 0;
        matched += test_mail_claim_answer(call, a, 1) ? 1 : 0;
        matched += test_mail_claim_answer(call, b, 0) ? 1 : 0;
    }
    CHECK(matched == 3, "the three answers name their mail and item index");
    const size_t items_before = w[0].s->inventory().items().size();
    const size_t got_before = w[0].got.size();
    CHECK(test_mail_claim_all(w, 0, {a, b}), "the same claim-all is sent again");
    w.settle();
    CHECK(w[0].s->inventory().items().size() == items_before, "the second call adds no second copy");
    CHECK(store.bag_saves == 1 && store.mail_saves == 2, "the second call saves nothing");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, got_before), "ClientClaimMailItem") == 0, "the second call answers nothing");
}

static void test_mail_claim_all_stops_at_unavailable() {
    std::printf("claim all stops at the first Unavailable mail save: the later mails are not saved or answered\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(15), b = test_mail_hex(16), c = test_mail_hex(17);
    const std::string a0 = test_mail_hex(7051), b0 = test_mail_hex(7052), c0 = test_mail_hex(7053);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {test_mail_attachment(a0, content_fixture::item::kChest)}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {test_mail_attachment(b0, content_fixture::item::kLegs)}));
    store.mails.push_back(test_mail_mail(c, "Sender", "C", "Body", 0, {test_mail_attachment(c0, content_fixture::item::kScepter)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d0a, 0, 0, 1}, 3), identity())) {
        CHECK(false, "the character with three mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.mail_answers = {svc::CallStatus::Ok, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim_all(w, 0, {a, b, c}), "ServerClaimAllMail names all three mails");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(a0) != nullptr, "the first mail's item is in the bags");
    CHECK(w[0].s->inventory().find_guid(b0) != nullptr && w[0].s->inventory().find_guid(c0) != nullptr,
          "the second and third items are in the one saved bag change too");
    const svc::Mail* stored_a = store.by_id(a);
    const svc::Mail* stored_b = store.by_id(b);
    const svc::Mail* stored_c = store.by_id(c);
    CHECK(stored_a && stored_a->items[0].claimed, "the first mail is saved claimed");
    CHECK(stored_b && !stored_b->items[0].claimed && stored_c && !stored_c->items[0].claimed, "the later mails stay unclaimed");
    CHECK(store.mail_saves == 2, "the third mail is not tried after the Unavailable one");
    const std::vector<repl::RpcCall> social = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social, "ClientClaimMailItem") == 1, "only the first mail's item is answered");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_mail_pc_calls(w, 0, from));
    CHECK(events.size() == 1, "only the first mail's item gets a storage event");
}

static void test_mail_read_and_delete_marks_are_kept() {
    std::printf("read and delete marks are OR-ed into the stored status, the count follows, and a deleted mail does not come back\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(18), b = test_mail_hex(19);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d0b, 0, 0, 1}, 2), identity())) {
        CHECK(false, "the character with two unread mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 2, "two mails are unread");
    size_t from = w[0].got.size();
    CHECK(test_mail_status(w, 0, a, svc::kMailStatusRead, true), "ServerUpdateMailStatus marks the first mail read");
    w.settle();
    const svc::Mail* stored_a = store.by_id(a);
    CHECK(stored_a && stored_a->status == svc::kMailStatusRead, "the store keeps the read bit");
    CHECK(test_mail_unread_state(w, 0, social) && social == 1, "the count drops to one");
    const std::vector<repl::RpcCall> read_answer_calls = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(read_answer_calls, "ClientUpdateMailStatus") == 1, "one status answer is sent");
    CHECK(!read_answer_calls.empty() && std::string(read_answer_calls[0].rpc->name) == "ClientUpdateMailStatus" &&
              test_mail_status_answer(read_answer_calls[0], a, svc::kMailStatusRead),
          "the answer names the mail and its read status");
    from = w[0].got.size();
    CHECK(test_mail_refresh(w, 0), "a refresh is sent");
    w.settle();
    const std::vector<repl::RpcCall> refresh_after_read = test_mail_pc_calls(w, 0, from);
    const repl::RpcCall* page = test_mail_first_named(refresh_after_read, "ClientUpdateMailHeaders");
    std::vector<repl::MailHeader> headers;
    bool has_version = false, complete = false;
    uint64_t version = 0;
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete) && headers.size() == 2, "the refresh shows both mails");
    CHECK(headers.size() == 2 && headers[0].status == svc::kMailStatusRead, "the first mail is still read after a new store read");
    const size_t saves_before = store.mail_saves;
    CHECK(test_mail_status(w, 0, a, svc::kMailStatusRead, true), "the same read mark is sent again");
    w.settle();
    CHECK(store.mail_saves == saves_before, "the same bits make no save");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientUpdateMailStatus") == 1, "and the mark is still answered");
    CHECK(store.by_id(a) && store.by_id(a)->status == svc::kMailStatusRead, "and change nothing in the store");
    from = w[0].got.size();
    CHECK(test_mail_status(w, 0, a, svc::kMailStatusDeleted, true), "the first mail is deleted");
    w.settle();
    CHECK(store.by_id(a) && store.by_id(a)->status == (svc::kMailStatusRead | svc::kMailStatusDeleted), "the delete bit joins the read bit");
    CHECK(test_mail_unread_state(w, 0, social) && social == 1, "the deleted mail was already read: the count stays one");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientUpdateMailStatus") == 1, "the delete is answered");
    from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "a list after the delete is sent");
    w.settle();
    const std::vector<repl::RpcCall> after_delete = test_mail_pc_calls(w, 0, from);
    page = test_mail_first_named(after_delete, "ClientUpdateMailHeaders");
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete) && headers.size() == 1, "the deleted mail is not in the list");
    CHECK(headers.size() == 1 && std::string(headers[0].subject) == "B", "the other mail is");
    from = w[0].got.size();
    CHECK(test_mail_status(w, 0, b, svc::kMailStatusDeleted, true), "the second mail is deleted");
    w.settle();
    CHECK(store.by_id(b) && store.by_id(b)->status == svc::kMailStatusDeleted, "the store keeps its delete bit");
    CHECK(test_mail_unread_state(w, 0, social) && social == 0, "the count is zero");
    from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "a list with no live mail is sent");
    w.settle();
    const std::vector<repl::RpcCall> empty_list = test_mail_pc_calls(w, 0, from);
    page = test_mail_first_named(empty_list, "ClientUpdateMailHeaders");
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete) && headers.empty(), "the list has no header");
    TestMailStore store_after;
    store_after.mails = store.mails;
    PartyWorld again;
    again.fx.world->set_position_sink(&store_after);
    if (!again.enter(0, test_mail_record("mailer", {0x4d0b, 0, 0, 1}, 0), identity())) {
        CHECK(false, "a new session on the same store joins");
        return;
    }
    CHECK(again[0].s->mails().empty(), "the new session's list holds no deleted mail");
    CHECK(test_mail_fetch(again, 0), "the new session's list is sent");
    again.settle();
    CHECK(again[0].s->mails().empty(), "the new session's list still holds no deleted mail");
}

static void test_mail_default_parameters_are_zero() {
    std::printf("an absent MailItemIndex claims item zero and an absent Status changes nothing\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(20);
    const std::string first = test_mail_hex(7061), second = test_mail_hex(7062);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0,
                                         {test_mail_attachment(first, content_fixture::item::kChest), test_mail_attachment(second, content_fixture::item::kLegs)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d0c, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    const size_t saves_before = store.mail_saves;
    CHECK(test_mail_status(w, 0, id, 0, false), "ServerUpdateMailStatus is sent with Status absent");
    w.settle();
    CHECK(store.mail_saves == saves_before, "the absent status saves nothing");
    CHECK(store.by_id(id) && store.by_id(id)->status == 0, "the store's status is unchanged");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientUpdateMailStatus") == 0, "the absent status answers nothing");
    CHECK(test_mail_log_count("ignoring the client's ServerUpdateMailStatus") == 0, "the call is not ignored");
    CHECK(test_mail_claim(w, 0, id, 0, false), "ServerClaimMailItem is sent with MailItemIndex absent");
    w.settle();
    const svc::InventoryItem* held = w[0].s->inventory().find_guid(first);
    CHECK(held != nullptr && held->item_record_id == content_fixture::item::kChest, "item zero is the one that moved");
    CHECK(w[0].s->inventory().find_guid(second) == nullptr, "the second item stays in the mail");
}

static void test_mail_bulk_status_updates() {
    std::printf("a bulk status ORs the bits into each named mail and answers each one\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(21), b = test_mail_hex(22);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d0d, 0, 0, 1}, 2), identity())) {
        CHECK(false, "the character with two unread mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    const size_t saves_before = store.mail_saves;
    CHECK(test_mail_bulk_status(w, 0, {a, b}, svc::kMailStatusRead), "ServerBulkUpdateMailStatus names both mails");
    w.settle();
    CHECK(store.mail_saves == saves_before + 2, "each mail is saved");
    CHECK(store.by_id(a) && store.by_id(a)->status == svc::kMailStatusRead && store.by_id(b) && store.by_id(b)->status == svc::kMailStatusRead,
          "both mails keep the read mark");
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 0, "the count drops to zero");
    const std::vector<repl::RpcCall> social_calls = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social_calls, "ClientUpdateMailStatus") == 2, "both mails are answered");
    size_t matched = 0;
    for (const repl::RpcCall& call : social_calls) {
        if (std::string(call.rpc ? call.rpc->name : "") != "ClientUpdateMailStatus") continue;
        matched += test_mail_status_answer(call, a, svc::kMailStatusRead) ? 1 : 0;
        matched += test_mail_status_answer(call, b, svc::kMailStatusRead) ? 1 : 0;
    }
    CHECK(matched == 2, "each answer names its own mail");
}

static void test_mail_bulk_status_updates_many() {
    std::printf("a bulk status of more than two hundred mails is answered in calls of at most two hundred, each mail once\n");
    std::fflush(stdout);
    TestMailStore store;
    std::vector<std::string> ids;
    for (uint32_t i = 0; i < 205; ++i) {
        ids.push_back(test_mail_hex(600 + i));
        store.mails.push_back(test_mail_mail(ids.back(), "Sender", "Subject", "Body", 0, {}));
    }
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d0e, 0, 0, 1}, 205), identity())) {
        CHECK(false, "the character with 205 mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    // a MailGuid is 128 bits and a client bunch holds 7824 bits including its call header: the 205 go in calls of 40, 40, 40, 40 and 45
    std::vector<std::vector<std::string>> calls;
    for (size_t at = 0; at < ids.size(); at += 40) {
        const size_t end = std::min(ids.size(), at + 40);
        calls.emplace_back(ids.begin() + static_cast<std::ptrdiff_t>(at), ids.begin() + static_cast<std::ptrdiff_t>(end));
    }
    for (size_t i = 0; i < calls.size(); ++i) {
        CHECK(test_mail_bulk_status(w, 0, calls[i], svc::kMailStatusRead), "ServerBulkUpdateMailStatus names one call of at most 45 mails");
        w.settle();
    }
    const std::vector<size_t> chains = test_mail_status_chain_sizes(w, 0, from);
    size_t answered_in_chains = 0;
    bool within_two_hundred = true;
    for (size_t n : chains) {
        answered_in_chains += n;
        within_two_hundred = within_two_hundred && n <= 200;
    }
    CHECK(within_two_hundred && answered_in_chains == 205, "the 205 status calls go out one per mail in chains of at most two hundred");
    const std::vector<repl::RpcCall> social = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social, "ClientUpdateMailStatus") == 205, "one status call per mail");
    std::vector<std::string> answered;
    for (const repl::RpcCall& call : social) {
        if (std::string(call.rpc ? call.rpc->name : "") != "ClientUpdateMailStatus") continue;
        const auto guid = test_mail_guid_param(call, "MailGuid");
        if (!guid) continue;
        char text[33];
        std::snprintf(text, sizeof text, "%08x%08x%08x%08x", guid->a, guid->b, guid->c, guid->d);
        answered.push_back(text);
    }
    std::sort(answered.begin(), answered.end());
    answered.erase(std::unique(answered.begin(), answered.end()), answered.end());
    CHECK(answered.size() == 205, "every mail appears once in the answers");
    int32_t social_count = -1;
    CHECK(test_mail_unread_state(w, 0, social_count) && social_count == 0, "all 205 are read");
}

static void test_mail_bulk_status_stops_at_unavailable() {
    std::printf("a bulk status stops at the first Unavailable and does not touch the mails after it\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(23), b = test_mail_hex(24), c = test_mail_hex(25);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {}));
    store.mails.push_back(test_mail_mail(c, "Sender", "C", "Body", 0, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d0f, 0, 0, 1}, 3), identity())) {
        CHECK(false, "the character with three unread mails joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.mail_answers = {svc::CallStatus::Ok, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
    const size_t from = w[0].got.size();
    const size_t saves_before = store.mail_saves;
    CHECK(test_mail_bulk_status(w, 0, {a, b, c}, svc::kMailStatusRead), "ServerBulkUpdateMailStatus names all three");
    w.settle();
    CHECK(store.mail_saves == saves_before + 2, "the third mail is not tried");
    CHECK(store.by_id(a) && store.by_id(a)->status == svc::kMailStatusRead, "the first mail keeps the read mark");
    CHECK(store.by_id(b) && store.by_id(b)->status == 0 && store.by_id(c) && store.by_id(c)->status == 0, "the later mails are unchanged");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientUpdateMailStatus") == 1, "only the first mail is answered");
}

static void test_mail_full_bag_keeps_the_second_item_unclaimed() {
    std::printf("a full bag refuses the claim and says so once; the item stays in the mail and nothing is saved\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(26), b = test_mail_hex(27);
    const std::string a0 = test_mail_hex(7071), a1 = test_mail_hex(7072), b0 = test_mail_hex(7073);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {test_mail_attachment(a0, content_fixture::item::kChest), test_mail_attachment(a1, content_fixture::item::kLegs)}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {test_mail_attachment(b0, content_fixture::item::kScepter)}));
    svc::Character character = test_mail_record("mailer", {0x4d10, 0, 0, 1}, 2);
    for (int slot = 0; slot < items::kBackpackSlots - 1; ++slot) {
        character.items.push_back(test_mail_item(test_mail_hex(8'000 + static_cast<uint32_t>(slot)), content_fixture::item::kChest, svc::ItemContainer::Backpack, slot));
    }
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, character, identity())) {
        CHECK(false, "the character with a nearly full bag joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    CHECK(test_mail_claim(w, 0, a, 0, true), "the first item's claim is sent");
    w.settle();
    const svc::InventoryItem* first = w[0].s->inventory().find_guid(a0);
    CHECK(first != nullptr && first->slot == items::kBackpackSlots - 1, "the first item takes the last free slot");
    CHECK(store.bag_saves == 1 && store.mail_saves == 1, "the first item's claim saved the bags and the mail");
    const size_t bag_saves = store.bag_saves, mail_saves = store.mail_saves;
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, a, 1, true), "the second item's claim is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(a1) == nullptr, "the second item did not move");
    const svc::Mail* stored_a = store.by_id(a);
    CHECK(stored_a && stored_a->items.size() == 2 && !stored_a->items[1].claimed, "the second item stays unclaimed in the store");
    CHECK(store.bag_saves == bag_saves && store.mail_saves == mail_saves, "a full bag saves nothing");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "a full bag answers no claim");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(), "a full bag sends no storage event");
    const auto lines = chat_lines_in(w[0].got, test_mail_pc_cls(), from);
    CHECK(lines.size() == 1 && lines[0].message == "Your bags are full." && lines[0].from == "Server" && lines[0].category == 124,
          "one line tells the player the bags are full");
    const size_t after_line = w[0].got.size();
    CHECK(test_mail_claim_all(w, 0, {b}), "a claim-all for the second mail is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(b0) == nullptr, "the claim-all moved nothing");
    CHECK(store.by_id(b) && !store.by_id(b)->items[0].claimed, "the second mail stays unclaimed");
    CHECK(store.bag_saves == bag_saves && store.mail_saves == mail_saves, "the full-bag claim-all saves nothing");
    const auto lines_after = chat_lines_in(w[0].got, test_mail_pc_cls(), after_line);
    CHECK(lines_after.size() == 1 && lines_after[0].message == "Your bags are full.", "the claim-all says it once as well");
}

static void test_mail_save_failures() {
    std::printf("a refused bag save moves nothing; a failed mail save keeps the item and the next claim marks the mail\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(28), item_guid = test_mail_hex(7081);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d11, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    store.bag_answers = {svc::CallStatus::Refused};
    size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is sent with a refused bag save");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) == nullptr, "the refused bag save rolled the item back");
    CHECK(store.by_id(id) && !store.by_id(id)->items[0].claimed, "the mail stays unclaimed");
    CHECK(store.bag_saves == 1 && store.mail_saves == 0, "no mail save follows a refused bag save");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "no claim answer follows");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(), "no storage event follows");
    store.bag_answers = {svc::CallStatus::Ok};
    store.mail_answers = {svc::CallStatus::Refused};
    from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is sent with a refused mail save");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) != nullptr, "the refused mail save keeps the bag change");
    CHECK(store.by_id(id) && !store.by_id(id)->items[0].claimed, "the mail stays unclaimed");
    CHECK(store.bag_saves == 2 && store.mail_saves == 1, "the bags are saved before the refused mail save");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "no claim answer follows the refused mail save");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(), "no storage event follows the refused mail save");
    from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is tried again");
    w.settle();
    const svc::InventoryItem* held = w[0].s->inventory().find_guid(item_guid);
    CHECK(held != nullptr && w[0].s->inventory().items().size() == 1, "the bag holds the item once");
    CHECK(store.by_id(id) && store.by_id(id)->items[0].claimed, "the retry marks the mail claimed");
    CHECK(store.bag_saves == 3 && store.mail_saves == 2, "the retry saves the bags again, then the mail");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).size() == 1, "the retry sends the item's Add from the bag copy");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1, "the retry answers the claim");
}

static void test_mail_logout_and_restart_through_the_store() {
    std::printf("a mail the admin sent waits through a logout and a new session, with its claimed item and read mark\n");
    std::fflush(stdout);
    TestMailStore store;
    std::string mail_id;
    {
        PartyWorld w;
        w.fx.world->set_position_sink(&store);
        if (!w.enter(0, test_mail_record("mailer", {0x4d12, 0, 0, 1}, 0), identity())) {
            CHECK(false, "the first session joins");
            return;
        }
        PlayerSession::GmMailResult sent = w[0].s->gm_send_mail(w[0].conn, "Admin Sender", "Admin Subject", "Admin body", content_fixture::item::kChest, 1, "tester");
        CHECK(sent.pending && sent.ticket != 0, "the admin entry hands the create to the worker");
        w.settle();
        CHECK(w[0].s->take_gm_mail(sent.ticket, sent) && sent.ok && !sent.mail_id.empty(), "the admin entry adds the mail when the job lands");
        mail_id = sent.mail_id;
        CHECK(test_mail_fetch(w, 0), "the first session's list is sent");
        w.settle();
        CHECK(test_mail_claim(w, 0, mail_id, 0, true), "the first session claims the item");
        w.settle();
        CHECK(test_mail_status(w, 0, mail_id, svc::kMailStatusRead, true), "the first session marks the mail read");
        w.settle();
    }
    const svc::Mail* stored = store.by_id(mail_id);
    CHECK(stored && stored->items.size() == 1 && stored->items[0].claimed && stored->status == svc::kMailStatusRead, "the store kept the claim and the read mark");
    if (!stored || stored->items.empty()) return;
    const std::string item_guid = stored->items[0].item.instance_guid;
    PartyWorld again;
    again.fx.world->set_position_sink(&store);
    svc::Character character = test_mail_record("mailer", {0x4d12, 0, 0, 1}, 0);
    character.items = store.bag_items;
    if (!again.enter(0, character, identity())) {
        CHECK(false, "a new session on the same store joins");
        return;
    }
    const svc::InventoryItem* held = again[0].s->inventory().find_guid(item_guid);
    CHECK(held != nullptr && held->item_record_id == content_fixture::item::kChest && again[0].s->inventory().items().size() == 1, "the new session's bag holds the item once");
    CHECK(test_mail_fetch(again, 0), "the new session's list is sent");
    again.settle();
    const std::vector<repl::RpcCall> new_session_pc = test_mail_pc_calls(again, 0, 0);
    const repl::RpcCall* page = test_mail_first_named(new_session_pc, "ClientUpdateMailHeaders");
    std::vector<repl::MailHeader> headers;
    bool has_version = false, complete = false;
    uint64_t version = 0;
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete) && headers.size() == 1, "the new session lists the mail");
    CHECK(headers.size() == 1 && headers[0].status == svc::kMailStatusRead, "the read mark came through the store");
    CHECK(test_mail_detail(again, 0, mail_id), "the new session's detail is sent");
    again.settle();
    const std::vector<repl::RpcCall> new_session_social = test_mail_social_calls(again, 0, 0);
    const repl::RpcCall* detail_call = test_mail_first_named(new_session_social, "ClientGetMailData");
    const repl::Value* value = detail_call ? repl::rpcf::value_of(*detail_call->rpc, detail_call->params, "MailData") : nullptr;
    const auto* detail = value ? value->get_if<repl::MailPersistenceData>() : nullptr;
    CHECK(detail && detail->items.size() == 1 && detail->items[0].claimed, "the detail shows the item claimed");
}

static void test_mail_admin_send_adds_a_mail_and_the_unread_count() {
    std::printf("the admin entry adds one mail for the character, the cache lists it and the unread count rises\n");
    std::fflush(stdout);
    TestMailStore store;
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d13, 0, 0, 1}, 0), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    PlayerSession::GmMailResult sent =
        w[0].s->gm_send_mail(w[0].conn, "Fake Sender", "Fake Subject", "Fake body", content_fixture::item::kChest, 1, "tester");
    CHECK(sent.pending && sent.ticket != 0, "the admin entry hands the create to the worker");
    w.settle();
    CHECK(w[0].s->take_gm_mail(sent.ticket, sent) && sent.ok, "the admin entry accepts the mail when the job lands");
    CHECK(!sent.mail_id.empty() && store.mails.size() == 1 && store.mails[0].id == sent.mail_id, "the new mail is the store's");
    CHECK(sent.unread == 1, "the result carries the count");
    CHECK(store.last_created.sender == "Fake Sender" && store.last_created.subject == "Fake Subject" && store.last_created.body == "Fake body",
          "the store got the three texts");
    CHECK(store.last_created.items.size() == 1 && !store.last_created.items[0].claimed, "the store got one unclaimed attachment");
    CHECK(w[0].s->mails().size() == 1, "the cache lists the new mail");
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 1, "the unread count reaches the client");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, 0), "ClientUpdateMailHeaders") == 0, "the admin send sends no headers until the client asks");
    const size_t from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "the client's fetch is sent");
    w.settle();
    const std::vector<repl::RpcCall> admin_fetch_calls = test_mail_pc_calls(w, 0, from);
    const repl::RpcCall* page = test_mail_first_named(admin_fetch_calls, "ClientUpdateMailHeaders");
    std::vector<repl::MailHeader> headers;
    bool has_version = false, complete = false;
    uint64_t version = 0;
    CHECK(page && test_mail_page(*page, headers, has_version, version, complete) && headers.size() == 1, "the fetch lists the new mail");
    const size_t creates = store.creates;
    store.create_status = svc::CallStatus::Refused;
    PlayerSession::GmMailResult refused =
        w[0].s->gm_send_mail(w[0].conn, "Fake Sender", "Refused Subject", "Fake body", content_fixture::item::kChest, 1, "tester");
    CHECK(refused.pending, "the refused create is handed to the worker too");
    w.settle();
    CHECK(w[0].s->take_gm_mail(refused.ticket, refused) && !refused.ok && refused.refusal == "the store refused the mail",
          "a refused create answers the store's refusal");
    CHECK(store.creates == creates + 1 && store.mails.size() == 1 && w[0].s->mails().size() == 1, "the refusal changes no cache");
}

static void test_mail_admin_send_splits_a_count_into_stacks() {
    std::printf("the admin entry splits a count into stacks of the record's size and refuses more than twenty\n");
    std::fflush(stdout);
    TestMailStore store;
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d14, 0, 0, 1}, 0), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    PlayerSession::GmMailResult sent = w[0].s->gm_send_mail(w[0].conn, "Sender", "Subject", "Body", content_fixture::item::kChest, 3, "tester");
    CHECK(sent.pending, "the admin entry hands the create to the worker");
    w.settle();
    CHECK(w[0].s->take_gm_mail(sent.ticket, sent) && sent.ok, "three of a stack-one item are accepted");
    CHECK(store.last_created.items.size() == 3, "the store got three attachments");
    bool stacks_one = store.last_created.items.size() == 3;
    bool empty_guids = true, unclaimed = true;
    for (const svc::MailItem& item : store.last_created.items) {
        stacks_one = stacks_one && item.item.quantity == 1;
        empty_guids = empty_guids && item.item.instance_guid.empty() && item.item.created_at_ms == 0;
        unclaimed = unclaimed && !item.claimed;
    }
    CHECK(stacks_one, "each attachment is a stack of one");
    CHECK(empty_guids, "the store fills the guids and the creation times");
    CHECK(unclaimed, "every attachment is unclaimed");
    CHECK(store.mails.size() == 1 && store.mails[0].items.size() == 3, "the created mail keeps all three");
    const size_t creates = store.creates;
    const PlayerSession::GmMailResult too_many = w[0].s->gm_send_mail(w[0].conn, "Sender", "Subject", "Body", content_fixture::item::kChest, 21, "tester");
    CHECK(!too_many.ok && !too_many.refusal.empty(), "more than twenty stacks is refused with a reason");
    CHECK(store.creates == creates, "the refused count reaches no store");
}

static void test_mail_admin_send_keeps_the_item_durability() {
    std::printf("the admin entry's chest attachment carries the record's durability\n");
    std::fflush(stdout);
    TestMailStore store;
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d15, 0, 0, 1}, 0), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    PlayerSession::GmMailResult sent = w[0].s->gm_send_mail(w[0].conn, "Sender", "Subject", "Body", content_fixture::item::kChest, 1, "tester");
    CHECK(sent.pending, "the admin entry hands the create to the worker");
    w.settle();
    CHECK(w[0].s->take_gm_mail(sent.ticket, sent) && sent.ok, "the chest is accepted");
    CHECK(store.last_created.items.size() == 1, "the store got its one attachment");
    CHECK(store.last_created.items.size() == 1 && store.last_created.items[0].item.durability == 11.0f,
          "the attachment carries the record's durability of eleven");
}

static void test_mail_unknown_mail_changes_nothing() {
    std::printf("a call for a mail the character does not have changes nothing and answers nothing\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string known = test_mail_hex(29), unknown = test_mail_hex(999);
    const std::string item_guid = test_mail_hex(7091);
    store.mails.push_back(test_mail_mail(known, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d16, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    const size_t bag_saves = store.bag_saves, mail_saves = store.mail_saves;
    CHECK(test_mail_detail(w, 0, unknown), "a detail for an unknown mail is sent");
    CHECK(test_mail_claim(w, 0, unknown, 0, true), "a claim for an unknown mail is sent");
    CHECK(test_mail_currency(w, 0, unknown), "a currency claim for an unknown mail is sent");
    CHECK(test_mail_status(w, 0, unknown, svc::kMailStatusRead, true), "a status for an unknown mail is sent");
    CHECK(test_mail_claim_all(w, 0, {unknown}), "a claim-all for an unknown mail is sent");
    w.settle();
    CHECK(store.bag_saves == bag_saves && store.mail_saves == mail_saves, "the unknown mail saves nothing");
    CHECK(w[0].s->inventory().find_guid(item_guid) == nullptr, "the known mail's item did not move");
    CHECK(store.by_id(known) && !store.by_id(known)->items[0].claimed && store.by_id(known)->status == 0, "the known mail is unchanged");
    const std::vector<repl::RpcCall> social = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social, "ClientGetMailData") == 0 && test_mail_count_named(social, "ClientClaimMailItem") == 0 &&
              test_mail_count_named(social, "ClientUpdateMailStatus") == 0,
          "the unknown mail answers nothing");
    CHECK(test_mail_claim(w, 0, known, 0, true), "the known mail's claim is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) != nullptr && store.by_id(known) && store.by_id(known)->items[0].claimed,
          "the known mail's item moves");
}

static void test_mail_pages() {
    std::printf("a list of twenty-one mails is two pages, the first of twenty, the second of one, with one version\n");
    std::fflush(stdout);
    TestMailStore store;
    for (uint32_t i = 0; i < 21; ++i) store.mails.push_back(test_mail_mail(test_mail_hex(700 + i), "Sender", "Subject", "Body", svc::kMailStatusRead, {}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d17, 0, 0, 1}, 0), identity())) {
        CHECK(false, "the character with twenty-one mails joins");
        return;
    }
    const size_t from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const std::vector<repl::RpcCall> calls = test_mail_pc_calls(w, 0, from);
    CHECK(test_mail_count_named(calls, "ClientUpdateMailHeaders") == 2, "two pages are sent");
    std::vector<repl::MailHeader> first, second;
    bool first_has_version = false, first_complete = true;
    bool second_has_version = false, second_complete = false;
    uint64_t first_version = 0, second_version = 0;
    size_t page_number = 0;
    for (const repl::RpcCall& call : calls) {
        if (std::string(call.rpc ? call.rpc->name : "") != "ClientUpdateMailHeaders") continue;
        std::vector<repl::MailHeader> page;
        bool has = false, done = false;
        uint64_t version = 0;
        CHECK(test_mail_page(call, page, has, version, done), "a page reads back");
        if (page_number == 0) {
            first = page;
            first_has_version = has;
            first_version = version;
            first_complete = done;
        } else {
            second = page;
            second_has_version = has;
            second_version = version;
            second_complete = done;
        }
        ++page_number;
    }
    CHECK(first.size() == kMailTestPage && second.size() == 1, "the first page holds twenty and the second one");
    CHECK(first_has_version && second_has_version && first_version != 0 && first_version == second_version, "both pages carry the same version");
    CHECK(!first_complete && second_complete, "the first page is not complete and the second is");
}

static void test_mail_bad_item_index_changes_nothing() {
    std::printf("a negative or past-the-end item index moves and saves nothing; index zero is valid\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(30);
    const std::string first = test_mail_hex(7101), second = test_mail_hex(7102);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(first, content_fixture::item::kChest), test_mail_attachment(second, content_fixture::item::kLegs)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d18, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, -1, true), "a claim with index -1 is sent");
    CHECK(test_mail_claim(w, 0, id, 9, true), "a claim with index 9 is sent");
    w.settle();
    CHECK(store.bag_saves == 0 && store.mail_saves == 0, "neither bad index saves anything");
    CHECK(w[0].s->inventory().find_guid(first) == nullptr && w[0].s->inventory().find_guid(second) == nullptr, "neither item moved");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "neither bad index is answered");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(), "neither bad index sends an event");
    CHECK(test_mail_claim(w, 0, id, 0, false), "a claim with index absent claims item zero");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(first) != nullptr && w[0].s->inventory().find_guid(second) == nullptr, "item zero moves and item one stays");
}

static void test_mail_material_goes_to_the_resource_bag() {
    std::printf("a material attachment goes to the first resource bag with room, under its own guid\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(31), material = test_mail_hex(7111);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(material, content_fixture::resource::kCoal, 3)}));
    svc::Character character = test_mail_record("mailer", {0x4d19, 0, 0, 1}, 1);
    character.items.push_back(test_mail_item(test_mail_hex(7112), content_fixture::resource::kBagMining, svc::ItemContainer::Equipment, 23));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, character, identity())) {
        CHECK(false, "the character with the mining bag joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the material's claim is sent");
    w.settle();
    const svc::InventoryItem* held = w[0].s->inventory().find_guid(material);
    CHECK(held != nullptr, "the material is in the bags");
    CHECK(held && held->container == svc::ItemContainer::Resource0 && held->quantity == 3, "it is in the first resource bag with its count");
    CHECK(store.by_id(id) && store.by_id(id)->items[0].claimed, "the mail is claimed");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).size() == 1, "one Add event is sent");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1, "one claim answer is sent");
}

static void test_mail_new_session_claim_sends_no_add() {
    std::printf("a session that finds the item already in its bags claims the mail without a second Add\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(32), item_guid = test_mail_hex(7121);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest)}));
    svc::Character character = test_mail_record("mailer", {0x4d1a, 0, 0, 1}, 1);
    character.items.push_back(test_mail_item(item_guid, content_fixture::item::kChest, svc::ItemContainer::Backpack, 0));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, character, identity())) {
        CHECK(false, "the character whose bags already hold the item joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, id, 0, true), "the claim is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) != nullptr && w[0].s->inventory().items().size() == 1, "the bag holds the item once");
    CHECK(store.by_id(id) && store.by_id(id)->items[0].claimed, "the mail is claimed");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(), "no Add is sent for an item the login already showed");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 1, "the claim is answered");
}

static void test_mail_no_sink_refuses_before_the_bag_change() {
    std::printf("a claim without a sink moves nothing, in the one-item and the claim-all paths\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string a = test_mail_hex(33), b = test_mail_hex(34);
    const std::string a0 = test_mail_hex(7131), b0 = test_mail_hex(7132);
    store.mails.push_back(test_mail_mail(a, "Sender", "A", "Body", 0, {test_mail_attachment(a0, content_fixture::item::kChest)}));
    store.mails.push_back(test_mail_mail(b, "Sender", "B", "Body", 0, {test_mail_attachment(b0, content_fixture::item::kLegs)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d1b, 0, 0, 1}, 2), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    CHECK(test_mail_claim(w, 0, a, 0, true), "the first mail's claim with a sink is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(a0) != nullptr, "the first claim moves its item");
    w[0].s->set_mail_sink(nullptr);
    const size_t bag_saves = store.bag_saves, mail_saves = store.mail_saves;
    const size_t from = w[0].got.size();
    CHECK(test_mail_claim(w, 0, b, 0, true), "the second mail's claim without a sink is sent");
    CHECK(test_mail_claim_all(w, 0, {b}), "a claim-all without a sink is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(b0) == nullptr, "the second item did not move");
    CHECK(store.by_id(b) && !store.by_id(b)->items[0].claimed, "the second mail stays unclaimed");
    CHECK(store.bag_saves == bag_saves && store.mail_saves == mail_saves, "no sink saves nothing");
    CHECK(test_mail_count_named(test_mail_social_calls(w, 0, from), "ClientClaimMailItem") == 0, "no sink answers no claim");
    CHECK(test_guild_storage_events(test_mail_pc_calls(w, 0, from)).empty(), "no sink sends no storage event");
}

static void test_mail_headers_log_once_when_they_do_not_build() {
    std::printf("a header value that does not fit logs once and sends no headers\n");
    std::fflush(stdout);
    TestMailStore store;
    std::string bad = "bad";
    bad.push_back('\0');
    bad += "subject";
    store.mails.push_back(test_mail_mail(test_mail_hex(35), "Sender", "bad", "Body", 0, {}));
    store.mails.back().subject = bad;   // an FString may not hold a NUL
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d1c, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    const size_t from = w[0].got.size();
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    CHECK(test_mail_count_named(test_mail_pc_calls(w, 0, from), "ClientUpdateMailHeaders") == 0, "no headers are sent");
    CHECK(test_mail_log_count("the mail headers do not build") == 1, "the failure is logged once");
    int32_t social = -1;
    CHECK(test_mail_unread_state(w, 0, social) && social == 1, "the count still reaches the client");
}

static void test_mail_currency_claim_changes_nothing() {
    std::printf("a currency claim is registered, changes nothing and answers nothing; the item claim still works\n");
    std::fflush(stdout);
    TestMailStore store;
    const std::string id = test_mail_hex(36), item_guid = test_mail_hex(7141);
    store.mails.push_back(test_mail_mail(id, "Sender", "Subject", "Body", 0, {test_mail_attachment(item_guid, content_fixture::item::kChest)}));
    PartyWorld w;
    w.fx.world->set_position_sink(&store);
    if (!w.enter(0, test_mail_record("mailer", {0x4d1d, 0, 0, 1}, 1), identity())) {
        CHECK(false, "the character joins");
        return;
    }
    CHECK(test_mail_fetch(w, 0), "ServerFetchMail is sent");
    w.settle();
    const size_t from = w[0].got.size();
    CHECK(test_mail_currency(w, 0, id), "ServerClaimMailCurrency is sent");
    w.settle();
    CHECK(store.bag_saves == 0 && store.mail_saves == 0, "the currency claim saves nothing");
    const std::vector<repl::RpcCall> social = test_mail_social_calls(w, 0, from);
    CHECK(test_mail_count_named(social, "ClientClaimMailCurrency") == 0, "the currency claim answers nothing");
    CHECK(test_mail_log_count("ignoring the client's ServerClaimMailCurrency") == 0, "the currency call is registered");
    CHECK(test_mail_claim(w, 0, id, 0, true), "the item claim is sent");
    w.settle();
    CHECK(w[0].s->inventory().find_guid(item_guid) != nullptr, "the item claim moves the item");
}
AOC_REGISTER_TEST(test_mail_calls_are_registered);
AOC_REGISTER_TEST(test_mail_list_shows_the_headers_the_store_holds);
AOC_REGISTER_TEST(test_mail_login_unread_count_comes_from_the_character_load);
AOC_REGISTER_TEST(test_mail_login_count_reaches_the_client_at_the_first_call);
AOC_REGISTER_TEST(test_mail_detail);
AOC_REGISTER_TEST(test_mail_refresh_reads_the_store_again);
AOC_REGISTER_TEST(test_mail_refresh_budget);
AOC_REGISTER_TEST(test_mail_claim_one_item);
AOC_REGISTER_TEST(test_mail_claim_all_items);
AOC_REGISTER_TEST(test_mail_claim_all_stops_at_unavailable);
AOC_REGISTER_TEST(test_mail_read_and_delete_marks_are_kept);
AOC_REGISTER_TEST(test_mail_default_parameters_are_zero);
AOC_REGISTER_TEST(test_mail_bulk_status_updates);
AOC_REGISTER_TEST(test_mail_bulk_status_updates_many);
AOC_REGISTER_TEST(test_mail_bulk_status_stops_at_unavailable);
AOC_REGISTER_TEST(test_mail_full_bag_keeps_the_second_item_unclaimed);
AOC_REGISTER_TEST(test_mail_save_failures);
AOC_REGISTER_TEST(test_mail_logout_and_restart_through_the_store);
AOC_REGISTER_TEST(test_mail_admin_send_adds_a_mail_and_the_unread_count);
AOC_REGISTER_TEST(test_mail_admin_send_splits_a_count_into_stacks);
AOC_REGISTER_TEST(test_mail_admin_send_keeps_the_item_durability);
AOC_REGISTER_TEST(test_mail_unknown_mail_changes_nothing);
AOC_REGISTER_TEST(test_mail_pages);
AOC_REGISTER_TEST(test_mail_bad_item_index_changes_nothing);
AOC_REGISTER_TEST(test_mail_material_goes_to_the_resource_bag);
AOC_REGISTER_TEST(test_mail_new_session_claim_sends_no_add);
AOC_REGISTER_TEST(test_mail_no_sink_refuses_before_the_bag_change);
AOC_REGISTER_TEST(test_mail_headers_log_once_when_they_do_not_build);
AOC_REGISTER_TEST(test_mail_currency_claim_changes_nothing);
AOC_REGISTER_TEST(test_mail_load_does_not_hold_the_tick);
AOC_REGISTER_TEST(test_mail_claim_does_not_hold_the_tick);
AOC_REGISTER_TEST(test_mail_bulk_status_is_one_job);
AOC_REGISTER_TEST(test_mail_failed_load_backs_off);
AOC_REGISTER_TEST(test_mail_double_claim_while_in_flight_grants_once);
AOC_REGISTER_TEST(test_mail_noop_status_makes_no_save);
AOC_REGISTER_TEST(test_mail_session_removed_with_a_job_in_flight);
AOC_REGISTER_TEST(test_mail_claim_sends_the_top_up_event);
AOC_REGISTER_TEST(test_mail_claim_sends_both_events_of_a_split);
AOC_REGISTER_TEST(test_mail_retry_after_a_failed_mail_save_keeps_the_events);
