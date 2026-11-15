// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_party_tests.inc
//
//  Registered tests of test_worldd_session: social step 8, a party of two to eight.  An invitation reaches the named player on line as
//  ClientRequestJoinGroup and is answered with ServerAcceptJoinGroup or ServerDeclineJoinGroup; the accept sends ClientUpdateGroup to every
//  member (the leader included) and then ClientUpdateGroupSettings; a leave, a kick, a leader change, a logout and a disband reach every
//  frame; the party holds eight members and an invitation stands five minutes.  The group's frame carries the whole state: the two GUIDs
//  and the master looter's, the group type, and the squad with its members and its leader.  The two-member frame's fields and its bit
//  size are checked against the game's own frame; the one-member frame and the settings update have their own sizes.
// ============================================================================
#include "protocol/repl/rpc_fields.h"

namespace {

/// The most players one test of this file joins: the eight members of a full party and the two players who test the refusals.
constexpr size_t kMaxPartyTestPlayers = 10;

/// How long an invitation stands (the game's group timeout).
constexpr auto kPartyInviteLifetime = std::chrono::minutes(5);

constexpr const char* kGuidWord[4] = {"A", "B", "C", "D"};

/// The two characters of the game's recorded two-member frame.  The sizes the test asserts are that frame's (1,168, 1,596 and
/// 84 bits); the names and the four-word GUIDs below are made up, of the same lengths, so the sizes still hold.
constexpr std::array<uint32_t, 4> kMadeUpLeaderGuid = {0x1a2b3c4du, 0x5e6f7081u, 0x92a3b4c5u, 0xd6e7f809u};
constexpr std::array<uint32_t, 4> kMadeUpMemberGuid = {0x2468ace0u, 0x13579bdfu, 0x0f1e2d3cu, 0x4b5a6978u};

/// The all-zero GUID of the empty group.
const std::array<uint32_t, 4> kNoPartyGuid{};

/// One member of a ClientUpdateGroup as the client reads it.
struct PartyMemberFrame {
    std::string             name;
    std::array<uint32_t, 4> guid{};
    uint64_t                primary_class = 0;
    bool                    online = false;
    float                   health_percent = 0.0f, max_health = 0.0f, mana_percent = 0.0f, max_mana = 0.0f, heading = 0.0f;
    int64_t                 level = 0;
    uint64_t                alive_status = 0;
    repl::QuantVec          location, marker;
};

/// One ClientUpdateGroup as the client reads it: the whole group and its squad.
struct PartyFrame {
    bool                              present = false;
    size_t                            param_bits = 0;
    std::array<uint32_t, 4>           group_guid{}, leader_guid{}, master_looter_guid{};
    uint64_t                          group_type = 0;
    uint64_t                          squad_number = 0;
    std::vector<PartyMemberFrame>     members;
    std::vector<std::array<uint32_t, 4>> squad_leaders;
    bool                              member_shouts = false;
    uint64_t                          shout_privilege = 0;
    bool                              override_loot = false;
    uint64_t                          raid_default = 0, raid_common = 0, raid_uncommon = 0;
    uint64_t                          party_default = 0, party_common = 0, party_uncommon = 0;
};

/// One ClientRequestJoinGroup as the invited client reads it.
struct PartyRequest {
    bool                    present = false;
    std::array<uint32_t, 4> group_guid{};
    std::string             inviter;
    uint64_t                group_type = 0;
};

/// One ClientUpdateGroupSettings as the client reads it.
struct PartySettingsFrame {
    bool     present = false;
    size_t   param_bits = 0;
    bool     party_member_shouts = false;
    uint64_t party_default = 0, party_common = 0, party_uncommon = 0;
    uint64_t raid_shout_privilege = 0;
    bool     raid_override = false;
    uint64_t raid_default = 0, raid_common = 0, raid_uncommon = 0;
    bool     squad_member_shouts = false;
};

/// The one decoded RPC of a delivered chain, when it is `name`.
static std::optional<repl::RpcCall> party_single_rpc(const fake::Delivered& d, const schema::ClassSchema& pc_cls, const char* name) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return std::nullopt;
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = repl::read_rpc_fields(pc_cls, d.content.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!calls || calls->size() != 1) return std::nullopt;
    if (!(*calls)[0].decoded || std::string((*calls)[0].rpc->name) != name) return std::nullopt;
    return std::move((*calls)[0]);
}

/// The four int32 words of the GUID at `prefix` of a call.
static std::array<uint32_t, 4> party_words(const repl::RpcCall& call, const std::string& prefix) {
    std::array<uint32_t, 4> words{};
    for (int i = 0; i < 4; ++i) {
        const std::string path = prefix + "." + kGuidWord[i];
        if (const auto v = repl::rpcf::int_or(repl::rpcf::value_of(*call.rpc, call.params, path), 0)) words[i] = static_cast<uint32_t>(*v);
    }
    return words;
}

/// The words of a GUID held by an array element's local handle.
static std::array<uint32_t, 4> party_element_words(const repl::ElementState& e, int first) {
    std::array<uint32_t, 4> words{};
    for (int i = 0; i < 4; ++i) {
        if (const auto v = repl::rpcf::int_or(repl::rpcf::element_value(e, static_cast<uint16_t>(first + i)), 0)) words[i] = static_cast<uint32_t>(*v);
    }
    return words;
}

/// One ClientUpdateGroup of a delivered chain.
static std::optional<PartyFrame> party_frame_of(const fake::Delivered& d, const schema::ClassSchema& pc_cls) {
    const auto call = party_single_rpc(d, pc_cls, "ClientUpdateGroup");
    if (!call) return std::nullopt;
    PartyFrame frame;
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
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.bEnableMemberShouts"), false)) frame.member_shouts = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.LootAllocationMethods.DefaultLootAllocationMethod"), 0)) frame.party_default = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.LootAllocationMethods.CommonLootAllocationMethod"), 0)) frame.party_common = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.PartySettings.LootAllocationMethods.UncommonLootAllocationMethod"), 0)) frame.party_uncommon = *v;
    const repl::Value* squads = repl::rpcf::value_of(*call->rpc, call->params, "NewGroup.Squads");
    const auto* squad_list = squads ? squads->get_if<repl::ArrayValue>() : nullptr;
    if (!squad_list || squad_list->elements.size() != 1) return frame;   // (the empty group carries no squad)
    const repl::ElementState& squad = squad_list->elements.front();
    if (const auto v = repl::rpcf::int_or(repl::rpcf::element_value(squad, 1), 0)) frame.squad_number = static_cast<uint64_t>(*v);
    const repl::Value* members = repl::rpcf::element_value(squad, 2);
    const auto* member_list = members ? members->get_if<repl::ArrayValue>() : nullptr;
    if (member_list) {
        for (const repl::ElementState& e : member_list->elements) {
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
            frame.members.push_back(std::move(m));
        }
    }
    if (const repl::Value* leaders = repl::rpcf::element_value(squad, 3)) {
        if (const auto* leader_list = leaders->get_if<repl::ArrayValue>()) {
            for (const repl::ElementState& e : leader_list->elements) frame.squad_leaders.push_back(party_element_words(e, 1));
        }
    }
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::element_value(squad, 4), false)) frame.member_shouts = *v;
    return frame;
}

/// One ClientRequestJoinGroup of a delivered chain.
static std::optional<PartyRequest> party_request_of(const fake::Delivered& d, const schema::ClassSchema& pc_cls) {
    const auto call = party_single_rpc(d, pc_cls, "ClientRequestJoinGroup");
    if (!call) return std::nullopt;
    PartyRequest request;
    request.present = true;
    request.group_guid = party_words(*call, "GroupGuid");
    if (const auto* s = repl::rpcf::as<std::string>(repl::rpcf::value_of(*call->rpc, call->params, "InviterName"))) request.inviter = *s;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "GroupType"), 0)) request.group_type = *v;
    return request;
}

