// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_guild_rules_tests.inc: the guilds, second pass (Social, step 9)
//
//  What the first guild tests did not pin:
//    - the join is the retail login's, chain for chain: the pawn's guild component and the GuildMember alignment come in a chain of their own after the phase-in, for a
//      character in no guild (an empty component) and for a member;
//    - an invitation belongs to the invited player: it is gone when that player logs out, and a guild has one out for each player it asked, not one at a time;
//    - the guild is stored the way the character service takes it (a store that checks what the service checks: the leader, the members, the versions): a guild
//      the GM command makes, a join, a leave, a disband and a rank change leave the store with the guild the world has, and no save is refused;
//    - a name that a guild no member of which is online here has is refused before the guild is made;
//    - the GM command checks the guild it names before the character leaves the one it is in, and changes a rank inside the guild it is in;
//    - the saver keeps the order of one guild's saves: a save that waits and a newer one are written over the version the service holds;
//    - one guild is one tier of the PvP rules: a guildmate may be helped, not harmed;
//    - a member who logs out shows offline to the others;
//    - two invitations for the last place (the first accept wins), and a leader who kicks the only officer.
// ============================================================================
#include "friend_fixes_rig.inc"
#include "guild_scroll_rig.inc"
#include "combat/pvp.h"

namespace {

/// A store that takes a guild the way the character service does (validateGuild and SaveGuild of services/internal/character/guilds.go): a create needs a free name
/// and one leader among one to fifty members, any other save the version the store holds (and makes it one more), a disband the version too.
struct TestGuildStore : TestGuildSink {
    std::map<std::string, svc::Guild> store;   // id -> the guild as the store holds it (its version is the store's)
    std::vector<std::string>          reasons; // every refusal, in words
    bool                              answers_creates = false;   // true: create_guild_now answers as the service does (Ok or Refused); false: Unavailable

