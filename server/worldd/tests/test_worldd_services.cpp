// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_services.cpp: svc/grpc_services against in-process gRPC fakes
//  of the gateway, character and mesh services.
// ============================================================================
#include "svc/grpc_services.h"
#include "svc/service_token.h"
#include "aoc/character/v1/character.grpc.pb.h"
#include "aoc/gateway/v1/gateway.grpc.pb.h"
#include "aoc/mesh/v1/mesh.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <utility>

using namespace aoc::worldd;
namespace gatewayv1 = aoc::gateway::v1;
namespace characterv1 = aoc::character::v1;
namespace meshv1 = aoc::mesh::v1;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kToken(64, 'a');

static std::string auth_of(grpc::ServerContext* ctx) {
    const auto& md = ctx->client_metadata();
    auto it = md.find(svc::kAuthorizationMetadata);
    return it == md.end() ? "" : std::string(it->second.data(), it->second.size());
}

struct Gateway final : gatewayv1::GatewayService::Service {
    grpc::StatusCode code = grpc::StatusCode::OK;
    std::string auth, ticket, character;
    uint32_t server = 0;
    std::chrono::system_clock::time_point deadline;
    grpc::Status RedeemTicket(grpc::ServerContext* ctx, const gatewayv1::RedeemTicketRequest* req,
                              gatewayv1::RedeemTicketResponse* resp) override {
        auth = auth_of(ctx);
        deadline = ctx->deadline();
        ticket = req->ticket();
        character = req->character_id();
        server = req->server_id();
        if (code != grpc::StatusCode::OK) return grpc::Status(code, "no");
        resp->set_account_id(3);
        resp->set_character_id(req->character_id());
        return grpc::Status::OK;
    }
};

struct SavedState {
    std::string id;
    bool has_position = false;
    double x = 0, y = 0, z = 0;
    float yaw = 0;
    std::string last_region;
    int32_t level = 0;
    int calls = 0;
};
// C.O.A with C Y B

/// Mail (migration 0027): a mail as the character service sends it, written from worldd's form.
static void set_proto_mail(characterv1::Mail* out, const svc::Mail& m) {
    out->set_id(m.id);
    out->set_sender(m.sender);
    out->set_subject(m.subject);
    out->set_body(m.body);
    out->set_status(m.status);
    out->set_issued_at_ms(m.issued_at_ms);
    out->set_expires_at_ms(m.expires_at_ms);
    for (const svc::MailItem& it : m.items) {
        auto* p = out->add_items();
        p->set_item_record_id(it.item.item_record_id);
        p->set_instance_guid(it.item.instance_guid);
        p->set_quantity(it.item.quantity);
        p->set_quality(it.item.quality);
        p->set_durability(it.item.durability);
        p->set_created_at_ms(it.item.created_at_ms);
        if (it.item.persisted_health) p->set_persisted_health(*it.item.persisted_health);
        if (it.item.persisted_mana) p->set_persisted_mana(*it.item.persisted_mana);
        p->set_lock_expiration(it.item.lock_expiration);
        p->set_claimed(it.claimed);
    }
}

/// Mail (migration 0027): the service's mail as worldd reads it; an attachment's container and slot are the placeholders until a claim moves it into the bags.
static svc::Mail mail_from_proto(const characterv1::Mail& m) {
    svc::Mail out;
    out.id = m.id();
    out.sender = m.sender();
    out.subject = m.subject();
    out.body = m.body();
    out.status = m.status();
    out.issued_at_ms = m.issued_at_ms();
    out.expires_at_ms = m.expires_at_ms();
    for (const auto& i : m.items()) {
        svc::MailItem it;
        it.item.container = svc::ItemContainer::Backpack;
        it.item.slot = 0;
        it.item.item_record_id = i.item_record_id();
        it.item.instance_guid = i.instance_guid();
        it.item.quantity = i.quantity();
        it.item.quality = i.quality();
        it.item.durability = i.durability();
        it.item.created_at_ms = i.created_at_ms();
        if (i.has_persisted_health()) it.item.persisted_health = i.persisted_health();
        if (i.has_persisted_mana()) it.item.persisted_mana = i.persisted_mana();
        it.item.lock_expiration = i.lock_expiration();
        it.claimed = i.claimed();
        out.items.push_back(std::move(it));
    }
    return out;
}

/// Mail (migration 0027): the create request as worldd reads it.
static svc::NewMail new_mail_from_proto(const characterv1::CreateMailRequest& r) {
    svc::NewMail out;
    out.sender = r.sender();
    out.subject = r.subject();
    out.body = r.body();
    out.expires_in_ms = r.expires_in_ms();
    for (const auto& i : r.items()) {
        svc::MailItem it;
        it.item.container = svc::ItemContainer::Backpack;
        it.item.slot = 0;
        it.item.item_record_id = i.item_record_id();
        it.item.instance_guid = i.instance_guid();
        it.item.quantity = i.quantity();
        it.item.quality = i.quality();
        it.item.durability = i.durability();
        it.item.created_at_ms = i.created_at_ms();
        if (i.has_persisted_health()) it.item.persisted_health = i.persisted_health();
        if (i.has_persisted_mana()) it.item.persisted_mana = i.persisted_mana();
        it.item.lock_expiration = i.lock_expiration();
        it.claimed = i.claimed();
        out.items.push_back(std::move(it));
    }
    return out;
}