/// One ClientUpdateGroupSettings of a delivered chain.
static std::optional<PartySettingsFrame> party_settings_of(const fake::Delivered& d, const schema::ClassSchema& pc_cls) {
    const auto call = party_single_rpc(d, pc_cls, "ClientUpdateGroupSettings");
    if (!call) return std::nullopt;
    PartySettingsFrame frame;
    frame.present = true;
    frame.param_bits = call->param_bits;
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::value_of(*call->rpc, call->params, "PartySettings.bEnableMemberShouts"), false)) frame.party_member_shouts = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "PartySettings.LootAllocationMethods.DefaultLootAllocationMethod"), 0)) frame.party_default = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "PartySettings.LootAllocationMethods.CommonLootAllocationMethod"), 0)) frame.party_common = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "PartySettings.LootAllocationMethods.UncommonLootAllocationMethod"), 0)) frame.party_uncommon = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "RaidSettings.ShoutPrivilege"), 0)) frame.raid_shout_privilege = *v;
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::value_of(*call->rpc, call->params, "RaidSettings.bOverridePartyLootSettings"), false)) frame.raid_override = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "RaidSettings.LootAllocationMethods.DefaultLootAllocationMethod"), 0)) frame.raid_default = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "RaidSettings.LootAllocationMethods.CommonLootAllocationMethod"), 0)) frame.raid_common = *v;
    if (const auto v = repl::rpcf::uint_or(repl::rpcf::value_of(*call->rpc, call->params, "RaidSettings.LootAllocationMethods.UncommonLootAllocationMethod"), 0)) frame.raid_uncommon = *v;
    if (const auto v = repl::rpcf::bool_or(repl::rpcf::value_of(*call->rpc, call->params, "SquadSettings.bEnableMemberShouts"), false)) frame.squad_member_shouts = *v;
    return frame;
}

/// True when exactly one settings update follows the first group frame from chain `from` on, whatever the frame's member count.
static bool settings_follow_the_first_group_frame(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    size_t group_at = SIZE_MAX, settings_at = SIZE_MAX, settings_count = 0;
    for (size_t i = from; i < got.size(); ++i) {
        if (party_frame_of(got[i], pc_cls) && group_at == SIZE_MAX) group_at = i;
        if (party_settings_of(got[i], pc_cls)) {
            ++settings_count;
            if (settings_at == SIZE_MAX) settings_at = i;
        }
    }
    return group_at != SIZE_MAX && settings_count == 1 && settings_at > group_at;
}

/// The party frames a player was given from chain `from` on, in order.
static std::vector<PartyFrame> party_frames_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<PartyFrame> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto frame = party_frame_of(got[i], pc_cls)) out.push_back(std::move(*frame));
    }
    return out;
}

/// The settings updates a player was given from chain `from` on, in order.
static std::vector<PartySettingsFrame> party_settings_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<PartySettingsFrame> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto frame = party_settings_of(got[i], pc_cls)) out.push_back(std::move(*frame));
    }
    return out;
}

/// True when exactly one settings update follows the first two-member group frame from chain `from` on.
static bool settings_follow_the_first_two_member_frame(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    size_t group_at = SIZE_MAX, settings_at = SIZE_MAX, settings_count = 0;
    for (size_t i = from; i < got.size(); ++i) {
        if (const auto frame = party_frame_of(got[i], pc_cls)) {
            if (frame->members.size() == 2 && group_at == SIZE_MAX) group_at = i;
        }
        if (party_settings_of(got[i], pc_cls)) {
            ++settings_count;
            if (settings_at == SIZE_MAX) settings_at = i;
        }
    }
    return group_at != SIZE_MAX && settings_count == 1 && settings_at > group_at;
}

/// The join requests a player was given from chain `from` on, in order.
static std::vector<PartyRequest> party_requests_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    std::vector<PartyRequest> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (auto request = party_request_of(got[i], pc_cls)) out.push_back(std::move(*request));
    }
    return out;
}

/// The last party frame a player was given, or a frame that is not there.
static PartyFrame last_party_frame(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls) {
    for (size_t i = got.size(); i > 0; --i) {
        if (auto frame = party_frame_of(got[i - 1], pc_cls)) return *frame;
    }
    return PartyFrame{};
}

/// True when the frame lists exactly `names` in this order and its leader is `leader`.
static bool frame_lists(const PartyFrame& frame, const std::vector<std::string>& names, const std::array<uint32_t, 4>& leader, std::string* why) {
    if (!frame.present) {
        *why = "there is no group frame";
        return false;
    }
    if (frame.members.size() != names.size()) {
        *why = "the frame lists " + std::to_string(frame.members.size()) + " member(s), not " + std::to_string(names.size());
        return false;
    }
    for (size_t i = 0; i < names.size(); ++i) {
        if (frame.members[i].name != names[i]) {
            *why = "member " + std::to_string(i + 1) + " is " + frame.members[i].name + ", not " + names[i];
            return false;
        }
    }
    if (frame.leader_guid != leader) {
        *why = "the leader mark is not on the expected character";
        return false;
    }
    if (frame.squad_leaders.size() != 1 || frame.squad_leaders[0] != leader) {
        *why = "the squad's leader list does not hold the current leader";
        return false;
    }
    return true;
}

/// True when the frame is the empty group: no GUID and no squad.
static bool frame_is_empty(const PartyFrame& frame) {
    return frame.present && frame.group_guid == std::array<uint32_t, 4>{} && frame.leader_guid == std::array<uint32_t, 4>{} &&
           frame.master_looter_guid == std::array<uint32_t, 4>{} && frame.members.empty();
}

/// True when every field of the two-member frame is the game's shape (the members and the GUIDs are the caller's).
static bool frame_has_the_game_shape(const PartyFrame& frame, const std::array<uint32_t, 4>& leader, const std::array<uint32_t, 4>& member, std::string* why) {
    if (frame.group_type != 0) {
        *why = "the group type is not 0 (a party)";
        return false;
    }
    if (frame.squad_number != 1) {
        *why = "the squad number is not 1";
        return false;
    }
    if (frame.members.size() != 2) {
        *why = "the squads list does not hold two members";
        return false;
    }
    const PartyMemberFrame& first = frame.members[0];
    const PartyMemberFrame& second = frame.members[1];
    if (first.guid != leader || second.guid != member) {
        *why = "the members' GUIDs are not the two characters'";
        return false;
    }
    for (const PartyMemberFrame* m : {&first, &second}) {
        if (!m->online || m->health_percent != 0.0f || m->max_health != 0.0f || m->mana_percent != 0.0f || m->max_mana != 0.0f ||
            m->level != 1 || m->alive_status != 0 || !(m->location == repl::QuantVec{}) || !(m->marker == repl::QuantVec{}) || m->heading != 0.0f) {
            *why = "a member's waiting stats are not zero with level 1";
            return false;
        }
    }
    if (first.primary_class == 0 || second.primary_class == 0) {
        *why = "a member's class is not on the frame";
        return false;
    }
    if (frame.leader_guid != leader || frame.master_looter_guid != leader) {
        *why = "the leader and the master looter are not the leader";
        return false;
    }
    if (frame.squad_leaders.size() != 1 || frame.squad_leaders[0] != leader) {
        *why = "the squad's leader list is not the leader";
        return false;
    }
    if (!frame.member_shouts || frame.shout_privilege != 2 || frame.override_loot || frame.raid_default != 0 || frame.raid_common != 0 ||
        frame.raid_uncommon != 2 || frame.party_default != 0 || frame.party_common != 0 || frame.party_uncommon != 2) {
        *why = "the group's loot settings are not the defaults";
        return false;
    }
    return true;
}

/// A world with up to ten sessions on it, each with its own connection and client and ticked together: what a party's frames reach.
struct PartyWorld {
    Fixture fx;
    Clock::time_point t = t0;
    struct Player {
        Connection conn{ConnectionConfig{kOuter, kCf, kServerSeq, kClientSeq}, t0};
        fake::Client client{kOuter, kCf, kServerSeq, kClientSeq};
        std::unique_ptr<PlayerSession> s;
        std::vector<fake::Delivered> got;
        std::string error;
    };
    std::array<Player, kMaxPartyTestPlayers> p;

    Player& operator[](size_t i) { return p[i]; }

