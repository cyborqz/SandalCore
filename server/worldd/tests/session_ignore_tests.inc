// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_ignore_tests.inc
//
//  Registered tests of test_worldd_session: social step 6, the ignore list.  The client calls ServerIgnoreCharacter(InCharacterName) and ServerRemoveIgnoreCharacter(InCharacterName) and
//  waits for no answer call: the pawn's OwnerInfo array IgnoredCharacterNames changes, and the game client shows it and hides the chat lines of those names by itself.  The session keeps
//  the list (at most 50 names, the letters A to Z in either case the same name, a name that is not UTF-8 text refused, the character's own name refused, a name that is offline or unknown
//  accepted), the changes go to the saver with the progress, and the saved lists are on the pawn from the join.  A second login or a reconnect starts from the old session's lists, and so does
//  a login that lands after the old session was removed (its last hand-over waits in the saver); a saved name that is not UTF-8 text is left out at the join.  The guild half (ServerIgnoreGuild)
//  is not served in this step.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "protocol/repl/rpc_fields.h"

#include <condition_variable>
#include <mutex>

namespace {

using IgnoreNames = std::vector<std::string>;

/// Where the session hands what it saves: the progress hand-overs, with the character they were for.
struct IgnoreSink : PositionSink {
    std::vector<std::pair<std::string, svc::Progress>> progress;
    void save(const std::string&, const svc::Position&) override {}
    void save_progress(const std::string& id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }
};

/// cyb's record with the lists the character already ignores.
static svc::Character ignoring_record(IgnoreNames players = {}, IgnoreNames guilds = {}) {
    svc::Character c = cyb_record();
    c.ignored_character_names = std::move(players);
    c.ignored_guild_names = std::move(guilds);
    return c;
}

/// A client ServerIgnoreCharacter or ServerRemoveIgnoreCharacter with its one string; `with_name` false leaves the string out (an empty string is not on the wire).
static BunchWriter ignore_call(const char* rpc_name, const std::string& name, bool with_name = true) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc(rpc_name);
    repl::RpcParams params;
    if (!rpc || (with_name && !repl::set_rpc_param(*rpc, params, "InCharacterName", repl::Value::str(name)))) return BunchWriter{};
    return client_rpc(pc_cls, rpc_name, params);
}

/// Joins `rec` with `sink` as the saver and enters the world (the phase-in sent and everything settled).
static bool ignore_enter_world(Harness& h, IgnoreSink& sink, const svc::Character& rec = cyb_record()) {
    h.fx.world->set_position_sink(&sink);
    if (!h.join(rec) || !h.s || h.s->failed()) return false;
    h.settle();
    h.client_sends({h.client.actor(h.s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    return true;
}

/// The client sends `call` on the PlayerController's channel and the server answers.
static void ignore_send_call(Harness& h, const BunchWriter& call) {
    h.client_sends({h.client.actor(h.s->channel_of(bs::Role::PlayerController), call, true)});
    h.settle();
}
static void ignore_send(Harness& h, const char* rpc_name, const std::string& name) { ignore_send_call(h, ignore_call(rpc_name, name)); }

/// The OwnerInfo array `property` in one delivered chain of the pawn, or nullopt when the chain does not carry it.
static std::optional<repl::ArrayValue> ignore_array_in(const fake::Delivered& d, const schema::ClassSchema& owner_cls, const FIntrepidNetworkGUID& owner, const char* property) {
    const size_t start = d.open ? read_new_actor(d.content, false).end : 0;
    const auto state = block_of(d, start, false, owner, owner_cls);
    const repl::Value* v = state ? state->get(owner_cls, property) : nullptr;
    const auto* array = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!array) return std::nullopt;
    return *array;
}

/// How many of the chains from `from` on carry the OwnerInfo array `property` (the join's first state, or an update that changed it).
static size_t ignore_carried(const Harness& h, size_t from, const char* property) {
    const schema::ClassSchema* owner_cls = gen::find_component_schema("OwnerInfo");
    if (!owner_cls || !h.s) return 0;
    const FIntrepidNetworkGUID owner = h.s->bootstrap().setup().guids.owner_info;
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    size_t n = 0;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == pawn_ch && ignore_array_in(h.got[i], *owner_cls, owner, property)) ++n;
    }
    return n;
}

