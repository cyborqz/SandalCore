// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_guild_tests.inc: the guilds (Social, step 9)
//
//  Players in one world (the party tests' PartyWorld, their records carrying the character service's guild), with a sink that records every guild save and cooldown
//  the sessions hand over.  What is read: the pawn's guild component (the guild's name, the roster, the member's own row and the join cooldown), the reply the client
//  gets (ClientGuildRequestSucceeded / Failed with its request type and message), and the sink's saves.
//
//  Covered: creating (the client's name form, the data's 0 cost, the three ranks and 50 places), the 60 s invitation and its expiry, accept, decline, leave with the
//  leader's leaving, kick (an officer's permissions and the rank hierarchy), disband with members online and offline, promote and demote, the roster's levels,
//  classes, rank and online flags with its refresh, guild chat, two guilds side by side, a full guild, the same request twice, a member offline at a change, a
//  reconnect in the middle of an invitation, a relog, and the guild instance surviving a death.
//
//  What another player's client is shown (the nameplate's guild name and the guildmate colour) is tests/others_guild_tests.inc in test_worldd_mounts_ride, whose
//  rig has a watcher's view.
// ============================================================================
#include "guild_scroll_rig.inc"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kTestGuildLeader = (1ull << 35) - 1;
constexpr uint64_t kTestGuildOfficer = 0x7ffcfc7f;
constexpr uint64_t kTestGuildMember = 0x65006014;
constexpr uint64_t kTestGuildAlignment = 6064630560782155776ull;

/// A character of the guild tests in `guild` at `rank`, with the whole guild the character service sends (the ranks, every member and the reading member's rank).
static svc::Character guild_record(const char* name, const std::array<uint32_t, 4>& words, const std::string& guild_id, const std::string& guild_name, int rank,
                                   const std::vector<std::pair<const svc::Character*, int>>& members, int64_t cooldown_ms = 0, int64_t version = 3) {
    svc::Character c = party_record(name, words);
    svc::Guild g;
    g.id = guild_id;
    g.name = guild_name;
    g.version = version;
    g.ranks = {{"Guild Leader", kTestGuildLeader}, {"Officer", kTestGuildOfficer}, {"Member", kTestGuildMember}};
    g.level = 1;
    g.experience = 0;
    g.capacity = 50;
    g.level_cap = 10;
    for (const auto& [who, member_rank] : members) {
        svc::GuildMember row;
        row.character_id = who->id;
        row.name = who->name;
        row.rank = member_rank;
        row.level = who->level;
        row.class_id = who->class_id;
        row.joined_ms = 1763000000000;
        g.members.push_back(std::move(row));
    }
    g.self_rank = rank;
    g.cooldown_until_ms = cooldown_ms;
    c.guild = g;
    return c;
}

/// The 32 hex digits of a guild's id for a test number.
static std::string test_guild_id(int n) {
    char text[33];
    std::snprintf(text, sizeof text, "%032llx", 0x1111000000000000ull + static_cast<unsigned long long>(n));
    return text;
}

/// A guild name no other test used: the guild book is one process's and the tests do not clear it.
static std::string test_guild_name(int n) {
    static const char* letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    return std::string("Guild ") + letters[n % 26] + " Makers";
}

/// The sink: every guild save and cooldown the sessions hand over, in order.
struct TestGuildSink : PositionSink {
    std::vector<svc::Guild>                      saves;
    std::vector<std::pair<std::string, int64_t>> cooldowns;
    void save(const std::string&, const svc::Position&) override {}
    void save_progress(const std::string&, svc::Progress) override {}
    void save_guild(const svc::Guild& guild) override { saves.push_back(guild); }
    void save_guild_cooldown(const std::string& id, int64_t until_ms) override { cooldowns.emplace_back(id, until_ms); }
    const svc::Guild* last(const std::string& id) const {
        for (auto it = saves.rbegin(); it != saves.rend(); ++it) {
            if (it->id == id) return &*it;
        }
        return nullptr;
    }
    std::optional<int64_t> cooldown_of(const std::string& id) const {
        std::optional<int64_t> out;
        for (const auto& [who, until] : cooldowns) {
            if (who == id) out = until;
        }
        return out;
    }
};

/// The pawn's guild component.
static const repl::Subobject* test_guild_component(const PlayerSession& s) {
    for (const repl::Subobject& sub : s.bootstrap().pawn().subobjects) {
        if (sub.obj.schema && std::string(sub.obj.schema->name) == "UIntrepidGuildComponent") return &sub;
    }
    return nullptr;
}

static const repl::Value* test_guild_value(const PlayerSession& s, const char* path) {
    const repl::Subobject* sub = test_guild_component(s);
    return sub && sub->obj.schema ? sub->obj.state.get(*sub->obj.schema, path) : nullptr;
}

