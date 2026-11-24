// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_raid_tests.inc
//
//  The raids (lane 13 milestone 8): a party becomes a raid of 2 to 40 in up to 10 squads of 8; the 9th invitation
//  asks the leader (ClientRequestConvertToRaid) and his yes (ServerAcceptConvertToRaid) converts and invites; the
//  convert button; the move, the squad leaders, the squad member shouts and the raid shout privilege; the invitations
//  at once; leave, kick, leadership, logout and disband; the frame is ClientUpdateGroup with GroupType 1 and the
//  squads.  The helpers live in session_party_tests.inc (the party's world, its invites and accepts, the frame
//  reader); this file reuses them and adds the raid calls and the multi-squad frame reader.
// ============================================================================

namespace {

/// One squad of a raid as ClientUpdateGroup carries it.
struct RaidSquad {
    int64_t                          number = 0;
    std::vector<PartyMemberFrame>    members;
    std::vector<std::array<uint32_t, 4>> leaders;
    bool                             member_shouts = false;
};

/// One raid frame: the whole group and its squads.
struct RaidFrame {
    bool                          present = false;
    size_t                        param_bits = 0;
    std::array<uint32_t, 4>       group_guid{}, leader_guid{}, master_looter_guid{};
    uint64_t                      group_type = 0;
    uint64_t                      shout_privilege = 0;
    bool                          override_loot = false;
    uint64_t                      raid_default = 0, raid_common = 0, raid_uncommon = 0;
    uint64_t                      party_default = 0, party_common = 0, party_uncommon = 0;
    bool                          party_member_shouts = false;
    std::vector<RaidSquad>        squads;
};

/// One ClientRequestConvertToRaid as the leader's client reads it.
struct RaidAsk {
    bool        present = false;
    std::string invited;
};

/// One member of a squad element, as the client reads it (the party frame's own reader's fields).
static PartyMemberFrame raid_member_of(const repl::ElementState& e) {
    PartyMemberFrame m;
    if (const auto* s = repl::rpcf::as<std::string>(repl::rpcf::element_value(e, 1))) m.name = *s;
    m.guid = party_element_words(e, 2);
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::element_value(e, 6), 0)) m.primary_class = *v;
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::element_value(e, 7), false)) m.online = *v;
    if (const auto* v = repl::rpcf::element_value(e, 8)) m.health_percent = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
    if (const auto* v = repl::rpcf::element_value(e, 9)) m.max_health = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
    if (const auto* v = repl::rpcf::element_value(e, 10)) m.mana_percent = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
    if (const auto* v = repl::rpcf::element_value(e, 11)) m.max_mana = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
    if (const auto v = repl::rpcf::int_or(repl::rpcf::element_value(e, 12), 0)) m.level = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::element_value(e, 13), 0)) m.alive_status = *v;
    if (const auto* v = repl::rpcf::element_value(e, 14)) if (const auto* q = v->get_if<repl::QuantVec>()) m.location = *q;
    if (const auto* v = repl::rpcf::element_value(e, 15)) if (const auto* q = v->get_if<repl::QuantVec>()) m.marker = *q;
    if (const auto* v = repl::rpcf::element_value(e, 16)) m.heading = v->get_if<float>() ? *v->get_if<float>() : 0.0f;
    return m;
}

/// One ClientUpdateGroup of a delivered chain, with every squad.
static std::optional<RaidFrame> raid_frame_of(const fake::Delivered& d, const schema::ClassSchema& pc_cls) {
    const auto call = party_single_rpc(d, pc_cls, "ClientUpdateGroup");
    if (!call) return std::nullopt;
    RaidFrame frame;
    frame.present = true;
    frame.param_bits = call->param_bits;
    frame.group_guid = party_words(*call, "NewGroup.GroupGuid");
    frame.leader_guid = party_words(*call, "NewGroup.LeaderGuid");
    frame.master_looter_guid = party_words(*call, "NewGroup.MasterLooterGuid");
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.GroupType"), 0)) frame.group_type = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.RaidSettings.ShoutPrivilege"), 0)) frame.shout_privilege = *v;
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.RaidSettings.bOverridePartyLootSettings"), false)) frame.override_loot = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.RaidSettings.LootAllocationMethods.DefaultLootAllocationMethod"), 0)) frame.raid_default = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.RaidSettings.LootAllocationMethods.CommonLootAllocationMethod"), 0)) frame.raid_common = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.RaidSettings.LootAllocationMethods.UncommonLootAllocationMethod"), 0)) frame.raid_uncommon = *v;
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.bEnableMemberShouts"), false)) frame.party_member_shouts = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.LootAllocationMethods.DefaultLootAllocationMethod"), 0)) frame.party_default = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.LootAllocationMethods.CommonLootAllocationMethod"), 0)) frame.party_common = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.LootAllocationMethods.UncommonLootAllocationMethod"), 0)) frame.party_uncommon = *v;
    const repl::Value* squads = repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.Squads");
    const auto* list = squads ? squads->get_if<repl::ArrayValue>() : nullptr;
    if (!list) return frame;
    for (const repl::ElementState& e : list->elements) {
        RaidSquad squad;
        if (const auto v = repl::rpcf::int_or(repl::rpcf::element_value(e, 1), 0)) squad.number = *v;
        const repl::Value* members = repl::rpcf::element_value(e, 2);
        const auto* member_list = members ? members->get_if<repl::ArrayValue>() : nullptr;
        if (member_list) {
            for (const repl::ElementState& m : member_list->elements) squad.members.push_back(raid_member_of(m));
        }
        if (const repl::Value* leaders = repl::rpcf::element_value(e, 3)) {
            if (const auto* leader_list = leaders->get_if<repl::ArrayValue>()) {
                for (const repl::ElementState& l : leader_list->elements) squad.leaders.push_back(party_element_words(l, 1));
            }
        }
        if (const auto v = repl::rpcf::bool_or(repl::rpcf::element_value(e, 4), false)) squad.member_shouts = *v;
        frame.squads.push_back(std::move(squad));
    }
    return frame;
}

/// One ClientRequestConvertToRaid of a delivered chain.
static std::optional<RaidAsk> raid_ask_of(const fake::Delivered& d, const schema::ClassSchema& pc_cls) {
    const auto call = party_single_rpc(d, pc_cls, "ClientRequestConvertToRaid");
    if (!call) return std::nullopt;
    RaidAsk ask;
    ask.present = true;
    if (const auto* s = repl::rpcf::as<std::string>(repl::rpcf::value_of(*call->rpc, call->params, "InvitedPlayer"))) ask.invited = *s;
    return ask;
}

/// The raid frames a player was given from chain `from` on.
static std::vector<RaidFrame> raid_frames_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<RaidFrame> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto frame = raid_frame_of(got[i], pc_cls)) out.push_back(std::move(*frame));
    }
    return out;
}

/// The asks a player was given from chain `from` on.
static std::vector<RaidAsk> raid_asks_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<RaidAsk> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto ask = raid_ask_of(got[i], pc_cls)) out.push_back(std::move(*ask));
    }
    return out;
}

/// The last raid frame a player was given, or a frame that is not there.
static RaidFrame last_raid_frame(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls) {
    for (size_t i = got.size(); i > 0; --i) {
        if (auto frame = raid_frame_of(got[i - 1], pc_cls)) return *frame;
    }
    return RaidFrame{};
}

/// The squad whose member names include `name`, or nullptr.
static const RaidSquad* raid_squad_of(const RaidFrame& frame, const std::string& name) {
    for (const RaidSquad& squad : frame.squads) {
        for (const PartyMemberFrame& m : squad.members) {
            if (m.name == name) return &squad;
        }
    }
    return nullptr;
}

/// True when the squad lists `names` in this order.
static bool raid_squad_names(const RaidSquad& squad, const std::vector<std::string>& names, std::string* why) {
    if (squad.members.size() != names.size()) {
        *why = "the squad has " + std::to_string(squad.members.size()) + " member(s), not " + std::to_string(names.size());
        return false;
    }
    for (size_t i = 0; i < names.size(); ++i) {
        if (squad.members[i].name != names[i]) {
            *why = "member " + std::to_string(i + 1) + " is " + squad.members[i].name + ", not " + names[i];
            return false;
        }
    }
    return true;
}