/// The OwnerInfo array `property` as the client holds it: the join's list with every later update applied.  An update of an array carries its new count and the elements that changed, so an
/// element an update leaves out keeps what the client had.
static IgnoreNames ignore_client_list(const Harness& h, const char* property) {
    IgnoreNames held;
    const schema::ClassSchema* owner_cls = gen::find_component_schema("OwnerInfo");
    if (!owner_cls || !h.s) return held;
    const FIntrepidNetworkGUID owner = h.s->bootstrap().setup().guids.owner_info;
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    for (const fake::Delivered& d : h.got) {
        if (d.channel != pawn_ch) continue;
        const auto array = ignore_array_in(d, *owner_cls, owner, property);
        if (!array) continue;
        held.resize(array->elements.size());
        for (size_t k = 0; k < array->elements.size(); ++k) {
            const repl::Value* n = repl::rpcf::element_value(array->elements[k], 1);
            const auto* text = n ? n->get_if<std::string>() : nullptr;
            if (text) held[k] = *text;
        }
    }
    return held;
}

static std::string ignore_names_text(const IgnoreNames& names) {
    std::string out = "[";
    for (size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + names[i];
    return out + "]";
}

/// True when the client holds exactly `want` in the OwnerInfo array `property`; says what it holds when not.
static bool client_has(const Harness& h, const char* property, const IgnoreNames& want) {
    const IgnoreNames held = ignore_client_list(h, property);
    if (held == want) return true;
    std::printf("    the client holds %s in %s, wanted %s\n", ignore_names_text(held).c_str(), property, ignore_names_text(want).c_str());
    return false;
}

static bool list_is(const IgnoreNames& got, const IgnoreNames& want) { return got == want; }

/// What the log said since `from`.
static std::string ignore_log_since(size_t from) {
    spdlog::default_logger()->flush();
    const std::string all = g_log.str();
    return from < all.size() ? all.substr(from) : std::string();
}

/// The newest hand-over of the progress, or nullptr.
static const svc::Progress* last_progress(const IgnoreSink& sink) { return sink.progress.empty() ? nullptr : &sink.progress.back().second; }

/// The client sends a ServerIgnoreCharacter or ServerRemoveIgnoreCharacter through the endpoint at `now` and takes what came.
static void ignore_ep_send(Rig& rig, Joined& j, const Address& addr, const char* rpc_name, const std::string& name, Clock::time_point now) {
    const auto pkt = j.client->packet({j.client->actor(3, ignore_call(rpc_name, name), true)});
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

}  // namespace

static void test_ignore_adds_a_name_and_the_client_is_given_the_array() {
    std::printf("ServerIgnoreCharacter puts the name on the list, the client is given the array, the list goes to the saver\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    CHECK(h.s->ignored_character_names().empty() && h.s->ignored_guild_names().empty(), "a character that ignores nobody starts with two empty lists");
    CHECK(ignore_carried(h, 0, "IgnoredCharacterNames") == 0 && ignore_carried(h, 0, "IgnoredGuildNames") == 0, "and the join sent no list");
    CHECK(sink.progress.empty(), "and saved nothing");
    const size_t before = h.got.size(), log_from = g_log.str().size();
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc"}), "the session's list holds the name");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc"}), "the client is given IgnoredCharacterNames with the one name");
    CHECK(ignore_carried(h, before, "IgnoredGuildNames") == 0, "the guild list is not sent: it did not change");
    const svc::Progress* saved = last_progress(sink);
    CHECK(sink.progress.size() == 1 && sink.progress[0].first == kCyb && saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames{"Brannoc"} &&
              saved->ignored_guild_names.empty(),
          "the progress goes to the saver with the list: replace_ignored_names, the one name, no guild");
    CHECK(ignore_log_since(log_from).find("ignores Brannoc") != std::string::npos, "the change is logged");
    CHECK(h.s->ignore_changes() == 1 && h.s->ignore_refused() == 0 && h.s->ignore_unchanged() == 0, "it is counted as one change");
}

AOC_REGISTER_TEST(test_ignore_adds_a_name_and_the_client_is_given_the_array);

static void test_ignore_and_unignore_keep_the_order_without_regard_to_case() {
    std::printf("names are added at the end and removed in either case; the last removal empties the array\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    const size_t before = h.got.size();
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
    ignore_send(h, "ServerIgnoreCharacter", "Mirelle");
    ignore_send(h, "ServerIgnoreCharacter", "Tavish");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc", "Mirelle", "Tavish"}), "three names: the order they were added in");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc", "Mirelle", "Tavish"}), "the client's array has the three, in that order");
    CHECK(sink.progress.size() == 3 && last_progress(sink) && last_progress(sink)->ignored_character_names == IgnoreNames({"Brannoc", "Mirelle", "Tavish"}), "three changes, three hand-overs, the last with all three");

    const size_t second = h.got.size();
    ignore_send(h, "ServerRemoveIgnoreCharacter", "mirelle");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc", "Tavish"}), "ServerRemoveIgnoreCharacter takes a name off whatever the case of its letters");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc", "Tavish"}), "the client's array shrinks to the two");
    CHECK(sink.progress.size() == 4 && last_progress(sink) && last_progress(sink)->ignored_character_names == IgnoreNames({"Brannoc", "Tavish"}), "and the removal is saved");

    const size_t third = h.got.size();
    ignore_send(h, "ServerRemoveIgnoreCharacter", "BRANNOC");
    ignore_send(h, "ServerRemoveIgnoreCharacter", "Tavish");
    CHECK(h.s->ignored_character_names().empty(), "the other two names go as well");
    CHECK(client_has(h, "IgnoredCharacterNames", {}), "the client's array is empty again: the last update has a count of 0");
    const svc::Progress* saved = last_progress(sink);
    CHECK(sink.progress.size() == 6 && saved && saved->replace_ignored_names && saved->ignored_character_names.empty(), "and an empty list is saved as a replacement, not skipped");
    CHECK(h.s->ignore_changes() == 6, "six changes in all");
}