    static std::string fold(std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
    bool name_taken(const std::string& name, const std::string& except_id) const {
        for (const auto& [id, g] : store) {
            if (id != except_id && fold(g.name) == fold(name)) return true;
        }
        return false;
    }
    /// The store's answer to one save; the refusals are counted in `reasons`.
    svc::CallStatus apply(const svc::Guild& g) {
        if (g.disband) {
            const auto it = store.find(g.id);
            if (g.version <= 0 || it == store.end()) {
                reasons.push_back("a disband of a guild the store does not hold (" + g.name + ")");
                return svc::CallStatus::NotFound;
            }
            if (it->second.version != g.version) {
                reasons.push_back("a stale disband of " + g.name);
                return svc::CallStatus::Failed;
            }
            store.erase(it);
            return svc::CallStatus::Ok;
        }
        int leaders = 0;
        for (const svc::GuildMember& m : g.members) leaders += m.rank == 0 ? 1 : 0;
        if (g.members.empty() || g.members.size() > 50 || leaders != 1 || g.ranks.size() != 3) {
            reasons.push_back("a guild the store's rules refuse: " + g.name + " has " + std::to_string(g.members.size()) + " member(s), " + std::to_string(leaders) + " leader(s)");
            return svc::CallStatus::Refused;
        }
        if (g.version == 0) {
            if (store.count(g.id) != 0 || name_taken(g.name, g.id)) {
                reasons.push_back("a create of a taken name: " + g.name);
                return svc::CallStatus::Refused;
            }
            svc::Guild kept = g;
            kept.version = 1;
            store[g.id] = kept;
            return svc::CallStatus::Ok;
        }
        const auto it = store.find(g.id);
        if (it == store.end()) {
            reasons.push_back("a save of a guild the store does not hold (" + g.name + ", version " + std::to_string(g.version) + ")");
            return svc::CallStatus::NotFound;
        }
        if (it->second.version != g.version) {
            reasons.push_back("a stale save of " + g.name + " (the store holds " + std::to_string(it->second.version) + ", the save writes over " + std::to_string(g.version) + ")");
            return svc::CallStatus::Failed;
        }
        svc::Guild kept = g;
        kept.version = g.version + 1;
        it->second = kept;
        return svc::CallStatus::Ok;
    }
    void save_guild(const svc::Guild& guild) override {
        TestGuildSink::save_guild(guild);
        apply(guild);
    }
    svc::CallStatus create_guild_now(const svc::Guild& guild) override {
        if (!answers_creates) return svc::CallStatus::Unavailable;
        creates.push_back(guild);
        return apply(guild);
    }
    std::vector<svc::Guild> creates;   // every create asked of the store at once
};

/// The store holds the guild a record carries: the character service had it before the world read it at the login.
static void seed_store(TestGuildStore& sink, const svc::Character& record) {
    if (!record.guild) return;
    svc::Guild held = *record.guild;
    held.self_rank = -1;
    held.cooldown_until_ms = 0;
    sink.store[held.id] = held;
}

/// The chains of a player's client that hold a block of the pawn's guild component, by their place in `got`.
static std::vector<size_t> guild_block_chains(const PartyWorld& w, size_t who) {
    std::vector<size_t> out;
    const uint64_t object = w.p[who].s->bootstrap().setup().guids.guild.ObjectId;
    for (size_t i = 0; i < w.p[who].got.size(); ++i) {
        const fake::Delivered& d = w.p[who].got[i];
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& v : *blocks) {
            if (!v.header.is_actor && v.header.object.ObjectId == object) {
                out.push_back(i);
                break;
            }
        }
    }
    return out;
}

/// The bit `at` of a bit string, LSB first.
static bool guild_bit(const uint8_t* data, size_t at) { return ((data[at >> 3] >> (at & 7)) & 1) != 0; }

/// Whether `value` (64 bits, LSB first) stands anywhere in the first `bits` bits of `data`.
static bool guild_bits_hold_u64(const uint8_t* data, size_t bits, uint64_t value) {
    for (size_t off = 0; off + 64 <= bits; ++off) {
        bool same = true;
        for (size_t k = 0; k < 64 && same; ++k) same = guild_bit(data, off + k) == (((value >> k) & 1) != 0);
        if (same) return true;
    }
    return false;
}

/// Whether the ASCII `text` stands anywhere in the first `bits` bits of `data` (the exports write a name as its characters, not at a byte border).
static bool guild_bits_hold_text(const uint8_t* data, size_t bits, const std::string& text) {
    for (size_t off = 0; off + text.size() * 8 <= bits; ++off) {
        bool same = true;
        for (size_t i = 0; i < text.size() && same; ++i) {
            for (size_t k = 0; k < 8 && same; ++k) same = guild_bit(data, off + i * 8 + k) == (((static_cast<uint8_t>(text[i]) >> k) & 1) != 0);
        }
        if (same) return true;
    }
    return false;
}

/// Whether `value` (32 bits, LSB first) stands anywhere in the first `bits` bits of `data`.
static bool guild_bits_hold_u32(const uint8_t* data, size_t bits, uint32_t value) {
    for (size_t off = 0; off + 32 <= bits; ++off) {
        bool same = true;
        for (size_t k = 0; k < 32 && same; ++k) same = guild_bit(data, off + k) == (((value >> k) & 1u) != 0);
        if (same) return true;
    }
    return false;
}

static bool chain_holds_text(const fake::Delivered& d, const std::string& text) {
    for (const BunchWriter& e : d.exports) {
        if (guild_bits_hold_text(e.data(), e.bit_pos(), text)) return true;
    }
    return guild_bits_hold_text(d.content.data(), d.content.bit_pos(), text);
}

static constexpr size_t kRetailJoinChains = 16;   // the retail login's chains (test_worldd_bootstrap's join_sequence)

}  // namespace