/// A client call on the PlayerController (the party file's own sender, named for this file).
static bool raid_send(PartyWorld& w, size_t who, const char* name, const repl::RpcParams& params) { return party_send(w, who, name, params); }

static bool raid_convert(PartyWorld& w, size_t who) { return raid_send(w, who, "ServerConvertPartyToRaid", repl::RpcParams{}); }
static bool raid_convert_back(PartyWorld& w, size_t who) { return raid_send(w, who, "ServerConvertRaidToParty", repl::RpcParams{}); }

static bool raid_accept_convert(PartyWorld& w, size_t who, const std::string& invited) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerAcceptConvertToRaid");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "InvitedPlayer", repl::Value::str(invited))) return false;
    return raid_send(w, who, "ServerAcceptConvertToRaid", params);
}

static bool raid_move(PartyWorld& w, size_t who, const std::string& name, int64_t squad) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerMoveRaidMember");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterName", repl::Value::str(name)) ||
        !repl::set_rpc_param(*rpc, params, "NewSquadNum", repl::Value::i64(squad))) return false;
    return raid_send(w, who, "ServerMoveRaidMember", params);
}

static bool raid_set_squad_leader(PartyWorld& w, size_t who, const std::string& name) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerSetSquadLeader");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterNameToPromote", repl::Value::str(name))) return false;
    return raid_send(w, who, "ServerSetSquadLeader", params);
}

static bool raid_demote_squad_leader(PartyWorld& w, size_t who, const std::string& name) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerDemoteSquadLeader");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterNameToDemote", repl::Value::str(name))) return false;
    return raid_send(w, who, "ServerDemoteSquadLeader", params);
}

static bool raid_set_squad_shouts(PartyWorld& w, size_t who, bool on) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerSetSquadEnableMemberShouts");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "bEnableMemberShouts", repl::Value::boolean(on))) return false;
    return raid_send(w, who, "ServerSetSquadEnableMemberShouts", params);
}

static bool raid_set_shout_privilege(PartyWorld& w, size_t who, uint64_t value) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerSetRaidShoutPrivilege");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "ShoutPrivilege", repl::Value::u64(value))) return false;
    return raid_send(w, who, "ServerSetRaidShoutPrivilege", params);
}

/// `count` players (p1..pN) of a party: p1 invites each and each accepts.
static bool form_party(PartyWorld& w, size_t count, const schema::ClassSchema& pc_cls) {
    for (size_t i = 1; i < count; ++i) {
        const size_t before = w[i].got.size();
        if (!party_invite(w, 0, w[i].s->name())) return false;
        w.settle();
        const auto requests = party_requests_in(w[i].got, pc_cls, before);
        if (requests.size() != 1) return false;
        if (!party_accept(w, i, requests[0].group_guid, requests[0].inviter)) return false;
        w.settle();
    }
    return true;
}

/// A raid of `count` players: a party of 8, then the 9th invitation's ask and yes, then ordinary invitations.
static bool form_raid(PartyWorld& w, size_t count, const schema::ClassSchema& pc_cls) {
    if (count < 2) return false;
    if (count <= 8) {
        if (!form_party(w, count, pc_cls)) return false;
        if (!raid_convert(w, 0)) return false;
        w.settle();
        return last_raid_frame(w[0].got, pc_cls).group_type == 1;
    }
    if (!form_party(w, 8, pc_cls)) return false;
    const std::string ninth = w[8].s->name();
    const size_t ask_from = w[0].got.size();
    const size_t join_from = w[8].got.size();
    if (!party_invite(w, 0, ninth)) return false;
    w.settle();
    if (raid_asks_in(w[0].got, pc_cls, ask_from).size() != 1) return false;
    if (!raid_accept_convert(w, 0, ninth)) return false;
    w.settle();
    const auto join = party_requests_in(w[8].got, pc_cls, join_from);
    if (join.size() != 1) return false;
    const std::array<uint32_t, 4> group = join[0].group_guid;
    if (!party_accept(w, 8, group, join[0].inviter)) return false;
    w.settle();
    for (size_t i = 9; i < count; ++i) {
        const size_t before = w[i].got.size();
        if (!party_invite(w, 0, w[i].s->name())) return false;
        w.settle();
        const auto requests = party_requests_in(w[i].got, pc_cls, before);
        if (requests.size() != 1) return false;
        if (!party_accept(w, i, group, requests[0].inviter)) return false;
        w.settle();
    }
    return last_raid_frame(w[0].got, pc_cls).squads.size() >= 1;
}

}  // namespace

// -------------------------------------------------------------------------------------------------------------------------------
//  the convert button (rule 1)
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_convert_button_leader_only() {
    std::printf("the convert button is the leader's; a member's and a player in no group's call get one line and no frame\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 4), "four players enter the world");
    if (!w[0].s || !w[3].s) return;
    CHECK(form_party(w, 3, pc_cls), "p1, p2 and p3 form a party");
    const size_t before_member = w[1].got.size();
    const size_t before_third = w[2].got.size();
    const size_t before_groupless = w[3].got.size();
    CHECK(raid_convert(w, 1), "p2, a member, calls the convert button");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[1].got, pc_cls, before_member, "Only the group leader may convert the party to a raid.", &why),
          ("p2 reads the leader-only line: " + why).c_str());
    CHECK(raid_frames_in(w[0].got, pc_cls, before_member).empty(), "no frame moves for the member's call");
    CHECK(raid_convert(w, 3), "p4, in no group, calls the convert button");
    w.settle();
    why.clear();
    CHECK(one_system_line(w[3].got, pc_cls, before_groupless, "Only the group leader may convert the party to a raid.", &why),
          ("p4 reads the same line: " + why).c_str());
    const size_t before_leader = w[0].got.size();
    const std::array<uint32_t, 4> party_guid = last_party_frame(w[0].got, pc_cls).group_guid;
    CHECK(raid_convert(w, 0), "p1, the leader, calls the convert button");
    w.settle();
    const auto frames = raid_frames_in(w[0].got, pc_cls, before_leader);
    CHECK(frames.size() == 1, "the leader gets one ClientUpdateGroup for the conversion");
    if (frames.size() == 1) {
        const RaidFrame& frame = frames[0];
        CHECK(frame.group_type == 1, "the group type is 1 (a raid)");
        CHECK(frame.group_guid == party_guid && party_guid != kNoPartyGuid, "the group GUID is the party's");
        CHECK(frame.leader_guid == w.guid_of(0) && frame.master_looter_guid == w.guid_of(0), "the leader and the master looter stay p1");
        CHECK(frame.squads.size() == 1 && frame.squads[0].number == 1, "one squad, number 1");
        if (!frame.squads.empty()) {
            std::string names_why;
            CHECK(raid_squad_names(frame.squads[0], {"p1", "p2", "p3"}, &names_why), ("squad 1 lists the three: " + names_why).c_str());
            CHECK(frame.squads[0].leaders.size() == 1 && frame.squads[0].leaders[0] == w.guid_of(0), "squad 1's leader is p1");
            CHECK(frame.squads[0].member_shouts, "squad 1's member shouts are on");
        }
        CHECK(frame.shout_privilege == 2 && !frame.override_loot && frame.raid_default == 0 && frame.raid_common == 0 && frame.raid_uncommon == 2,
              "the raid's settings are the defaults (shout privilege 2, the loot methods 0 and 2)");
        CHECK(frame.party_member_shouts && frame.party_default == 0 && frame.party_common == 0 && frame.party_uncommon == 2, "the party settings stay the defaults");
    }
    CHECK(raid_frames_in(w[1].got, pc_cls, before_member).size() == 1, "p2 gets the same frame");
    CHECK(raid_frames_in(w[2].got, pc_cls, before_third).size() == 1, "p3 too");
}

AOC_REGISTER_TEST(test_raid_convert_button_leader_only);