    bool join(size_t i, const svc::Character& rec, const bs::LoginIdentity& id) {
        p[i].s = PlayerSession::join(p[i].conn, *fx.world, rec, id, world_time_at(t), p[i].error);
        return p[i].s != nullptr;
    }
    void take(Player& pl, const fake::Received& r) {
        for (const auto& d : r.delivered) pl.got.push_back(d);
    }
    void flush(Player& pl) {
        for (auto& pkt : pl.conn.flush(t)) take(pl, pl.client.receive(pkt));
    }
    void client_sends(Player& pl, std::vector<fake::Bunch> bunches) {
        const auto pkt = pl.client.packet(std::move(bunches));
        const std::vector<InBunch> in = pl.conn.receive(pkt, t);
        pl.s->take_acks(pl.conn);
        for (const InBunch& b : in) pl.s->on_bunch(pl.conn, b, t);
    }
    /// 20 ms: every session's full tick, then the clients receive and acknowledge.
    void step() {
        t += milliseconds(20);
        for (Player& pl : p) {
            if (!pl.s) continue;
            pl.s->tick(pl.conn, t);
            flush(pl);
            pl.s->take_acks(pl.conn);
        }
        for (Player& pl : p) {
            if (!pl.s) continue;
            client_sends(pl, {});
        }
    }
    size_t total_got() const {
        size_t n = 0;
        for (const Player& pl : p) n += pl.got.size();
        return n;
    }
    size_t pending() const {
        size_t n = 0;
        for (const Player& pl : p) {
            if (pl.s) n += pl.s->join_pending() + pl.s->replies_pending();
        }
        return n;
    }
    /// Steps until nothing is pending and a step brings nothing new.
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = total_got();
            step();
            quiet = total_got() == before && pending() == 0 ? quiet + 1 : 0;
        }
    }
    /// Joins player `i`, settles the join and sends its phase-in ready.
    bool enter(size_t i, const svc::Character& rec, const bs::LoginIdentity& id) {
        if (!join(i, rec, id) || !p[i].s || p[i].s->failed()) return false;
        settle();
        client_sends(p[i], {p[i].client.actor(p[i].s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
        settle();
        return true;
    }
    /// The character GUID of player `i`, as the frame carries it.
    std::array<uint32_t, 4> guid_of(size_t i) const {
        const auto& g = p[i].s->bootstrap().setup().character_guid;
        return {g[0], g[1], g[2], g[3]};
    }
};

/// The 32 hex digits of a character GUID (the id a record holds).
static std::string party_hex(const std::array<uint32_t, 4>& words) {
    char text[33];
    std::snprintf(text, sizeof(text), "%08x%08x%08x%08x", words[0], words[1], words[2], words[3]);
    return text;
}

/// A character of the party tests: the chat tests' record at the spawn, with the name and GUID given.
static svc::Character party_record(const char* name, const std::array<uint32_t, 4>& words) {
    return chat_record(name, party_hex(words).c_str(), kChatX, kChatY, kChatZ);
}

/// A client call of the party on the PlayerController.
static bool party_send(PartyWorld& w, size_t who, const char* name, const repl::RpcParams& params) {
    if (!w[who].s) return false;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const BunchWriter call = client_rpc(pc_cls, name, params);
    if (call.bit_pos() == 0) return false;
    w.client_sends(w[who], {w[who].client.actor(w[who].s->channel_of(bs::Role::PlayerController), call, true)});
    return true;
}

static bool party_invite(PartyWorld& w, size_t who, const std::string& name) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerInviteToGroup");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterName", repl::Value::str(name))) return false;
    return party_send(w, who, "ServerInviteToGroup", params);
}

static bool put_words(const schema::RpcSchema& rpc, repl::RpcParams& params, const char* prefix, const std::array<uint32_t, 4>& words) {
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        ok = repl::set_rpc_param(rpc, params, std::string(prefix) + "." + kGuidWord[i], repl::Value::i64(static_cast<int32_t>(words[i]))) && ok;
    }
    return ok;
}

static bool party_accept(PartyWorld& w, size_t who, const std::array<uint32_t, 4>& group, const std::string& inviter) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerAcceptJoinGroup");
    repl::RpcParams params;
    if (!rpc || !put_words(*rpc, params, "GroupGuid", group) || !repl::set_rpc_param(*rpc, params, "InviterName", repl::Value::str(inviter))) return false;
    return party_send(w, who, "ServerAcceptJoinGroup", params);
}

static bool party_decline(PartyWorld& w, size_t who, const std::array<uint32_t, 4>& group, const std::string& inviter) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerDeclineJoinGroup");
    repl::RpcParams params;
    if (!rpc || !put_words(*rpc, params, "GroupGuid", group) || !repl::set_rpc_param(*rpc, params, "InviterName", repl::Value::str(inviter))) return false;
    return party_send(w, who, "ServerDeclineJoinGroup", params);
}

static bool party_leave(PartyWorld& w, size_t who) { return party_send(w, who, "ServerLeaveGroup", repl::RpcParams{}); }
static bool party_disband(PartyWorld& w, size_t who) { return party_send(w, who, "ServerDisbandGroup", repl::RpcParams{}); }

static bool party_kick(PartyWorld& w, size_t who, const std::string& name) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerKickFromGroup");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterNameToKick", repl::Value::str(name))) return false;
    return party_send(w, who, "ServerKickFromGroup", params);
}

static bool party_promote(PartyWorld& w, size_t who, const std::string& name) {
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc_cls.rpc("ServerTransferGroupLeadership");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterNameToPromote", repl::Value::str(name))) return false;
    return party_send(w, who, "ServerTransferGroupLeadership", params);
}

/// Joins `count` players named "p1".."pN" (each its own GUID) and lets them enter the world.
static bool party_enter(PartyWorld& w, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const std::string name = "p" + std::to_string(i + 1);
        std::array<uint32_t, 4> words{0x51a00000u + static_cast<uint32_t>(i), 0x1000u + static_cast<uint32_t>(i), 0x2000u + static_cast<uint32_t>(i), 0x3000u + static_cast<uint32_t>(i)};
        if (!w.enter(i, party_record(name.c_str(), words), identity())) return false;
    }
    return true;
}

/// The chat line a party refusal sends a player from chain `from` on: the one SYSBROADCAST line from Server.
static bool one_system_line(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from, const std::string& text, std::string* why) {
    const auto lines = chat_lines_in(got, pc_cls, from);
    if (lines.size() != 1) {
        *why = "the client got " + std::to_string(lines.size()) + " chat line(s), not one";
        return false;
    }
    if (lines[0].message != text || lines[0].from != "Server" || lines[0].category != 124) {
        *why = "the line is \"" + lines[0].message + "\" from \"" + lines[0].from + "\", category " + std::to_string(lines[0].category);
        return false;
    }
    return true;
}

/// The inviter invites `name` and the target accepts the request the client was given: one party of two.
static bool party_pair(PartyWorld& w, size_t leader, size_t member, const schema::ClassSchema& pc_cls) {
    const size_t before = w[member].got.size();
    if (!party_invite(w, leader, w[member].s->name())) return false;
    w.settle();
    const auto requests = party_requests_in(w[member].got, pc_cls, before);
    if (requests.size() != 1) return false;
    if (!party_accept(w, member, requests[0].group_guid, requests[0].inviter)) return false;
    w.settle();
    return true;
}

/// A party of three: `leader` invites `first` and then `second`, and both accept.
static bool party_three(PartyWorld& w, size_t leader, size_t first, size_t second, const schema::ClassSchema& pc_cls) {
    if (!party_pair(w, leader, first, pc_cls)) return false;
    const size_t before = w[second].got.size();
    if (!party_invite(w, leader, w[second].s->name())) return false;
    w.settle();
    const auto requests = party_requests_in(w[second].got, pc_cls, before);
    if (requests.size() != 1) return false;
    if (!party_accept(w, second, requests[0].group_guid, requests[0].inviter)) return false;
    w.settle();
    return true;
}

}  // namespace