static void test_the_join_holds_no_guild_component_and_the_component_follows_it() {
    std::printf("the join is the retail login's: a character in no guild gets the pawn's guild component, empty, in a chain after the phase-in\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    if (!w.enter(0, test_guild_scroll_record("cyb", {0x3a01, 0, 0, 1}, 1, 51), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    const std::vector<size_t> chains = guild_block_chains(w, 0);
    CHECK(chains.size() == 1, "one chain of the client's holds a block of the guild component");
    CHECK(!chains.empty() && chains.front() >= kRetailJoinChains, "...and it is not one of the join's own 16 chains (the pawn's second state is the retail login's, bit for bit)");
    bool join_names_it = false;
    for (size_t i = 0; i < std::min<size_t>(kRetailJoinChains, w[0].got.size()); ++i) join_names_it = join_names_it || chain_holds_text(w[0].got[i], "GuildComponent");
    CHECK(!join_names_it, "no export of the join's 16 chains names the guild component");
    if (chains.empty()) return;
    const fake::Delivered& open = w[0].got[chains.front()];
    CHECK(chain_holds_text(open, "GuildComponent"), "the chain behind the phase-in exports the component by its name (the client learns its object from it)");
    bool has_checksum = false;
    for (const BunchWriter& e : open.exports) has_checksum = has_checksum || guild_bits_hold_u32(e.data(), e.bit_pos(), 0xd2a5e52cu);
    CHECK(has_checksum, "...with the checksum every retail export of the guild component carries (0xd2a5e52c)");
    const auto blocks = repl::read_content_blocks(open.content.data(), open.content.bit_pos(), 0);
    size_t payload = 1;
    if (blocks) {
        for (const repl::BlockView& v : *blocks) {
            if (!v.header.is_actor && v.header.object.ObjectId == w[0].s->bootstrap().setup().guids.guild.ObjectId) payload = v.payload_bits;
        }
    }
    CHECK(payload == 0, "...with nothing in it: a character in no guild and on no cooldown has the game's defaults, and no empty roster goes out");
    CHECK(!guild_bits_hold_u64(open.content.data(), open.content.bit_pos(), kTestGuildAlignment), "and no GuildMember alignment");
    // the create call goes to that component: it answers (it holds the scroll the create pays with)
    size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "Review Smiths"), "the client calls ServerCreateGuild on it, with a guild creation scroll");
    w.settle();
    const std::optional<TestGuildReply> ok = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded");
    CHECK(ok && test_guild_reply_whole(*ok, "RequestType") == 1, "the create is answered with ClientGuildRequestSucceeded(CreateGuild)");
    CHECK(ok && test_guild_reply_text(*ok, "Message") == "Guild Review Smiths was successfully created!", "...in the game's own words");
    CHECK(test_guild_text(*w[0].s, "GuildName") == "Review Smiths", "the component carries the new guild's name");
}