static void test_raid_convert_button_needs_two() {
    std::printf("a one-member party cannot become a raid: one line and no frame (the minimum is 2)\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before_invite = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2 and stands alone while the invitation waits");
    w.settle();
    CHECK(party_requests_in(w[1].got, pc_cls, before_invite).size() == 1, "(p2 is invited)");
    const size_t before = w[0].got.size();
    CHECK(raid_convert(w, 0), "the lone leader calls the convert button");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before, "The raid needs at least 2 members.", &why), ("the leader reads the minimum line: " + why).c_str());
    CHECK(raid_frames_in(w[0].got, pc_cls, before).empty(), "no raid frame goes out");
    CHECK(last_party_frame(w[0].got, pc_cls).group_type == 0 && last_party_frame(w[0].got, pc_cls).members.size() == 1,
          "the group stays a one-member party");
}

AOC_REGISTER_TEST(test_raid_convert_button_needs_two);

// -------------------------------------------------------------------------------------------------------------------------------
//  the 9th invitation: the ask and the yes (rules 2 and 8)
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_ninth_invite_asks_first() {
    std::printf("the leader's 9th invitation asks him to convert (ClientRequestConvertToRaid) and sends nothing else; no answer keeps the party\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 9), "nine players enter the world");
    if (!w[0].s || !w[8].s) return;
    CHECK(form_party(w, 8, pc_cls), "the first eight form a party");
    const size_t before_leader = w[0].got.size();
    const size_t before_ninth = w[8].got.size();
    CHECK(party_invite(w, 0, "p9"), "p1, the leader, invites the ninth player");
    w.settle();
    const auto asks = raid_asks_in(w[0].got, pc_cls, before_leader);
    CHECK(asks.size() == 1 && asks[0].invited == "p9", "the leader gets exactly one ask naming p9");
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "p9 gets no join request");
    CHECK(raid_frames_in(w[0].got, pc_cls, before_leader).empty(), "no group frame goes out for the ask");
    w.t += kPartyInviteLifetime + std::chrono::seconds(1);
    w.settle();
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "five minutes later p9 still has no join request");
    CHECK(raid_asks_in(w[0].got, pc_cls, before_leader).size() == 1, "the ask did not time out or repeat");
    CHECK(last_party_frame(w[0].got, pc_cls).members.size() == 8 && last_party_frame(w[0].got, pc_cls).group_type == 0, "the party is still the eight");
}

AOC_REGISTER_TEST(test_raid_ninth_invite_asks_first);

static void test_raid_ninth_invite_only_leader() {
    std::printf("a non-leader's 9th invitation is the party's full line and no ask; the leader's own gets the ask\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 9), "nine players enter the world");
    if (!w[0].s || !w[8].s) return;
    CHECK(form_party(w, 8, pc_cls), "the first eight form a party");
    const size_t before_member = w[1].got.size();
    const size_t before_leader = w[0].got.size();
    const size_t before_ninth = w[8].got.size();
    CHECK(party_invite(w, 1, "p9"), "p2, a member and not the leader, invites p9");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[1].got, pc_cls, before_member, "p9 cannot join: the party is full.", &why), ("the member reads the full line: " + why).c_str());
    CHECK(raid_asks_in(w[0].got, pc_cls, before_leader).empty(), "the leader is asked nothing");
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "p9 gets no join request");
    CHECK(party_invite(w, 0, "p9"), "p1, the leader, invites p9");
    w.settle();
    const auto asks = raid_asks_in(w[0].got, pc_cls, before_leader);
    CHECK(asks.size() == 1 && asks[0].invited == "p9", "the leader's own invitation gets the ask");
}

AOC_REGISTER_TEST(test_raid_ninth_invite_only_leader);

static void test_raid_accept_convert_invites() {
    std::printf("the leader's yes converts the party to a raid and invites the ninth; the ninth joins squad 2\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 9), "nine players enter the world");
    if (!w[0].s || !w[8].s) return;
    CHECK(form_party(w, 8, pc_cls), "the first eight form a party");
    const std::array<uint32_t, 4> party_guid = last_party_frame(w[0].got, pc_cls).group_guid;
    const size_t ask_from = w[0].got.size();
    const size_t join_from = w[8].got.size();
    CHECK(party_invite(w, 0, "p9"), "p1 invites p9");
    w.settle();
    CHECK(raid_asks_in(w[0].got, pc_cls, ask_from).size() == 1, "(the ask goes out)");
    const size_t before_eight = w[0].got.size();
    CHECK(raid_accept_convert(w, 0, "p9"), "p1 presses yes");
    w.settle();
    const auto eight = raid_frames_in(w[0].got, pc_cls, before_eight);
    CHECK(eight.size() == 1, "the eight members get one raid frame");
    if (eight.size() == 1) {
        CHECK(eight[0].group_type == 1 && eight[0].group_guid == party_guid, "the frame is the raid with the party's GUID");
        CHECK(eight[0].squads.size() == 1 && eight[0].squads[0].members.size() == 8, "one squad of eight");
        CHECK(!eight[0].squads.empty() && eight[0].squads[0].leaders.size() == 1 && eight[0].squads[0].leaders[0] == w.guid_of(0),
              "squad 1's leader is p1");
    }
    const auto requests = party_requests_in(w[8].got, pc_cls, join_from);
    CHECK(requests.size() == 1 && requests[0].group_type == 1 && requests[0].group_guid == party_guid,
          "p9 gets exactly one join request, a raid with the party's GUID");
    if (requests.size() != 1) return;
    const size_t before_join = w[0].got.size();
    CHECK(party_accept(w, 8, requests[0].group_guid, requests[0].inviter), "p9 accepts");
    w.settle();
    const RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.group_type == 1 && frame.group_guid == party_guid, "the raid keeps its type and GUID");
    CHECK(frame.squads.size() == 2, "the frame now has two squads");
    if (frame.squads.size() == 2) {
        CHECK(frame.squads[0].number == 1 && frame.squads[0].members.size() == 8, "squad 1 keeps the eight");
        CHECK(frame.squads[1].number == 2 && frame.squads[1].members.size() == 1 && frame.squads[1].members[0].name == "p9",
              "p9 joins squad 2, the lowest with room");
    }
    CHECK(last_party_frame(w[0].got, pc_cls).present == false || last_party_frame(w[0].got, pc_cls).group_type == 1,
          "(no party frame after the converstion)");
    CHECK(w[0].got.size() > before_join, "the frame reached the leader");
}

AOC_REGISTER_TEST(test_raid_accept_convert_invites);

static void test_raid_accept_convert_offline_still_converts() {
    std::printf("the yes converts even when the invitation then fails: an offline ninth gets the not-online line and the group is a raid\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 9), "nine players enter the world");
    if (!w[0].s || !w[8].s) return;
    CHECK(form_party(w, 8, pc_cls), "the first eight form a party");
    CHECK(party_invite(w, 0, "p9"), "p1 invites p9");
    w.settle();
    w[8].s->finish("the invited player logged out");
    w.settle();
    const size_t before_yes = w[0].got.size();
    const size_t before_ninth = w[8].got.size();
    CHECK(raid_accept_convert(w, 0, "p9"), "p1 presses yes after p9 left");
    w.settle();
    const auto frames = raid_frames_in(w[0].got, pc_cls, before_yes);
    CHECK(frames.size() == 1 && frames[0].group_type == 1, "the party became a raid anyway");
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before_yes, "p9 is not online.", &why), ("the leader reads the not-online line: " + why).c_str());
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "no join request goes to the gone player");
    CHECK(last_raid_frame(w[0].got, pc_cls).squads.size() == 1 && last_raid_frame(w[0].got, pc_cls).squads[0].members.size() == 8,
          "the raid is the eight, one squad");
}

AOC_REGISTER_TEST(test_raid_accept_convert_offline_still_converts);

static void test_raid_accept_convert_nonleader_ignored() {
    std::printf("a non-leader's yes changes nothing; the leader's yes then converts and invites\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 9), "nine players enter the world");
    if (!w[0].s || !w[8].s) return;
    CHECK(form_party(w, 8, pc_cls), "the first eight form a party");
    CHECK(party_invite(w, 0, "p9"), "p1 invites p9");
    w.settle();
    const size_t before_member = w[0].got.size();
    const size_t before_ninth = w[8].got.size();
    CHECK(raid_accept_convert(w, 1, "p9"), "p2, a member, presses yes");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_member).empty(), "a non-leader's yes sends no frame");
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "and no join request");
    const size_t before_leader = w[0].got.size();
    CHECK(raid_accept_convert(w, 0, "p9"), "p1, the leader, presses yes");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_leader).size() == 1, "the leader's yes sends the raid frame");
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).size() == 1, "and the join request");
}