static std::string test_guild_text(const PlayerSession& s, const char* path) {
    const repl::Value* v = test_guild_value(s, path);
    const std::string* text = v ? v->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

static const repl::GuildData* test_guild_data(const PlayerSession& s) {
    const repl::Value* v = test_guild_value(s, "GuildData");
    return v ? v->get_if<repl::GuildData>() : nullptr;
}

/// The GuildMember instance the pawn's AlignmentComponent carries, by the guild's GUID (nullptr: none).
static const repl::AlignmentContainer::Instance* test_guild_instance(const PlayerSession& s) {
    for (const repl::Subobject& sub : s.bootstrap().pawn().subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAlignmentComponent") continue;
        const repl::Value* v = sub.obj.state.get(*sub.obj.schema, "Alignments");
        const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
        if (!c) continue;
        for (const repl::AlignmentContainer::Instance& i : c->instances) {
            if (i.key == kTestGuildAlignment) return &i;
        }
    }
    return nullptr;
}

/// The lines another player's guild message, or the server's own guild text, reached this session (session/player_chat.cpp's count).
static size_t guild_chat_received(const PlayerSession& s) { return s.chat_lines_received(); }

/// A reply the client was sent on the guild component: the call's name, schema and parameters.
struct TestGuildReply {
    std::string               name;
    const schema::RpcSchema*  rpc = nullptr;
    repl::RpcParams           params;
    size_t                    param_bits = 0;
};

/// The guild component's calls in the chains a player's client was given from `from` on.
static std::vector<TestGuildReply> test_guild_replies(const PartyWorld& w, size_t who, size_t from) {
    std::vector<TestGuildReply> out;
    const schema::ClassSchema& cls = class_named("UIntrepidGuildComponent");
    const uint32_t guild_object = w.p[who].s ? w.p[who].s->bootstrap().setup().guids.guild.ObjectId : 0;
    for (size_t i = from; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& d = w.p[who].got[i];
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& v : *blocks) {
            // The guild component's own RPC block (not the component's property block, and not another subobject's call read as this class).
            if (v.header.is_actor || v.header.has_rep_layout || v.header.object.ObjectId != guild_object) continue;
            size_t p = v.payload_pos;
            const auto calls = repl::read_rpc_fields(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
            if (!calls) continue;
            for (const repl::RpcCall& c : *calls) {
                TestGuildReply r;
                r.name = c.rpc ? c.rpc->name : "";
                r.rpc = c.rpc;
                r.params = c.params;
                r.param_bits = c.param_bits;
                out.push_back(std::move(r));
            }
        }
    }
    return out;
}

/// The last reply named `name` (nullopt: none was sent).  A copy: callers pass the result of test_guild_replies, a temporary.
static std::optional<TestGuildReply> test_guild_reply(const std::vector<TestGuildReply>& replies, const char* name) {
    for (auto it = replies.rbegin(); it != replies.rend(); ++it) {
        if (it->name == name) return *it;
    }
    return std::nullopt;
}

/// A text parameter of a reply (empty when it is not there).
static std::string test_guild_reply_text(const TestGuildReply& r, const char* path) {
    if (!r.rpc) return std::string();
    const repl::Value* v = repl::rpcf::value_of(*r.rpc, r.params, path);
    const std::string* text = v ? v->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

/// A whole parameter of a reply (0 when it is not there).
static uint64_t test_guild_reply_whole(const TestGuildReply& r, const char* path) {
    if (!r.rpc) return 0;
    return repl::rpcf::uint_or(repl::rpcf::value_of(*r.rpc, r.params, path), 0).value_or(0);
}

/// Sends a guild call on the pawn's guild component.
static bool test_guild_send(PartyWorld& w, size_t who, const char* name, const repl::RpcParams& params = {}) {
    if (!w[who].s) return false;
    const schema::ClassSchema& cls = class_named("UIntrepidGuildComponent");
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return false;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) return false;
    BunchWriter out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.object = w[who].s->bootstrap().setup().guids.guild;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    // The call reads back with the component's class before it is sent (the server's own decoder): a bunch that does not read would be ignored there.
    const auto parsed = repl::read_client_bunch(cls, [&](const emit::FIntrepidNetworkGUID&) { return &cls; }, out.data(), out.bit_pos(), {});
    if (!parsed || parsed->rpcs.size() != 1 || !parsed->rpcs[0].call.decoded) {
        std::printf("  [FAIL] the client's %s does not read back (%zu bits)\n", name, out.bit_pos());
        return false;
    }
    w.client_sends(w[who], {w[who].client.actor(w[who].s->channel_of(bs::Role::Pawn), out, true)});
    return true;
}

static bool test_guild_put_words(const schema::RpcSchema& rpc, repl::RpcParams& params, const char* prefix, const std::array<uint32_t, 4>& words) {
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok = repl::set_rpc_param(rpc, params, std::string(prefix) + "." + "ABCD"[i], repl::Value::i64(static_cast<int32_t>(words[i]))) && ok;
    return ok;
}

static bool test_guild_create(PartyWorld& w, size_t who, const std::string& name) {
    const schema::RpcSchema* rpc = class_named("UIntrepidGuildComponent").rpc("ServerCreateGuild");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "InGuildName", repl::Value::str(name))) return false;
    return test_guild_send(w, who, "ServerCreateGuild", params);
}

static bool test_guild_invite(PartyWorld& w, size_t who, const std::string& name) {
    const schema::RpcSchema* rpc = class_named("UIntrepidGuildComponent").rpc("ServerInvitePlayerToGuild");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "InviteeName", repl::Value::str(name))) return false;
    return test_guild_send(w, who, "ServerInvitePlayerToGuild", params);
}

static bool test_guild_kick(PartyWorld& w, size_t who, const std::array<uint32_t, 4>& member) {
    const schema::RpcSchema* rpc = class_named("UIntrepidGuildComponent").rpc("ServerKickPlayerFromGuild");
    repl::RpcParams params;
    if (!rpc || !test_guild_put_words(*rpc, params, "MemberGuidToKick", member)) return false;
    return test_guild_send(w, who, "ServerKickPlayerFromGuild", params);
}

static bool test_guild_assign_leader(PartyWorld& w, size_t who, const std::array<uint32_t, 4>& member) {
    const schema::RpcSchema* rpc = class_named("UIntrepidGuildComponent").rpc("ServerAssignGuildLeader");
    repl::RpcParams params;
    if (!rpc || !test_guild_put_words(*rpc, params, "TargetCharacterGuid", member)) return false;
    return test_guild_send(w, who, "ServerAssignGuildLeader", params);
}

static bool test_guild_reassign(PartyWorld& w, size_t who, const std::array<uint32_t, 4>& member, int rank) {
    const schema::RpcSchema* rpc = class_named("UIntrepidGuildComponent").rpc("ServerReassignMemberRanks");
    if (!rpc) return false;
    repl::ArrayValue list;
    repl::ElementState e;
    for (int i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(i + 1)] = repl::Value::i64(static_cast<int32_t>(member[static_cast<size_t>(i)]));
    e.values[5] = repl::Value::u64(static_cast<uint64_t>(rank));
    list.elements.push_back(std::move(e));
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "MemberRanksToUpdate", repl::Value(std::move(list)))) return false;
    return test_guild_send(w, who, "ServerReassignMemberRanks", params);
}