AOC_REGISTER_TEST(test_ignore_and_unignore_keep_the_order_without_regard_to_case);

static void test_ignoring_oneself_is_refused() {
    std::printf("the character's own name is refused, in any case and with spaces around it, and logged\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
    const size_t chains = h.got.size(), saves = sink.progress.size(), log_from = g_log.str().size();
    ignore_send(h, "ServerIgnoreCharacter", "cyb");
    ignore_send(h, "ServerIgnoreCharacter", "CYB");
    ignore_send(h, "ServerIgnoreCharacter", "  Cyb ");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc"}), "the list is as it was");
    CHECK(ignore_carried(h, chains, "IgnoredCharacterNames") == 0, "the client is sent no array");
    CHECK(sink.progress.size() == saves, "nothing is saved");
    CHECK(h.s->ignore_refused() == 3 && h.s->ignore_changes() == 1, "three refusals are counted");
    const std::string log = ignore_log_since(log_from);
    CHECK(log.find("refused") != std::string::npos && log.find("own name") != std::string::npos, "the refusal is logged and says why");
    CHECK(!h.s->failed(), "the session goes on");
}

AOC_REGISTER_TEST(test_ignoring_oneself_is_refused);

static void test_ignoring_a_name_twice_or_removing_one_that_is_not_there_changes_nothing() {
    std::printf("a name that is on the list already, or a removal of one that is not, is accepted silently and logged\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
    const size_t chains = h.got.size(), saves = sink.progress.size(), log_from = g_log.str().size();
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
    ignore_send(h, "ServerIgnoreCharacter", "BRANNOC");
    ignore_send(h, "ServerIgnoreCharacter", " brannoc ");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc"}), "the name stays once, as it was first typed");
    ignore_send(h, "ServerRemoveIgnoreCharacter", "Nobody");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc"}), "removing a name that is not there leaves the list as it was");
    CHECK(ignore_carried(h, chains, "IgnoredCharacterNames") == 0, "no new array goes to the client (it already holds this one)");
    CHECK(sink.progress.size() == saves, "nothing is saved again");
    CHECK(h.s->ignore_unchanged() == 4 && h.s->ignore_refused() == 0 && h.s->ignore_changes() == 1, "the four calls are counted as unchanged, none as refused");
    const std::string log = ignore_log_since(log_from);
    CHECK(log.find("on the list already") != std::string::npos, "the repeat is logged");
    CHECK(log.find("Nobody is not on the list") != std::string::npos, "the removal of a name that is not there is logged");
    CHECK(!h.s->failed(), "the session goes on");
}

AOC_REGISTER_TEST(test_ignoring_a_name_twice_or_removing_one_that_is_not_there_changes_nothing);

static void test_a_name_nobody_has_is_accepted() {
    std::printf("a name that is offline or that no character has goes on the list\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    ignore_send(h, "ServerIgnoreCharacter", "Nobody_xyz");
    ignore_send(h, "ServerIgnoreCharacter", "Two Words");
    CHECK(list_is(h.s->ignored_character_names(), {"Nobody_xyz", "Two Words"}), "both names are on the list as typed");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Nobody_xyz", "Two Words"}), "and in the client's array");
    CHECK(h.s->ignore_refused() == 0, "nothing is refused");
}

AOC_REGISTER_TEST(test_a_name_nobody_has_is_accepted);

static void test_the_list_holds_at_most_50_names() {
    std::printf("50 names fit, the 51st is refused and logged, a place freed is taken again\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");   // (the cap is 50 names, whatever their length)
    for (int i = 1; i < 50; ++i) ignore_send(h, "ServerIgnoreCharacter", "Name" + std::to_string(100 + i));
    CHECK(h.s->ignored_character_names().size() == 50 && h.s->ignore_changes() == 50 && h.s->ignore_refused() == 0, "50 names are on the list");
    const IgnoreNames wire = ignore_client_list(h, "IgnoredCharacterNames");
    CHECK(wire.size() == 50 && wire.front() == "Brannoc" && wire.back() == "Name149", "and in the client's array, the first and the last in their places");
    const size_t chains = h.got.size(), saves = sink.progress.size(), log_from = g_log.str().size();
    ignore_send(h, "ServerIgnoreCharacter", "OneTooMany");
    CHECK(h.s->ignored_character_names().size() == 50 && !h.s->ignored_character_names().empty() && h.s->ignored_character_names().back() == "Name149", "the 51st name is not on the list");
    CHECK(ignore_carried(h, chains, "IgnoredCharacterNames") == 0 && sink.progress.size() == saves, "the client is sent nothing and nothing is saved");
    CHECK(h.s->ignore_refused() == 1, "the refusal is counted");
    const std::string log = ignore_log_since(log_from);
    CHECK(log.find("OneTooMany") != std::string::npos && log.find("refused") != std::string::npos && log.find("50") != std::string::npos, "and logged with the name and the limit");
    ignore_send(h, "ServerRemoveIgnoreCharacter", "Name120");
    ignore_send(h, "ServerIgnoreCharacter", "OneTooMany");
    CHECK(h.s->ignored_character_names().size() == 50 && h.s->ignored_character_names().back() == "OneTooMany", "once a name is removed the next one fits");
    ignore_send(h, "ServerIgnoreCharacter", "Name120");
    CHECK(h.s->ignored_character_names().size() == 50 && h.s->ignored_character_names().back() == "OneTooMany", "and the list is full again");
}

AOC_REGISTER_TEST(test_the_list_holds_at_most_50_names);

static void test_names_that_cannot_be_ignored_are_refused() {
    std::printf("a blank name, a name over 64 bytes and a name with a control character are refused; spaces at the ends are cut off\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    const size_t chains = h.got.size(), log_from = g_log.str().size();
    ignore_send_call(h, ignore_call("ServerIgnoreCharacter", "", /*with_name=*/false));
    ignore_send(h, "ServerIgnoreCharacter", "");
    ignore_send(h, "ServerIgnoreCharacter", "     ");
    ignore_send(h, "ServerIgnoreCharacter", std::string(65, 'n'));
    ignore_send(h, "ServerIgnoreCharacter", std::string("bad\nname"));
    ignore_send(h, "ServerIgnoreCharacter", std::string("bad\tname"));
    CHECK(h.s->ignored_character_names().empty() && h.s->ignore_refused() == 6 && h.s->ignore_changes() == 0, "none of the six is on the list; all six are counted as refused");
    CHECK(ignore_carried(h, chains, "IgnoredCharacterNames") == 0 && sink.progress.empty(), "nothing is sent and nothing is saved");
    const std::string log = ignore_log_since(log_from);
    CHECK(log.find("refused") != std::string::npos && log.find("bad\nname") == std::string::npos, "the refusals are logged and the new line a name carried is not written into the log");
    ignore_send(h, "ServerIgnoreCharacter", std::string(64, 'n'));
    CHECK(h.s->ignored_character_names().size() == 1 && h.s->ignored_character_names()[0].size() == 64, "a name of 64 bytes is accepted");
    ignore_send(h, "ServerIgnoreCharacter", "  Dorrin  ");
    CHECK(h.s->ignored_character_names().size() == 2 && h.s->ignored_character_names()[1] == "Dorrin", "the spaces at the ends of a name are cut off");
    ignore_send(h, "ServerRemoveIgnoreCharacter", " DORRIN");
    CHECK(h.s->ignored_character_names().size() == 1, "and a removal cuts them off as well");
    CHECK(!h.s->failed(), "the session goes on");
}

AOC_REGISTER_TEST(test_names_that_cannot_be_ignored_are_refused);

static void test_the_saved_lists_are_on_the_pawn_from_the_join() {
    std::printf("the lists a character saved are on the pawn's OwnerInfo with the join, and a later change keeps both\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink, ignoring_record({"Brannoc", "Mirelle"}, {"Sandal Makers"}))) { CHECK(false, "the session joins and enters the world"); return; }
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc", "Mirelle"}) && list_is(h.s->ignored_guild_names(), {"Sandal Makers"}), "the session holds the saved players and guild");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc", "Mirelle"}), "the join gave the client IgnoredCharacterNames in the saved order");
    CHECK(client_has(h, "IgnoredGuildNames", {"Sandal Makers"}), "and IgnoredGuildNames");
    CHECK(sink.progress.empty(), "loading the lists saves nothing");
    const size_t before = h.got.size();
    ignore_send(h, "ServerIgnoreCharacter", "Tavish");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc", "Mirelle", "Tavish"}), "a new name is added after the saved ones");
    CHECK(ignore_carried(h, before, "IgnoredGuildNames") == 0, "the guild list is not sent again");
    const svc::Progress* saved = last_progress(sink);
    CHECK(sink.progress.size() == 1 && saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames({"Brannoc", "Mirelle", "Tavish"}) &&
              saved->ignored_guild_names == IgnoreNames{"Sandal Makers"},
          "the save carries both lists: the guild list is saved back as it was loaded");
}

AOC_REGISTER_TEST(test_the_saved_lists_are_on_the_pawn_from_the_join);

static void test_a_saved_list_that_breaks_the_rules_is_cleaned_at_the_join() {
    std::printf("a saved list with a blank name, a repeat or more than 50 names is cut to what the rules allow\n");
    IgnoreNames players = {"Brannoc", "BRANNOC", "", "   ", "Mirelle", std::string(70, 'x'), "Tavish"};
    for (int i = 0; i < 60; ++i) players.push_back("Extra" + std::to_string(100 + i));
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink, ignoring_record(players, {"Sandal Makers", "SANDAL MAKERS"}))) { CHECK(false, "the session joins and enters the world"); return; }
    const IgnoreNames& kept = h.s->ignored_character_names();
    CHECK(kept.size() == 50 && kept[0] == "Brannoc" && kept[1] == "Mirelle" && kept[2] == "Tavish" && kept[3] == "Extra100" && kept.back() == "Extra146",
          "the players: the repeat, the blank names and the name over 64 bytes are left out, the list stops at 50");
    CHECK(list_is(h.s->ignored_guild_names(), {"Sandal Makers"}), "the guilds: the repeat is left out");
    CHECK(client_has(h, "IgnoredCharacterNames", kept), "the client is given the cleaned list");
}