AOC_REGISTER_TEST(test_raid_accept_convert_nonleader_ignored);

static void test_raid_accept_convert_one_member_party() {
    std::printf("the yes on a one-member party: the minimum line, then the invitation as an ordinary party invitation\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    const size_t before_invite = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2 and stands alone");
    w.settle();
    CHECK(party_requests_in(w[1].got, pc_cls, before_invite).size() == 1, "(p2 is invited)");
    const size_t before_yes = w[0].got.size();
    const size_t before_third = w[2].got.size();
    CHECK(raid_accept_convert(w, 0, "p3"), "p1 presses yes for p3");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before_yes, "The raid needs at least 2 members.", &why), ("the leader reads the minimum line: " + why).c_str());
    const auto requests = party_requests_in(w[2].got, pc_cls, before_third);
    CHECK(requests.size() == 1 && requests[0].group_type == 0, "p3 gets an ordinary party invitation");
    CHECK(last_party_frame(w[0].got, pc_cls).group_type == 0 && last_party_frame(w[0].got, pc_cls).members.size() == 1,
          "the group is still the one-member party");
}

AOC_REGISTER_TEST(test_raid_accept_convert_one_member_party);

// -------------------------------------------------------------------------------------------------------------------------------
//  the convert back (rule 5)
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_convert_back_party() {
    std::printf("the convert back: the raid leader only, eight or fewer; the members become the one party; a party is refused with one line\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 10), "ten players enter the world");
    if (!w[0].s || !w[3].s) return;
    CHECK(form_raid(w, 3, pc_cls), "a raid of three");
    CHECK(raid_move(w, 0, "p3", 2) && (w.settle(), true), "p3 moves to squad 2");
    const size_t before_member = w[1].got.size();
    CHECK(raid_convert_back(w, 1), "p2, a member, calls the convert back");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[1].got, pc_cls, before_member, "Only the raid leader may convert the raid to a party.", &why),
          ("p2 reads the raid-leader line: " + why).c_str());
    const size_t before_leader = w[0].got.size();
    const std::array<uint32_t, 4> raid_guid = last_raid_frame(w[0].got, pc_cls).group_guid;
    CHECK(raid_convert_back(w, 0), "p1, the raid leader, calls the convert back");
    w.settle();
    const auto frames = raid_frames_in(w[0].got, pc_cls, before_leader);
    CHECK(frames.size() == 1, "the members get one frame");
    if (frames.size() == 1) {
        const RaidFrame& frame = frames[0];
        CHECK(frame.group_type == 0 && frame.group_guid == raid_guid, "the group is a party with the same GUID");
        CHECK(frame.squads.size() == 1 && frame.squads[0].number == 1 && frame.squads[0].members.size() == 3, "one squad of three");
        CHECK(frame.squads[0].leaders.size() == 1 && frame.squads[0].leaders[0] == w.guid_of(0), "the squad's leader is the group's leader p1");
        CHECK(frame.shout_privilege == 2, "the shout privilege is the party's 2");
    }
    const size_t before_again = w[0].got.size();
    CHECK(raid_convert_back(w, 0), "the party leader calls the convert back again");
    w.settle();
    why.clear();
    CHECK(one_system_line(w[0].got, pc_cls, before_again, "The group is not a raid.", &why), ("the leader reads the not-a-raid line: " + why).c_str());
}

AOC_REGISTER_TEST(test_raid_convert_back_party);

static void test_raid_convert_back_too_many() {
    std::printf("a raid of nine or more cannot become a party: one line and the raid stays\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 10), "ten players enter the world");
    if (!w[0].s || !w[9].s) return;
    CHECK(form_raid(w, 9, pc_cls), "a raid of nine");
    const size_t before = w[0].got.size();
    CHECK(raid_convert_back(w, 0), "the raid leader calls the convert back");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before, "The raid has more than 8 members: it cannot become a party.", &why),
          ("the leader reads the too-many line: " + why).c_str());
    CHECK(raid_frames_in(w[0].got, pc_cls, before).empty(), "no frame moves");
    CHECK(last_raid_frame(w[0].got, pc_cls).group_type == 1, "the group stays a raid");
}

AOC_REGISTER_TEST(test_raid_convert_back_too_many);

// -------------------------------------------------------------------------------------------------------------------------------
//  the squads: the move (rule 6), the leaders (rule 7), the shouts (rules 8 and 9)
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_move_member_basics() {
    std::printf("the move: the raid leader only, a squad 1 to 10 with room; a non-member, -1, 0, 11 and the same squad are one line each\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    const size_t before_move = w[0].got.size();
    CHECK(raid_move(w, 0, "p3", 2), "p1 moves p3 to squad 2");
    w.settle();
    RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.squads.size() == 2, "two squads");
    const RaidSquad* one = raid_squad_of(frame, "p1");
    const RaidSquad* two = raid_squad_of(frame, "p3");
    CHECK(one && one->number == 1 && one->members.size() == 7, "squad 1 is down to seven");
    CHECK(two && two->number == 2 && two->members.size() == 3, "squad 2 has three");
    const size_t before_member = w[1].got.size();
    CHECK(raid_move(w, 1, "p4", 2), "p2, a member, moves p4");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[1].got, pc_cls, before_member, "Only the raid leader may move a member.", &why), ("p2 reads the leader-only line: " + why).c_str());
    const size_t before_missing = w[0].got.size();
    CHECK(raid_move(w, 0, "Nobody_xyz", 2), "the leader moves a name that is not in the raid");
    w.settle();
    why.clear();
    CHECK(one_system_line(w[0].got, pc_cls, before_missing, "Nobody_xyz is not in the raid.", &why), ("the leader reads the not-in-the-raid line: " + why).c_str());
    for (const int64_t bad : {int64_t{-1}, int64_t{0}, int64_t{11}}) {
        const size_t before = w[0].got.size();
        CHECK(raid_move(w, 0, "p4", bad), ("the leader moves p4 to squad " + std::to_string(bad)).c_str());
        w.settle();
        why.clear();
        const std::string line = "p4 cannot be moved: squad " + std::to_string(bad) + " is not a squad from 1 to 10.";
        CHECK(one_system_line(w[0].got, pc_cls, before, line, &why), ("the leader reads the not-a-squad line: " + why).c_str());
    }
    const size_t before_same = w[0].got.size();
    CHECK(raid_move(w, 0, "p3", 2), "the leader moves p3 to its own squad");
    w.settle();
    why.clear();
    CHECK(one_system_line(w[0].got, pc_cls, before_same, "p3 is already in squad 2.", &why), ("the leader reads the already-in line: " + why).c_str());
    CHECK(raid_frames_in(w[1].got, pc_cls, before_member).empty(), "the member's move sent no frame");
}

AOC_REGISTER_TEST(test_raid_move_member_basics);

static void test_raid_move_member_full_squad() {
    std::printf("a move to a full squad is one line; the squads stay as they are\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    for (int i = 3; i <= 8; ++i) {
        CHECK(raid_move(w, 0, "p" + std::to_string(i), 2), ("p1 moves p" + std::to_string(i) + " to squad 2").c_str());
        w.settle();
    }
    const RaidFrame before_frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* two = raid_squad_of(before_frame, "p3");
    CHECK(two && two->members.size() == 8, "squad 2 is full");
    const size_t before = w[0].got.size();
    CHECK(raid_move(w, 0, "p1", 2), "the leader moves p1 into the full squad");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before, "p1 cannot join squad 2: it has 8 members.", &why), ("the leader reads the full-squad line: " + why).c_str());
    CHECK(raid_frames_in(w[0].got, pc_cls, before).empty(), "no frame moves");
}

AOC_REGISTER_TEST(test_raid_move_member_full_squad);