struct Characters final : characterv1::CharacterService::Service {
    grpc::StatusCode code = grpc::StatusCode::OK;
    grpc::StatusCode save_code = grpc::StatusCode::OK;
    // Gathering stage 2: SaveProgress's professions as the service received them, and what GetCharacter answers with
    struct Prof {
        int64_t guid = 0;
        int32_t tier = 0, level = 0;
        int64_t xp = 0;
    };
    std::vector<Prof> got_professions, reply_professions;
    // Social step 6: SaveProgress's ignored names as the service received them, and what GetCharacter answers with
    bool got_replace_ignored = false;
    std::vector<std::string> got_ignored_characters, got_ignored_guilds, reply_ignored_characters, reply_ignored_guilds;
    // Social step 7: SaveProgress's friends and pending requests as the service received them, and what GetCharacter answers with
    bool got_replace_friends = false, got_replace_friend_requests = false;
    std::vector<svc::Friend> got_friends, reply_friends;
    std::vector<svc::FriendRequest> got_friend_requests, reply_friend_requests;
    int progress_calls = 0;
    int32_t got_level = 0;
    std::chrono::milliseconds save_delay{0};   // a service that hangs on SaveCharacterState
    bool with_position = true;
    std::string auth;
    SavedState saved;
    // Social step 7: GetCharacterByName's answer and what the call carried
    grpc::StatusCode name_code = grpc::StatusCode::OK;
    std::string got_name;
    int name_calls = 0;
    // Mail (migration 0027): the answers of GetMails, SaveMail and CreateMail, and what each call carried
    grpc::StatusCode mails_code = grpc::StatusCode::OK, save_mail_code = grpc::StatusCode::OK, create_mail_code = grpc::StatusCode::OK;
    std::string mails_reason = "no mails", save_mail_reason = "the mail save is stale", create_mail_reason = "the mail is refused";
    std::vector<svc::Mail> reply_mails;
    svc::Mail reply_created, got_mail;
    svc::NewMail got_new_mail;
    std::string got_mails_character, got_save_mail_character, got_create_mail_character;
    int32_t reply_unread_mail_count = 0;
    int mails_calls = 0, save_mail_calls = 0, create_mail_calls = 0;
    grpc::Status GetMails(grpc::ServerContext* ctx, const characterv1::GetMailsRequest* req, characterv1::GetMailsResponse* resp) override {
        auth = auth_of(ctx);
        ++mails_calls;
        got_mails_character = req->character_id();
        if (mails_code != grpc::StatusCode::OK) return grpc::Status(mails_code, mails_reason);
        for (const svc::Mail& m : reply_mails) set_proto_mail(resp->add_mails(), m);
        return grpc::Status::OK;
    }
    grpc::Status SaveMail(grpc::ServerContext* ctx, const characterv1::SaveMailRequest* req, characterv1::SaveMailResponse*) override {
        auth = auth_of(ctx);
        ++save_mail_calls;
        got_save_mail_character = req->character_id();
        got_mail = mail_from_proto(req->mail());
        if (save_mail_code != grpc::StatusCode::OK) return grpc::Status(save_mail_code, save_mail_reason);
        return grpc::Status::OK;
    }
    grpc::Status CreateMail(grpc::ServerContext* ctx, const characterv1::CreateMailRequest* req, characterv1::CreateMailResponse* resp) override {
        auth = auth_of(ctx);
        ++create_mail_calls;
        got_create_mail_character = req->character_id();
        got_new_mail = new_mail_from_proto(*req);
        if (create_mail_code != grpc::StatusCode::OK) return grpc::Status(create_mail_code, create_mail_reason);
        set_proto_mail(resp->mutable_mail(), reply_created);
        return grpc::Status::OK;
    }
    grpc::Status GetCharacterByName(grpc::ServerContext* ctx, const characterv1::GetCharacterByNameRequest* req,
                                    characterv1::GetCharacterByNameResponse* resp) override {
        auth = auth_of(ctx);
        ++name_calls;
        got_name = req->name();
        if (name_code != grpc::StatusCode::OK) return grpc::Status(name_code, "no");
        auto* c = resp->mutable_character();
        c->set_id("11112222333344445555666677778888");
        c->set_account_id(3);
        c->set_name("Dee");
        c->set_race_id(1);
        c->set_gender_id(0);
        c->set_class_id(4);
        c->set_archetype_id(17748);
        c->set_class_name("Fighter");
        c->set_level(9);
        return grpc::Status::OK;
    }
    grpc::Status SaveCharacterState(grpc::ServerContext* ctx, const characterv1::SaveCharacterStateRequest* req,
                                    characterv1::SaveCharacterStateResponse*) override {
        auth = auth_of(ctx);
        ++saved.calls;
        if (save_delay.count() > 0) std::this_thread::sleep_for(save_delay);
        saved.id = req->id();
        saved.has_position = req->has_position();
        saved.x = req->position().x();
        saved.y = req->position().y();
        saved.z = req->position().z();
        saved.yaw = req->position().yaw();
        saved.last_region = req->last_region();
        saved.level = req->level();
        return save_code == grpc::StatusCode::OK ? grpc::Status::OK : grpc::Status(save_code, "no");
    }
    grpc::Status SaveProgress(grpc::ServerContext* ctx, const characterv1::SaveProgressRequest* req, characterv1::SaveProgressResponse*) override {
        auth = auth_of(ctx);
        ++progress_calls;
        got_level = req->level();
        got_professions.clear();
        for (const auto& p : req->professions()) got_professions.push_back({p.profession_guid(), p.certification_tier(), p.level(), p.xp()});
        got_replace_ignored = req->replace_ignored_names();
        got_ignored_characters.assign(req->ignored_character_names().begin(), req->ignored_character_names().end());
        got_ignored_guilds.assign(req->ignored_guild_names().begin(), req->ignored_guild_names().end());
        got_replace_friends = req->replace_friends();
        got_friends.clear();
        for (const auto& f : req->friends()) got_friends.push_back({f.guid(), f.name(), f.level(), f.class_id(), f.sub_archetype(), f.guild_name(), f.still_friend()});
        got_replace_friend_requests = req->replace_friend_requests();
        got_friend_requests.clear();
        for (const auto& q : req->friend_requests()) got_friend_requests.push_back({q.sender_guid(), q.sender_name(), q.recipient_guid(), q.expires_at_ms()});
        return save_code == grpc::StatusCode::OK ? grpc::Status::OK : grpc::Status(save_code, "no");
    }
    grpc::Status GetCharacter(grpc::ServerContext* ctx, const characterv1::GetCharacterRequest* req,
                              characterv1::GetCharacterResponse* resp) override {
        auth = auth_of(ctx);
        if (code != grpc::StatusCode::OK) return grpc::Status(code, "no");
        auto* c = resp->mutable_character();
        c->set_id(req->id());
        c->set_account_id(3);
        c->set_name("cyb");
        c->set_race_id(6);
        c->set_gender_id(1);
        c->set_class_id(7);
        c->set_archetype_id(17754);
        c->set_class_name("Bard");
        c->set_appearance_json("{}");
        c->set_level(5);
        c->set_last_region("Riverlands");
        for (const std::string& n : reply_ignored_characters) c->add_ignored_character_names(n);
        for (const std::string& n : reply_ignored_guilds) c->add_ignored_guild_names(n);
        for (const svc::Friend& f : reply_friends) {
            auto* x = c->add_friends();
            x->set_guid(f.guid);
            x->set_name(f.name);
            x->set_level(f.level);
            x->set_class_id(f.class_id);
            x->set_sub_archetype(f.sub_archetype);
            x->set_guild_name(f.guild_name);
            x->set_still_friend(f.still_friend);
        }
        for (const svc::FriendRequest& q : reply_friend_requests) {
            auto* x = c->add_friend_requests();
            x->set_sender_guid(q.sender_guid);
            x->set_sender_name(q.sender_name);
            x->set_recipient_guid(q.recipient_guid);
            x->set_expires_at_ms(q.expires_at_ms);
        }
        for (const Prof& p : reply_professions) {
            auto* x = c->add_professions();
            x->set_profession_guid(p.guid);
            x->set_certification_tier(p.tier);
            x->set_level(p.level);
            x->set_xp(p.xp);
        }
        if (with_position) {
            c->mutable_position()->set_x(1.5);
            c->mutable_position()->set_y(-2.5);
            c->mutable_position()->set_z(3.0);
            c->mutable_position()->set_yaw(90.0f);
        }
        c->set_unread_mail_count(reply_unread_mail_count);
        return grpc::Status::OK;
    }
};