AOC_REGISTER_TEST(test_a_saved_list_that_breaks_the_rules_is_cleaned_at_the_join);

static void test_a_saved_name_that_is_not_utf8_is_left_out() {
    std::printf("a saved name that is not valid UTF-8 is left out at the join; the next save carries the player's progress and only names the service takes\n");
    svc::Character rec = ignoring_record({"El\xffta", "Brannoc"}, {"El\xffta", "Sandal Makers"});
    rec.level = 10;
    rec.experience.push_back({content::kGuidAdventuring, 500000, 7});
    const size_t log_from = g_log.str().size();
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink, rec)) { CHECK(false, "the session joins and enters the world"); return; }
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc"}), "the saved name that is not UTF-8 text is left out, the valid one stays");
    CHECK(list_is(h.s->ignored_guild_names(), {"Sandal Makers"}), "the guild list is cleaned the same way");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc"}), "the pawn is given the list without it");
    CHECK(sink.progress.empty(), "the join saves nothing");
    const std::string log = ignore_log_since(log_from);
    CHECK(log.find("left out") != std::string::npos && log.find("UTF-8") != std::string::npos, "the join says that a saved name was left out and why");
    ignore_send(h, "ServerIgnoreCharacter", "Zo\xc3\xab");
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc", "Zo\xc3\xab"}), "a valid name with a two-byte letter goes on");
    const svc::Progress* saved = last_progress(sink);
    CHECK(saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames({"Brannoc", "Zo\xc3\xab"}) && saved->ignored_guild_names == IgnoreNames{"Sandal Makers"},
          "the next save carries the valid names only");
    CHECK(saved && saved->level == 10 && saved->experience.size() == 1 && saved->experience[0].leveling_type_guid == content::kGuidAdventuring &&
              saved->experience[0].xp == 500000 && saved->experience[0].debt == 7,
          "and the player's level and experience go with it, so the character service takes the whole save");
}