static void test_a_members_login_sends_its_guild_with_the_component_and_not_in_the_join() {
    std::printf("a member's login sends the guild and the GuildMember alignment in the component's own chain, not in the join\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(31);
    svc::Character cyb = party_record("cyb", {0x3a11, 0, 0, 1});
    cyb = guild_record("cyb", {0x3a11, 0, 0, 1}, id, "Review Alpha", 0, {{&cyb, 0}});
    seed_store(sink, cyb);
    if (!w.enter(0, cyb, identity())) {
        CHECK(false, "the session joins");
        return;
    }
    const std::vector<size_t> chains = guild_block_chains(w, 0);
    CHECK(chains.size() == 1 && chains.front() >= kRetailJoinChains, "one chain after the join holds the guild component's block");
    bool join_has_guild = false;
    for (size_t i = 0; i < std::min<size_t>(kRetailJoinChains, w[0].got.size()); ++i) {
        join_has_guild = join_has_guild || guild_bits_hold_u64(w[0].got[i].content.data(), w[0].got[i].content.bit_pos(), kTestGuildAlignment) ||
                         chain_holds_text(w[0].got[i], "GuildComponent");
    }
    CHECK(!join_has_guild, "the join's own 16 chains hold neither the component nor the GuildMember alignment");
    if (chains.empty()) return;
    const fake::Delivered& open = w[0].got[chains.front()];
    CHECK(guild_bits_hold_u64(open.content.data(), open.content.bit_pos(), kTestGuildAlignment), "the chain with the component carries the GuildMember alignment");
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(test_guild_text(*w[0].s, "GuildName") == "Review Alpha" && data && data->members.size() == 1 && data->info.ranks.size() == 3,
          "...and the guild's name and its roster");
}

static void test_an_invitee_who_logs_out_frees_the_invitation_and_a_guild_has_many_out() {
    std::printf("an invited player who logs out takes the invitation with it; a guild has an invitation out for each player it asked\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(32);
    svc::Character cyb = party_record("cyb", {0x3a21, 0, 0, 1});
    svc::Character dee = party_record("dee", {0x3a22, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0x3a23, 0, 0, 3});
    cyb = guild_record("cyb", {0x3a21, 0, 0, 1}, id, "Review Beta", 0, {{&cyb, 0}, {&ayla, 2}});
    ayla = guild_record("ayla", {0x3a23, 0, 0, 3}, id, "Review Beta", 2, {{&cyb, 0}, {&ayla, 2}});
    seed_store(sink, cyb);
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity()) || !w.enter(3, party_record("bram", {0x3a24, 0, 0, 4}), identity())) {
        CHECK(false, "the players join");
        return;
    }
    size_t from = w[0].got.size();
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the invitation is sent");
    w[1].s->finish("the guild test's logout");   // dee leaves the world with the invitation unanswered
    w.settle();
    from = w[0].got.size();
    CHECK(test_guild_invite(w, 0, "bram"), "the leader invites bram");
    w.settle();
    const std::optional<TestGuildReply> asked = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded");
    const std::optional<TestGuildReply> refused = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed");
    CHECK(asked && !refused, ("the guild may invite bram: dee's invitation went with dee" + std::string(refused ? " (refused: " + test_guild_reply_text(*refused, "Message") + ")" : "")).c_str());
    // dee comes back: no invitation is waiting for it
    if (!w.enter(2, party_record("dee", {0x3a22, 0, 0, 2}), identity())) {
        CHECK(false, "dee logs in again");
        return;
    }
    CHECK(w[2].s->guild_pending_name().empty(), "the new session of dee holds no invitation");
    // the roster of the leader says dee's old session is gone (it was never a member; ayla is, and is offline)
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 2 && data->members[1].name == "ayla" && !data->members[1].online, "(the roster shows ayla, a member who is not in the world, as offline)");
    // two invitations at once: dee and another player
    if (!w.enter(4, party_record("fenn", {0x3a25, 0, 0, 5}), identity())) {
        CHECK(false, "fenn joins");
        return;
    }
    from = w[0].got.size();
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee at once");
    CHECK(test_guild_invite(w, 0, "fenn"), "...and fenn");
    w.settle();
    size_t oks = 0, failures = 0;
    for (const TestGuildReply& r : test_guild_replies(w, 0, from)) {
        oks += r.name == "ClientGuildRequestSucceeded" ? 1 : 0;
        failures += r.name == "ClientGuildRequestFailed" ? 1 : 0;
    }
    CHECK(oks == 2 && failures == 0, "both invitations are sent: a guild is not held to one invitation at a time");
    CHECK(!w[2].s->guild_pending_inviter().empty() && !w[4].s->guild_pending_inviter().empty(), "...and both players hold theirs");
    CHECK(test_guild_accept(w, 2) && test_guild_accept(w, 4), "both accept");
    w.settle();
    CHECK(w[2].s->guild_name() == "Review Beta" && w[4].s->guild_name() == "Review Beta", "both are in the guild");
    CHECK(sink.reasons.empty(), "and the store refused no save");
}

