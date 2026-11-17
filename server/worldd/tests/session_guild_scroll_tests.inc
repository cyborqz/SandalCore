// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_guild_scroll_tests.inc: the Guild Creation Scroll (Social, step 9)
//
//  The scroll is the data's: its item_use activation is OpenGuildCreationMenu.  Its use answers ClientOpenCreateGuildPanel (no parameters) and leaves
//  the scroll; ServerCreateGuild (the create panel's button) takes one scroll out of the item bag, as one Remove storage event, when it makes the
//  guild.  A create with no scroll is refused and makes nothing; a refused create (a taken name, the join cooldown, a character already in a guild)
//  keeps the scroll, and a character already in a guild that uses the scroll still opens the panel (the create is what refuses it).  The GM command is
//  not this path: it makes a guild with no scroll (session_guild_rules_tests.inc).
// ============================================================================
#include "guild_scroll_rig.inc"
#include "protocol/repl/storage_rpc.h"

namespace {

static bool test_guild_has_scroll(const PartyWorld& w, size_t who, int slot, const std::string& guid) {
    const svc::InventoryItem* held = w.p[who].s ? w.p[who].s->inventory().at(svc::ItemContainer::Backpack, slot) : nullptr;
    return held && held->item_record_id == kTestGuildScroll && held->instance_guid == guid;
}

/// One ServerItemUsed or ServerActivateItem for the item in item bag `slot`, as the retail client writes it.
static BunchWriter test_guild_item_use(const char* rpc_name, int slot, const std::string& guid_hex) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc(rpc_name);
    const auto g = items::guid_of(guid_hex);
    if (!rpc || !g) return BunchWriter();
    const repl::GuidValue gv = *g;
    repl::RpcParams params;
    repl::set_rpc_param(*rpc, params, "Storage", repl::Value::u64(3));   // the item bag
    repl::set_rpc_param(*rpc, params, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, params, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, params, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, params, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, params, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, params, "RequestedEquipSlot", repl::Value::i64(-1));
    return client_rpc(pawn, rpc_name, params);
}

/// The scroll's use, both calls in one datagram as the retail client sends them.
static bool test_guild_use_scroll(PartyWorld& w, size_t who, int slot, const std::string& guid) {
    if (!w[who].s) return false;
    const BunchWriter used = test_guild_item_use("ServerItemUsed", slot, guid);
    const BunchWriter activated = test_guild_item_use("ServerActivateItem", slot, guid);
    if (used.bit_pos() == 0 || activated.bit_pos() == 0) return false;
    const uint32_t channel = w[who].s->channel_of(bs::Role::Pawn);
    w.client_sends(w[who], {w[who].client.actor(channel, used, true), w[who].client.actor(channel, activated, true)});
    return true;
}

/// The PlayerController's decoded calls the client was given from `from` on, in order.
static std::vector<repl::RpcCall> test_guild_pc_calls(const PartyWorld& w, size_t who, size_t from) {
    std::vector<repl::RpcCall> out;
    if (!w.p[who].s) return out;
    const uint32_t pc_channel = w.p[who].s->channel_of(bs::Role::PlayerController);
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& d = w.p[who].got[i];
        if (d.channel != pc_channel || d.open || d.close) continue;
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t p = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), b.payload_pos + b.payload_bits, p);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

static size_t test_guild_calls_named(const std::vector<repl::RpcCall>& calls, const char* name) {
    return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const repl::RpcCall& c) { return c.rpc && std::string(c.rpc->name) == name; }));
}

/// The storage events the client was sent (the item bag is the only container the scroll tests move).
static std::vector<repl::ItemStorageEvent> test_guild_storage_events(const std::vector<repl::RpcCall>& calls) {
    std::vector<repl::ItemStorageEvent> out;
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveStorageEvent") continue;
        const auto event = repl::parse_storage_event(c);
        if (event) out.push_back(*event);
    }
    return out;
}

static bool test_guild_slot_is_empty(const PartyWorld& w, size_t who, int slot) {
    return w.p[who].s && w.p[who].s->inventory().at(svc::ItemContainer::Backpack, slot) == nullptr;
}

}  // namespace