struct Mesh final : meshv1::MeshService::Service {
    grpc::StatusCode heartbeat_code = grpc::StatusCode::OK;
    std::string auth, address;
    uint32_t heartbeat_id = 0;
    grpc::Status Register(grpc::ServerContext* ctx, const meshv1::RegisterRequest* req,
                          meshv1::RegisterResponse* resp) override {
        auth = auth_of(ctx);
        address = req->address();
        resp->set_server_id(7);
        resp->set_heartbeat_interval_ms(5000);
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext* ctx, const meshv1::HeartbeatRequest* req,
                           meshv1::HeartbeatResponse*) override {
        auth = auth_of(ctx);
        heartbeat_id = req->server_id();
        return heartbeat_code == grpc::StatusCode::OK ? grpc::Status::OK : grpc::Status(heartbeat_code, "no");
    }
};

/// Social, step 7: GetCharacter's friends and pending requests reach the character record, SaveProgress sends both lists back with their replace flags, and the progress equality sees
/// every new field (the saver skips a save only when nothing changed).
static void test_friends_mapping(svc::GrpcServices& s, Characters& ch) {
    std::printf("friends (social step 7)\n");
    CHECK(s.get_character("x").character.friends.empty() && s.get_character("x").character.friend_requests.empty(),
          "a character record with no friends and no pending requests maps to none");
    const std::vector<svc::Friend> friends = {{"0123456789abcdef0123456789abcdef", "Brannoc", 25, 7, 2, "Sandal Makers", true},
                                              {"fedcba9876543210fedcba9876543210", "Mirelle", 1, 0, 0, "", false}};
    const std::vector<svc::FriendRequest> requests = {{"11111111111111111111111111111111", "Brannoc", "0123456789abcdef0123456789abcdef", 1772209600000},
                                                      {"22222222222222222222222222222222", "Mirelle", "0123456789abcdef0123456789abcdef", 1772209601000}};
    ch.reply_friends = friends;
    ch.reply_friend_requests = requests;
    {
        const auto fr = s.get_character("x");
        CHECK(fr.status == svc::CallStatus::Ok && fr.character.friends == friends && fr.character.friend_requests == requests,
              "GetCharacter maps the friends and the pending requests, each list in order and with every field");
    }
    {
        svc::Progress p;
        p.level = 3;
        p.replace_friends = true;
        p.friends = friends;
        p.replace_friend_requests = true;
        p.friend_requests = requests;
        CHECK(s.save_progress("x", p) == svc::CallStatus::Ok && ch.got_replace_friends && ch.got_friends == friends && ch.got_replace_friend_requests && ch.got_friend_requests == requests,
              "SaveProgress sends replace_friends and replace_friend_requests with both lists, each in order and with every field");
        svc::Progress none;
        none.level = 3;
        CHECK(s.save_progress("x", none) == svc::CallStatus::Ok && !ch.got_replace_friends && ch.got_friends.empty() && !ch.got_replace_friend_requests && ch.got_friend_requests.empty(),
              "a save that does not replace the lists sends neither flag nor an entry");
        svc::Progress emptied;
        emptied.replace_friends = true;
        emptied.replace_friend_requests = true;
        CHECK(s.save_progress("x", emptied) == svc::CallStatus::Ok && ch.got_replace_friends && ch.got_friends.empty() && ch.got_replace_friend_requests && ch.got_friend_requests.empty(),
              "an empty replacement sends both flags alone");
        svc::Progress a = p, b = p;
        CHECK(a == b, "equal progress with equal friends and pending requests");
        b.friends[0].still_friend = false;
        CHECK(!(a == b), "progress that differs in a friend's still-friend flag is not equal");
        b = p;
        b.friends[1].level += 1;
        CHECK(!(a == b), "progress that differs in a friend's level is not equal");
        b = p;
        b.friend_requests[1].expires_at_ms += 1000;
        CHECK(!(a == b), "progress that differs in a request's expiry is not equal");
        b = p;
        b.replace_friend_requests = false;
        CHECK(!(a == b), "progress that differs in replace_friend_requests is not equal");
    }
    ch.reply_friends.clear();
    ch.reply_friend_requests.clear();
    ch.progress_calls = 0;
}