static void test_raid_move_member_leader_mark() {
    std::printf("a squad leader moved out leaves its old squad without one\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    CHECK(raid_set_squad_leader(w, 0, "p2") && (w.settle(), true), "p1 makes p2 squad 1's leader");
    const size_t before = w[0].got.size();
    CHECK(raid_move(w, 0, "p2", 2), "p1 moves p2 to squad 2");
    w.settle();
    const RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* one = raid_squad_of(frame, "p1");
    const RaidSquad* two = raid_squad_of(frame, "p2");
    CHECK(one && one->leaders.empty(), "squad 1 has no leader after the move");
    CHECK(two && two->number == 2 && two->members.size() == 3, "p2 is in squad 2");
    CHECK(w[0].got.size() > before, "the frame reached the leader");
}

AOC_REGISTER_TEST(test_raid_move_member_leader_mark);

static void test_raid_squad_leaders() {
    std::printf("the squad leaders: the raid leader only, one per squad, a new one replaces the old; a demote clears it\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    CHECK(raid_set_squad_leader(w, 0, "p2") && (w.settle(), true), "p1 makes p2 squad 1's leader");
    CHECK(raid_set_squad_leader(w, 0, "p3") && (w.settle(), true), "p1 makes p3 squad 1's leader");
    RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* one = raid_squad_of(frame, "p3");
    CHECK(one && one->leaders.size() == 1 && one->leaders[0] == w.guid_of(2), "squad 1 lists only p3");
    CHECK(raid_set_squad_leader(w, 0, "p9") && (w.settle(), true), "p1 makes p9 squad 2's leader");
    frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* two = raid_squad_of(frame, "p9");
    CHECK(two && two->leaders.size() == 1 && two->leaders[0] == w.guid_of(8), "squad 2 lists p9");
    CHECK(raid_demote_squad_leader(w, 0, "p3") && (w.settle(), true), "p1 demotes p3");
    frame = last_raid_frame(w[0].got, pc_cls);
    one = raid_squad_of(frame, "p1");
    CHECK(one && one->leaders.empty(), "squad 1 has no leader again");
    const size_t before_leader = w[0].got.size();
    CHECK(raid_set_squad_leader(w, 1, "p4"), "p2, a member, sets a squad leader");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_leader).empty(), "a non-leader's set changes no frame");
    CHECK(raid_demote_squad_leader(w, 1, "p4"), "p2, a member, demotes");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_leader).empty(), "a non-leader's demote changes no frame");
}

AOC_REGISTER_TEST(test_raid_squad_leaders);

static void test_raid_squad_member_shouts() {
    std::printf("the squad member shouts: the caller's own squad, for its leader or the raid leader; the frame carries it per squad\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    CHECK(raid_set_squad_leader(w, 0, "p9") && (w.settle(), true), "p1 makes p9 squad 2's leader");
    const size_t before_leader = w[0].got.size();
    CHECK(raid_set_squad_shouts(w, 0, false), "p1, the raid leader in squad 1, sets its own squad's shouts off");
    w.settle();
    RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* one = raid_squad_of(frame, "p1");
    const RaidSquad* two = raid_squad_of(frame, "p9");
    CHECK(one && !one->member_shouts, "squad 1's shouts are off");
    CHECK(two && two->member_shouts, "squad 2's stay on");
    const size_t before_squad2 = w[8].got.size();
    CHECK(raid_set_squad_shouts(w, 8, false), "p9, squad 2's leader, sets squad 2's shouts off");
    w.settle();
    frame = last_raid_frame(w[0].got, pc_cls);
    two = raid_squad_of(frame, "p9");
    CHECK(two && !two->member_shouts, "squad 2's shouts are off");
    const size_t before_plain = w[3].got.size();
    CHECK(raid_set_squad_shouts(w, 2, true), "p3, a plain member, sets its squad's shouts on");
    w.settle();
    CHECK(raid_frames_in(w[8].got, pc_cls, before_squad2).size() == 1, "p9 got the squad 2 change");
    CHECK(raid_frames_in(w[3].got, pc_cls, before_plain).empty(), "a plain member's call changes nothing");
    CHECK(w[8].got.size() > before_squad2 && w[0].got.size() > before_leader, "(the frames reached the members)");
}

AOC_REGISTER_TEST(test_raid_squad_member_shouts);

static void test_raid_shout_privilege() {
    std::printf("the raid shout privilege: the raid leader only, kept and sent in RaidSettings as set\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    CHECK(raid_set_shout_privilege(w, 0, 1) && (w.settle(), true), "the raid leader sets 1");
    CHECK(last_raid_frame(w[0].got, pc_cls).shout_privilege == 1, "the frame carries 1");
    CHECK(raid_set_shout_privilege(w, 0, 3) && (w.settle(), true), "the raid leader sets 3");
    CHECK(last_raid_frame(w[0].got, pc_cls).shout_privilege == 3, "the frame carries 3");
    const size_t before_member = w[0].got.size();
    CHECK(raid_set_shout_privilege(w, 1, 0), "a member sets 0");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_member).empty(), "a member's call changes no frame");
    CHECK(last_raid_frame(w[0].got, pc_cls).shout_privilege == 3, "the value stays 3");
    PartyWorld p2;
    CHECK(party_enter(p2, 2), "two more players enter a second world");
    if (!p2[0].s || !p2[1].s) return;
    CHECK(party_pair(p2, 0, 1, pc_cls), "they form a party");
    const size_t before_party = p2[0].got.size();
    CHECK(raid_set_shout_privilege(p2, 0, 1), "the party leader sets a raid shout privilege");
    p2.settle();
    CHECK(raid_frames_in(p2[0].got, pc_cls, before_party).empty(), "a party's call changes nothing");
}

AOC_REGISTER_TEST(test_raid_shout_privilege);

// -------------------------------------------------------------------------------------------------------------------------------
//  the invitations: several at once; a full group at an accept
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_several_invitations() {
    std::printf("a leader invites three players at once: three join requests, each accepted, the party grows\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 4), "four players enter the world");
    if (!w[0].s || !w[3].s) return;
    const std::array<size_t, 3> targets = {1, 2, 3};
    std::vector<size_t> from;
    for (const size_t i : targets) {
        from.push_back(w[i].got.size());
        CHECK(party_invite(w, 0, w[i].s->name()), ("p1 invites " + w[i].s->name()).c_str());
        w.settle();
    }
    for (size_t k = 0; k < targets.size(); ++k) {
        const auto requests = party_requests_in(w[targets[k]].got, pc_cls, from[k]);
        CHECK(requests.size() == 1, ("p" + std::to_string(targets[k] + 1) + " has one join request").c_str());
    }
    for (size_t k = 0; k < targets.size(); ++k) {
        const auto requests = party_requests_in(w[targets[k]].got, pc_cls, from[k]);
        if (requests.size() != 1) return;
        CHECK(party_accept(w, targets[k], requests[0].group_guid, requests[0].inviter), ("p" + std::to_string(targets[k] + 1) + " accepts").c_str());
        w.settle();
    }
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2", "p3", "p4"}, w.guid_of(0), &why), ("the party holds all four: " + why).c_str());
}

AOC_REGISTER_TEST(test_raid_several_invitations);

static void test_raid_full_accept_refused_and_removed() {
    std::printf("an invitation that meets a full party at its accept is refused, removed and the leader is told; the leader can invite again, and that is the ask\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 10), "ten players enter the world");
    if (!w[0].s || !w[9].s) return;
    std::vector<size_t> from;
    for (size_t i = 1; i <= 9; ++i) {
        from.push_back(w[i].got.size());
        CHECK(party_invite(w, 0, w[i].s->name()), ("p1 invites p" + std::to_string(i + 1)).c_str());
        w.settle();
    }
    for (size_t i = 1; i <= 7; ++i) {
        const auto requests = party_requests_in(w[i].got, pc_cls, from[i - 1]);
        CHECK(requests.size() == 1 && party_accept(w, i, requests[0].group_guid, requests[0].inviter), ("p" + std::to_string(i + 1) + " accepts").c_str());
        w.settle();
    }
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8"}, w.guid_of(0), &why),
          ("the party is full: " + why).c_str());
    for (size_t i = 8; i <= 9; ++i) {
        const auto requests = party_requests_in(w[i].got, pc_cls, from[i - 1]);
        CHECK(requests.size() == 1, ("p" + std::to_string(i + 1) + " is still invited").c_str());
        if (requests.size() != 1) return;
        const size_t before_player = w[i].got.size();
        const size_t before_leader = w[0].got.size();
        CHECK(party_accept(w, i, requests[0].group_guid, requests[0].inviter), ("p" + std::to_string(i + 1) + " accepts into the full party").c_str());
        w.settle();
        why.clear();
        CHECK(one_system_line(w[i].got, pc_cls, before_player, "the party is full.", &why), ("the accepting player reads the full line: " + why).c_str());
        why.clear();
        CHECK(one_system_line(w[0].got, pc_cls, before_leader, "p" + std::to_string(i + 1) + " cannot join: the party is full.", &why),
              ("the leader is told: " + why).c_str());
        CHECK(party_requests_in(w[i].got, pc_cls, from[i - 1]).size() == 1, "no second invitation is made");
    }
    const size_t before_again = w[0].got.size();
    const size_t before_ninth = w[8].got.size();
    CHECK(party_invite(w, 0, "p9"), "p1 invites p9 again");
    w.settle();
    const auto asks = raid_asks_in(w[0].got, pc_cls, before_again);
    CHECK(asks.size() == 1 && asks[0].invited == "p9", "the new invitation is the 9th and asks to convert");
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "p9 gets no join request from the new invitation (the ask is for the leader)");
}