AOC_REGISTER_TEST(test_a_saved_name_that_is_not_utf8_is_left_out);

static void test_the_end_of_the_session_hands_the_lists_over_again() {
    std::printf("the final save of a session that changed its list carries the lists; a session that only loaded them saves nothing\n");
    {
        Harness h;
        IgnoreSink sink;
        if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
        ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
        ignore_send(h, "ServerIgnoreCharacter", "Mirelle");
        CHECK(sink.progress.size() == 2, "two changes, two hand-overs");
        h.s->finish("test");
        const svc::Progress* saved = last_progress(sink);
        CHECK(sink.progress.size() == 3 && saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames({"Brannoc", "Mirelle"}), "the end of the session hands the progress over once more, with both names");
    }
    {
        Harness h;
        IgnoreSink sink;
        if (!ignore_enter_world(h, sink, ignoring_record({"Brannoc"}, {"Sandal Makers"}))) { CHECK(false, "the session joins and enters the world"); return; }
        h.s->finish("test");
        CHECK(sink.progress.empty(), "a session that changed nothing hands no progress over: the lists in the character service stay as they are");
    }
}

AOC_REGISTER_TEST(test_the_end_of_the_session_hands_the_lists_over_again);

static void test_a_second_login_takes_over_the_ignored_names() {
    std::printf("a second login of the character starts from the first session's ignore lists, and its next save writes them\n");
    Fixture fx;
    IgnoreSink sink;
    fx.world->set_position_sink(&sink);
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, ignoring_record({"Brannoc"}, {"Sandal Makers"}), t0);
    join_now(rig, a, kA, t0 + milliseconds(40));
    const PlayerSession* pa = rig.ep.player_at(kA);
    CHECK(pa && list_is(pa->ignored_character_names(), {"Brannoc"}) && list_is(pa->ignored_guild_names(), {"Sandal Makers"}), "the first session holds the saved lists");
    ignore_ep_send(rig, a, kA, "ServerIgnoreCharacter", "Mirelle", t0 + milliseconds(100));
    ignore_ep_send(rig, a, kA, "ServerRemoveIgnoreCharacter", "Brannoc", t0 + milliseconds(120));
    CHECK(pa && list_is(pa->ignored_character_names(), {"Mirelle"}), "the first session's list is the character's: Mirelle added, Brannoc removed");
    // the second login reads the record as it was before those changes
    Joined b = handshake(rig, kB, t0 + milliseconds(200));
    log_in(rig, b, kB, ignoring_record({"Brannoc"}, {}), t0 + milliseconds(210));
    join_now(rig, b, kB, t0 + milliseconds(250));
    const PlayerSession* pb = rig.ep.player_at(kB);
    CHECK(pb && list_is(pb->ignored_character_names(), {"Mirelle"}), "the second session holds the first one's list: the addition is kept and the removal is not undone");
    CHECK(pb && list_is(pb->ignored_guild_names(), {"Sandal Makers"}), "and the guild list the record no longer has");
    ignore_ep_send(rig, b, kB, "ServerIgnoreCharacter", "Tavish", t0 + milliseconds(300));
    const svc::Progress* saved = last_progress(sink);
    CHECK(saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames({"Mirelle", "Tavish"}) &&
              saved->ignored_guild_names == IgnoreNames{"Sandal Makers"},
          "the next save writes the carried lists with the new name");
}