static bool test_guild_decline(PartyWorld& w, size_t who) { return test_guild_send(w, who, "ServerDeclineInviteToGuild"); }
static bool test_guild_accept(PartyWorld& w, size_t who) { return test_guild_send(w, who, "ServerAcceptInviteToGuild"); }
static bool test_guild_leave(PartyWorld& w, size_t who) { return test_guild_send(w, who, "ServerLeaveGuild"); }
static bool test_guild_disband(PartyWorld& w, size_t who) { return test_guild_send(w, who, "ServerDisbandGuild"); }
static bool test_guild_refresh(PartyWorld& w, size_t who) { return test_guild_send(w, who, "ServerGetUpdatedGuildData"); }

static bool test_guild_chat(PartyWorld& w, size_t who, const std::string& text, uint64_t category = 11) {
    const schema::RpcSchema* rpc = class_named("UIntrepidGuildComponent").rpc("ServerSendMessageToGuild");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "Message", repl::Value::str(text)) || !repl::set_rpc_param(*rpc, params, "Category", repl::Value::u64(category)) ||
        !repl::set_rpc_param(*rpc, params, "EmblemsToSend", repl::Value::str(std::string()))) {
        return false;
    }
    return test_guild_send(w, who, "ServerSendMessageToGuild", params);
}

/// The 32 hex digits of the character GUID of player `i`.
static std::string test_guild_character(const PartyWorld& w, size_t i) { return party_hex(w.guid_of(i)); }

/// A record for the next login of a character, from the guild state the sink last saved for it (the character service's next read): the guild's rows and the
/// character's own rank, or no guild when the save removed it.  A character with a cooldown and no guild carries the cooldown for its next create.
static svc::Character test_guild_relog(const svc::Character& previous, const TestGuildSink& sink, const std::string& guild_id) {
    svc::Character c = previous;
    c.guild.reset();
    const auto until = sink.cooldown_of(previous.id);
    if (const svc::Guild* g = sink.last(guild_id)) {
        svc::Guild out = *g;
        out.self_rank = -1;
        for (const svc::GuildMember& m : g->members) {
            if (m.character_id == previous.id) out.self_rank = m.rank;
        }
        if (out.self_rank >= 0) {
            out.cooldown_until_ms = until.value_or(0);
            c.guild = out;
            return c;
        }
    }
    if (until) {   // (the character service's cooldown-only guild: no id, the cooldown alone)
        svc::Guild empty;
        empty.self_rank = -1;
        empty.cooldown_until_ms = *until;
        c.guild = empty;
    }
    return c;
}

/// The wall clock in Unix milliseconds, for the join cooldown the sessions save (session/player_guild.inc's clock).
static int64_t test_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}


}  // namespace

static void test_the_guild_is_created_with_the_datas_rules_and_saved() {
    const std::string guild_name = test_guild_name(0);
    std::printf("a guild is created with the data's three ranks and 50 places, costs no money, pays one guild creation scroll, and every create is answered\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    if (!w.enter(0, test_guild_scroll_record("cyb", {0xaaa1, 0, 0, 1}, 1, 1), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    CHECK(test_guild_component(*w[0].s) != nullptr, "the pawn carries the guild component right behind the join, even in no guild");
    CHECK(test_guild_text(*w[0].s, "GuildName").empty(), "a character in no guild has an empty guild name");
    CHECK(test_guild_instance(*w[0].s) == nullptr, "...and no GuildMember alignment");
    size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, guild_name), "ServerCreateGuild is sent with the scroll the create pays with");
    w.settle();
    const std::vector<TestGuildReply> created = test_guild_replies(w, 0, from);
    const std::optional<TestGuildReply> ok = test_guild_reply(created, "ClientGuildRequestSucceeded");
    CHECK(ok && test_guild_reply_whole(*ok, "RequestType") == 1, "the create is answered with ClientGuildRequestSucceeded(CreateGuild)");
    CHECK(test_guild_text(*w[0].s, "GuildName") == guild_name, "the pawn's GuildName is the new guild's name (the nameplate)");
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->info.name == guild_name && data->info.ranks.size() == 3 && data->info.ranks[0].name == "Guild Leader" &&
              data->info.ranks[0].permissions == kTestGuildLeader && data->info.ranks[1].permissions == kTestGuildOfficer && data->info.ranks[2].permissions == kTestGuildMember,
          "GuildData carries the data's three ranks and their 35, 26 and 8 permissions");
    CHECK(data && data->info.capacity == 50 && data->info.level == 1 && data->members.size() == 1 && data->members[0].name == "cyb" && data->members[0].rank == 0 &&
              data->members[0].online,
          "the roster holds the creator, the leader, online, and the data's 50 places");
    CHECK(test_guild_instance(*w[0].s) != nullptr && test_guild_instance(*w[0].s)->guids.size() == 1,
          "the pawn's AlignmentComponent carries the GuildMember instance (the guild's GUID)");
    CHECK(!sink.saves.empty() && sink.saves.back().name == guild_name && sink.saves.back().version == 0 && sink.saves.back().ranks.size() == 3 &&
              sink.saves.back().members.size() == 1 && sink.saves.back().members[0].rank == 0,
          "the guild is saved whole: a create (version 0), its three ranks and its one member");
    // (the data's GuildCost is 0 Money: nothing is spent, and the session reads no purse)
    from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "Second Guild"), "a second create is sent");
    w.settle();
    const std::vector<TestGuildReply> second = test_guild_replies(w, 0, from);
    CHECK(test_guild_reply(second, "ClientGuildRequestFailed").has_value(), "a character already in a guild gets ClientGuildRequestFailed");
    CHECK(test_guild_data(*w[0].s) && test_guild_data(*w[0].s)->info.name == guild_name, "...and its guild is unchanged");
    // a name the client's rule refuses, and a name another guild has in another case
    if (!w.enter(1, party_record("dee", {0xaaa2, 0, 0, 2}), identity())) {
        CHECK(false, "a second player joins");
        return;
    }
    from = w[1].got.size();
    CHECK(test_guild_create(w, 1, "Sandal  Makers"), "a name with a double space is sent");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed").has_value(), "the client's name form refuses a double space");
    std::string lower_name = guild_name;
    for (char& c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    from = w[1].got.size();
    CHECK(test_guild_create(w, 1, lower_name), "a name another guild has in another case is sent");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed").has_value(), "a guild name taken in another case is refused");
    CHECK(test_guild_text(*w[1].s, "GuildName").empty(), "...and the second player is in no guild");
}

