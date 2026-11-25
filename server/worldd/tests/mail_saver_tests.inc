// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/mail_saver_tests.inc: the character saver's mail forwards and the claim's blocking bag save.
//
//  The character service fake answers the four calls and can hold one inventory write in the worker's thread, so the direct bag save's wait, its
//  drop of an older queued bag and its order after the write in flight are checked without a server.
// ============================================================================
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

static std::string test_mail_saver_hex(uint32_t n) {
    char text[33];
    std::snprintf(text, sizeof text, "%032x", n);
    return text;
}

/// A character service that keeps mails and the inventory writes it got, and can hold one inventory write of one character in its caller.
struct MailSaverApi : FriendStoreApi {
    std::mutex                                   mail_mu;
    std::vector<std::string>                     calls;
    std::vector<svc::Mail>                       mails;
    std::vector<std::vector<svc::InventoryItem>> inventory_writes;
    svc::NewMail                                 last_created;
    Gate                                         inventory_gate;
    std::string                                  inventory_gate_id;
    bool                                         inventory_gate_on = false;
    svc::CallStatus                              inventory_answer = svc::CallStatus::Ok;
    svc::CallStatus                              create_status = svc::CallStatus::Ok;
    std::string                                  create_reason = "the store refused the mail";
    bool                                         throw_load = false, throw_save = false, throw_create = false, throw_inventory = false;
    size_t                                       loads = 0, saves = 0, creates = 0, inventories = 0, six_argument_calls = 0;

    svc::CallStatus record_inventory(const std::string& character_id, const std::vector<svc::InventoryItem>& items, bool six_argument) {
        {
            std::lock_guard<std::mutex> lock(mail_mu);
            calls.push_back("inventory " + character_id);
            ++inventories;
            if (six_argument) ++six_argument_calls;
        }
        if (inventory_gate_on && character_id == inventory_gate_id) inventory_gate.block();
        std::lock_guard<std::mutex> lock(mail_mu);
        if (throw_inventory) throw std::runtime_error("the character store is down");
        inventory_writes.push_back(items);
        return inventory_answer;
    }

    svc::MailsResult load_mails(const std::string& character_id) override {
        std::lock_guard<std::mutex> lock(mail_mu);
        calls.push_back("load " + character_id);
        ++loads;
        if (throw_load) throw std::runtime_error("the mail store is down");
        svc::MailsResult result;
        result.status = svc::CallStatus::Ok;
        result.mails = mails;
        return result;
    }

    svc::MailSaveResult save_mail(const std::string& character_id, const svc::Mail& mail) override {
        std::lock_guard<std::mutex> lock(mail_mu);
        calls.push_back("mail " + character_id);
        ++saves;
        if (throw_save) throw std::runtime_error("the mail store is down");
        svc::MailSaveResult result;
        result.status = svc::CallStatus::Ok;
        for (svc::Mail& stored : mails) {
            if (stored.id != mail.id) continue;
            stored = mail;
            return result;
        }
        result.status = svc::CallStatus::NotFound;
        result.reason = "no such mail";
        return result;
    }

    svc::MailResult create_mail(const std::string& character_id, const svc::NewMail& mail) override {
        std::lock_guard<std::mutex> lock(mail_mu);
        calls.push_back("create " + character_id);
        ++creates;
        last_created = mail;
        if (throw_create) throw std::runtime_error("the mail store is down");
        svc::MailResult result;
        if (create_status != svc::CallStatus::Ok) {
            result.status = create_status;
            result.reason = create_reason;
            return result;
        }
        svc::Mail made;
        made.id = test_mail_saver_hex(900 + static_cast<uint32_t>(creates));
        made.sender = mail.sender;
        made.subject = mail.subject;
        made.body = mail.body;
        made.issued_at_ms = 1'700'000'000'000ll;
        made.expires_at_ms = mail.expires_in_ms == 0 ? 0 : made.issued_at_ms + mail.expires_in_ms;
        made.items = mail.items;
        for (svc::MailItem& item : made.items) {
            item.item.instance_guid = test_mail_saver_hex(950 + static_cast<uint32_t>(creates));
            item.item.created_at_ms = made.issued_at_ms;
        }
        mails.insert(mails.begin(), made);
        result.status = svc::CallStatus::Ok;
        result.mail = made;
        return result;
    }

    svc::CallStatus save_inventory(const std::string& character_id, const std::vector<svc::InventoryItem>& items) override {
        return record_inventory(character_id, items, false);
    }