/// Social, step 7: the real by-name call (GrpcServices::get_character_by_name) against the character fake: the fields the friend request and the name check
/// need, and the two statuses the call can meet.  Its cost is measured beside it so the check on the network thread can be judged.
static void test_name_lookup(svc::GrpcServices& s, Characters& ch) {
    std::printf("name lookup (social step 7)\n");
    const auto found = s.get_character_by_name("Dee");
    CHECK(found.status == svc::CallStatus::Ok && found.character.id == "11112222333344445555666677778888" && found.character.name == "Dee" &&
              found.character.level == 9 && found.character.class_id == 4,
          "get_character_by_name maps the id, name, level and class of the character it finds");
    CHECK(ch.got_name == "Dee", "the call carries the name the client sent");
    ch.name_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.get_character_by_name("Nobody_xyz").status == svc::CallStatus::NotFound, "NotFound -> NotFound");
    ch.name_code = grpc::StatusCode::UNAVAILABLE;
    CHECK(s.get_character_by_name("Dee").status == svc::CallStatus::Unavailable, "Unavailable -> Unavailable (worth retrying)");
    ch.name_code = grpc::StatusCode::OK;
    // the cost of one call: how many a 33.3 ms tick (30 a second) can carry
    constexpr int kCalls = 200;
    const auto start = std::chrono::steady_clock::now();
    svc::CallStatus last = svc::CallStatus::Failed;
    for (int i = 0; i < kCalls; ++i) last = s.get_character_by_name("Dee").status;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kCalls;
    const double per_tick = (1000.0 / 30.0) * 1000.0 / us;
    std::printf("    one by-name call: %.1f us (loopback, in-process service); a 33.3 ms tick carries %.0f of them\n", us, per_tick);
    CHECK(last == svc::CallStatus::Ok && us > 0.0 && us < (1000.0 / 30.0) * 1000.0, "one by-name call costs less than one tick");
    ch.name_calls = 0;
}