static void test_an_invitation_stands_sixty_seconds_and_expires() {
    const std::string guild_name = test_guild_name(1);
    std::printf("an invitation stands 60 s: the invited client is asked, an accept after it is refused, and a second one works\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(1);
    svc::Character cyb = party_record("cyb", {0xbbb1, 0, 0, 1});
    cyb = guild_record("cyb", {0xbbb1, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}});
    if (!w.enter(0, cyb, identity())) {
        CHECK(false, "the leader joins");
        return;
    }
    if (!w.enter(1, party_record("dee", {0xbbb2, 0, 0, 2}), identity())) {
        CHECK(false, "the invited player joins");
        return;
    }
    size_t from = w[1].got.size();
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee");
    w.settle();
    const std::vector<TestGuildReply> asked = test_guild_replies(w, 1, from);
    const std::optional<TestGuildReply> invite = test_guild_reply(asked, "ClientInvitePlayerToGuild");
    CHECK(invite && test_guild_reply_text(*invite, "SourceCharacterName") == "cyb" && test_guild_reply_text(*invite, "InGuildName") == guild_name,
          "dee's client is asked by ClientInvitePlayerToGuild(cyb, the guild)");
    CHECK(w[1].s->guild_pending_inviter() == "cyb", "...and dee's session holds the invitation");
    // it stands 60 s
    w.t += std::chrono::seconds(61);
    w.step();
    CHECK(w[1].s->guild_pending_inviter().empty(), "61 s later the invitation is gone");
    from = w[1].got.size();
    CHECK(test_guild_accept(w, 1), "dee accepts");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed").has_value(), "an accept after the invitation expired is refused");
    CHECK(test_guild_text(*w[1].s, "GuildName").empty(), "...and dee is in no guild");
    // a second invitation, accepted in its 60 s
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee again");
    w.settle();
    from = w[1].got.size();
    CHECK(test_guild_accept(w, 1), "dee accepts the new invitation");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestSucceeded").has_value(), "the accept is answered with ClientGuildRequestSucceeded");
    CHECK(test_guild_text(*w[1].s, "GuildName") == guild_name, "dee's pawn carries the guild's name");
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 2 && data->members[1].name == "dee" && data->members[1].rank == 2 && data->members[1].online,
          "the leader's roster has dee, at the lowest rank and online");
    const svc::Guild* saved = sink.last(id);
    CHECK(saved && saved->members.size() == 2 && saved->version == cyb.guild->version,
          "the guild is saved with dee in it (the record's version, the save that makes it one more)");
}