static void test_the_scroll_opens_the_create_panel_and_is_taken_when_the_guild_is_made() {
    std::printf("the guild creation scroll's use opens the create panel and leaves the scroll; the create takes it and makes the guild\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string guid = test_guild_scroll_guid(1);
    if (!w.enter(0, test_guild_scroll_record("cyb", {0x4a01, 0, 0, 1}, 1, 1), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    CHECK(test_guild_has_scroll(w, 0, 4, guid), "the scroll is in item bag slot 4");
    size_t from = w[0].got.size();
    CHECK(test_guild_use_scroll(w, 0, 4, guid), "ServerItemUsed and ServerActivateItem are sent for the scroll");
    w.settle();
    const std::vector<repl::RpcCall> use_calls = test_guild_pc_calls(w, 0, from);
    CHECK(test_guild_calls_named(use_calls, "ClientOpenCreateGuildPanel") == 1, "the use answers ClientOpenCreateGuildPanel");
    CHECK(use_calls.size() == 1, "and nothing else: no item cooldown, no storage event (the panel's answer only)");
    CHECK(test_guild_has_scroll(w, 0, 4, guid), "the scroll stays in the bag while the panel is open");
    from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "Sandal Makers"), "ServerCreateGuild names the guild");
    w.settle();
    const std::optional<TestGuildReply> made = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded");
    CHECK(made && test_guild_reply_whole(*made, "RequestType") == 1, "the create is answered with ClientGuildRequestSucceeded(CreateGuild)");
    CHECK(made && test_guild_reply_text(*made, "Message") == "Guild Sandal Makers was successfully created!", "...in the game's own words");
    CHECK(w[0].s->guild_name() == "Sandal Makers" && test_guild_text(*w[0].s, "GuildName") == "Sandal Makers", "the player is the leader of the new guild");
    CHECK(test_guild_slot_is_empty(w, 0, 4) && !test_guild_has_scroll(w, 0, 4, guid), "the scroll left the bag when the guild was made");
    const std::vector<repl::ItemStorageEvent> events = test_guild_storage_events(test_guild_pc_calls(w, 0, from));
    CHECK(events.size() == 1, "one storage event takes the scroll");
    if (events.size() == 1) {
        const auto scroll = items::guid_of(guid);
        const repl::ItemStorageEvent& remove = events.front();
        const int32_t slot = remove.targets.size() == 1 && remove.targets[0].slots.size() == 1 ? remove.targets[0].slots[0].slot : -1;
        const int32_t taken = remove.targets.size() == 1 && remove.targets[0].slots.size() == 1 ? remove.targets[0].slots[0].count : -1;
        CHECK(remove.type == static_cast<uint8_t>(repl::StorageEventType::Remove) && remove.item.record_id == kTestGuildScroll && scroll && remove.item.guid == *scroll &&
                  slot == 4 && taken == 1,
              ("the removal names the scroll's record and instance, item bag slot 4 and the one scroll taken (slot " + std::to_string(slot) + ", count " +
               std::to_string(taken) + ")")
                  .c_str());
    }
}

static void test_a_create_without_a_scroll_is_refused() {
    std::printf("a guild create with no scroll is refused, makes no guild and saves nothing\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    if (!w.enter(0, party_record("cyb", {0x4a11, 0, 0, 1}), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    const std::string free_name = test_guild_name(15);   // (22 is test_guild_name(48)'s letter too: 48 % 26)
    const size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, free_name), "ServerCreateGuild is sent with no scroll in the bag");
    w.settle();
    const std::optional<TestGuildReply> refused = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed");
    CHECK(refused && test_guild_reply_whole(*refused, "RequestType") == 1, "the create is answered with ClientGuildRequestFailed(CreateGuild)");
    const std::string got = refused ? test_guild_reply_text(*refused, "Message") : std::string("(no failure)");
    CHECK(refused && got == "You need a guild creation scroll to create a guild.", ("...and says the scroll is needed (got: " + got + ")").c_str());
    CHECK(w[0].s->guild_name().empty() && test_guild_text(*w[0].s, "GuildName").empty(), "the player is in no guild");
    CHECK(sink.saves.empty(), "nothing was saved: no guild was made");
}