    svc::CallStatus save_inventory(const std::string& character_id, const std::vector<svc::InventoryItem>& items, const std::vector<svc::WalletEntry>& wallet,
                                   const std::vector<svc::ProcessingJob>& jobs, const std::vector<int64_t>& known_recipes,
                                   const std::vector<svc::WarehouseItem>& warehouse) override {
        (void)wallet;
        (void)jobs;
        (void)known_recipes;
        (void)warehouse;
        return record_inventory(character_id, items, true);
    }
};

static svc::InventoryItem test_mail_saver_item(uint32_t n) {
    svc::InventoryItem item;
    item.container = svc::ItemContainer::Backpack;
    item.slot = 0;
    item.item_record_id = 6064630214426886486ll;
    item.instance_guid = test_mail_saver_hex(n);
    item.quantity = 1;
    item.quality = 1;
    item.created_at_ms = 1'700'000'000'000ll;
    return item;
}

static svc::Mail test_mail_saver_mail(uint32_t n) {
    svc::Mail mail;
    mail.id = test_mail_saver_hex(n);
    mail.sender = "Sender";
    mail.subject = "Subject";
    mail.body = "Body";
    mail.issued_at_ms = 1'700'000'000'000ll;
    mail.expires_at_ms = 1'700'003'600'000ll;
    mail.items.push_back(svc::MailItem{test_mail_saver_item(n + 100), false});
    return mail;
}

}  // namespace

static void test_mail_saver_forwards_the_three_mail_calls() {
    std::printf("the saver forwards the three mail calls to the character service and returns its results\n");
    std::fflush(stdout);
    MailSaverApi api;
    api.mails.push_back(test_mail_saver_mail(1));
    CharacterSaver saver(api);
    const svc::MailsResult loaded = saver.load_mails("char-a");
    CHECK(loaded.status == svc::CallStatus::Ok, "the load answers the service's Ok");
    CHECK(loaded.mails.size() == 1 && loaded.mails[0].id == test_mail_saver_hex(1), "the load carries the service's mails");
    CHECK(api.loads == 1 && !api.calls.empty() && api.calls[0] == "load char-a", "the service got the character's id");
    const svc::Mail changed = test_mail_saver_mail(2);
    api.mails.push_back(changed);
    const svc::MailSaveResult saved = saver.save_mail("char-a", changed);
    CHECK(saved.status == svc::CallStatus::Ok, "the mail save answers the service's Ok");
    CHECK(api.saves == 1 && api.calls.size() >= 2 && api.calls[1] == "mail char-a", "the service got the character's id and the mail");
    bool stored = false;
    for (const svc::Mail& mail : api.mails) stored = stored || (mail.id == changed.id && mail.subject == changed.subject);
    CHECK(stored, "the service got the mail whole");
    const svc::MailSaveResult missing = saver.save_mail("char-a", test_mail_saver_mail(77));
    CHECK(missing.status == svc::CallStatus::NotFound, "the service's NotFound comes back");
    svc::NewMail fresh;
    fresh.sender = "Admin";
    fresh.subject = "A new mail";
    fresh.body = "With an item";
    fresh.items.push_back(svc::MailItem{test_mail_saver_item(300), false});
    const svc::MailResult made = saver.create_mail("char-a", fresh);
    CHECK(made.status == svc::CallStatus::Ok && made.mail.id == test_mail_saver_hex(901), "the create answers the service's new mail");
    CHECK(api.last_created.sender == "Admin" && api.last_created.subject == "A new mail" && api.last_created.items.size() == 1,
          "the service got the new mail's fields");
    api.create_status = svc::CallStatus::Refused;
    const svc::MailResult refused = saver.create_mail("char-a", fresh);
    CHECK(refused.status == svc::CallStatus::Refused && refused.reason == "the store refused the mail", "a refusal carries the service's reason");
}