/// Mail (migration 0027): the three calls map every field, attachment, status and claimed flag; a stale save and a store that keeps no mails end with different
/// statuses (the caller tells them apart by the status, never by the reason's words); and the unread count helper counts live, unread mails.
static void test_mail_mapping(svc::GrpcServices& s, Characters& ch) {
    std::printf("mail (migration 0027)\n");
    CHECK(svc::kMailStatusDeleted == 2 && svc::kMailStatusRead == 4, "the mail status bits are 2 (deleted) and 4 (read)");

    svc::Mail full;
    full.id = "11112222333344445555666677778888";
    full.sender = "Brannoc";
    full.subject = "Welcome";
    full.body = "Line one.\nLine two.";
    full.status = svc::kMailStatusRead | svc::kMailStatusDeleted;
    full.issued_at_ms = 1771000000123;
    full.expires_at_ms = 1771600000123;
    svc::MailItem first;
    first.item.container = svc::ItemContainer::Backpack;
    first.item.slot = 0;
    first.item.item_record_id = 6064630214426886486;
    first.item.instance_guid = "0123456789abcdef0123456789abcdef";
    first.item.quantity = 3;
    first.item.quality = 7;
    first.item.durability = 42.5f;
    first.item.created_at_ms = 1780000000123;
    first.item.persisted_health = 3210.5f;
    first.item.persisted_mana = 120.5f;
    first.item.lock_expiration = 1772000000;
    first.claimed = true;
    svc::MailItem second;
    second.item.item_record_id = 6064631131688402948;
    second.item.instance_guid = "fedcba9876543210fedcba9876543210";
    second.item.quantity = 1;
    second.item.quality = 1;
    second.item.durability = 0;
    second.item.created_at_ms = 1780000000124;
    second.item.lock_expiration = 0;
    second.claimed = false;
    full.items = {first, second};

    svc::Mail other;
    other.id = "88887777666655554444333322221111";
    other.sender = "Mirelle";
    other.subject = "No attachments";
    other.body = "";
    other.status = 0;
    other.issued_at_ms = 1770000000000;
    other.expires_at_ms = 0;

    ch.reply_mails = {full, other};
    ch.mails_code = grpc::StatusCode::OK;
    ch.auth.clear();
    const auto loaded = s.load_mails("char-a");
    CHECK(loaded.status == svc::CallStatus::Ok && loaded.reason.empty() && ch.auth == "Bearer " + kToken && ch.got_mails_character == "char-a",
          "load_mails carries the character id and the token, and an Ok result carries no reason");
    CHECK(loaded.mails.size() == 2, "load_mails maps the two mails the service returned");
    if (loaded.mails.size() == 2) {
        CHECK(loaded.mails[0] == full, "load_mails maps every field of a mail: id, sender, subject, body, status, times and its attachments in order");
        CHECK(loaded.mails[0].items.size() == 2 && loaded.mails[0].items[0].item.container == svc::ItemContainer::Backpack && loaded.mails[0].items[0].item.slot == 0,
              "an attachment's container and slot are the placeholders until a claim moves it into the bags");
        CHECK(loaded.mails[1] == other, "load_mails maps a mail with no attachments and no expiry");
    }
    ch.mails_code = grpc::StatusCode::NOT_FOUND;
    const auto missing = s.load_mails("nobody");
    CHECK(missing.status == svc::CallStatus::NotFound && missing.reason == "no mails", "load_mails: NotFound carries the service's reason");
    ch.mails_code = grpc::StatusCode::UNIMPLEMENTED;
    const auto noStore = s.load_mails("nobody");
    CHECK(noStore.status == svc::CallStatus::Unsupported && noStore.reason == "no mails", "load_mails: Unimplemented (a store that keeps no mails) -> Unsupported");
    ch.mails_code = grpc::StatusCode::OK;

    ch.auth.clear();
    const auto saved = s.save_mail("char-a", full);
    CHECK(saved.status == svc::CallStatus::Ok && saved.reason.empty() && ch.auth == "Bearer " + kToken && ch.got_save_mail_character == "char-a",
          "save_mail carries the character id and the token, and Ok carries no reason");
    CHECK(ch.got_mail == full, "save_mail sends every field and every attachment with its persisted state and claimed flag");
    ch.save_mail_code = grpc::StatusCode::FAILED_PRECONDITION;
    const auto stale = s.save_mail("char-a", full);
    CHECK(stale.status == svc::CallStatus::Failed && stale.reason == "the mail save is stale", "a stale save is Failed with the service's reason");
    ch.save_mail_code = grpc::StatusCode::INVALID_ARGUMENT;
    CHECK(s.save_mail("char-a", full).status == svc::CallStatus::Refused, "save_mail: InvalidArgument (a refused mail) -> Refused");
    ch.save_mail_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.save_mail("char-a", full).status == svc::CallStatus::NotFound, "save_mail: NotFound -> NotFound");
    ch.save_mail_code = grpc::StatusCode::UNIMPLEMENTED;
    CHECK(s.save_mail("char-a", full).status == svc::CallStatus::Unsupported, "save_mail: Unimplemented (a store that keeps no mails) -> Unsupported");
    ch.save_mail_code = grpc::StatusCode::OK;

    svc::NewMail fresh;
    fresh.sender = "Mirelle";
    fresh.subject = "For you";
    fresh.body = "Two things.";
    fresh.expires_in_ms = 3600000000LL;
    fresh.items = {first, second};
    fresh.items[0].claimed = false;
    ch.reply_created = full;
    ch.auth.clear();
    const auto made = s.create_mail("char-a", fresh);
    CHECK(made.status == svc::CallStatus::Ok && made.reason.empty() && ch.auth == "Bearer " + kToken && ch.got_create_mail_character == "char-a",
          "create_mail carries the character id and the token, and an Ok result carries no reason");
    CHECK(ch.got_new_mail == fresh, "create_mail sends the sender, subject, body, duration and the attachments");
    CHECK(made.mail == full, "create_mail maps the stored mail the service returns");
    ch.create_mail_code = grpc::StatusCode::INVALID_ARGUMENT;
    CHECK(s.create_mail("char-a", fresh).status == svc::CallStatus::Refused, "create_mail: InvalidArgument (a refused mail or a full mailbox) -> Refused");
    ch.create_mail_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.create_mail("char-a", fresh).status == svc::CallStatus::NotFound, "create_mail: NotFound -> NotFound");
    ch.create_mail_code = grpc::StatusCode::UNIMPLEMENTED;
    CHECK(s.create_mail("char-a", fresh).status == svc::CallStatus::Unsupported, "create_mail: Unimplemented (a store that keeps no mails) -> Unsupported");
    ch.create_mail_code = grpc::StatusCode::OK;

    std::vector<svc::Mail> statuses(4);
    statuses[0].status = 0;
    statuses[1].status = svc::kMailStatusRead;
    statuses[2].status = svc::kMailStatusRead | svc::kMailStatusDeleted;
    statuses[3].status = svc::kMailStatusDeleted;
    CHECK(svc::unread_count(statuses) == 1, "unread_count counts the live mails without the read bit (0, 4, 6 and 2 give 1)");
    CHECK(svc::unread_count(std::vector<svc::Mail>{}) == 0, "unread_count of no mails is 0");

    svc::Mail same = full;
    CHECK(same == full, "equal mails compare equal");
    same.subject = "Another title";
    CHECK(!(same == full), "mails that differ in the subject are not equal");
    same = full;
    same.items[1].claimed = true;
    CHECK(!(same == full), "mails that differ in an attachment's claimed flag are not equal");
    same = full;
    same.items[0].item.persisted_health = 1.0f;
    CHECK(!(same == full), "mails that differ in an attachment's persisted health are not equal");
    same = full;
    same.expires_at_ms += 1;
    CHECK(!(same == full), "mails that differ in their expiry are not equal");
    svc::NewMail a = fresh, b = fresh;
    CHECK(a == b, "equal new mails compare equal");
    b.expires_in_ms = 0;
    CHECK(!(a == b), "new mails that differ in the duration are not equal");
    ch.reply_mails.clear();
    ch.reply_created = {};
}