static void test_declining_an_invitation_clears_it() {
    const std::string guild_name = test_guild_name(2);
    std::printf("declining an invitation clears it, and another invitation can follow\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(2);
    svc::Character cyb = party_record("cyb", {0xccc1, 0, 0, 1});
    cyb = guild_record("cyb", {0xccc1, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, party_record("dee", {0xccc2, 0, 0, 2}), identity())) {
        CHECK(false, "the two players join");
        return;
    }
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee");
    w.settle();
    CHECK(test_guild_decline(w, 1), "dee declines");
    w.settle();
    CHECK(w[1].s->guild_pending_inviter().empty(), "dee's session holds no invitation any more");
    CHECK(test_guild_text(*w[1].s, "GuildName").empty(), "dee is in no guild");
    CHECK(w[0].s->guild_name() == guild_name, "the leader's guild is unchanged");
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee again at once");
    w.settle();
    CHECK(!w[1].s->guild_pending_inviter().empty(), "...and dee has the new invitation");
    CHECK(sink.saves.empty(), "the decline changed no membership, so nothing was saved");
}

static void test_leaving_and_the_leaders_leave() {
    const std::string guild_name = test_guild_name(3);
    std::printf("leaving gives the data's 432,000 s cooldown, and a leader's leaving makes the next-highest-ranking member the leader\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(3);
    svc::Character cyb = party_record("cyb", {0xddd1, 0, 0, 1});
    svc::Character dee = party_record("dee", {0xddd2, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0xddd3, 0, 0, 3});
    cyb = guild_record("cyb", {0xddd1, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    dee = guild_record("dee", {0xddd2, 0, 0, 2}, id, guild_name, 1, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    ayla = guild_record("ayla", {0xddd3, 0, 0, 3}, id, guild_name, 2, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity()) || !w.enter(2, ayla, identity())) {
        CHECK(false, "the three players join");
        return;
    }
    size_t from = w[0].got.size();
    CHECK(test_guild_leave(w, 0), "the leader leaves");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the leave is answered with ClientGuildRequestSucceeded");
    CHECK(w[0].s->guild_name().empty() && test_guild_text(*w[0].s, "GuildName").empty(), "the leader is in no guild and its pawn's name is empty");
    CHECK(test_guild_instance(*w[0].s) == nullptr, "and its GuildMember alignment is gone");
    const auto cooldown = sink.cooldown_of(test_guild_character(w, 0));
    CHECK(cooldown && *cooldown > test_wall_ms() - 1000 && *cooldown <= test_wall_ms() + 432000000 + 1000,
          "the leader's join cooldown is saved at 432,000 s from now");
    CHECK(w[1].s->guild_rank() == 0, "the officer (dee, the highest rank below the leader) is the leader now");
    const svc::Guild* saved = sink.last(id);
    CHECK(saved && saved->members.size() == 2 && saved->members[0].name == "dee" && saved->members[0].rank == 0 && saved->members[1].name == "ayla",
          "the save has dee as the leader and ayla as the member");
    // a member who is not the leader leaves next; the leader stays
    from = w[2].got.size();
    CHECK(test_guild_leave(w, 2), "the member leaves");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 2, from), "ClientGuildRequestSucceeded").has_value(), "the member's leave is answered");
    CHECK(w[1].s->guild_rank() == 0 && w[1].s->guild_name() == guild_name, "the leader stays the leader");
    CHECK(w[2].s->guild_name().empty(), "the member is out");
}

static void test_kick_permissions_and_the_rank_hierarchy() {
    const std::string guild_name = test_guild_name(4);
    std::printf("a kick needs bKickPlayer and a lower rank: a member may not kick, an officer may kick a member but not the leader\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(4);
    svc::Character cyb = party_record("cyb", {0xeee1, 0, 0, 1});
    svc::Character dee = party_record("dee", {0xeee2, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0xeee3, 0, 0, 3});
    cyb = guild_record("cyb", {0xeee1, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    dee = guild_record("dee", {0xeee2, 0, 0, 2}, id, guild_name, 1, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    ayla = guild_record("ayla", {0xeee3, 0, 0, 3}, id, guild_name, 2, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity()) || !w.enter(2, ayla, identity())) {
        CHECK(false, "the three players join");
        return;
    }
    // ayla, a member, may not kick
    size_t from = w[2].got.size();
    CHECK(test_guild_kick(w, 2, w.guid_of(1)), "a member sends a kick");
    w.settle();
    const std::optional<TestGuildReply> refused = test_guild_reply(test_guild_replies(w, 2, from), "ClientGuildRequestFailed");
    const std::string detail = refused ? std::to_string(refused->param_bits) + " bits, " + std::to_string(refused->params.values.size()) + " params, type " +
                                            std::to_string(test_guild_reply_whole(*refused, "RequestType"))
                                      : std::string();
    CHECK(refused && test_guild_reply_text(*refused, "Message") == "You do not have permissions to kick players from the guild.",
          ("the member's kick is refused with the game's own text (" + detail + ")").c_str());
    // dee, an officer, kicks ayla, a member
    from = w[1].got.size();
    CHECK(test_guild_kick(w, 1, w.guid_of(2)), "the officer kicks the member");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestSucceeded").has_value(), "the officer's kick of a member is answered with success");
    CHECK(w[2].s->guild_name().empty(), "the kicked member is out");
    CHECK(sink.cooldown_of(test_guild_character(w, 2)).has_value(), "the kicked member has the join cooldown");
    // an officer may not kick the leader
    from = w[1].got.size();
    CHECK(test_guild_kick(w, 1, w.guid_of(0)), "the officer tries to kick the leader");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed").has_value(), "the kick of the leader is refused");
    CHECK(w[0].s->guild_name() == guild_name, "the leader is still in the guild");
    // the leader kicks the last officer: only a leader is left
    from = w[0].got.size();
    CHECK(test_guild_kick(w, 0, w.guid_of(1)), "the leader kicks the officer");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the leader's kick of the officer is answered with success");
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 1 && data->members[0].name == "cyb", "the roster is the leader alone");
    CHECK(!sink.saves.empty(), "the kicks were saved");
}

static void test_promote_and_demote_within_the_three_ranks() {
    const std::string guild_name = test_guild_name(5);
    std::printf("promote and demote stay within the three ranks and their ranks' places: an officer may not make an officer, a member may change nothing\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(5);
    svc::Character cyb = party_record("cyb", {0xfa01, 0, 0, 1});
    svc::Character dee = party_record("dee", {0xfa02, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0xfa03, 0, 0, 3});
    cyb = guild_record("cyb", {0xfa01, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    dee = guild_record("dee", {0xfa02, 0, 0, 2}, id, guild_name, 1, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    ayla = guild_record("ayla", {0xfa03, 0, 0, 3}, id, guild_name, 2, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity()) || !w.enter(2, ayla, identity())) {
        CHECK(false, "the three players join");
        return;
    }
    // the leader promotes the member to officer
    size_t from = w[0].got.size();
    CHECK(test_guild_reassign(w, 0, w.guid_of(2), 1), "the leader promotes the member");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the promotion is answered with success");
    CHECK(w[2].s->guild_rank() == 1, "the member is an officer in its own session");
    // the officer may not make another officer
    from = w[1].got.size();
    CHECK(test_guild_reassign(w, 1, w.guid_of(2), 1), "an officer tries to promote a member (there is none: it names the officer)");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed").has_value(), "a change at or above the officer's own rank is refused");
    // the member (now an officer) may not reassign either, once demoted by the leader
    from = w[0].got.size();
    CHECK(test_guild_reassign(w, 0, w.guid_of(2), 2), "the leader demotes the officer");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the demotion is answered with success");
    CHECK(w[2].s->guild_rank() == 2, "the officer is a member again");
    from = w[2].got.size();
    CHECK(test_guild_reassign(w, 2, w.guid_of(1), 2), "the member tries to demote the officer");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 2, from), "ClientGuildRequestFailed").has_value(), "a member's rank change is refused");
    // only the leader can name the leader, with its own call
    from = w[1].got.size();
    CHECK(test_guild_assign_leader(w, 1, w.guid_of(2)), "an officer tries to name the leader");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 1, from), "ClientGuildRequestFailed").has_value(), "only the leader can name the leader");
    from = w[0].got.size();
    CHECK(test_guild_assign_leader(w, 0, w.guid_of(2)), "the leader names ayla the next leader");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the leader change is answered with success");
    CHECK(w[2].s->guild_rank() == 0 && w[0].s->guild_rank() == 1, "ayla leads and cyb is an officer now");
}