AOC_REGISTER_TEST(test_a_second_login_takes_over_the_ignored_names);

static void test_a_reconnect_takes_over_the_ignored_names() {
    std::printf("a reconnect that keeps its login starts from the first session's ignore lists\n");
    Fixture fx;
    IgnoreSink sink;
    fx.world->set_position_sink(&sink);
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, cyb_record(), t0);
    join_now(rig, a, kA, t0 + milliseconds(40));
    ignore_ep_send(rig, a, kA, "ServerIgnoreCharacter", "Brannoc", t0 + milliseconds(100));
    const PlayerSession* pa = rig.ep.player_at(kA);
    CHECK(pa && list_is(pa->ignored_character_names(), {"Brannoc"}), "the first session holds the name");
    // the same address sends a new Initial and Response with the same character and ticket: the login is carried
    Joined a2 = handshake(rig, kA, t0 + milliseconds(200));
    CHECK(a2.session == a.session, "the new session keeps the login's id");
    log_in(rig, a2, kA, cyb_record(), t0 + milliseconds(210));
    join_now(rig, a2, kA, t0 + milliseconds(250));
    const PlayerSession* p2 = rig.ep.player_at(kA);
    CHECK(p2 && list_is(p2->ignored_character_names(), {"Brannoc"}), "the new session holds the old session's list");
    ignore_ep_send(rig, a2, kA, "ServerIgnoreCharacter", "Mirelle", t0 + milliseconds(300));
    const svc::Progress* saved = last_progress(sink);
    CHECK(saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames({"Brannoc", "Mirelle"}), "and its next save writes the carried name with the new one");
}