/// Mail (migration 0027): the unread count at the login comes with the character load (GetCharacter with the items).
static void test_unread_mail_count_at_login(svc::GrpcServices& s, Characters& ch) {
    std::printf("mail unread count at login\n");
    ch.reply_unread_mail_count = 3;
    const auto loaded = s.get_character("char-a");
    CHECK(loaded.status == svc::CallStatus::Ok && loaded.character.unread_mail_count == 3, "GetCharacter maps the unread mail count for the login");
    ch.reply_unread_mail_count = 0;
    CHECK(s.get_character("char-a").character.unread_mail_count == 0, "a character with no unread mail carries 0");
}

int main() {
    Gateway gw;
    Characters ch;
    Mesh mesh;
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&gw);
    b.RegisterService(&ch);
    b.RegisterService(&mesh);
    auto server = b.BuildAndStart();
    CHECK(server && port > 0, "fake services listen");
    if (!server) return 1;
    const std::string addr = "127.0.0.1:" + std::to_string(port);
    svc::GrpcServices s({addr, addr, addr}, kToken, std::chrono::seconds(2));

    std::printf("gateway\n");
    const auto before = std::chrono::system_clock::now();
    auto r = s.redeem_ticket("0123456789abcdef0123456789abcdef", "bbec5c86eb101fe5f38c87f0fa22e7d0", 7);
    CHECK(r.status == svc::CallStatus::Ok && r.account_id == 3 && r.character_id == "bbec5c86eb101fe5f38c87f0fa22e7d0",
          "RedeemTicket returns the account and character");
    CHECK(gw.auth == "Bearer " + kToken, "the call carries the service token");
    CHECK(gw.ticket == "0123456789abcdef0123456789abcdef" && gw.server == 7, "ticket and server id are sent");
    CHECK(gw.deadline > before && gw.deadline <= before + std::chrono::seconds(3), "the call has a 2 s deadline");
    gw.code = grpc::StatusCode::PERMISSION_DENIED;
    CHECK(s.redeem_ticket("x", "y", 7).status == svc::CallStatus::Refused, "PermissionDenied -> Refused");
    gw.code = grpc::StatusCode::ALREADY_EXISTS;
    CHECK(s.redeem_ticket("x", "y", 7).status == svc::CallStatus::Refused, "AlreadyExists (a guild name another guild has) -> Refused");
    gw.code = grpc::StatusCode::UNAUTHENTICATED;
    CHECK(s.redeem_ticket("x", "y", 7).status == svc::CallStatus::Failed,
          "Unauthenticated (a wrong service token) -> Failed");