static void test_mail_saver_save_inventory_now_passes_the_six_arguments() {
    std::printf("the direct bag save passes the six arguments to the six-argument service call and returns its status\n");
    std::fflush(stdout);
    MailSaverApi api;
    CharacterSaver saver(api);
    const std::vector<svc::InventoryItem> items = {test_mail_saver_item(11), test_mail_saver_item(12)};
    const std::vector<svc::WalletEntry> wallet = {{1, 500}};
    const std::vector<svc::ProcessingJob> jobs(1);
    const std::vector<int64_t> recipes = {41, 42};
    const std::vector<svc::WarehouseItem> warehouse(1);
    const svc::CallStatus saved = saver.save_inventory_now("char-a", items, wallet, jobs, recipes, warehouse);
    CHECK(saved == svc::CallStatus::Ok, "the direct save answers the service's Ok");
    CHECK(saver.inventories_saved() == 1, "the direct save counts its Ok");
    CHECK(api.six_argument_calls == 1 && api.inventory_writes.size() == 1, "the service's six-argument save was called once");
    CHECK(api.inventory_writes.size() == 1 && api.inventory_writes[0].size() == 2 && api.inventory_writes[0][0].instance_guid == test_mail_saver_hex(11),
          "the items reached the service");
    api.inventory_answer = svc::CallStatus::Unavailable;
    CHECK(saver.save_inventory_now("char-a", items, wallet, jobs, recipes, warehouse) == svc::CallStatus::Unavailable,
          "the service's Unavailable comes back");
}

static void test_mail_saver_save_inventory_now_waits_for_the_same_characters_inventory() {
    std::printf("the direct bag save waits for the same character's inventory write and writes the newer bag after it\n");
    std::fflush(stdout);
    MailSaverApi api;
    api.inventory_gate_id = "char-a";
    api.inventory_gate_on = true;
    CharacterSaver saver(api);
    saver.save_inventory("char-a", {test_mail_saver_item(21)});
    CHECK(api.inventory_gate.wait_entered(), "the worker reached the held inventory write");
    std::atomic<bool> started{false}, returned{false};
    std::atomic<int>  answer{static_cast<int>(svc::CallStatus::Failed)};
    std::thread direct([&] {
        started = true;
        answer = static_cast<int>(saver.save_inventory_now("char-a", {test_mail_saver_item(22)}, {}, {}, {}, {}));
        returned = true;
    });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!returned, "the direct save waits while the same character's inventory write is in flight");
    api.inventory_gate.release();
    direct.join();
    CHECK(static_cast<svc::CallStatus>(answer.load()) == svc::CallStatus::Ok, "the direct save answers the service's Ok");
    CHECK(api.inventory_writes.size() == 2, "two inventory writes reached the service");
    CHECK(api.inventory_writes.size() == 2 && api.inventory_writes[0][0].instance_guid == test_mail_saver_hex(21) &&
              api.inventory_writes[1][0].instance_guid == test_mail_saver_hex(22),
          "the older write is first and the newer bag last");
}

static void test_mail_saver_mail_throw_answers_failed() {
    std::printf("a throw in each of the three mail forwards is caught and answered Failed\n");
    std::fflush(stdout);
    MailSaverApi api;
    CharacterSaver saver(api);
    api.mails.push_back(test_mail_saver_mail(31));
    CHECK(saver.save_mail("char-a", test_mail_saver_mail(31)).status == svc::CallStatus::Ok, "the mail save works before the throw");
    api.throw_save = true;
    CHECK(saver.save_mail("char-a", test_mail_saver_mail(31)).status == svc::CallStatus::Failed, "a throwing save answers Failed");
    api.throw_load = true;
    CHECK(saver.load_mails("char-a").status == svc::CallStatus::Failed, "a throwing load answers Failed");
    svc::NewMail fresh;
    fresh.subject = "Subject";
    api.throw_create = true;
    CHECK(saver.create_mail("char-a", fresh).status == svc::CallStatus::Failed, "a throwing create answers Failed");
}

static void test_mail_saver_inventory_throw_answers_unavailable() {
    std::printf("a throw in the direct bag save is caught and answered Unavailable\n");
    std::fflush(stdout);
    MailSaverApi api;
    CharacterSaver saver(api);
    CHECK(saver.save_inventory_now("char-a", {test_mail_saver_item(41)}, {}, {}, {}, {}) == svc::CallStatus::Ok,
          "the direct save works before the throw");
    api.throw_inventory = true;
    CHECK(saver.save_inventory_now("char-a", {test_mail_saver_item(42)}, {}, {}, {}, {}) == svc::CallStatus::Unavailable,
          "a throwing direct save answers Unavailable");
}
AOC_REGISTER_TEST(test_mail_saver_forwards_the_three_mail_calls);
AOC_REGISTER_TEST(test_mail_saver_save_inventory_now_passes_the_six_arguments);
AOC_REGISTER_TEST(test_mail_saver_save_inventory_now_waits_for_the_same_characters_inventory);
AOC_REGISTER_TEST(test_mail_saver_mail_throw_answers_failed);
AOC_REGISTER_TEST(test_mail_saver_inventory_throw_answers_unavailable);