static void test_party_wire_two_member_frame() {
    std::printf("the two-member party frame has the recorded sizes and the made-up characters' fields, and the settings follow it\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const svc::Character leader = party_record("Silverwhisper", kMadeUpLeaderGuid);
    const svc::Character member = party_record("Tavin", kMadeUpMemberGuid);
    CHECK(w.enter(0, leader, identity()) && w.enter(1, member, identity()), "the two characters enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before_leader = w[0].got.size();
    const size_t before_member = w[1].got.size();
    CHECK(party_invite(w, 0, "Tavin"), "Silverwhisper invites Tavin");
    w.settle();
    const auto requests = party_requests_in(w[1].got, pc_cls, before_member);
    CHECK(requests.size() == 1, "Tavin's client gets one ClientRequestJoinGroup");
    if (requests.size() != 1) return;
    CHECK(requests[0].inviter == "Silverwhisper" && requests[0].group_type == 0, "the request names Silverwhisper and a party (type 0)");
    const auto one_member = party_frames_in(w[0].got, pc_cls, before_leader);
    CHECK(one_member.size() == 1, "the leader's client gets the one-member state while the invitation waits");
    if (one_member.size() == 1) {
        std::string why;
        CHECK(frame_lists(one_member[0], {"Silverwhisper"}, kMadeUpLeaderGuid, &why), ("the one-member state lists the leader alone: " + why).c_str());
        CHECK(one_member[0].param_bits == 1168, "the one-member frame is 1,168 bits, the recorded frame's size");
        CHECK(one_member[0].group_guid != kNoPartyGuid && one_member[0].group_guid == requests[0].group_guid,
              "the frame's group GUID is the one the invitation carries");
    }
    const size_t before_accept = w[0].got.size();
    const size_t before_accept_b = w[1].got.size();
    CHECK(party_accept(w, 1, requests[0].group_guid, requests[0].inviter), "Tavin accepts the invitation");
    w.settle();
    const auto frames_a = party_frames_in(w[0].got, pc_cls, before_accept);
    const auto frames_b = party_frames_in(w[1].got, pc_cls, before_accept_b);
    CHECK(frames_a.size() == 1 && frames_b.size() == 1, "the accept sends every member one two-member frame");
    if (!frames_a.empty()) {
        const PartyFrame& frame = frames_a[0];
        std::string why;
        CHECK(frame_lists(frame, {"Silverwhisper", "Tavin"}, kMadeUpLeaderGuid, &why), ("the leader's frame lists both members: " + why).c_str());
        CHECK(frame.param_bits == 1596, "the two-member frame is 1,596 bits, the recorded frame's size");
        CHECK(frame_has_the_game_shape(frame, kMadeUpLeaderGuid, kMadeUpMemberGuid, &why),
              ("the frame has the game's fields (the GUIDs, the squad, the stats, the defaults): " + why).c_str());
        CHECK(frame.group_guid == requests[0].group_guid, "the two-member frame keeps the invitation's group GUID");
        CHECK(frame.leader_guid == kMadeUpLeaderGuid && frame.master_looter_guid == kMadeUpLeaderGuid && frame.squad_leaders.size() == 1 &&
                  frame.squad_leaders[0] == kMadeUpLeaderGuid,
              "LeaderGuid, MasterLooterGuid and SquadLeaders all name Silverwhisper");
    }
    if (!frames_b.empty()) {
        std::string why;
        CHECK(frame_lists(frames_b[0], {"Silverwhisper", "Tavin"}, kMadeUpLeaderGuid, &why), ("Tavin's frame lists both members too: " + why).c_str());
        CHECK(frames_b[0].param_bits == 1596 && frames_b[0].group_guid == requests[0].group_guid, "Tavin's frame is the same 1,596-bit group");
    }
    const auto settings_a = party_settings_in(w[0].got, pc_cls, before_accept);
    const auto settings_b = party_settings_in(w[1].got, pc_cls, before_accept_b);
    CHECK(settings_a.size() == 1 && settings_b.size() == 1, "every member gets one ClientUpdateGroupSettings after the two-member frame");
    if (!settings_a.empty()) {
        const PartySettingsFrame& s = settings_a[0];
        CHECK(s.param_bits == 84, "the settings update is 84 bits, the recorded frame's size");
        CHECK(s.party_member_shouts && s.party_default == 0 && s.party_common == 0 && s.party_uncommon == 2 && s.raid_shout_privilege == 2 && !s.raid_override &&
                  s.raid_default == 0 && s.raid_common == 0 && s.raid_uncommon == 2 && s.squad_member_shouts,
              "the settings update carries the defaults: member shouts on, shout privilege 2, the loot methods 0 and 2");
    }
    CHECK(!w[0].s || w[0].got.size() > before_leader, "the leader received its frames");
}

AOC_REGISTER_TEST(test_party_wire_two_member_frame);

static void test_party_invite_accept_updates_both() {
    std::printf("an invite, an accept and the group update reach both members, the leader included\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before_a = w[0].got.size();
    const size_t before_b = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto requests = party_requests_in(w[1].got, pc_cls, before_b);
    CHECK(requests.size() == 1, "p2's client gets exactly one ClientRequestJoinGroup");
    CHECK(party_requests_in(w[0].got, pc_cls, before_a).empty(), "the inviter gets no join request of its own");
    if (requests.size() != 1) return;
    CHECK(party_accept(w, 1, requests[0].group_guid, requests[0].inviter), "p2 accepts");
    w.settle();
    const PartyFrame frame_a = last_party_frame(w[0].got, pc_cls);
    const PartyFrame frame_b = last_party_frame(w[1].got, pc_cls);
    std::string why;
    CHECK(frame_lists(frame_a, {"p1", "p2"}, w.guid_of(0), &why), ("p1's frame lists both with p1 as the leader: " + why).c_str());
    why.clear();
    CHECK(frame_lists(frame_b, {"p1", "p2"}, w.guid_of(0), &why), ("p2's frame lists both with p1 as the leader: " + why).c_str());
    CHECK(frame_a.group_guid == frame_b.group_guid && frame_a.group_guid != kNoPartyGuid, "both frames carry the same, non-zero group GUID");
    CHECK(frame_a.param_bits == frame_b.param_bits && frame_a.param_bits > 0, "both frames are the same size");
}

AOC_REGISTER_TEST(test_party_invite_accept_updates_both);

static void test_party_any_member_may_invite() {
    std::printf("any member may invite, not only the leader\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before = w[2].got.size();
    CHECK(party_invite(w, 1, "p3"), "p2, a member and not the leader, invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before);
    CHECK(requests.size() == 1 && requests[0].inviter == "p2", "p3's invitation names p2");
    if (requests.size() != 1) return;
    CHECK(party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2", "p3"}, w.guid_of(0), &why),
          ("the leader's frame lists all three under p1: " + why).c_str());
    why.clear();
    CHECK(frame_lists(last_party_frame(w[2].got, pc_cls), {"p1", "p2", "p3"}, w.guid_of(0), &why), ("p3's frame agrees: " + why).c_str());
}

AOC_REGISTER_TEST(test_party_any_member_may_invite);

static void test_party_owner_playerb_flow() {
    std::printf("the owner's two-player flow: invite Playerb, accept, Playerb leaves, a second invite and a leader transfer\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const svc::Character owner = chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ);
    const std::array<uint32_t, 4> playerb_id = {0x5157e2b0u, 0x4a1d1001u, 0x8f2c2002u, 0x9b3d3003u};
    CHECK(w.enter(0, owner, identity()) && w.enter(1, party_record("Playerb", playerb_id), identity()), "cyb and Playerb enter the world");
    if (!w[0].s || !w[1].s) return;
    // 1: cyb invites Playerb by name
    const size_t before = w[1].got.size();
    CHECK(party_invite(w, 0, "Playerb"), "cyb invites Playerb");
    w.settle();
    auto requests = party_requests_in(w[1].got, pc_cls, before);
    CHECK(requests.size() == 1 && requests[0].inviter == "cyb", "Playerb's client gets the invitation from cyb");
    if (requests.size() != 1) return;
    // 2: Playerb accepts; cyb's frame shows both with cyb as the leader
    CHECK(party_accept(w, 1, requests[0].group_guid, requests[0].inviter), "Playerb accepts");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"cyb", "Playerb"}, w.guid_of(0), &why),
          ("cyb's frame lists both with cyb as the leader: " + why).c_str());
    CHECK(frame_lists(last_party_frame(w[1].got, pc_cls), {"cyb", "Playerb"}, w.guid_of(0), &why),
          ("Playerb's frame lists both with cyb as the leader: " + why).c_str());
    // 3: Playerb leaves; cyb's frame empties (the one-member state, then the empty group)
    const size_t before_leave = w[0].got.size();
    CHECK(party_leave(w, 1), "Playerb leaves");
    w.settle();
    const auto after_leave = party_frames_in(w[0].got, pc_cls, before_leave);
    CHECK(after_leave.size() == 2 && after_leave[0].members.size() == 1 && frame_is_empty(after_leave[1]),
          "cyb gets the one-member state and then the empty group");
    CHECK(frame_is_empty(last_party_frame(w[1].got, pc_cls)), "Playerb's frame empties too");
    // 4: a second invite; Playerb accepts; cyb transfers the leadership to Playerb
    const size_t before_second = w[1].got.size();
    CHECK(party_invite(w, 0, "Playerb"), "cyb invites Playerb a second time");
    w.settle();
    auto second = party_requests_in(w[1].got, pc_cls, before_second);
    CHECK(second.size() == 1, "Playerb's client gets the second invitation");
    if (second.size() != 1) return;
    CHECK(second[0].group_guid != requests[0].group_guid, "the second party gets a GUID of its own");
    CHECK(party_accept(w, 1, second[0].group_guid, second[0].inviter), "Playerb accepts the second invitation");
    w.settle();
    const size_t before_promote_a = w[0].got.size();
    const size_t before_promote_b = w[1].got.size();
    CHECK(party_promote(w, 0, "Playerb"), "cyb transfers the leadership to Playerb");
    w.settle();
    const PartyFrame promoted_a = last_party_frame(w[0].got, pc_cls);
    const PartyFrame promoted_b = last_party_frame(w[1].got, pc_cls);
    CHECK(frame_lists(promoted_a, {"cyb", "Playerb"}, w.guid_of(1), &why), ("cyb's frame now marks Playerb as the leader: " + why).c_str());
    why.clear();
    CHECK(frame_lists(promoted_b, {"cyb", "Playerb"}, w.guid_of(1), &why), ("Playerb's frame marks Playerb as the leader: " + why).c_str());
    CHECK(promoted_a.master_looter_guid == w.guid_of(0), "the master looter stays cyb, as the game's transfer keeps it");
    CHECK(promoted_a.group_guid == second[0].group_guid && promoted_b.group_guid == second[0].group_guid, "the transfer keeps the group GUID");
    CHECK(w[0].got.size() > before_promote_a && w[1].got.size() > before_promote_b, "both players received the transfer");
}

AOC_REGISTER_TEST(test_party_owner_playerb_flow);

static void test_party_ninth_member_refused() {
    std::printf("eight members form a party and the ninth is refused\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 9), "nine players enter the world");
    if (!w[0].s || !w[8].s) return;
    // the leader and seven members
    for (size_t i = 1; i <= 7; ++i) {
        const size_t before = w[i].got.size();
        CHECK(party_invite(w, 0, w[i].s->name()), ("p1 invites " + w[i].s->name()).c_str());
        w.settle();
        const auto requests = party_requests_in(w[i].got, pc_cls, before);
        CHECK(requests.size() == 1 && party_accept(w, i, requests[0].group_guid, requests[0].inviter), ("member " + std::to_string(i) + " accepts").c_str());
        w.settle();
    }
    const PartyFrame full = last_party_frame(w[0].got, pc_cls);
    std::vector<std::string> names;
    for (size_t i = 0; i < 8; ++i) names.push_back("p" + std::to_string(i + 1));
    std::string why;
    CHECK(frame_lists(full, names, w.guid_of(0), &why), ("the party holds eight members: " + why).c_str());
    const size_t before_ninth = w[8].got.size();
    const size_t before_leader = w[0].got.size();
    CHECK(party_invite(w, 0, "p9"), "p1 invites the ninth player");
    w.settle();
    CHECK(party_requests_in(w[8].got, pc_cls, before_ninth).empty(), "the ninth player gets no ClientRequestJoinGroup");
    std::string line_why;
    CHECK(one_system_line(w[0].got, pc_cls, before_leader, "p9 cannot join: the party is full.", &line_why),
          ("the leader is told the party is full: " + line_why).c_str());
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), names, w.guid_of(0), &why), "the eight-member party is unchanged");
}

AOC_REGISTER_TEST(test_party_ninth_member_refused);

static void test_party_invite_unknown_and_already_grouped() {
    std::printf("an invite to an unknown name and to a player already in a party is refused with one system line\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    const size_t before_unknown = w[0].got.size();
    CHECK(party_invite(w, 0, "Nobody_xyz"), "p1 invites a name that is not online");
    w.settle();
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before_unknown, "Nobody_xyz is not online.", &why),
          ("the inviter reads that the name is not online: " + why).c_str());
    CHECK(party_requests_in(w[1].got, pc_cls, 0).empty() && party_requests_in(w[2].got, pc_cls, 0).empty(), "nobody gets a join request");
    CHECK(party_frames_in(w[0].got, pc_cls, before_unknown).empty(), "an unknown name sends no group frame");
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_grouped = w[2].got.size();
    const size_t before_grouped_p2 = w[1].got.size();
    CHECK(party_invite(w, 2, "p2"), "p3 invites p2, who is already in a party");
    w.settle();
    CHECK(one_system_line(w[2].got, pc_cls, before_grouped, "p2 is already in a party.", &why),
          ("the inviter reads that the player is already in a party: " + why).c_str());
    CHECK(party_requests_in(w[1].got, pc_cls, before_grouped_p2).empty(), "p2 gets no second join request");
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), "the first party is unchanged");
}