// C-O-A x Cyb

    std::printf("character\n");
    auto c = s.get_character("bbec5c86eb101fe5f38c87f0fa22e7d0");
    CHECK(c.status == svc::CallStatus::Ok && c.character.name == "cyb" && c.character.account_id == 3 &&
          c.character.race_id == 6 && c.character.gender_id == 1 && c.character.class_id == 7 && c.character.archetype_id == 17754 &&
          c.character.class_name == "Bard" && c.character.level == 5 && c.character.last_region == "Riverlands",
          "GetCharacter maps the record");
    CHECK(c.character.has_position && c.character.position.x == 1.5 && c.character.position.y == -2.5 &&
          c.character.position.yaw == 90.0f,
          "with its position");
    ch.with_position = false;
    CHECK(!s.get_character("x").character.has_position, "no position until the first save");
    ch.code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.get_character("x").status == svc::CallStatus::NotFound, "NotFound -> NotFound");
    CHECK(ch.auth == "Bearer " + kToken, "it carries the token too");

    std::printf("professions (Gathering stage 2)\n");
    ch.code = grpc::StatusCode::OK;
    CHECK(s.get_character("x").character.professions.empty(), "a character record with no professions maps to none");
    ch.reply_professions = {{108752769996425, 2, 25, 250000}, {108752769996426, 0, 9, 32400}};
    {
        const auto pc = s.get_character("x");
        CHECK(pc.status == svc::CallStatus::Ok && pc.character.professions.size() == 2 &&
                  pc.character.professions[0] == (svc::ProfessionState{108752769996425, 2, 25, 250000}) &&
                  pc.character.professions[1] == (svc::ProfessionState{108752769996426, 0, 9, 32400}),
              "GetCharacter maps the professions: guid, certification tier, level and XP, in order");
    }
    {
        svc::Progress p;
        p.level = 12;
        p.professions = {{108752769996425, 3, 40, 640000}, {108752770061963, 1, 11, 48400}};
        CHECK(s.save_progress("bbec5c86eb101fe5f38c87f0fa22e7d0", p) == svc::CallStatus::Ok && ch.progress_calls == 1 && ch.got_level == 12 &&
                  ch.got_professions.size() == 2 && ch.got_professions[0].guid == 108752769996425 && ch.got_professions[0].tier == 3 &&
                  ch.got_professions[0].level == 40 && ch.got_professions[0].xp == 640000 && ch.got_professions[1].guid == 108752770061963 &&
                  ch.got_professions[1].tier == 1 && ch.got_professions[1].level == 11 && ch.got_professions[1].xp == 48400,
              "SaveProgress sends the professions, each with its tier, level and XP");
        svc::Progress none;
        none.level = 3;
        CHECK(s.save_progress("x", none) == svc::CallStatus::Ok && ch.got_professions.empty(), "a save without professions sends none");
        svc::Progress a = p, b = p;
        CHECK(a == b, "equal progress with equal professions");
        b.professions[1].xp += 1;
        CHECK(!(a == b), "progress that differs in a profession's XP is not equal (the saver skips a save only when nothing changed)");
    }
    ch.reply_professions.clear();
    ch.progress_calls = 0;

    std::printf("ignored names (social step 6)\n");
    CHECK(s.get_character("x").character.ignored_character_names.empty() && s.get_character("x").character.ignored_guild_names.empty(),
          "a character record with no ignored names maps to none");
    const std::vector<std::string> players = {"Brannoc", "Mirelle", "Tavish"}, guilds = {"Sandal Makers"};
    ch.reply_ignored_characters = players;
    ch.reply_ignored_guilds = guilds;
    {
        const auto ic = s.get_character("x");
        CHECK(ic.status == svc::CallStatus::Ok && ic.character.ignored_character_names == players && ic.character.ignored_guild_names == guilds,
              "GetCharacter maps the ignored players and guilds, each list in order");
    }
    {
        svc::Progress p;
        p.level = 3;
        p.replace_ignored_names = true;
        p.ignored_character_names = players;
        p.ignored_guild_names = guilds;
        CHECK(s.save_progress("x", p) == svc::CallStatus::Ok && ch.got_replace_ignored && ch.got_ignored_characters == players && ch.got_ignored_guilds == guilds,
              "SaveProgress sends replace_ignored_names and both lists, each in order");
        svc::Progress none;
        none.level = 3;
        CHECK(s.save_progress("x", none) == svc::CallStatus::Ok && !ch.got_replace_ignored && ch.got_ignored_characters.empty() && ch.got_ignored_guilds.empty(),
              "a save that does not replace the names sends neither the flag nor a name");
        svc::Progress emptied;
        emptied.replace_ignored_names = true;
        CHECK(s.save_progress("x", emptied) == svc::CallStatus::Ok && ch.got_replace_ignored && ch.got_ignored_characters.empty() && ch.got_ignored_guilds.empty(),
              "an empty replacement sends the flag alone");
        svc::Progress a = p, b = p;
        CHECK(a == b, "equal progress with equal ignored names");
        b.ignored_character_names.push_back("Dorrin");
        CHECK(!(a == b), "progress that differs in an ignored player is not equal (the saver skips a save only when nothing changed)");
        b = p;
        b.ignored_guild_names.clear();
        CHECK(!(a == b), "progress that differs in an ignored guild is not equal");
        b = p;
        b.replace_ignored_names = false;
        CHECK(!(a == b), "progress that differs in replace_ignored_names is not equal");
    }
    ch.reply_ignored_characters.clear();
    ch.reply_ignored_guilds.clear();
    ch.progress_calls = 0;

    test_friends_mapping(s, ch);
    test_name_lookup(s, ch);
    test_mail_mapping(s, ch);
    test_unread_mail_count_at_login(s, ch);

    std::printf("save_position (plan A6)\n");
    ch.auth.clear();
    CHECK(s.save_position("bbec5c86eb101fe5f38c87f0fa22e7d0", {-777664.5, 616578.6875, 15952.0498046875, 90.9f}) == svc::CallStatus::Ok,
          "SaveCharacterState with a position: Ok");
    CHECK(ch.saved.calls == 1 && ch.saved.id == "bbec5c86eb101fe5f38c87f0fa22e7d0" && ch.saved.has_position && ch.saved.x == -777664.5 &&
              ch.saved.y == 616578.6875 && ch.saved.z == 15952.0498046875 && ch.saved.yaw == 90.9f,
          "the request has the id and x, y, z (doubles) and the yaw (float) as given");
    CHECK(ch.saved.last_region.empty() && ch.saved.level == 0,
          "last_region and level are left unset: the service keeps them (character.proto)");
    CHECK(ch.auth == "Bearer " + kToken, "the save carries the service token");
    ch.save_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.save_position("x", {}) == svc::CallStatus::NotFound, "NotFound -> NotFound");
    ch.save_code = grpc::StatusCode::INVALID_ARGUMENT;
    CHECK(s.save_position("x", {}) == svc::CallStatus::Refused, "InvalidArgument (a position the store rejects) -> Refused");
    ch.save_code = grpc::StatusCode::UNAVAILABLE;
    CHECK(s.save_position("x", {}) == svc::CallStatus::Unavailable, "Unavailable -> Unavailable (worth retrying)");
    ch.save_code = grpc::StatusCode::OK;
    ch.code = grpc::StatusCode::OK;
    {
        // the save has a deadline of its own, shorter than the other calls' 5 s (the saver's worker is waited for when worldd stops):
        // 200 ms here; the default is 2 s
        svc::GrpcServices impatient({addr, addr, addr}, kToken, std::chrono::seconds(3), std::chrono::milliseconds(200));
        ch.save_delay = std::chrono::milliseconds(1200);
        const auto begin = std::chrono::steady_clock::now();
        const svc::CallStatus status = impatient.save_position("x", {});
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(status == svc::CallStatus::Unavailable && took < std::chrono::milliseconds(900),
              "a save to a service that hangs gives up at its own deadline (Unavailable, retried by the saver), not at the 3 s of the other calls");
        ch.save_delay = std::chrono::milliseconds(0);
        CHECK(impatient.save_position("x", {}) == svc::CallStatus::Ok && impatient.get_character("x").status == svc::CallStatus::Ok,
              "(a service that answers is saved to, and the other calls keep their own deadline)");
    }