static void test_disband_with_members_online_and_offline() {
    const std::string guild_name = test_guild_name(6);
    std::printf("a disband empties every member's guild, online or offline, saves the cooldowns and takes the guild away\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(6);
    svc::Character cyb = party_record("cyb", {0xfb01, 0, 0, 1});
    svc::Character dee = party_record("dee", {0xfb02, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0xfb03, 0, 0, 3});
    cyb = guild_record("cyb", {0xfb01, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    dee = guild_record("dee", {0xfb02, 0, 0, 2}, id, guild_name, 1, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    ayla = guild_record("ayla", {0xfb03, 0, 0, 3}, id, guild_name, 2, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity())) {
        CHECK(false, "two players join");
        return;
    }
    // ayla is offline when the disband happens
    size_t from = w[0].got.size();
    CHECK(test_guild_disband(w, 0), "the leader disbands");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the disband is answered with success");
    CHECK(w[0].s->guild_name().empty() && w[1].s->guild_name().empty(), "both online members are out");
    CHECK(test_guild_text(*w[0].s, "GuildName").empty() && test_guild_text(*w[1].s, "GuildName").empty(), "their pawns carry no guild name");
    CHECK(test_guild_instance(*w[0].s) == nullptr && test_guild_instance(*w[1].s) == nullptr, "and no GuildMember alignment");
    CHECK(sink.cooldown_of(test_guild_character(w, 0)).has_value() && sink.cooldown_of(test_guild_character(w, 1)).has_value() &&
              sink.cooldown_of(ayla.id).has_value(),
          "every member has the join cooldown, the offline one too");
    CHECK(!sink.saves.empty() && sink.saves.back().disband, "the guild's disband is saved");
    // the guild is gone: another character cannot find it by name
    if (w.enter(2, party_record("bram", {0xfb04, 0, 0, 4}), identity())) {
        from = w[2].got.size();
        CHECK(test_guild_invite(w, 2, "dee"), "a player in no guild tries to invite");
        w.settle();
        CHECK(test_guild_reply(test_guild_replies(w, 2, from), "ClientGuildRequestFailed").has_value(), "an invitation needs a guild");
    }
}

static void test_the_roster_and_its_refresh() {
    const std::string guild_name = test_guild_name(7);
    std::printf("the roster carries every member's level, class, rank, last online and online flag, and refreshes at a change\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(7);
    svc::Character cyb = party_record("cyb", {0xfc01, 0, 0, 1});
    svc::Character dee = party_record("dee", {0xfc02, 0, 0, 2});
    cyb = guild_record("cyb", {0xfc01, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}, {&dee, 2}});
    dee = guild_record("dee", {0xfc02, 0, 0, 2}, id, guild_name, 2, {{&cyb, 0}, {&dee, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity())) {
        CHECK(false, "the two players join");
        return;
    }
    const repl::GuildData* data = test_guild_data(*w[1].s);
    CHECK(data && data->members.size() == 2 && data->members[0].name == "cyb" && data->members[0].rank == 0 && data->members[0].class_id == 2 && data->members[0].online,
          "dee's roster has the leader, its class and its online flag");
    CHECK(data && data->members[1].name == "dee" && data->members[1].rank == 2 && data->members[1].online, "and dee, at the lowest rank, online");
    const repl::Value* own = test_guild_value(*w[1].s, "GuildMemberData.PersistedGuildMemberData.CharacterName");
    CHECK(own && own->get_if<std::string>() && *own->get_if<std::string>() == "dee", "the member's own row (GuildMemberData) is dee");
    // the leader gains a level: the roster is sent again to dee
    const auto xp = w[0].s->gm_give_experience(w[0].conn, 1000000, w.t, "the guild test");
    CHECK(xp.ok, "the leader gains experience");
    w.settle();
    w.step();
    w.settle();
    data = test_guild_data(*w[1].s);
    CHECK(data && data->members[0].level == xp.level_after, "dee's roster carries the leader's new level");
    // a refresh answers the leader (the state itself is the component, so the answer is what the request gets)
    const size_t from = w[0].got.size();
    CHECK(test_guild_refresh(w, 0), "the leader asks for its guild's data");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the refresh is answered");
}

static void test_guild_chat_reaches_the_ranks_that_may_read_it() {
    const std::string guild_name = test_guild_name(8);
    const std::string guild_name_two = test_guild_name(48);
    std::printf("guild chat reaches the guild's online readers, from a writer; a non-writer is told the game's text; two guilds do not mix\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string one = test_guild_id(8), two = test_guild_id(9);
    svc::Character cyb = party_record("cyb", {0xfd01, 0, 0, 1});
    svc::Character dee = party_record("dee", {0xfd02, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0xfd03, 0, 0, 3});
    svc::Character bram = party_record("bram", {0xfd04, 0, 0, 4});
    cyb = guild_record("cyb", {0xfd01, 0, 0, 1}, one, guild_name, 0, {{&cyb, 0}, {&dee, 2}});
    dee = guild_record("dee", {0xfd02, 0, 0, 2}, one, guild_name, 2, {{&cyb, 0}, {&dee, 2}});
    ayla = guild_record("ayla", {0xfd03, 0, 0, 3}, two, guild_name_two, 0, {{&ayla, 0}, {&bram, 2}});
    bram = guild_record("bram", {0xfd04, 0, 0, 4}, two, guild_name_two, 2, {{&ayla, 0}, {&bram, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity()) || !w.enter(2, ayla, identity()) || !w.enter(3, bram, identity())) {
        CHECK(false, "the four players join");
        return;
    }
    const size_t dee_before = guild_chat_received(*w[1].s);
    const size_t ayla_before = guild_chat_received(*w[2].s);
    CHECK(w[1].s->guild_name() == guild_name && w[1].s->guild_rank() == 2, "(dee is a member of the same guild)");
    CHECK(w[3].s->guild_name() == guild_name_two, "(bram is in the other guild)");
    CHECK(test_guild_chat(w, 0, "Hello guild"), "the leader sends a guild message");
    w.settle();
    CHECK(w[0].s->guild_name() == guild_name, "(the sender is in the guild)");
    {
        const std::string log = g_log.str();
        const auto at = log.rfind("[guild]");
        const std::string line = at == std::string::npos ? std::string("(no guild log)") : log.substr(at, 140);
        CHECK(guild_chat_received(*w[1].s) == dee_before + 1, line.c_str());
    }
    CHECK(guild_chat_received(*w[2].s) == ayla_before, "the other guild gets nothing");
    CHECK(w[0].s->chat_lines_refused() == 0, "the writer's line is not refused");
    CHECK(test_guild_chat(w, 2, "Hello from the River Runners"), "the other guild's leader sends its own message");
    w.settle();
    CHECK(guild_chat_received(*w[3].s) == 1, "its member gets it");
    // an empty line and a long one are refused as chat's are
    const size_t dee_lines = guild_chat_received(*w[1].s);
    CHECK(test_guild_chat(w, 0, "   "), "a blank guild line is sent");
    w.settle();
    CHECK(guild_chat_received(*w[1].s) == dee_lines, "a blank guild line reaches nobody");
    std::string long_line(600, 'a');
    CHECK(test_guild_chat(w, 0, long_line), "a 600-character guild line is sent");
    w.settle();
    CHECK(guild_chat_received(*w[1].s) == dee_lines, "an over-long guild line reaches nobody");
    // a rank without bWriteGuildChat may not send: its text is the game's own, and nobody receives the line
    const std::string id2 = test_guild_id(14);
    svc::Character mira = party_record("mira", {0xfd05, 0, 0, 5});
    svc::Character no_write = party_record("nora", {0xfd06, 0, 0, 6});
    mira = guild_record("mira", {0xfd05, 0, 0, 5}, id2, "Quiet Guild", 2, {{&mira, 2}});
    no_write = guild_record("nora", {0xfd06, 0, 0, 6}, id2, "Quiet Guild", 2, {{&mira, 2}, {&no_write, 2}});
    mira.guild->ranks[2].permissions = kTestGuildMember & ~(1ull << 30);   // bWriteGuildChat off for the member rank
    no_write.guild->ranks[2].permissions = mira.guild->ranks[2].permissions;
    if (!w.enter(4, mira, identity()) || !w.enter(5, no_write, identity())) {
        CHECK(false, "the quiet guild's two members join");
        return;
    }
    const size_t nora_before = guild_chat_received(*w[5].s);
    CHECK(test_guild_chat(w, 4, "Can I speak?"), "a member whose rank cannot write sends a line");
    w.settle();
    CHECK(guild_chat_received(*w[5].s) == nora_before, "nobody receives it");
    CHECK(w[4].s->chat_lines_received() == 1, "and the sender is told the game's own text");
}

static void test_a_full_guild_refuses_the_next_member() {
    const std::string guild_name = test_guild_name(9);
    std::printf("a guild of the data's 50 places refuses the 51st member, at the invitation and at the accept\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(10);
    svc::Character cyb = party_record("cyb", {0xfe01, 0, 0, 1});
    // 49 more rows, all offline (only the character service knows them here)
    std::vector<svc::Character> others(49);
    std::vector<std::pair<const svc::Character*, int>> members = {{&cyb, 0}};
    for (size_t i = 0; i < others.size(); ++i) {
        others[i] = party_record(("Filler" + std::to_string(i)).c_str(), {0xfe00, 0, static_cast<uint32_t>(i), 0xfe});
        members.emplace_back(&others[i], 2);
    }
    cyb = guild_record("cyb", {0xfe01, 0, 0, 1}, id, guild_name, 0, members);
    if (!w.enter(0, cyb, identity()) || !w.enter(1, party_record("dee", {0xfe02, 0, 0, 2}), identity())) {
        CHECK(false, "the two players join");
        return;
    }
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 50 && data->info.capacity == 50, "the roster holds the data's 50 members");
    size_t from = w[0].got.size();
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites a 51st");
    w.settle();
    const std::optional<TestGuildReply> refused = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed");
    const std::string refusal = refused ? test_guild_reply_text(*refused, "Message") : std::string("(no failure)");
    const std::string detail = refused ? std::to_string(refused->param_bits) + " bits, " + std::to_string(refused->params.values.size()) + " params, type " +
                                            std::to_string(test_guild_reply_whole(*refused, "RequestType"))
                                      : std::string();
    CHECK(refused && refusal == "Your guild is full.", ("the full guild's invitation is refused with the server's text (got: " + refusal + "; " + detail + ")").c_str());
}

static void test_the_same_request_twice() {
    const std::string guild_name = test_guild_name(10);
    std::printf("the same request twice changes nothing the second time and is answered each time\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    if (!w.enter(0, test_guild_scroll_record("cyb", {0xff01, 0, 0, 1}, 1, 41), identity()) || !w.enter(1, party_record("dee", {0xff02, 0, 0, 2}), identity())) {
        CHECK(false, "the two players join");
        return;
    }
    size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, guild_name), "the create is sent with a guild creation scroll");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the first create succeeds");
    if (sink.saves.empty()) {
        CHECK(false, "the create is saved");
        return;
    }
    const std::string id = sink.saves.back().id;
    from = w[0].got.size();
    CHECK(test_guild_create(w, 0, guild_name), "the same create is sent again");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed").has_value(), "the second create is refused");
    CHECK(sink.last(id) && sink.last(id)->members.size() == 1, "and the guild did not change");
    from = w[0].got.size();
    CHECK(test_guild_invite(w, 0, "dee"), "an invitation is sent");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the first invitation succeeds");
    const size_t saves = sink.saves.size();
    from = w[0].got.size();
    CHECK(test_guild_invite(w, 0, "dee"), "the same invitation is sent again");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed").has_value(), "the second invitation is refused (one stands)");
    CHECK(sink.saves.size() == saves, "and nothing was saved");
}

static void test_an_offline_member_and_a_reconnect_mid_invite() {
    const std::string guild_name = test_guild_name(11);
    std::printf("a member who is offline at a change keeps the change for its next login; an invitation is not inherited by a new session\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(11);
    svc::Character cyb = party_record("cyb", {0x1a01, 0, 0, 1});
    svc::Character dee = party_record("dee", {0x1a02, 0, 0, 2});
    cyb = guild_record("cyb", {0x1a01, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}, {&dee, 2}});
    dee = guild_record("dee", {0x1a02, 0, 0, 2}, id, guild_name, 2, {{&cyb, 0}, {&dee, 2}});
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity())) {
        CHECK(false, "the two players join");
        return;
    }
    // dee logs out; the leader promotes it while it is offline
    w[1].s->finish("the guild test's logout");
    w.settle();
    CHECK(w[0].s->guild_name() == guild_name, "the leader is still in the guild");
    size_t from = w[0].got.size();
    CHECK(test_guild_reassign(w, 0, w.guid_of(1), 1), "the leader promotes the offline member");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the promotion is answered");
    const svc::Guild* saved = sink.last(id);
    CHECK(saved && saved->members.size() == 2 && saved->members[1].name == "dee" && saved->members[1].rank == 1, "the save holds dee as an officer while offline");
    // dee logs in again from that save: it is an officer
    const svc::Character dee_again = test_guild_relog(dee, sink, id);
    CHECK(w.enter(2, dee_again, identity()), "dee logs in again");
    CHECK(w[2].s->guild_rank() == 1, "dee is an officer after the relog");
    const repl::GuildData* data = test_guild_data(*w[2].s);
    CHECK(data && data->members.size() == 2 && data->members[1].name == "dee" && data->members[1].rank == 1, "and its roster says so");
    // a character invited and then reconnecting does not inherit the invitation
    if (!w.enter(3, party_record("bram", {0x1a04, 0, 0, 4}), identity())) {
        CHECK(false, "a fourth player joins");
        return;
    }
    CHECK(test_guild_invite(w, 0, "bram"), "the leader invites bram");
    w.settle();
    CHECK(w[3].s->guild_pending_inviter() == "cyb", "bram holds the invitation");
    w[3].s->finish("the guild test's reconnect");
    w.settle();
    const svc::Character bram_again = test_guild_relog(party_record("bram", {0x1a04, 0, 0, 4}), sink, id);
    if (!w.enter(4, bram_again, identity())) {
        CHECK(false, "bram logs in again");
        return;
    }
    CHECK(w[4].s->guild_pending_inviter().empty(), "the new session holds no invitation");
    from = w[4].got.size();
    CHECK(test_guild_accept(w, 4), "the new session accepts");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 4, from), "ClientGuildRequestFailed").has_value(), "and its accept is refused");
}