static void test_a_refused_create_keeps_the_scroll_and_a_guilded_use_opens_the_panel() {
    std::printf("a taken name, the cooldown and a character already in a guild keep the scroll; a guilded character's use still opens the panel\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string first = test_guild_scroll_guid(11), second = test_guild_scroll_guid(12);
    if (!w.enter(0, test_guild_scroll_record("cyb", {0x4a21, 0, 0, 1}, 2, 11), identity())) {
        CHECK(false, "the first session joins");
        return;
    }
    const std::string cyb_guild = test_guild_name(23), dee_guild = test_guild_name(24), ayla_guild = test_guild_name(25);
    std::string taken_name = cyb_guild;
    for (char& c : taken_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, cyb_guild), "cyb creates its guild with two scrolls");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the create is made");
    CHECK(test_guild_slot_is_empty(w, 0, 4) && !test_guild_has_scroll(w, 0, 4, first), "the first scroll was taken");
    CHECK(test_guild_has_scroll(w, 0, 5, second), "the second scroll stays");
    from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "Second Makers"), "cyb creates again while in the guild");
    w.settle();
    const std::optional<TestGuildReply> again = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed");
    CHECK(again && test_guild_reply_text(*again, "Message") == "You are already in a guild.", "the create is refused: the player is in a guild");
    CHECK(test_guild_has_scroll(w, 0, 5, second), "...and the second scroll stays");
    from = w[0].got.size();
    CHECK(test_guild_use_scroll(w, 0, 5, second), "the guilded character uses its scroll");
    w.settle();
    CHECK(test_guild_calls_named(test_guild_pc_calls(w, 0, from), "ClientOpenCreateGuildPanel") == 1, "the use still answers ClientOpenCreateGuildPanel");
    CHECK(test_guild_has_scroll(w, 0, 5, second), "...and takes nothing: the create is what pays");
    from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "Third Makers"), "cyb creates after the use");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed").has_value(), "the create is still refused");
    CHECK(test_guild_has_scroll(w, 0, 5, second), "...and the scroll stays");
    // dee: a taken name keeps the scroll; a free name then takes it
    if (!w.enter(1, test_guild_scroll_record("dee", {0x4a22, 0, 0, 2}, 1, 21), identity())) {
        CHECK(false, "the second session joins");
        return;
    }
    from = w[1].got.size();
    CHECK(test_guild_create(w, 1, taken_name), "dee creates the taken name in another case");
    w.settle();
    const std::optional<TestGuildReply> taken = test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed");
    CHECK(taken && test_guild_reply_text(*taken, "Message") == "That guild name is taken.", "the taken name is refused");
    CHECK(test_guild_has_scroll(w, 1, 4, test_guild_scroll_guid(21)), "...and dee keeps its scroll");
    from = w[1].got.size();
    CHECK(test_guild_create(w, 1, dee_guild), "dee creates a free name");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestSucceeded").has_value(), "the free name is made");
    CHECK(test_guild_slot_is_empty(w, 1, 4) && !test_guild_has_scroll(w, 1, 4, test_guild_scroll_guid(21)), "...and takes dee's scroll");
    // ayla: the join cooldown keeps the scroll
    svc::Character ayla = test_guild_scroll_record("ayla", {0x4a23, 0, 0, 3}, 1, 31);
    svc::Guild cooling;
    cooling.self_rank = -1;
    cooling.cooldown_until_ms = test_wall_ms() + 60'000;
    ayla.guild = cooling;
    if (!w.enter(2, ayla, identity())) {
        CHECK(false, "the third session joins");
        return;
    }
    from = w[2].got.size();
    CHECK(test_guild_create(w, 2, ayla_guild), "ayla creates while on the join cooldown");
    w.settle();
    const std::optional<TestGuildReply> cooling_off = test_guild_reply(test_guild_replies(w, 2, from), "ClientGuildRequestFailed");
    CHECK(cooling_off && test_guild_reply_text(*cooling_off, "Message") == "You may not join a guild for a while.", "the cooldown is refused with the game's text");
    CHECK(test_guild_has_scroll(w, 2, 4, test_guild_scroll_guid(31)), "...and ayla keeps its scroll");
}

AOC_REGISTER_TEST(test_the_scroll_opens_the_create_panel_and_is_taken_when_the_guild_is_made);
AOC_REGISTER_TEST(test_a_create_without_a_scroll_is_refused);
AOC_REGISTER_TEST(test_a_refused_create_keeps_the_scroll_and_a_guilded_use_opens_the_panel);