AOC_REGISTER_TEST(test_raid_full_accept_refused_and_removed);

static void test_raid_invite_expiry_one_of_several() {
    std::printf("one invitation expires while another stands: the group stays, the late accept is refused, the other still accepts\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    const size_t from_b = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto b = party_requests_in(w[1].got, pc_cls, from_b);
    CHECK(b.size() == 1, "p2's invitation arrives");
    w.t += std::chrono::minutes(4);
    const size_t from_c = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3 four minutes later");
    w.settle();
    const auto c = party_requests_in(w[2].got, pc_cls, from_c);
    CHECK(c.size() == 1, "p3's invitation arrives");
    if (b.size() != 1 || c.size() != 1) return;
    w.t += std::chrono::minutes(1) + std::chrono::seconds(1);
    w.settle();
    const size_t before_late = w[1].got.size();
    CHECK(party_accept(w, 1, b[0].group_guid, b[0].inviter), "p2 accepts after its five minutes");
    w.settle();
    CHECK(w[1].got.size() == before_late, "the late accept sends p2 nothing");
    const size_t before_accept = w[2].got.size();
    CHECK(party_accept(w, 2, c[0].group_guid, c[0].inviter), "p3 accepts while its invitation stands");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p3"}, w.guid_of(0), &why), ("p2's expiry left p3's invitation: " + why).c_str());
    CHECK(w[2].got.size() > before_accept, "p3's accept reached the group");
}

AOC_REGISTER_TEST(test_raid_invite_expiry_one_of_several);

// -------------------------------------------------------------------------------------------------------------------------------
//  the limits: 40 members, the 41st, the 5 squads of 8, the lowest squad with room, 10 squads
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_40_members_and_41st_refused() {
    std::printf("a raid of 40: five squads of eight; the 41st invitation is one line and no join request\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 41), "forty-one players enter the world");
    if (!w[0].s || !w[40].s) return;
    CHECK(form_raid(w, 40, pc_cls), "a raid of forty");
    const RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.group_type == 1 && frame.squads.size() == 5, "the frame has five squads");
    bool eight_each = frame.squads.size() == 5;
    for (const RaidSquad& squad : frame.squads) eight_each = eight_each && squad.members.size() == 8;
    CHECK(eight_each, "each squad holds eight");
    const size_t before = w[0].got.size();
    const size_t before_forty_first = w[40].got.size();
    CHECK(party_invite(w, 0, w[40].s->name()), "p1 invites the forty-first player");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before, w[40].s->name() + " cannot join: the raid is full.", &why), ("the leader reads the raid-full line: " + why).c_str());
    CHECK(party_requests_in(w[40].got, pc_cls, before_forty_first).empty(), "the forty-first gets no join request");
    CHECK(last_raid_frame(w[0].got, pc_cls).squads.size() == 5, "the raid is still the forty");
}

AOC_REGISTER_TEST(test_raid_40_members_and_41st_refused);

static void test_raid_40_accept_second_refused() {
    std::printf("a raid of 39 with two invitations: the second accept is refused with one line, its invitation is gone and the raid stays 40\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 41), "forty-one players enter the world");
    if (!w[0].s || !w[40].s) return;
    CHECK(form_raid(w, 39, pc_cls), "a raid of thirty-nine");
    const size_t from_forty = w[39].got.size();
    const size_t from_forty_first = w[40].got.size();
    CHECK(party_invite(w, 0, w[39].s->name()), "p1 invites p40");
    w.settle();
    CHECK(party_invite(w, 0, w[40].s->name()), "p1 invites p41 too");
    w.settle();
    const auto forty = party_requests_in(w[39].got, pc_cls, from_forty);
    const auto forty_first = party_requests_in(w[40].got, pc_cls, from_forty_first);
    CHECK(forty.size() == 1 && forty_first.size() == 1, "both invitations arrive");
    if (forty.size() != 1 || forty_first.size() != 1) return;
    CHECK(party_accept(w, 39, forty[0].group_guid, forty[0].inviter), "p40 accepts into the forty");
    w.settle();
    CHECK(last_raid_frame(w[0].got, pc_cls).squads.size() == 5, "(the raid is forty)");
    const size_t before_accept = w[40].got.size();
    const size_t before_leader = w[0].got.size();
    CHECK(party_accept(w, 40, forty_first[0].group_guid, forty_first[0].inviter), "p41 accepts into the full raid");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[40].got, pc_cls, before_accept, "the raid is full.", &why), ("p41 reads the raid-full line: " + why).c_str());
    why.clear();
    CHECK(one_system_line(w[0].got, pc_cls, before_leader, w[40].s->name() + " cannot join: the raid is full.", &why), ("the leader is told: " + why).c_str());
    CHECK(last_raid_frame(w[0].got, pc_cls).squads.size() == 5, "the raid stays forty");
}

AOC_REGISTER_TEST(test_raid_40_accept_second_refused);

static void test_raid_join_lowest_squad_with_room() {
    std::printf("a new member joins the lowest-numbered squad with room, not the last\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 12), "twelve players enter the world");
    if (!w[0].s || !w[11].s) return;
    CHECK(form_raid(w, 9, pc_cls), "a raid of nine: squad 1 full, squad 2 one");
    CHECK(raid_move(w, 0, "p2", 3) && (w.settle(), true), "p2 moves to squad 3: squad 1 has 7");
    const size_t from_ten = w[9].got.size();
    CHECK(party_invite(w, 0, w[9].s->name()), "p1 invites p10");
    w.settle();
    const auto ten = party_requests_in(w[9].got, pc_cls, from_ten);
    CHECK(ten.size() == 1 && party_accept(w, 9, ten[0].group_guid, ten[0].inviter), "p10 accepts");
    w.settle();
    RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* one = raid_squad_of(frame, "p10");
    CHECK(one && one->number == 1, "p10 joins squad 1 (the lowest with room)");
    CHECK(one && one->members.size() == 8, "squad 1 is full again");
    const size_t from_eleven = w[10].got.size();
    CHECK(party_invite(w, 0, w[10].s->name()), "p1 invites p11");
    w.settle();
    const auto eleven = party_requests_in(w[10].got, pc_cls, from_eleven);
    CHECK(eleven.size() == 1 && party_accept(w, 10, eleven[0].group_guid, eleven[0].inviter), "p11 accepts");
    w.settle();
    frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* two = raid_squad_of(frame, "p11");
    CHECK(two && two->number == 2 && two->members.size() == 2, "p11 joins squad 2, the next with room");
}

AOC_REGISTER_TEST(test_raid_join_lowest_squad_with_room);

static void test_raid_ten_squads_and_squad_cap() {
    std::printf("the raid has up to ten squads; a move to squad 11 is one line\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    for (int i = 2; i <= 10; ++i) {
        CHECK(raid_move(w, 0, "p" + std::to_string(i), i), ("p1 moves p" + std::to_string(i) + " to squad " + std::to_string(i)).c_str());
        w.settle();
    }
    const RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.squads.size() == 10, "the frame lists ten squads");
    bool numbers = frame.squads.size() == 10;
    for (size_t i = 0; i < frame.squads.size() && numbers; ++i) numbers = frame.squads[i].number == static_cast<int64_t>(i + 1) && frame.squads[i].members.size() == 1;
    CHECK(numbers, "the ten squads are 1 to 10, one member each");
    const size_t before = w[0].got.size();
    CHECK(raid_move(w, 0, "p2", 11), "the leader moves p2 to squad 11");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before, "p2 cannot be moved: squad 11 is not a squad from 1 to 10.", &why), ("the leader reads the line: " + why).c_str());
    CHECK(raid_frames_in(w[0].got, pc_cls, before).empty(), "no frame moves");
}