AOC_REGISTER_TEST(test_party_invite_unknown_and_already_grouped);

static void test_party_invite_expires_after_five_minutes() {
    std::printf("an invitation stands five minutes; a later accept is refused and a lone leader's frame empties\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto requests = party_requests_in(w[1].got, pc_cls, before);
    CHECK(requests.size() == 1, "p2 gets the invitation");
    if (requests.size() != 1) return;
    w.t += kPartyInviteLifetime + std::chrono::seconds(1);
    const size_t before_expiry = w[0].got.size();
    CHECK(party_accept(w, 1, requests[0].group_guid, requests[0].inviter), "p2 accepts after five minutes");
    w.settle();
    const auto frames = party_frames_in(w[0].got, pc_cls, before_expiry);
    CHECK(frames.size() == 2 && frames[0].members.size() == 1 && frame_is_empty(frames[1]),
          "the leader gets the one-member state and then the empty group");
    CHECK(!last_party_frame(w[1].got, pc_cls).present, "the late accept leaves p2 in no group");
    // the boundary: one second under five minutes the accept still works
    const size_t before_second = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2 again");
    w.settle();
    const auto second = party_requests_in(w[1].got, pc_cls, before_second);
    CHECK(second.size() == 1, "p2 gets the second invitation");
    if (second.size() != 1) return;
    w.t += kPartyInviteLifetime - std::chrono::seconds(1);
    CHECK(party_accept(w, 1, second[0].group_guid, second[0].inviter), "p2 accepts one second before the five minutes are over");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), ("the party forms inside the window: " + why).c_str());
}

AOC_REGISTER_TEST(test_party_invite_expires_after_five_minutes);

static void test_party_invite_expires_on_the_tick() {
    std::printf("an invitation that is five minutes old goes without an accept and the lone leader's frame empties\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto requests = party_requests_in(w[1].got, pc_cls, before);
    CHECK(requests.size() == 1, "p2 gets the invitation");
    if (requests.size() != 1) return;
    w.t += kPartyInviteLifetime + std::chrono::seconds(1);
    const size_t before_tick = w[0].got.size();
    w.settle();   // the session's tick sees the expired invitation
    const auto frames = party_frames_in(w[0].got, pc_cls, before_tick);
    CHECK(frames.size() == 2 && frames[0].members.size() == 1 && frame_is_empty(frames[1]),
          "the lone leader gets the one-member state and then the empty group");
    const size_t before_late = w[1].got.size();
    CHECK(party_accept(w, 1, requests[0].group_guid, requests[0].inviter), "p2 accepts the expired invitation");
    w.settle();
    CHECK(party_frames_in(w[0].got, pc_cls, before_tick).size() == 2, "the late accept sends no further frame");
    CHECK(!last_party_frame(w[1].got, pc_cls).present && w[1].got.size() == before_late, "the late accept adds no group and sends p2 nothing");
}

AOC_REGISTER_TEST(test_party_invite_expires_on_the_tick);

static void test_party_accept_without_invite_refused() {
    std::printf("an accept from a player who was not invited is refused and changes no group\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before_a = w[0].got.size();
    const size_t before_b = w[1].got.size();
    const std::array<uint32_t, 4> made_up = {1, 2, 3, 4};
    CHECK(party_accept(w, 1, made_up, "p1"), "p2 accepts a group it was never invited to");
    w.settle();
    CHECK(party_frames_in(w[0].got, pc_cls, before_a).empty() && party_frames_in(w[1].got, pc_cls, before_b).empty(), "no frame moves for the refused accept");
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto requests = party_requests_in(w[1].got, pc_cls, before_b);
    CHECK(requests.size() == 1, "the real invitation arrives");
    if (requests.size() != 1) return;
    CHECK(party_accept(w, 1, made_up, requests[0].inviter), "p2 accepts with the wrong group GUID");
    w.settle();
    CHECK(!last_party_frame(w[0].got, pc_cls).present || last_party_frame(w[0].got, pc_cls).members.size() == 1, "the wrong GUID joins nobody");
    const size_t before_wrong_name_a = w[0].got.size();
    CHECK(party_accept(w, 1, requests[0].group_guid, "SomebodyElse"), "p2 accepts with the wrong inviter name");
    w.settle();
    CHECK(party_frames_in(w[0].got, pc_cls, before_wrong_name_a).empty(), "the wrong inviter sends no frame");
    CHECK(party_accept(w, 1, requests[0].group_guid, requests[0].inviter), "p2 accepts with the invitation's own GUID and inviter");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), ("the correct accept forms the party: " + why).c_str());
}

AOC_REGISTER_TEST(test_party_accept_without_invite_refused);