static void test_the_guild_is_stored_the_way_the_service_takes_it() {
    std::printf("a guild the GM command makes, a join, a rank change, a leave and a disband leave the store with the world's guild and no save refused\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    if (!w.enter(0, party_record("cyb", {0x3a31, 0, 0, 1}), identity()) || !w.enter(1, party_record("dee", {0x3a32, 0, 0, 2}), identity()) ||
        !w.enter(2, party_record("ayla", {0x3a33, 0, 0, 3}), identity())) {
        CHECK(false, "the players join");
        return;
    }
    // the GM makes a guild with cyb as its leader (the guild does not exist yet), puts dee in and ayla in at officer
    const auto made = w[0].s->gm_guild(w[0].conn, "join", "Review Gamma", "leader", w.t, "the guild test");
    CHECK(made.ok, ("the GM makes the guild: " + made.refusal).c_str());
    const auto joined = w[1].s->gm_guild(w[1].conn, "join", "Review Gamma", "", w.t, "the guild test");   // (no rank: the command's hint says member)
    CHECK(joined.ok && w[1].s->guild_rank() == 2, ("dee joins it, with no rank said, as a member: " + joined.refusal).c_str());
    const auto officer = w[2].s->gm_guild(w[2].conn, "join", "Review Gamma", "officer", w.t, "the guild test");
    CHECK(officer.ok, ("ayla joins it at officer: " + officer.refusal).c_str());
    w.settle();
    CHECK(sink.store.size() == 1, "the store holds the guild");
    const svc::Guild* held = sink.store.empty() ? nullptr : &sink.store.begin()->second;
    CHECK(held && held->name == "Review Gamma" && held->members.size() == 3, "...with its three members");
    CHECK(sink.reasons.empty(), ("no save was refused" + (sink.reasons.empty() ? std::string() : ": " + sink.reasons.front())).c_str());
    // a rank change inside the guild: dee becomes an officer
    const auto promoted = w[1].s->gm_guild(w[1].conn, "join", "Review Gamma", "officer", w.t, "the guild test");
    CHECK(promoted.ok && w[1].s->guild_rank() == 1, ("a GM changes dee's rank inside the guild it is in: " + promoted.refusal).c_str());
    CHECK(w[1].s->guild_name() == "Review Gamma" && w[0].s->guild_name() == "Review Gamma", "...nobody left the guild");
    // the leader cannot be made a member while no one else leads
    const auto none = w[0].s->gm_guild(w[0].conn, "join", "Review Gamma", "member", w.t, "the guild test");
    CHECK(!none.ok && w[0].s->guild_rank() == 0, "a GM does not take the guild's only leader down to member");
    w.settle();
    // a GM sends a character to a guild that does not exist: it is refused and the character stays where it is
    const auto nowhere = w[1].s->gm_guild(w[1].conn, "join", "Review Nowhere", "member", w.t, "the guild test");
    CHECK(!nowhere.ok && w[1].s->guild_name() == "Review Gamma", "a GM's join of a guild that does not exist leaves the character in the guild it is in");
    // dee leaves, the leader disbands
    CHECK(test_guild_leave(w, 1), "dee leaves");
    w.settle();
    CHECK(sink.store.size() == 1 && sink.store.begin()->second.members.size() == 2, "the store has the guild without dee");
    CHECK(test_guild_disband(w, 0), "the leader disbands");
    w.settle();
    CHECK(sink.store.empty(), "the store holds no guild after the disband");
    CHECK(sink.reasons.empty(), ("no save was refused in all of it" + (sink.reasons.empty() ? std::string() : ": " + sink.reasons.front())).c_str());
}

static void test_a_name_a_stored_guild_has_is_refused_before_the_guild_is_made() {
    std::printf("a guild name that a guild none of whose members is online here has is refused before the guild is made, and a free one is created at once\n");
    std::fflush(stdout);
    TestGuildStore sink;
    sink.answers_creates = true;
    svc::Guild held;   // a guild only the store knows
    held.id = test_guild_id(33);
    held.name = "Review Delta";
    held.version = 4;
    held.ranks = {{"Guild Leader", kTestGuildLeader}, {"Officer", kTestGuildOfficer}, {"Member", kTestGuildMember}};
    svc::GuildMember lead;
    lead.character_id = "00000000000000000000000000000001";
    lead.name = "Offline";
    lead.rank = 0;
    held.members.push_back(lead);
    sink.store[held.id] = held;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    if (!w.enter(0, test_guild_scroll_record("cyb", {0x3a41, 0, 0, 1}, 1, 61), identity())) {
        CHECK(false, "the session joins");
        return;
    }
    size_t from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "review delta"), "a create of a name the store has (in another case) is sent, with a guild creation scroll");
    w.settle();
    const std::optional<TestGuildReply> refused = test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestFailed");
    CHECK(refused && test_guild_reply_text(*refused, "Message") == "That guild name is taken.", "it is refused: the name is taken");
    CHECK(w[0].s->guild_name().empty() && test_guild_text(*w[0].s, "GuildName").empty(), "...and the player is in no guild");
    CHECK(w[0].s->inventory().at(svc::ItemContainer::Backpack, 4) != nullptr, "...and the refused create kept the scroll");
    CHECK(sink.saves.empty(), "nothing is queued for the store");
    from = w[0].got.size();
    CHECK(test_guild_create(w, 0, "Review Epsilon"), "a free name is sent");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "it is created");
    CHECK(w[0].s->inventory().at(svc::ItemContainer::Backpack, 4) == nullptr, "...and the scroll paid for it");
    CHECK(sink.creates.size() == 2 && sink.store.size() == 2, "the store was asked at once and holds both guilds");
    CHECK(sink.saves.empty(), "the store has the guild already: no create is queued after it");
    // a later change is a save over the version the store made (1)
    if (!w.enter(1, party_record("dee", {0x3a42, 0, 0, 2}), identity())) {
        CHECK(false, "a second player joins");
        return;
    }
    CHECK(test_guild_invite(w, 0, "dee"), "the leader invites dee");
    w.settle();
    CHECK(test_guild_accept(w, 1), "dee accepts");
    w.settle();
    CHECK(!sink.saves.empty() && sink.saves.back().version == 1 && sink.reasons.size() == 1, "the join is saved over version 1 and the store refused only the one create of the taken name");
}