AOC_REGISTER_TEST(test_a_reconnect_takes_over_the_ignored_names);

/// Holds the saver's worker in one call, so that a later hand-over stays queued in the saver.
struct SaverGate {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    entered = false, open = false;

    bool wait_until_entered() {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(3), [this] { return entered; });
    }
    void block() {
        std::unique_lock<std::mutex> lock(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return open; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mu);
        open = true;
        cv.notify_all();
    }
};

/// The character service the saver calls: a position of `block_for` waits at the gate; the progress calls are kept, in order.
struct SaverApi : svc::CharacterApi {
    SaverGate*                                         gate = nullptr;
    std::string                                        block_for;
    std::vector<std::pair<std::string, svc::Progress>> progress;

    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus save_position(const std::string& id, const svc::Position&) override {
        if (gate && id == block_for) gate->block();
        return svc::CallStatus::Ok;
    }
    svc::CallStatus save_progress(const std::string& id, const svc::Progress& p) override {
        progress.emplace_back(id, p);
        return svc::CallStatus::Ok;
    }
    /// The newest progress the saver wrote for `id`, or nullptr.
    const svc::Progress* last_progress_of(const std::string& id) const {
        for (auto it = progress.rbegin(); it != progress.rend(); ++it) {
            if (it->first == id) return &it->second;
        }
        return nullptr;
    }
};

static void test_pending_lists_win_when_the_old_session_is_gone() {
    std::printf("an old session removed while a new login is in flight leaves its lists in the saver: the new session takes them\n");
    SaverGate gate;
    SaverApi api;
    api.gate = &gate;
    api.block_for = "ffffffffffffffffffffffffffffffff";
    CharacterSaver saver(api);
    Fixture fx;
    fx.world->set_position_sink(&saver);
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    svc::Position park;
    park.x = 1.0;
    saver.save(api.block_for, park);   // (the worker is held there: the old session's hand-overs wait in the saver)
    const bool held = gate.wait_until_entered();
    CHECK(held, "(the saver is busy with another character, so the old session's hand-over waits)");
    if (!held) {
        gate.release();
        return;
    }
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, ignoring_record({"Brannoc"}, {}), t0);
    join_now(rig, a, kA, t0 + milliseconds(40));
    ignore_ep_send(rig, a, kA, "ServerIgnoreCharacter", "Mirelle", t0 + milliseconds(100));
    ignore_ep_send(rig, a, kA, "ServerRemoveIgnoreCharacter", "Brannoc", t0 + milliseconds(120));
    const PlayerSession* pa = rig.ep.player_at(kA);
    CHECK(pa && list_is(pa->ignored_character_names(), {"Mirelle"}), "(the old session's list is Mirelle)");
    // the new login is submitted and still waits for its result
    Joined b = handshake(rig, kB, t0 + milliseconds(180));
    // the old session ends: its last hand-over goes to the saver, which holds it (the worker is still held)
    exchange(rig, a, kA, {a.client->close_channel()}, t0 + milliseconds(200));
    CHECK(rig.ep.session_count() == 1 && !rig.ep.session_of(kCyb), "the old session is removed while the new login is in flight");
    // the login's result is the record it read, from before the change
    exchange(rig, b, kB, {b.client->hello()}, t0 + milliseconds(210));
    LoginResult r;
    r.accepted = true;
    r.character = ignoring_record({"Brannoc"}, {});
    rig.ep.post_login_result(b.session, r);
    rig.ep.tick(t0 + milliseconds(220));
    drain(rig, b, kB);
    exchange(rig, b, kB, {b.client->login(kMachine, kMachine)}, t0 + milliseconds(230));
    join_now(rig, b, kB, t0 + milliseconds(280));
    const PlayerSession* pb = rig.ep.player_at(kB);
    CHECK(pb && list_is(pb->ignored_character_names(), {"Mirelle"}), "the new session holds the lists the saver still held, not the record's older ones");
    ignore_ep_send(rig, b, kB, "ServerIgnoreCharacter", "Tavish", t0 + milliseconds(300));
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)), "(the saver writes what it held)");
    const svc::Progress* saved = api.last_progress_of(kCyb);
    CHECK(saved && saved->replace_ignored_names && saved->ignored_character_names == IgnoreNames({"Mirelle", "Tavish"}),
          "the new session's next save writes the newer list with the new name");
}