static void test_party_leave_promotes_earliest() {
    std::printf("a leader's leave promotes the earliest remaining member and every remaining frame follows\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_join = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_join);
    CHECK(requests.size() == 1 && party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts and the party holds three");
    w.settle();
    const size_t before_leave = w[1].got.size();
    const size_t before_leave_c = w[2].got.size();
    const size_t before_leave_a = w[0].got.size();
    CHECK(party_leave(w, 0), "p1, the leader, leaves");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[1].got, pc_cls), {"p2", "p3"}, w.guid_of(1), &why), ("p2's frame lists p2 and p3 with p2 as the leader: " + why).c_str());
    why.clear();
    CHECK(frame_lists(last_party_frame(w[2].got, pc_cls), {"p2", "p3"}, w.guid_of(1), &why), ("p3's frame agrees: " + why).c_str());
    CHECK(frame_is_empty(last_party_frame(w[0].got, pc_cls)), "p1's frame empties");
    CHECK(w[0].got.size() > before_leave_a && w[1].got.size() > before_leave && w[2].got.size() > before_leave_c, "all three players were told");
    // the party drops below two: the last member gets the one-member state and then the empty group
    const size_t before_last = w[1].got.size();
    CHECK(party_leave(w, 2), "p3 leaves");
    w.settle();
    const auto last = party_frames_in(w[1].got, pc_cls, before_last);
    CHECK(last.size() == 2 && last[0].members.size() == 1 && last[0].members[0].name == "p2" && frame_is_empty(last[1]),
          "p2 gets the one-member state and then the empty group");
}

AOC_REGISTER_TEST(test_party_leave_promotes_earliest);

static void test_party_logout_promotes_and_removes() {
    std::printf("a logout removes the member at once, promotes the next and tells the rest only\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_join = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_join);
    CHECK(requests.size() == 1 && party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    w.settle();
    const size_t got_a = w[0].got.size();
    const size_t before_b = w[1].got.size();
    const size_t before_c = w[2].got.size();
    w[0].s->finish("the player logged out");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[1].got, pc_cls), {"p2", "p3"}, w.guid_of(1), &why), ("p2's frame keeps p2 and p3 under p2: " + why).c_str());
    why.clear();
    CHECK(frame_lists(last_party_frame(w[2].got, pc_cls), {"p2", "p3"}, w.guid_of(1), &why), ("p3's frame agrees: " + why).c_str());
    CHECK(w[0].got.size() == got_a, "the logging-out player is sent nothing");
    CHECK(w[1].got.size() > before_b && w[2].got.size() > before_c, "the two remaining players were told");
    // the next leader logs out too: the last member gets the one-member state and then the empty group
    const size_t before_last = w[2].got.size();
    w[1].s->finish("the player logged out");
    w.settle();
    const auto last = party_frames_in(w[2].got, pc_cls, before_last);
    CHECK(last.size() == 2 && last[0].members.size() == 1 && last[0].members[0].name == "p3" && frame_is_empty(last[1]),
          "p3 gets the one-member state and then the empty group");
}

AOC_REGISTER_TEST(test_party_logout_promotes_and_removes);

static void test_party_has_no_saved_state_after_logout() {
    std::printf("a party is world-local: a player who logs out and returns comes back in no party\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const svc::Character p1 = party_record("p1", {0x51a00000u, 0x1000u, 0x2000u, 0x3000u});
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_b = w[1].got.size();
    w[0].s->finish("the player logged out");
    w.settle();
    const auto frames = party_frames_in(w[1].got, pc_cls, before_b);
    CHECK(frames.size() == 2 && frames[0].members.size() == 1 && frame_is_empty(frames[1]), "p1's logout leaves p2 with the one-member state and the empty group");
    CHECK(w.enter(2, p1, identity()), "p1 joins again in a new session");
    if (!w[2].s) return;
    const size_t before_invite = w[2].got.size();
    CHECK(party_invite(w, 1, "p1"), "p2 invites the returning p1");
    w.settle();
    CHECK(party_requests_in(w[2].got, pc_cls, before_invite).size() == 1, "the returning p1 gets an invitation: no party was kept");
}

AOC_REGISTER_TEST(test_party_has_no_saved_state_after_logout);

static void test_party_kick_updates_survivors() {
    std::printf("a kick empties the removed member's frame and updates every remaining frame; only the leader can kick\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_join = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_join);
    CHECK(requests.size() == 1 && party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    w.settle();
    const size_t before_nonleader = w[0].got.size();
    CHECK(party_kick(w, 1, "p3"), "p2, not the leader, tries to kick p3");
    w.settle();
    CHECK(party_frames_in(w[0].got, pc_cls, before_nonleader).empty(), "a non-leader's kick changes no frame");
    CHECK(party_kick(w, 0, "p3"), "the leader kicks p3");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), ("the leader's frame keeps p1 and p2: " + why).c_str());
    why.clear();
    CHECK(frame_lists(last_party_frame(w[1].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), ("p2's frame agrees: " + why).c_str());
    CHECK(frame_is_empty(last_party_frame(w[2].got, pc_cls)), "the kicked player's frame empties");
    const size_t before_missing = w[0].got.size();
    CHECK(party_kick(w, 0, "Nobody_xyz"), "the leader kicks a name that is not in the party");
    w.settle();
    CHECK(party_frames_in(w[0].got, pc_cls, before_missing).empty(), "a kick of a non-member changes no frame");
}

AOC_REGISTER_TEST(test_party_kick_updates_survivors);

static void test_party_transfer_moves_leader_mark() {
    std::printf("a leader transfer moves LeaderGuid and SquadLeaders to the new leader; the master looter stays\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before = w[0].got.size();
    const size_t before_b = w[1].got.size();
    CHECK(party_promote(w, 0, "p2"), "p1 transfers the leadership to p2");
    w.settle();
    const PartyFrame frame_a = last_party_frame(w[0].got, pc_cls);
    const PartyFrame frame_b = last_party_frame(w[1].got, pc_cls);
    std::string why;
    CHECK(frame_lists(frame_a, {"p1", "p2"}, w.guid_of(1), &why), ("p1's frame marks p2 as the leader: " + why).c_str());
    why.clear();
    CHECK(frame_lists(frame_b, {"p1", "p2"}, w.guid_of(1), &why), ("p2's frame marks p2 as the leader: " + why).c_str());
    CHECK(frame_a.leader_guid == w.guid_of(1) && frame_a.squad_leaders.size() == 1 && frame_a.squad_leaders[0] == w.guid_of(1),
          "LeaderGuid and SquadLeaders both name p2");
    CHECK(frame_a.master_looter_guid == w.guid_of(0), "MasterLooterGuid still names p1, as the game's transfer leaves it");
    CHECK(party_settings_in(w[0].got, pc_cls, before).size() == 1 && party_settings_in(w[1].got, pc_cls, before_b).size() == 1,
          "the transfer sends every member exactly one settings update after the group update");
    CHECK(settings_follow_the_first_group_frame(w[0].got, pc_cls, before) && settings_follow_the_first_group_frame(w[1].got, pc_cls, before_b),
          "every member gets the group update and then the settings");
    const size_t before_refused = w[0].got.size();
    CHECK(party_promote(w, 0, "p2"), "p1, no longer the leader, tries to transfer again");
    w.settle();
    CHECK(party_frames_in(w[0].got, pc_cls, before_refused).empty(), "a non-leader's transfer changes no frame");
}

AOC_REGISTER_TEST(test_party_transfer_moves_leader_mark);

static void test_party_settings_after_the_first_accept_and_transfer() {
    std::printf("the settings update follows the first two-member frame and a transfer, not a third member's accept\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const size_t before_a = w[0].got.size();
    const size_t before_b = w[1].got.size();
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form the party");
    CHECK(settings_follow_the_first_two_member_frame(w[0].got, pc_cls, before_a), "p1 gets one settings update after the two-member frame");
    CHECK(settings_follow_the_first_two_member_frame(w[1].got, pc_cls, before_b), "p2 gets it in the same order");
    const size_t after_a = w[0].got.size();
    const size_t after_b = w[1].got.size();
    const size_t after_c = w[2].got.size();
    const size_t before_c_req = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_c_req);
    CHECK(requests.size() == 1, "p3 gets the invitation");
    if (requests.size() != 1) return;
    CHECK(party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    w.settle();
    CHECK(party_settings_in(w[0].got, pc_cls, after_a).empty() && party_settings_in(w[1].got, pc_cls, after_b).empty() &&
              party_settings_in(w[2].got, pc_cls, after_c).empty(),
          "the third member's accept sends no settings update");
    const size_t before_transfer_a = w[0].got.size();
    const size_t before_transfer_b = w[1].got.size();
    const size_t before_transfer_c = w[2].got.size();
    CHECK(party_promote(w, 0, "p2"), "p1 transfers the leadership to p2");
    w.settle();
    CHECK(party_settings_in(w[0].got, pc_cls, before_transfer_a).size() == 1 && party_settings_in(w[1].got, pc_cls, before_transfer_b).size() == 1 &&
              party_settings_in(w[2].got, pc_cls, before_transfer_c).size() == 1,
          "the transfer sends every member exactly one settings update");
    CHECK(settings_follow_the_first_group_frame(w[0].got, pc_cls, before_transfer_a) &&
              settings_follow_the_first_group_frame(w[1].got, pc_cls, before_transfer_b) &&
              settings_follow_the_first_group_frame(w[2].got, pc_cls, before_transfer_c),
          "the transfer's group frame comes before the settings update on every member");
}

AOC_REGISTER_TEST(test_party_settings_after_the_first_accept_and_transfer);

static void test_party_master_looter_follows_the_leader() {
    std::printf("a master looter's leave, kick and logout move the master looter to the current leader\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    // a party of three, then a transfer: the old leader stays the master looter
    CHECK(party_three(w, 0, 1, 2, pc_cls), "p1, p2 and p3 form a party");
    CHECK(party_promote(w, 0, "p2"), "p1 makes p2 the leader");
    w.settle();
    CHECK(last_party_frame(w[0].got, pc_cls).master_looter_guid == w.guid_of(0), "the transfer keeps p1 as the master looter");
    // p1, an ordinary member now, leaves: the next frame every remaining member gets names p2
    CHECK(party_leave(w, 0), "p1, the master looter, leaves");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[1].got, pc_cls), {"p2", "p3"}, w.guid_of(1), &why),
          ("p2's frame still lists p2 and p3 under p2: " + why).c_str());
    CHECK(last_party_frame(w[1].got, pc_cls).master_looter_guid == w.guid_of(1),
          "p2's frame names the current leader p2 as the master looter after the leave");
    CHECK(last_party_frame(w[2].got, pc_cls).master_looter_guid == w.guid_of(1), "p3's frame agrees");
    // a new party; the old leader is kicked after a transfer
    CHECK(party_disband(w, 1), "p2 disbands the two-member party");
    w.settle();
    CHECK(party_three(w, 1, 0, 2, pc_cls), "p2, p1 and p3 form a new party");
    CHECK(party_promote(w, 1, "p1"), "p2 makes p1 the leader");
    w.settle();
    CHECK(last_party_frame(w[1].got, pc_cls).master_looter_guid == w.guid_of(1), "the transfer keeps p2 as the master looter");
    CHECK(party_kick(w, 0, "p2"), "p1, the new leader, kicks p2, the master looter");
    w.settle();
    CHECK(last_party_frame(w[0].got, pc_cls).master_looter_guid == w.guid_of(0),
          "p1's frame names the current leader p1 as the master looter after the kick");
    CHECK(last_party_frame(w[2].got, pc_cls).master_looter_guid == w.guid_of(0), "p3's frame agrees");
    // a third party; the old leader logs out after a transfer
    CHECK(party_disband(w, 0), "p1 disbands the two-member party");
    w.settle();
    CHECK(party_three(w, 0, 1, 2, pc_cls), "p1, p2 and p3 form a third party");
    CHECK(party_promote(w, 0, "p2"), "p1 makes p2 the leader again");
    w.settle();
    CHECK(last_party_frame(w[0].got, pc_cls).master_looter_guid == w.guid_of(0), "the transfer keeps p1 as the master looter");
    w[0].s->finish("the master looter logged out");
    w.settle();
    CHECK(last_party_frame(w[1].got, pc_cls).master_looter_guid == w.guid_of(1),
          "p2's frame names the current leader p2 as the master looter after the logout");
    CHECK(last_party_frame(w[2].got, pc_cls).master_looter_guid == w.guid_of(1), "p3's frame agrees");
}

AOC_REGISTER_TEST(test_party_master_looter_follows_the_leader);

static void test_party_master_looter_who_is_the_leader() {
    std::printf("a leader who is also the master looter leaves without a transfer; the next frames name the new leader, clean or destroyed\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 4), "four players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s || !w[3].s) return;
    CHECK(party_three(w, 0, 1, 2, pc_cls), "p1, p2 and p3 form a party with p1 the leader and master looter");
    CHECK(party_leave(w, 0), "p1 leaves as the leader and master looter");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[1].got, pc_cls), {"p2", "p3"}, w.guid_of(1), &why),
          ("p2's frame still lists p2 and p3 under p2: " + why).c_str());
    CHECK(last_party_frame(w[1].got, pc_cls).master_looter_guid == w.guid_of(1), "p2's frame names the new leader p2 as the master looter");
    CHECK(last_party_frame(w[2].got, pc_cls).master_looter_guid == w.guid_of(1), "p3's frame agrees");
    // a new party; the leader and master looter is destroyed without finish()
    CHECK(party_disband(w, 1), "p2 disbands the two-member party");
    w.settle();
    CHECK(party_three(w, 0, 1, 2, pc_cls), "p1, p2 and p3 form a new party");
    w[0].s.reset();
    w.settle();
    const size_t before_d = w[3].got.size();
    CHECK(party_invite(w, 1, "p4"), "p2, the new leader, invites p4");
    w.settle();
    const auto requests = party_requests_in(w[3].got, pc_cls, before_d);
    CHECK(requests.size() == 1 && party_accept(w, 3, requests[0].group_guid, requests[0].inviter), "p4 accepts");
    w.settle();
    CHECK(last_party_frame(w[1].got, pc_cls).master_looter_guid == w.guid_of(1), "p2's next frame names p2 as the master looter after the destroyed leader");
    CHECK(last_party_frame(w[2].got, pc_cls).master_looter_guid == w.guid_of(1), "p3's next frame agrees");
}