static void test_guildmates_may_be_helped_and_not_harmed() {
    std::printf("one guild is one tier of the PvP rules: a guildmate may be helped, not harmed, with or without the force flag\n");
    namespace pvp = combat::pvp;
    pvp::Side a, b;
    a.actor = 1;
    b.actor = 2;
    a.flag = pvp::Flag::Combatant;
    b.flag = pvp::Flag::Combatant;
    const pvp::Pair none{};
    CHECK(pvp::deciding_tier(a, b, none) == pvp::Tier::Players, "two players with no guild stand as players");
    a.guild = {1, 2, 3, 4};
    b.guild = {1, 2, 3, 4};
    CHECK(pvp::deciding_tier(a, b, none) == pvp::Tier::Guild, "two members of one guild are one tier");
    CHECK((pvp::permissions(a, b, none, true) & pvp::kHarm) == 0 && (pvp::permissions(a, b, none, true) & pvp::kHelp) != 0, "...they may help each other and not harm each other, even with the force flag");
    b.guild = {1, 2, 3, 5};
    CHECK(pvp::deciding_tier(a, b, none) == pvp::Tier::Players, "members of two guilds stand as players");
    b.guild = {};
    CHECK(pvp::deciding_tier(a, b, none) == pvp::Tier::Players, "a member and a player in no guild stand as players");
    a.guild = {};
    b.guild = {};
    CHECK(pvp::deciding_tier(a, b, none) == pvp::Tier::Players, "and two players in no guild (the zero guild is no guild)");
}

static void test_a_logout_shows_the_member_offline_to_the_others() {
    std::printf("a member who logs out shows offline in the roster of the others, with a last-online time\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(34);
    svc::Character cyb = party_record("cyb", {0x3a51, 0, 0, 1});
    svc::Character dee = party_record("dee", {0x3a52, 0, 0, 2});
    cyb = guild_record("cyb", {0x3a51, 0, 0, 1}, id, "Review Zeta", 0, {{&cyb, 0}, {&dee, 2}});
    dee = guild_record("dee", {0x3a52, 0, 0, 2}, id, "Review Zeta", 2, {{&cyb, 0}, {&dee, 2}});
    seed_store(sink, cyb);
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity())) {
        CHECK(false, "the two players join");
        return;
    }
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 2 && data->members[1].online, "the leader's roster has dee online");
    w[1].s->finish("the guild test's logout");
    w.settle();
    data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 2 && !data->members[1].online && data->members[1].last_online > 0, "after dee's logout the leader's roster has dee offline, with a last-online time");
    CHECK(sink.last(id) && sink.last(id)->members.size() == 2, "and the guild was saved with its two members");
}