AOC_REGISTER_TEST(test_raid_ten_squads_and_squad_cap);

// -------------------------------------------------------------------------------------------------------------------------------
//  leave, kick, leadership, logout and disband (rule 10)
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_leave_kick_promote_logout() {
    std::printf("a raid's leave, kick, leadership and logout follow the party's: the earliest member is promoted, a squad leader's mark goes\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    CHECK(raid_set_squad_leader(w, 0, "p2") && (w.settle(), true), "p1 makes p2 squad 1's leader");
    const size_t before_leave = w[1].got.size();
    CHECK(party_leave(w, 0), "p1, the raid leader, leaves");
    w.settle();
    RaidFrame frame = last_raid_frame(w[1].got, pc_cls);
    CHECK(frame.leader_guid == w.guid_of(1), "p2, the earliest remaining, is the raid leader");
    CHECK(frame.group_type == 1 && frame.squads.size() == 2, "the raid keeps its squads");
    const RaidSquad* one = raid_squad_of(frame, "p2");
    CHECK(one && one->number == 1 && one->leaders.size() == 1 && one->leaders[0] == w.guid_of(1), "p2 is still squad 1's leader");
    CHECK(frame_is_empty(last_party_frame(w[0].got, pc_cls)), "p1's frame emptied");
    const size_t before_second = w[2].got.size();
    CHECK(party_leave(w, 1), "p2, the leader and squad 1's leader, leaves");
    w.settle();
    frame = last_raid_frame(w[2].got, pc_cls);
    CHECK(frame.leader_guid == w.guid_of(2), "p3 is the raid leader");
    one = raid_squad_of(frame, "p3");
    CHECK(one && one->leaders.empty(), "squad 1 has no leader after p2 left");
    const size_t before_kick = w[8].got.size();
    CHECK(party_kick(w, 2, "p9"), "p3, the raid leader, kicks p9, squad 2's leader-to-be");
    w.settle();
    CHECK(frame_is_empty(last_party_frame(w[8].got, pc_cls)), "p9's frame emptied");
    frame = last_raid_frame(w[2].got, pc_cls);
    const RaidSquad* two = raid_squad_of(frame, "p10");
    CHECK(two && two->number == 2, "p10 is still in squad 2");
    const size_t before_logout = w[2].got.size();
    const size_t before_others = w[3].got.size();
    w[2].s->finish("the raid leader logged out");
    w.settle();
    CHECK(raid_frames_in(w[3].got, pc_cls, before_others).size() >= 1, "the remaining members are told");
    CHECK(last_raid_frame(w[3].got, pc_cls).leader_guid == w.guid_of(3), "p4 is the raid leader");
    CHECK(w[2].got.size() == before_logout, "the logging-out player is sent nothing");
}

AOC_REGISTER_TEST(test_raid_leave_kick_promote_logout);

static void test_raid_drops_to_one_ends() {
    std::printf("a raid that drops to one member sends the one-member party state and then the empty group\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 5), "five players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(form_raid(w, 3, pc_cls), "a raid of three");
    const size_t before_first = w[0].got.size();
    CHECK(party_leave(w, 2), "p3 leaves");
    w.settle();
    const auto first = raid_frames_in(w[0].got, pc_cls, before_first);
    CHECK(first.size() == 1 && first[0].group_type == 1 && first[0].squads.size() == 1 && first[0].squads[0].members.size() == 2,
          "p1 and p2 keep the raid of two");
    const size_t before_second = w[0].got.size();
    CHECK(party_leave(w, 1), "p2 leaves");
    w.settle();
    const auto frames = raid_frames_in(w[0].got, pc_cls, before_second);
    CHECK(frames.size() == 2 && frames[0].squads.size() == 1 && frames[0].squads[0].members.size() == 1 && frames[0].group_type == 0,
          "p1 gets the one-member party state");
    CHECK(frame_is_empty(last_party_frame(w[0].got, pc_cls)), "and then the empty group");
}

AOC_REGISTER_TEST(test_raid_drops_to_one_ends);

static void test_raid_disband() {
    std::printf("a raid's disband: the leader only; every member's frame empties\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 5), "five players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(form_raid(w, 3, pc_cls), "a raid of three");
    const size_t before_leader = w[0].got.size();
    CHECK(party_disband(w, 1), "p2, a member, disbands");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_leader).empty(), "a non-leader's disband changes no frame");
    CHECK(party_disband(w, 0), "p1, the raid leader, disbands");
    w.settle();
    CHECK(frame_is_empty(last_party_frame(w[0].got, pc_cls)) && frame_is_empty(last_party_frame(w[1].got, pc_cls)) &&
              frame_is_empty(last_party_frame(w[2].got, pc_cls)),
          "every member's frame empties");
    CHECK(w[0].got.size() > before_leader, "the empty group reached them");
}

AOC_REGISTER_TEST(test_raid_disband);

static void test_raid_not_saved_after_logout() {
    std::printf("a raid is world-local: a session that ends leaves it and a returning player has no raid\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 4), "four players enter the world");
    if (!w[0].s || !w[3].s) return;
    CHECK(form_raid(w, 3, pc_cls), "a raid of three");
    const svc::Character p1 = party_record("p1", {0x51a00000u, 0x1000u, 0x2000u, 0x3000u});
    w[0].s->finish("the player logged out");
    w.settle();
    w[1].s->finish("the player logged out");
    w.settle();
    CHECK(frame_is_empty(last_party_frame(w[2].got, pc_cls)), "the last member's frame emptied");
    CHECK(w.enter(3, p1, identity()), "p1 joins again in a new session");
    if (!w[3].s) return;
    const size_t before = w[3].got.size();
    CHECK(party_invite(w, 2, "p1"), "p3 invites the returning p1");
    w.settle();
    const auto requests = party_requests_in(w[3].got, pc_cls, before);
    CHECK(requests.size() == 1 && requests[0].group_type == 0, "the returning p1 gets an ordinary party invitation: no raid was kept");
}

AOC_REGISTER_TEST(test_raid_not_saved_after_logout);

// -------------------------------------------------------------------------------------------------------------------------------
//  the frame and the left-out calls
// -------------------------------------------------------------------------------------------------------------------------------

static void test_raid_frame_carries_the_game_shape() {
    std::printf("the raid frame carries the game's shape: type 1, the squads, the settings, the master looter and the stats\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.present && frame.group_type == 1, "the group type is 1");
    CHECK(frame.leader_guid == w.guid_of(0) && frame.master_looter_guid == w.guid_of(0), "the leader and the master looter are p1");
    CHECK(frame.shout_privilege == 2 && !frame.override_loot && frame.raid_default == 0 && frame.raid_common == 0 && frame.raid_uncommon == 2,
          "the raid's settings are the defaults");
    CHECK(frame.party_member_shouts && frame.party_default == 0 && frame.party_common == 0 && frame.party_uncommon == 2, "the party settings are the defaults");
    CHECK(frame.squads.size() == 2, "two squads");
    if (frame.squads.size() == 2) {
        CHECK(frame.squads[0].number == 1 && frame.squads[0].members.size() == 8, "squad 1 holds the eight");
        CHECK(frame.squads[1].number == 2 && frame.squads[1].members.size() == 2, "squad 2 holds the last two");
        CHECK(frame.squads[0].leaders.size() == 1 && frame.squads[0].leaders[0] == w.guid_of(0), "squad 1's leader is p1");
        CHECK(frame.squads[1].leaders.empty(), "squad 2 has no leader");
        CHECK(frame.squads[0].members[0].guid == w.guid_of(0) && frame.squads[0].members[0].name == "p1", "the member rows carry the GUIDs and names");
        CHECK(frame.squads[0].members[0].primary_class != 0 && frame.squads[0].members[0].online, "the class and the online flag are on the row");
        const PartyStatsExpected want = party_stats_expected(*w[0].s);
        const PartyMemberFrame& m = frame.squads[0].members[0];
        CHECK(party_near(m.health_percent, want.health_percent) && m.max_health == want.max_health && party_near(m.mana_percent, want.mana_percent) &&
                  m.max_mana == want.max_mana && m.level == want.level && m.alive_status == want.alive && m.location == want.location,
              "the member's stats are the member's own");
    }
    CHECK(raid_move(w, 0, "p3", 2) && (w.settle(), true), "p3 moves to squad 2");
    const size_t before_transfer = w[0].got.size();
    const std::array<uint32_t, 4> master = last_raid_frame(w[0].got, pc_cls).master_looter_guid;
    CHECK(party_promote(w, 0, "p2"), "p1 transfers the leadership to p2");
    w.settle();
    frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.leader_guid == w.guid_of(1), "the leader mark moved to p2");
    CHECK(frame.master_looter_guid == master, "the master looter stays p1");
    CHECK(frame.squads.size() == 2 && frame.squads[0].leaders.size() == 1 && frame.squads[0].leaders[0] == w.guid_of(0),
          "the raid's squads' leader marks stay as they were (squad 1: p1)");
    CHECK(party_settings_in(w[0].got, pc_cls, before_transfer).empty(), "a raid's transfer sends no standalone party settings");
}

AOC_REGISTER_TEST(test_raid_frame_carries_the_game_shape);

static void test_raid_ignored_calls() {
    std::printf("the left-out calls (raid loot methods, the override, the summon) change no frame and the loot defaults stay\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 5), "five players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(form_raid(w, 3, pc_cls), "a raid of three");
    const size_t before = w[0].got.size();
    {
        const schema::RpcSchema* rpc = pc_cls.rpc("ServerTrySetRaidLootAllocationMethods");
        repl::RpcParams params;
        bool ok = rpc != nullptr;
        const char* const fields[] = {"DefaultLootAllocationMethod", "CommonLootAllocationMethod", "UncommonLootAllocationMethod", "RareLootAllocationMethod",
                                      "HeroicLootAllocationMethod", "EpicLootAllocationMethod", "LegendaryLootAllocationMethod", "ArtifactLootAllocationMethod"};
        for (const char* f : fields) ok = ok && repl::set_rpc_param(*rpc, params, std::string("Methods.") + f, repl::Value::u64(1));
        CHECK(ok && raid_send(w, 0, "ServerTrySetRaidLootAllocationMethods", params), "the raid loot methods call is sent");
    }
    {
        const schema::RpcSchema* rpc = pc_cls.rpc("ServerSetOverridePartyLootSettings");
        repl::RpcParams params;
        const bool ok = rpc && repl::set_rpc_param(*rpc, params, "bOverridePartyLootSettings", repl::Value::boolean(true));
        CHECK(ok && raid_send(w, 0, "ServerSetOverridePartyLootSettings", params), "the override call is sent");
    }
    CHECK(raid_send(w, 0, "ServerSummonRaid", repl::RpcParams{}), "the summon call is sent");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before).empty(), "none of them sends a frame");
    const RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    CHECK(frame.shout_privilege == 2 && !frame.override_loot && frame.raid_default == 0 && frame.raid_common == 0 && frame.raid_uncommon == 2,
          "the frame keeps the loot defaults it sends now");
}

AOC_REGISTER_TEST(test_raid_ignored_calls);

static void test_raid_ally_hit_reaches_a_squad_member() {
    std::printf("the party's group flag covers the whole raid; the masks narrow it to the squad or the raid\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 5), "five players enter the world");
    if (!w[0].s || !w[4].s) return;
    CHECK(form_raid(w, 3, pc_cls), "a raid of three");
    CHECK(raid_move(w, 0, "p3", 2) && (w.settle(), true), "p3 moves to squad 2");
    const uint64_t third = w[2].s->bootstrap().setup().guids.pawn.ObjectId;
    CHECK(w[0].s->in_party_with(third), "the flat group flag is true for the other squad's member");
    if (!w[0].s->in_party_with(third)) return;
}

AOC_REGISTER_TEST(test_raid_ally_hit_reaches_a_squad_member);

static void test_raid_repeated_messages() {
    std::printf("a repeated yes, accept, convert and move keep the state and send no second join request\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 9, pc_cls), "a raid of nine");
    const size_t before_yes = w[0].got.size();
    CHECK(raid_accept_convert(w, 0, "p9"), "the leader presses yes again");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before_yes, "p9 is already in a party.", &why), ("the repeated yes is refused: " + why).c_str());
    const size_t before_again = w[0].got.size();
    CHECK(raid_convert(w, 0), "the leader calls the convert button on a raid");
    w.settle();
    CHECK(raid_frames_in(w[0].got, pc_cls, before_again).empty(), "the button on a raid changes nothing");
    const size_t before_move = w[0].got.size();
    CHECK(raid_move(w, 0, "p3", 2) && (w.settle(), true), "p3 moves to squad 2");
    CHECK(raid_move(w, 0, "p3", 2), "p3 is moved to squad 2 again");
    w.settle();
    why.clear();
    CHECK(one_system_line(w[0].got, pc_cls, before_move, "p3 is already in squad 2.", &why), ("the repeated move is refused: " + why).c_str());
}

AOC_REGISTER_TEST(test_raid_repeated_messages);

static void test_raid_two_writers_same_tick() {
    std::printf("two raid calls in one tick: the leader's move then the squad leader set; a member's accept with another's leave\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 11), "eleven players enter the world");
    if (!w[0].s || !w[10].s) return;
    CHECK(form_raid(w, 10, pc_cls), "a raid of ten");
    CHECK(raid_move(w, 0, "p3", 2), "p1 moves p3 to squad 2");
    CHECK(raid_set_squad_leader(w, 0, "p3"), "and makes p3 squad 2's leader in the same tick");
    w.settle();
    RaidFrame frame = last_raid_frame(w[0].got, pc_cls);
    const RaidSquad* two = raid_squad_of(frame, "p3");
    CHECK(two && two->number == 2 && two->leaders.size() == 1 && two->leaders[0] == w.guid_of(2), "both calls took effect in order");
    const size_t from_nine = w[8].got.size();
    const size_t before = w[0].got.size();
    // p9 leaves while p11 is invited and accepts
    const size_t from_eleven = w[10].got.size();
    CHECK(party_invite(w, 0, "p11"), "p1 invites p11");
    w.settle();
    const auto eleven = party_requests_in(w[10].got, pc_cls, from_eleven);
    CHECK(eleven.size() == 1, "p11 is invited");
    if (eleven.size() != 1) return;
    CHECK(party_accept(w, 10, eleven[0].group_guid, eleven[0].inviter), "p11 accepts");
    CHECK(party_leave(w, 8), "p9 leaves in the same tick");
    w.settle();
    frame = last_raid_frame(w[0].got, pc_cls);
    bool has_nine = false, has_eleven = false;
    for (const RaidSquad& squad : frame.squads) {
        for (const PartyMemberFrame& m : squad.members) {
            has_nine = has_nine || m.name == "p9";
            has_eleven = has_eleven || m.name == "p11";
        }
    }
    CHECK(!has_nine && has_eleven, "the final frame has p11 and not p9");
    CHECK(w[8].got.size() > from_nine, "p9's client heard of its leave");
    CHECK(w[0].got.size() > before, "the leader's frame followed both changes");
}

AOC_REGISTER_TEST(test_raid_two_writers_same_tick);