AOC_REGISTER_TEST(test_pending_lists_win_when_the_old_session_is_gone);

static void test_a_call_before_the_world_is_kept_and_sent_with_the_join() {
    std::printf("a name ignored before the phase-in is not lost: it is on the pawn when the world opens\n");
    Harness h;
    IgnoreSink sink;
    h.fx.world->set_position_sink(&sink);
    if (!h.join() || !h.s || h.s->failed()) { CHECK(false, "the session joins"); return; }
    ignore_send(h, "ServerIgnoreCharacter", "Brannoc");   // (before the client asked for anything: the join's chains are still going out)
    h.settle();
    CHECK(list_is(h.s->ignored_character_names(), {"Brannoc"}), "the session lists the name");
    CHECK(client_has(h, "IgnoredCharacterNames", {"Brannoc"}), "the client has the array once the join is through");
    CHECK(sink.progress.size() == 1, "and the list was saved");
}

AOC_REGISTER_TEST(test_a_call_before_the_world_is_kept_and_sent_with_the_join);

static void test_the_guild_calls_are_not_served_in_this_step() {
    std::printf("ServerIgnoreGuild and ServerRemoveIgnoreGuild change no list in this step\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink, ignoring_record({"Brannoc"}, {"Sandal Makers"}))) { CHECK(false, "the session joins and enters the world"); return; }
    const size_t chains = h.got.size();
    ignore_send(h, "ServerIgnoreGuild", "Moonlit Order");
    ignore_send(h, "ServerRemoveIgnoreGuild", "Sandal Makers");
    CHECK(list_is(h.s->ignored_guild_names(), {"Sandal Makers"}) && list_is(h.s->ignored_character_names(), {"Brannoc"}), "both lists are as they were");
    CHECK(ignore_carried(h, chains, "IgnoredGuildNames") == 0 && ignore_carried(h, chains, "IgnoredCharacterNames") == 0 && sink.progress.empty(), "nothing is sent and nothing is saved");
    CHECK(!h.s->failed(), "the session goes on");
}

AOC_REGISTER_TEST(test_the_guild_calls_are_not_served_in_this_step);

static void test_many_calls_in_a_row_leave_the_session_sound() {
    std::printf("120 ignore and unignore pairs in a row end with the empty list, the queue drained and the session alive\n");
    Harness h;
    IgnoreSink sink;
    if (!ignore_enter_world(h, sink)) { CHECK(false, "the session joins and enters the world"); return; }
    for (int i = 0; i < 120; ++i) {
        ignore_send(h, "ServerIgnoreCharacter", "Brannoc");
        ignore_send(h, "ServerRemoveIgnoreCharacter", "Brannoc");
    }
    CHECK(h.s->ignored_character_names().empty() && h.s->ignore_changes() == 240, "240 changes, the list is empty");
    CHECK(h.s->replies_pending() == 0 && !h.s->failed(), "nothing waits in the reply queue and the session is alive");
    CHECK(client_has(h, "IgnoredCharacterNames", {}), "the client's array ended empty");
}

AOC_REGISTER_TEST(test_many_calls_in_a_row_leave_the_session_sound);