// (C | O | A | C Y B)

    std::printf("mesh\n");
    auto reg = s.register_server("127.0.0.1:7777", {});
    CHECK(reg.status == svc::CallStatus::Ok && reg.server_id == 7 && reg.heartbeat_interval_ms == 5000 &&
          mesh.address == "127.0.0.1:7777",
          "Register returns the server id and heartbeat interval");
    CHECK(s.heartbeat(7) == svc::CallStatus::Ok && mesh.heartbeat_id == 7, "Heartbeat");
    mesh.heartbeat_code = grpc::StatusCode::NOT_FOUND;
    CHECK(s.heartbeat(7) == svc::CallStatus::NotFound, "Heartbeat NotFound: mesh forgot the server");
    CHECK(mesh.auth == "Bearer " + kToken, "mesh calls carry the token");

    std::printf("status mapping and failures\n");
    CHECK(svc::map_grpc_status(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "")) == svc::CallStatus::Unavailable &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::UNAVAILABLE, "")) == svc::CallStatus::Unavailable &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "")) == svc::CallStatus::Refused &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "")) == svc::CallStatus::Unsupported &&
          svc::map_grpc_status(grpc::Status(grpc::StatusCode::INTERNAL, "")) == svc::CallStatus::Failed,
          "deadline and unavailable retry; invalid argument is a refusal; unimplemented is unsupported; internal fails");
    svc::GrpcServices down({"127.0.0.1:1", "127.0.0.1:1", "127.0.0.1:1"}, kToken, std::chrono::milliseconds(300));
    CHECK(down.redeem_ticket("x", "y", 1).status == svc::CallStatus::Unavailable, "an unreachable service -> Unavailable");
    server->Shutdown();

    std::printf("service token file\n");
    std::string err;
    const std::string path = "test_worldd_services.token";
    { std::ofstream(path) << "  " << kToken << "\r\n"; }
    const auto tok = svc::load_service_token(path, &err);
    CHECK(tok && *tok == kToken, "loads 64 hex digits, ignoring whitespace");
    { std::ofstream(path) << "ABC"; }
    CHECK(!svc::load_service_token(path, &err) && err.find("ABC") == std::string::npos,
          "refuses anything else, without quoting the file");
    std::remove(path.c_str());
    CHECK(!svc::load_service_token(path, &err) && err.find("services.ps1") != std::string::npos,
          "a missing file names the script that creates it");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