static void test_a_login_elsewhere_keeps_the_guild() {
    const std::string guild_name = test_guild_name(12);
    std::printf("a character that logs out and back in joins its guild again, with its rank and its roster\n");
    std::fflush(stdout);
    const std::string id = test_guild_id(12);
    TestGuildSink sink;
    svc::Character holder = party_record("cyb", {0x1b01, 0, 0, 1});
    svc::Character dee = party_record("dee", {0x1b02, 0, 0, 2});
    holder = guild_record("cyb", {0x1b01, 0, 0, 1}, id, guild_name, 0, {{&holder, 0}, {&dee, 2}});
    {
        PartyWorld w;
        w.fx.world->set_position_sink(&sink);
        if (!w.enter(0, holder, identity())) {
            CHECK(false, "the first session joins");
            return;
        }
        w[0].s->finish("the guild test's logout");
    }
    CHECK(sink.last(id) && sink.last(id)->members.size() == 2, "the logout saved the guild with its members");
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const svc::Character again = test_guild_relog(holder, sink, id);
    if (!w.enter(0, again, identity())) {
        CHECK(false, "the next session joins");
        return;
    }
    CHECK(w[0].s->guild_name() == guild_name && w[0].s->guild_rank() == 0, "the next session holds the guild and the leader's rank");
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 2 && data->members[1].name == "dee", "and the roster it saved");
}