static void test_the_last_place_goes_to_the_first_to_accept() {
    std::printf("two invitations for the last place: the first accept fills the guild and the second is told it is full\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(35);
    svc::Character cyb = party_record("cyb", {0x3a61, 0, 0, 1});
    std::vector<svc::Character> others(48);   // 48 more rows, all offline: the guild holds 49 of its 50 places
    std::vector<std::pair<const svc::Character*, int>> members = {{&cyb, 0}};
    for (size_t i = 0; i < others.size(); ++i) {
        others[i] = party_record(("Filler" + std::to_string(i)).c_str(), {0x3a60, 0, static_cast<uint32_t>(i), 0xfe});
        members.emplace_back(&others[i], 2);
    }
    cyb = guild_record("cyb", {0x3a61, 0, 0, 1}, id, "Review Eta", 0, members);
    seed_store(sink, cyb);
    if (!w.enter(0, cyb, identity()) || !w.enter(1, party_record("dee", {0x3a62, 0, 0, 2}), identity()) || !w.enter(2, party_record("ayla", {0x3a63, 0, 0, 3}), identity())) {
        CHECK(false, "the players join");
        return;
    }
    CHECK(test_guild_invite(w, 0, "dee") && test_guild_invite(w, 0, "ayla"), "the leader invites two players for the 50th place");
    w.settle();
    CHECK(!w[1].s->guild_pending_inviter().empty() && !w[2].s->guild_pending_inviter().empty(), "both hold an invitation");
    size_t from = w[2].got.size();
    CHECK(test_guild_accept(w, 1), "dee accepts first");
    w.settle();
    CHECK(w[1].s->guild_name() == "Review Eta", "dee has the 50th place");
    CHECK(test_guild_accept(w, 2), "ayla accepts after it");
    w.settle();
    const std::optional<TestGuildReply> full = test_guild_reply(test_guild_replies(w, 2, from), "ClientGuildRequestFailed");
    CHECK(full && test_guild_reply_text(*full, "Message") == "That guild is full." && w[2].s->guild_name().empty(), "ayla is refused: the guild is full, and she is in no guild");
    const repl::GuildData* data = test_guild_data(*w[0].s);
    CHECK(data && data->members.size() == 50, "the roster holds 50 members");
}

static void test_the_leader_kicks_the_only_officer() {
    std::printf("a leader kicks the guild's only officer: the officer is out with the cooldown, the guild is saved without it\n");
    std::fflush(stdout);
    TestGuildStore sink;
    PartyWorld w;
    w.fx.world->set_position_sink(&sink);
    const std::string id = test_guild_id(36);
    svc::Character cyb = party_record("cyb", {0x3a71, 0, 0, 1});
    svc::Character dee = party_record("dee", {0x3a72, 0, 0, 2});
    svc::Character ayla = party_record("ayla", {0x3a73, 0, 0, 3});
    cyb = guild_record("cyb", {0x3a71, 0, 0, 1}, id, "Review Theta", 0, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    dee = guild_record("dee", {0x3a72, 0, 0, 2}, id, "Review Theta", 1, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    ayla = guild_record("ayla", {0x3a73, 0, 0, 3}, id, "Review Theta", 2, {{&cyb, 0}, {&dee, 1}, {&ayla, 2}});
    seed_store(sink, cyb);
    if (!w.enter(0, cyb, identity()) || !w.enter(1, dee, identity()) || !w.enter(2, ayla, identity())) {
        CHECK(false, "the three players join");
        return;
    }
    size_t from = w[0].got.size();
    CHECK(test_guild_kick(w, 0, w.guid_of(1)), "the leader kicks the officer");
    w.settle();
    CHECK(test_guild_reply(test_guild_replies(w, 0, from), "ClientGuildRequestSucceeded").has_value(), "the kick is answered with success");
    CHECK(w[1].s->guild_name().empty() && test_guild_text(*w[1].s, "GuildName").empty() && test_guild_instance(*w[1].s) == nullptr, "the officer is in no guild, has no guild name and no GuildMember alignment");
    CHECK(sink.cooldown_of(test_guild_character(w, 1)).has_value(), "its join cooldown is saved");
    const repl::GuildData* data = test_guild_data(*w[2].s);
    CHECK(data && data->members.size() == 2 && data->members[0].name == "cyb" && data->members[1].name == "ayla", "the member's roster has the two that remain");
    CHECK(sink.last(id) && sink.last(id)->members.size() == 2, "the guild is saved without the officer");
    CHECK(w[0].s->guild_rank() == 0, "the leader is still the leader");
    CHECK(sink.reasons.empty(), "and the store refused no save");
}

AOC_REGISTER_TEST(test_the_join_holds_no_guild_component_and_the_component_follows_it);
AOC_REGISTER_TEST(test_a_members_login_sends_its_guild_with_the_component_and_not_in_the_join);
AOC_REGISTER_TEST(test_an_invitee_who_logs_out_frees_the_invitation_and_a_guild_has_many_out);
AOC_REGISTER_TEST(test_the_guild_is_stored_the_way_the_service_takes_it);
AOC_REGISTER_TEST(test_a_name_a_stored_guild_has_is_refused_before_the_guild_is_made);
AOC_REGISTER_TEST(test_guildmates_may_be_helped_and_not_harmed);
AOC_REGISTER_TEST(test_a_logout_shows_the_member_offline_to_the_others);
AOC_REGISTER_TEST(test_the_last_place_goes_to_the_first_to_accept);
AOC_REGISTER_TEST(test_the_leader_kicks_the_only_officer);