AOC_REGISTER_TEST(test_party_master_looter_who_is_the_leader);

static void test_party_settings_after_a_new_party_from_one() {
    std::printf("a party that drops to one member starts again with one settings update after its new first two-member frame\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    CHECK(party_three(w, 0, 1, 2, pc_cls), "p1, p2 and p3 form a party");
    CHECK(party_leave(w, 2), "p3 leaves");
    w.settle();
    CHECK(party_leave(w, 1), "p2 leaves");
    w.settle();
    CHECK(frame_is_empty(last_party_frame(w[0].got, pc_cls)), "p1's frame is empty");
    const size_t before_a = w[0].got.size();
    const size_t before_b = w[1].got.size();
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a new party");
    CHECK(settings_follow_the_first_two_member_frame(w[0].got, pc_cls, before_a), "p1 gets exactly one settings after the new two-member frame");
    CHECK(settings_follow_the_first_two_member_frame(w[1].got, pc_cls, before_b), "p2 gets it in the same order");
}

AOC_REGISTER_TEST(test_party_settings_after_a_new_party_from_one);

static void test_party_disband_clears_every_frame() {
    std::printf("a disband empties every member's frame and a new party gets a new group GUID\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_join = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_join);
    CHECK(requests.size() == 1 && party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    w.settle();
    const PartyFrame before_disband = last_party_frame(w[0].got, pc_cls);
    const size_t before_a = w[0].got.size();
    const size_t before_b = w[1].got.size();
    const size_t before_c = w[2].got.size();
    const size_t before_nonleader = w[1].got.size();
    CHECK(party_disband(w, 1), "p2, not the leader, tries to disband");
    w.settle();
    CHECK(party_frames_in(w[1].got, pc_cls, before_nonleader).empty(), "a non-leader's disband changes no frame");
    CHECK(party_disband(w, 0), "the leader disbands");
    w.settle();
    CHECK(frame_is_empty(last_party_frame(w[0].got, pc_cls)) && frame_is_empty(last_party_frame(w[1].got, pc_cls)) &&
              frame_is_empty(last_party_frame(w[2].got, pc_cls)),
          "every member's frame empties");
    CHECK(w[0].got.size() > before_a && w[1].got.size() > before_b && w[2].got.size() > before_c, "the empty group reached all three");
    // a second party gets a new group GUID
    const size_t before_second = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2 into a new party");
    w.settle();
    const auto second = party_requests_in(w[1].got, pc_cls, before_second);
    CHECK(second.size() == 1, "p2 gets the new invitation");
    if (second.size() != 1) return;
    CHECK(second[0].group_guid != before_disband.group_guid && second[0].group_guid != kNoPartyGuid, "the new group mints a new, non-zero GUID");
    CHECK(party_accept(w, 1, second[0].group_guid, second[0].inviter), "p2 accepts into the new party");
    w.settle();
    std::string why;
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), ("the new party's frame lists both: " + why).c_str());
}

AOC_REGISTER_TEST(test_party_disband_clears_every_frame);

static void test_party_second_invite_keeps_first() {
    std::printf("a second invitation while one is pending keeps the first and sends no second join request\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const size_t before_b = w[1].got.size();
    const size_t before_c = w[2].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto first = party_requests_in(w[1].got, pc_cls, before_b);
    CHECK(first.size() == 1, "p2 gets the first invitation");
    const size_t before_second = w[0].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3 while p2's invitation is pending");
    w.settle();
    CHECK(party_requests_in(w[2].got, pc_cls, before_c).empty(), "p3 gets no second join request");
    std::string why;
    CHECK(one_system_line(w[0].got, pc_cls, before_second, "p3 cannot join yet: the invitation of p2 is still pending.", &why),
          ("the leader is told whose invitation is still pending: " + why).c_str());
    if (first.size() != 1) return;
    CHECK(party_accept(w, 1, first[0].group_guid, first[0].inviter), "p2 accepts the first invitation");
    w.settle();
    CHECK(frame_lists(last_party_frame(w[0].got, pc_cls), {"p1", "p2"}, w.guid_of(0), &why), ("the first invitation still forms the party: " + why).c_str());
    const size_t before_third = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3 now that no invitation is pending");
    w.settle();
    CHECK(party_requests_in(w[2].got, pc_cls, before_third).size() == 1, "p3's invitation arrives then");
}

AOC_REGISTER_TEST(test_party_second_invite_keeps_first);

static void test_party_decline_removes_pending() {
    std::printf("a decline is a normal refusal: the pending invitation goes and a lone leader's frame empties\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    const size_t before = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    const auto requests = party_requests_in(w[1].got, pc_cls, before);
    CHECK(requests.size() == 1, "p2 gets the invitation");
    if (requests.size() != 1) return;
    const size_t before_leader = w[0].got.size();
    CHECK(party_decline(w, 1, requests[0].group_guid, requests[0].inviter), "p2 declines the invitation");
    w.settle();
    const auto frames = party_frames_in(w[0].got, pc_cls, before_leader);
    CHECK(frames.size() == 2 && frames[0].members.size() == 1 && frame_is_empty(frames[1]), "the leader gets the one-member state and then the empty group");
    const size_t before_again = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2 again");
    w.settle();
    CHECK(party_requests_in(w[1].got, pc_cls, before_again).size() == 1, "a new invitation arrives after the decline");
}

AOC_REGISTER_TEST(test_party_decline_removes_pending);

static void test_party_invited_player_logout_clears_the_invitation() {
    std::printf("an invited player's logout ends the invitation, empties a lone leader's frame and frees the next invite\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const size_t before_invite = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    CHECK(party_requests_in(w[1].got, pc_cls, before_invite).size() == 1, "p2 gets the invitation");
    const size_t before_logout = w[0].got.size();
    w[1].s->finish("the invited player logged out");
    w.settle();
    const auto frames = party_frames_in(w[0].got, pc_cls, before_logout);
    CHECK(frames.size() == 2 && frames[0].members.size() == 1 && frame_is_empty(frames[1]),
          "the lone leader gets the one-member state and then the empty group");
    const size_t before_c = w[2].got.size();
    const size_t before_new = w[0].got.size();
    CHECK(party_invite(w, 0, "p3"), "the leader invites another player at once");
    w.settle();
    CHECK(party_requests_in(w[2].got, pc_cls, before_c).size() == 1, "the other player gets the invitation, not a pending-invitation line");
    CHECK(party_frames_in(w[0].got, pc_cls, before_new).size() == 1 && last_party_frame(w[0].got, pc_cls).members.size() == 1,
          "the new invitation creates the one-member party again");
}

AOC_REGISTER_TEST(test_party_invited_player_logout_clears_the_invitation);

static void test_party_destroyed_invited_player_clears_the_invitation() {
    std::printf("a destroyed invited player's session ends the invitation, empties a lone leader's frame and frees the next invite\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    const size_t before_invite = w[1].got.size();
    CHECK(party_invite(w, 0, "p2"), "p1 invites p2");
    w.settle();
    CHECK(party_requests_in(w[1].got, pc_cls, before_invite).size() == 1, "p2 gets the invitation");
    const size_t before_destroy = w[0].got.size();
    w[1].s.reset();   // the session is destroyed without finish()
    w.settle();
    const auto frames = party_frames_in(w[0].got, pc_cls, before_destroy);
    CHECK(frames.size() == 2 && frames[0].members.size() == 1 && frame_is_empty(frames[1]),
          "the lone leader gets the one-member state and then the empty group");
    const size_t before_c = w[2].got.size();
    const size_t before_new = w[0].got.size();
    CHECK(party_invite(w, 0, "p3"), "the leader invites another player at once");
    w.settle();
    CHECK(party_requests_in(w[2].got, pc_cls, before_c).size() == 1, "the other player gets the invitation, not a pending-invitation line");
    CHECK(party_frames_in(w[0].got, pc_cls, before_new).size() == 1 && last_party_frame(w[0].got, pc_cls).members.size() == 1,
          "the new invitation creates the one-member party again");
}

AOC_REGISTER_TEST(test_party_destroyed_invited_player_clears_the_invitation);

static void test_party_cross_world_is_not_online() {
    std::printf("a player in another world is not online for this world's party\n");
    PartyWorld near;
    PartyWorld far;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const std::array<uint32_t, 4> far_id = {0x51f00001u, 0x1001u, 0x2001u, 0x3001u};
    CHECK(party_enter(near, 1) && far.enter(0, party_record("far1", far_id), identity()), "one player enters each of two worlds");
    if (!near[0].s || !far[0].s) return;
    const std::string far_name = far[0].s->name();
    const size_t before = near[0].got.size();
    CHECK(party_invite(near, 0, far_name), "the near world's player invites the far world's player");
    near.settle();
    std::string why;
    CHECK(one_system_line(near[0].got, pc_cls, before, far_name + " is not online.", &why),
          ("the inviter reads that the other world's player is not online: " + why).c_str());
    CHECK(party_requests_in(far[0].got, pc_cls, 0).empty(), "the far world's player gets no join request");
}

AOC_REGISTER_TEST(test_party_cross_world_is_not_online);

static void test_party_load_answer_carries_group() {
    std::printf("the client's load answer carries the player's group instead of the empty group\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    // a player in no party: the load answer's group is still the original's empty one
    const size_t before_solo = w[0].got.size();
    CHECK(party_send(w, 0, "ServerRequestClientLoad", repl::RpcParams{}), "p1 asks for its load while it is in no party");
    w.settle();
    bool solo_empty = false;
    for (size_t i = before_solo; i < w[0].got.size(); ++i) {
        if (const auto frame = party_frame_of(w[0].got[i], pc_cls)) solo_empty = solo_empty || (frame_is_empty(*frame) && frame->param_bits == 483);
    }
    CHECK(solo_empty, "a player in no party still gets the empty group on the load");
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    const size_t before_load = w[1].got.size();
    CHECK(party_send(w, 1, "ServerRequestClientLoad", repl::RpcParams{}), "p2 asks for its load while it is in the party");
    w.settle();
    PartyFrame loaded;
    for (size_t i = before_load; i < w[1].got.size(); ++i) {
        if (const auto frame = party_frame_of(w[1].got[i], pc_cls)) loaded = *frame;
    }
    std::string why;
    CHECK(frame_lists(loaded, {"p1", "p2"}, w.guid_of(0), &why), ("the load answer carries the two-member group: " + why).c_str());
}

AOC_REGISTER_TEST(test_party_load_answer_carries_group);

static void test_party_group_guid_is_per_group() {
    std::printf("two groups in one world carry two different GUIDs, each steady across its members' frames\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 4), "four players enter the world");
    if (!w[0].s || !w[3].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form the first party");
    CHECK(party_pair(w, 2, 3, pc_cls), "p3 and p4 form the second party");
    const PartyFrame first = last_party_frame(w[0].got, pc_cls);
    const PartyFrame second = last_party_frame(w[2].got, pc_cls);
    CHECK(first.present && second.present, "both parties have a frame");
    CHECK(first.group_guid != kNoPartyGuid && second.group_guid != kNoPartyGuid, "both GUIDs are non-zero");
    CHECK(first.group_guid != second.group_guid, "the two parties have different GUIDs");
    CHECK(last_party_frame(w[1].got, pc_cls).group_guid == first.group_guid && last_party_frame(w[3].got, pc_cls).group_guid == second.group_guid,
          "each member's frame carries its own group's GUID");
}

AOC_REGISTER_TEST(test_party_group_guid_is_per_group);