static void test_a_cooldown_survives_the_relog() {
    std::printf("a character that left a guild is on its join cooldown at the next login, and a create is refused with the game's text\n");
    std::fflush(stdout);
    const std::string guild_name = test_guild_name(14);
    TestGuildSink sink;
    const std::string id = test_guild_id(20);
    svc::Character cyb = party_record("cyb", {0x2a01, 0, 0, 1});
    cyb = guild_record("cyb", {0x2a01, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}});
    {
        PartyWorld w;
        w.fx.world->set_position_sink(&sink);
        if (!w.enter(0, cyb, identity())) {
            CHECK(false, "the session joins");
            return;
        }
        CHECK(test_guild_leave(w, 0), "the leader leaves its guild");
        w.settle();
    }
    const auto cooldown = sink.cooldown_of(cyb.id);
    CHECK(cooldown && *cooldown > test_wall_ms(), "the leave's cooldown was saved");
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const svc::Character again = test_guild_relog(cyb, sink, id);
    if (!w.enter(0, again, identity())) {
        CHECK(false, "the next session joins");
        return;
    }
    CHECK(w[0].s->guild_name().empty(), "the character is in no guild at the next login");
    const size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, guild_name), "it tries to create a guild again");
    w.settle();
    const std::optional<TestGuildReply> refused = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed");
    CHECK(refused && test_guild_reply_text(*refused, "Message") == "You may not join a guild for a while.",
          "the cooldown is kept across the login and the create is refused with the game's own text");
}

static void test_the_guild_instance_survives_death() {
    const std::string guild_name = test_guild_name(13);
    std::printf("a member's GuildMember alignment and guild name stay when it dies\n");
    std::fflush(stdout);
    TestGuildSink sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(13);
    svc::Character cyb = party_record("cyb", {0x1c01, 0, 0, 1});
    cyb = guild_record("cyb", {0x1c01, 0, 0, 1}, id, guild_name, 0, {{&cyb, 0}});
    if (!w.enter(0, cyb, identity())) {
        CHECK(false, "the player joins");
        return;
    }
    CHECK(test_guild_instance(*w[0].s) != nullptr, "the guild instance is on the alignment");
    const size_t from = w[0].got.size();
    const auto killed = w[0].s->gm_kill(w[0].conn, w.t, "the guild test");
    CHECK(killed.ok, "the member is killed");
    w.settle();
    (void)from;
    CHECK(test_guild_instance(*w[0].s) != nullptr, "the guild instance stays after the death");
    CHECK(test_guild_text(*w[0].s, "GuildName") == guild_name, "and the guild's name stays on the pawn");
    CHECK(w[0].s->guild_name() == guild_name, "the session is still in the guild");
}

AOC_REGISTER_TEST(test_the_guild_is_created_with_the_datas_rules_and_saved);
AOC_REGISTER_TEST(test_an_invitation_stands_sixty_seconds_and_expires);
AOC_REGISTER_TEST(test_declining_an_invitation_clears_it);
AOC_REGISTER_TEST(test_leaving_and_the_leaders_leave);
AOC_REGISTER_TEST(test_kick_permissions_and_the_rank_hierarchy);
AOC_REGISTER_TEST(test_promote_and_demote_within_the_three_ranks);
AOC_REGISTER_TEST(test_disband_with_members_online_and_offline);
AOC_REGISTER_TEST(test_the_roster_and_its_refresh);
AOC_REGISTER_TEST(test_guild_chat_reaches_the_ranks_that_may_read_it);
AOC_REGISTER_TEST(test_a_full_guild_refuses_the_next_member);
AOC_REGISTER_TEST(test_the_same_request_twice);
AOC_REGISTER_TEST(test_an_offline_member_and_a_reconnect_mid_invite);
AOC_REGISTER_TEST(test_a_login_elsewhere_keeps_the_guild);
AOC_REGISTER_TEST(test_a_cooldown_survives_the_relog);
AOC_REGISTER_TEST(test_the_guild_instance_survives_death);
