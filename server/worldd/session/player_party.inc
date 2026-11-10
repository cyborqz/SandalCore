// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_party.inc  (social, step 8: the party)
//
//  The party of two to eight.  It is world-local and has no saved state: a session that ends leaves it.  The client calls
//  ServerInviteToGroup(CharacterName), ServerAcceptJoinGroup(GroupGuid, InviterName), ServerDeclineJoinGroup(GroupGuid,
//  InviterName), ServerLeaveGroup, ServerKickFromGroup(CharacterNameToKick), ServerTransferGroupLeadership(CharacterNameToPromote)
//  and ServerDisbandGroup; the server answers the invitee with ClientRequestJoinGroup(GroupGuid, InviterName, GroupType) and
//  every member with ClientUpdateGroup(NewGroup), the leader included.  A new two-member party also gets one
//  ClientUpdateGroupSettings with the settings a party starts with, and a leader transfer gets one after its group update.
//
//  What a group holds: its FGuid (minted once, kept for the group's life), LeaderGuid, MasterLooterGuid, the members in
//  SquadMembers order, and one pending invitation (the invitee, the member who sent it, and the five-minute expiry).  The leader
//  is LeaderGuid and nothing else; SquadLeaders carries the current leader's GUID as well, and the frame never reads it as the
//  mark.  MasterLooterGuid is the master looter: the leader when the group is made, and it stays at a transfer.
//
//  The frame: the members' names, character GUIDs, classes, their online flag and the waiting stats of this step (zero health
//  and mana, level 1).  Step 9 fills the stats and the positions.  The members are the sessions that are in the world now.
//
//  The rules: a party holds 8 members and a 9th is refused; one pending invitation at a time; a name that is not online, a
//  player already in a party and a name that already has an invitation are refused with one SYSBROADCAST line and no request.
//  A leave, a kick, a leader change, a disband and a logout reach every member's frame; the leader's leave or logout promotes
//  the earliest remaining member, and a group that drops to one member sends that member the one-member state and then the
//  empty group.  A disband sends every member the empty group.  An invitation that is five minutes old is gone, and a leader
//  still alone gets the one-member state and the empty group; the invited player's session ending ends the invitation the same way.
// ============================================================================
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

/// The most members a party holds.
constexpr size_t kMaxPartyMembers = 8;

/// How long an invitation stands.
constexpr std::chrono::minutes kPartyInviteLifetime{5};

/// The chat category of a server line.
constexpr int kPartySysBroadcast = 124;

/// The letters A to Z folded to lower case; every other byte is as it is.
char party_fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

/// True when two names are the same without regard to the case of A to Z (the directory looks a name up the same way).
bool party_same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return party_fold(x) == party_fold(y); });
}

/// True when two GUIDs are the same four words.
bool party_same_guid(const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b) { return a == b; }

/// A player's text as it may go into the log: a control character becomes '?', and a long text is cut.
std::string party_shown(const std::string& text) {
    std::string out = text.substr(0, 80);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    }
    if (text.size() > 80) out += "...";
    return out;
}

/// The four words of a character GUID as the group frame carries them.
std::array<uint32_t, 4> party_guid_of(const PlayerSession& s) {
    const auto& g = s.bootstrap().setup().character_guid;
    return {g[0], g[1], g[2], g[3]};
}

/// A random FGuid for a new group (one per group, kept for its life).
std::array<uint32_t, 4> mint_party_guid() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::array<uint32_t, 4> guid{};
    for (uint32_t& word : guid) word = static_cast<uint32_t>(rng());
    return guid;
}

/// The groups of this process: one world's sessions only (a name is looked up in the world's directory before it joins one).
std::vector<std::unique_ptr<PlayerSession::PartyGroup>>& party_groups() {
    static std::vector<std::unique_ptr<PlayerSession::PartyGroup>> groups;
    return groups;
}

/// Takes `group` out of the registry.  The caller has cleared the members' pointers.
void erase_group(PlayerSession::PartyGroup* group) {
    auto& groups = party_groups();
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [group](const std::unique_ptr<PlayerSession::PartyGroup>& held) { return held.get() == group; }),
                 groups.end());
}

/// The group whose invitation names `name` and carries `guid`, or nullptr.
PlayerSession::PartyGroup* find_pending_group(const std::array<uint32_t, 4>& guid, const std::string& name) {
    for (const auto& group : party_groups()) {
        if (!group->pending_name.empty() && party_same_guid(group->guid, guid) && party_same_name(group->pending_name, name)) return group.get();
    }
    return nullptr;
}

/// The group that has an invitation for `name`, or nullptr (any invitation to that player is already pending).
PlayerSession::PartyGroup* find_pending_for(const std::string& name) {
    for (const auto& group : party_groups()) {
        if (!group->pending_name.empty() && party_same_name(group->pending_name, name)) return group.get();
    }
    return nullptr;
}

/// The group frame's data: the GUIDs and the members in their order.
bootstrap::GroupClientData party_frame_data(const PlayerSession::PartyGroup& group) {
    bootstrap::GroupClientData data;
    data.group_guid = group.guid;
    data.leader_guid = group.leader_guid;
    data.master_looter_guid = group.master_looter_guid;
    for (PlayerSession* member : group.members) {
        if (!member) continue;
        bootstrap::GroupMember shown;
        shown.name = member->name();
        shown.guid = party_guid_of(*member);
        shown.primary_class = member->class_id();
        data.members.push_back(std::move(shown));
    }
    return data;
}

/// The leader's mark compared with `who`.
bool is_party_leader(const PlayerSession::PartyGroup& group, const PlayerSession& who) { return party_same_guid(group.leader_guid, party_guid_of(who)); }

/// The name a call carries at `path`, empty when the string is not on the wire; nullopt when the call does not decode.
std::optional<std::string> party_call_name(const repl::ClientRpc& rpc, const char* path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    const std::string* text = value ? value->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

/// The four int32 words of the FGuid at `prefix` of a client call; nullopt when the call does not decode.
std::optional<std::array<uint32_t, 4>> party_call_guid(const repl::ClientRpc& rpc, const char* prefix) {
    if (!rpc.call.decoded) return std::nullopt;
    std::array<uint32_t, 4> words{};
    for (int i = 0; i < 4; ++i) {
        const std::string path = std::string(prefix) + "." + "ABCD"[i];
        const auto word = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), 0);
        if (!word) return std::nullopt;
        words[static_cast<size_t>(i)] = static_cast<uint32_t>(*word);
    }
    return words;
}

}  // namespace

void PlayerSession::start_party() {
    auto group = std::make_unique<PartyGroup>();
    group->guid = mint_party_guid();
    group->leader_guid = party_guid_of(*this);
    group->master_looter_guid = group->leader_guid;
    group->members.push_back(this);
    party_group_ = group.get();
    party_groups().push_back(std::move(group));
}

void PlayerSession::send_party_frame(PlayerSession& to, const PartyGroup& group, const char* what) {
    Connection* conn = to.chat_entry_.connection;
    if (!conn || to.finished_ || to.failed_) return;
    const bootstrap::GroupClientData data = party_frame_data(group);
    to.queue(what, [data](bootstrap::PlayerBootstrap& pb) { return pb.group_update(data); }, *conn);
}

void PlayerSession::send_party_frame(PartyGroup& group, const char* what) {
    for (PlayerSession* member : group.members) {
        if (member) send_party_frame(*member, group, what);
    }
}

void PlayerSession::send_party_settings(const PartyGroup& group) {
    for (PlayerSession* member : group.members) {
        if (!member) continue;
        Connection* conn = member->chat_entry_.connection;
        if (!conn || member->finished_ || member->failed_) continue;
        member->queue("ClientUpdateGroupSettings", [](bootstrap::PlayerBootstrap& pb) { return pb.group_settings(); }, *conn);
    }
}

void PlayerSession::send_empty_group(PlayerSession& to) {
    Connection* conn = to.chat_entry_.connection;
    if (!conn || to.finished_ || to.failed_) return;
    to.queue("ClientUpdateGroup (empty)", [](bootstrap::PlayerBootstrap& pb) { return pb.empty_group(); }, *conn);
}

void PlayerSession::send_party_line(PlayerSession& to, const std::string& text) {
    Connection* conn = to.chat_entry_.connection;
    if (!conn || to.finished_ || to.failed_) return;
    to.queue("ClientReceiveChatMessages (party)", [text](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(text, kPartySysBroadcast, "Server"); }, *conn);
}

void PlayerSession::queue_party_or_empty(Connection& conn) {
    if (party_group_) {
        const bootstrap::GroupClientData data = party_frame_data(*party_group_);
        queue("ClientUpdateGroup", [data](bootstrap::PlayerBootstrap& pb) { return pb.group_update(data); }, conn);
    } else {
        queue("ClientUpdateGroup", [](bootstrap::PlayerBootstrap& pb) { return pb.empty_group(); }, conn);
    }
}

void PlayerSession::expire_party_invite(PartyGroup& group) {
    group.pending_name.clear();
    group.pending_inviter.clear();
    if (group.members.size() != 1) return;
    send_party_frame(group, "the invitation expired");
    PlayerSession* last = group.members.front();
    send_empty_group(*last);
    last->party_group_ = nullptr;
    erase_group(&group);
}

void PlayerSession::remove_from_party(PlayerSession& who, bool tell_who, const char* why) {
    PartyGroup* group = who.party_group_;
    if (!group) return;
    const auto at = std::find(group->members.begin(), group->members.end(), &who);
    if (at == group->members.end()) {
        who.party_group_ = nullptr;
        return;
    }
    const bool was_leader = is_party_leader(*group, who);
    const bool was_master_looter = party_same_guid(group->master_looter_guid, party_guid_of(who));
    group->members.erase(at);
    who.party_group_ = nullptr;
    if (tell_who) send_empty_group(who);
    if (group->members.empty()) {
        erase_group(group);
        return;
    }
    if (was_leader) {
        group->leader_guid = party_guid_of(*group->members.front());
    }
    if (was_master_looter) group->master_looter_guid = group->leader_guid;
    if (group->members.size() == 1) {
        send_party_frame(*group, "the party is down to one member");
        PlayerSession* last = group->members.front();
        send_empty_group(*last);
        last->party_group_ = nullptr;
        erase_group(group);
        return;
    }
    send_party_frame(*group, why);
}

void PlayerSession::clear_pending_invitations() {
    // an invitation that waits for this session ends with it; a leader still alone then gets the empty group, as on an expiry
    std::vector<PartyGroup*> waiting;
    for (const auto& group : party_groups()) {
        if (!group->pending_name.empty() && party_same_name(group->pending_name, name())) waiting.push_back(group.get());
    }
    for (PartyGroup* group : waiting) {
        if (group->pending_name.empty()) continue;
        spdlog::log(level_for(party_logged_, 20), "[party] {}: the invited player's session ending ends the invitation",
                    group->members.empty() ? name() : group->members.front()->name());
        expire_party_invite(*group);
    }
}

void PlayerSession::leave_party_on_finish() {
    clear_pending_invitations();
    remove_from_party(*this, /*tell_who=*/false, "a member logged out");
}

void PlayerSession::forget_party() {
    clear_pending_invitations();
    PartyGroup* group = party_group_;
    party_group_ = nullptr;
    if (!group) return;
    const auto at = std::find(group->members.begin(), group->members.end(), this);
    if (at == group->members.end()) return;
    const bool was_leader = is_party_leader(*group, *this);
    const bool was_master_looter = party_same_guid(group->master_looter_guid, party_guid_of(*this));
    group->members.erase(at);
    if (group->members.empty()) {
        erase_group(group);
        return;
    }
    if (was_leader) {
        group->leader_guid = party_guid_of(*group->members.front());
    }
    if (was_master_looter) group->master_looter_guid = group->leader_guid;
}

void PlayerSession::run_party(Connection& conn, Clock::time_point now) {
    (void)conn;
    std::vector<PartyGroup*> expired;
    for (const auto& group : party_groups()) {
        if (!group->pending_name.empty() && now >= group->pending_expires) expired.push_back(group.get());
    }
    for (PartyGroup* group : expired) {
        if (group->pending_name.empty() || now < group->pending_expires) continue;
        spdlog::log(level_for(party_logged_, 20), "[party] {}: the invitation of {} expired", group->members.empty() ? name() : group->members.front()->name(),
                    group->pending_name);
        expire_party_invite(*group);
    }
}

void PlayerSession::on_party_invite(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    const std::optional<std::string> sent = party_call_name(rpc, "CharacterName");
    if (!sent) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerInviteToGroup does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    if (sent->empty()) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an invitation with no name is refused", name());
        return;
    }
    if (party_same_name(*sent, name())) {
        send_party_line(*this, *sent + " cannot join: that is your own name.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an invitation of its own name is refused", name());
        return;
    }
    ChatEntry* entry = ChatEntry::find(world_, *sent);
    if (!entry || !entry->self) {
        send_party_line(*this, *sent + " is not online.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: not online", name(), party_shown(*sent));
        return;
    }
    PlayerSession& target = *entry->self;
    if (target.party_group_) {
        send_party_line(*this, target.name() + " is already in a party.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: already in a party", name(), target.name());
        return;
    }
    if (find_pending_for(target.name())) {
        send_party_line(*this, target.name() + " already has a party invitation pending.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: an invitation is pending", name(), target.name());
        return;
    }
    if (!party_group_) {
        start_party();
        send_party_frame(*party_group_, "the party is created");
        spdlog::info("[party] {}: the party is created with its leader alone", name());
    }
    PartyGroup& group = *party_group_;
    if (!group.pending_name.empty()) {
        send_party_line(*this, target.name() + " cannot join: an invitation is already pending.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: {} is still pending", name(), target.name(), group.pending_name);
        return;
    }
    if (group.members.size() >= kMaxPartyMembers) {
        send_party_line(*this, target.name() + " cannot join: the party is full.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: the party holds {}", name(), target.name(), group.members.size());
        return;
    }
    Connection* target_conn = target.chat_entry_.connection;
    if (!target_conn) {
        send_party_line(*this, target.name() + " is not online.");
        return;
    }
    group.pending_name = target.name();
    group.pending_inviter = name();
    group.pending_expires = now + kPartyInviteLifetime;
    const std::array<uint32_t, 4> group_guid = group.guid;
    const std::string inviter = name();
    target.queue("ClientRequestJoinGroup", [group_guid, inviter](bootstrap::PlayerBootstrap& pb) { return pb.group_invite(group_guid, inviter); }, *target_conn);
    spdlog::log(level_for(party_logged_, 20), "[party] {}: invites {} ({} member(s) in the party)", name(), target.name(), group.members.size());
}

void PlayerSession::on_party_accept(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    const std::optional<std::array<uint32_t, 4>> group_guid = party_call_guid(rpc, "GroupGuid");
    const std::optional<std::string> inviter = party_call_name(rpc, "InviterName");
    if (!group_guid || !inviter) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerAcceptJoinGroup does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    PartyGroup* group = find_pending_group(*group_guid, name());
    if (!group || !party_same_name(group->pending_inviter, *inviter)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept of a group it was not invited to is refused", name());
        return;
    }
    if (now >= group->pending_expires) {
        expire_party_invite(*group);
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept after the invitation expired is refused", name());
        return;
    }
    if (party_group_) {
        expire_party_invite(*group);
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept while it is in a party is refused", name());
        return;
    }
    if (group->members.size() >= kMaxPartyMembers) {
        send_party_line(*this, "the party is full.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept of a full party is refused", name());
        return;
    }
    group->pending_name.clear();
    group->pending_inviter.clear();
    const bool first_two_members = group->members.size() == 1;
    group->members.push_back(this);
    party_group_ = group;
    send_party_frame(*group, "a member joined the party");
    if (first_two_members) send_party_settings(*group);
    spdlog::log(level_for(party_logged_, 20), "[party] {}: joins the party ({} member(s))", name(), group->members.size());
}

void PlayerSession::on_party_decline(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    const std::optional<std::array<uint32_t, 4>> group_guid = party_call_guid(rpc, "GroupGuid");
    const std::optional<std::string> inviter = party_call_name(rpc, "InviterName");
    if (!group_guid || !inviter) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerDeclineJoinGroup does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    PartyGroup* group = find_pending_group(*group_guid, name());
    if (!group || !party_same_name(group->pending_inviter, *inviter)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a decline of a group it was not invited to is ignored", name());
        return;
    }
    if (now >= group->pending_expires) {
        expire_party_invite(*group);
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a decline after the invitation expired is ignored", name());
        return;
    }
    group->pending_name.clear();
    group->pending_inviter.clear();
    if (group->members.size() == 1) {
        send_party_frame(*group, "the invitation was declined");
        PlayerSession* last = group->members.front();
        send_empty_group(*last);
        last->party_group_ = nullptr;
        erase_group(group);
    }
    spdlog::log(level_for(party_logged_, 20), "[party] {}: declines the invitation", name());
}

void PlayerSession::on_party_leave(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a leave while it is in no party is ignored", name());
        return;
    }
    remove_from_party(*this, /*tell_who=*/true, "a member left the party");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: leaves its party", name());
}

void PlayerSession::on_party_kick(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a kick while it is not the leader is refused", name());
        return;
    }
    const std::optional<std::string> sent = party_call_name(rpc, "CharacterNameToKick");
    if (!sent || sent->empty()) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a kick with no name is refused", name());
        return;
    }
    PlayerSession* target = nullptr;
    for (PlayerSession* member : party_group_->members) {
        if (member && member != this && party_same_name(member->name(), *sent)) {
            target = member;
            break;
        }
    }
    if (!target) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a kick of {}, who is not in the party, is ignored", name(), party_shown(*sent));
        return;
    }
    const std::string removed = target->name();
    remove_from_party(*target, /*tell_who=*/true, "a member was removed from the party");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: kicks {}", name(), removed);
}

void PlayerSession::on_party_transfer(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a leader change while it is not the leader is refused", name());
        return;
    }
    const std::optional<std::string> sent = party_call_name(rpc, "CharacterNameToPromote");
    if (!sent || sent->empty()) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a leader change with no name is refused", name());
        return;
    }
    PlayerSession* target = nullptr;
    for (PlayerSession* member : party_group_->members) {
        if (member && member != this && party_same_name(member->name(), *sent)) {
            target = member;
            break;
        }
    }
    if (!target) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a leader change to {}, who is not in the party, is ignored", name(), party_shown(*sent));
        return;
    }
    party_group_->leader_guid = party_guid_of(*target);
    send_party_frame(*party_group_, "the leader changed");
    send_party_settings(*party_group_);
    spdlog::log(level_for(party_logged_, 20), "[party] {}: makes {} the leader", name(), target->name());
}

void PlayerSession::on_party_disband(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a disband while it is not the leader is refused", name());
        return;
    }
    PartyGroup* group = party_group_;
    const std::vector<PlayerSession*> members = group->members;
    for (PlayerSession* member : members) {
        member->party_group_ = nullptr;
        send_empty_group(*member);
    }
    erase_group(group);
    spdlog::log(level_for(party_logged_, 20), "[party] {}: disbands the party ({} member(s))", name(), members.size());
}

void PlayerSession::register_party_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerInviteToGroup", [](PlayerSession& s, const RpcContext& c) { s.on_party_invite(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerAcceptJoinGroup", [](PlayerSession& s, const RpcContext& c) { s.on_party_accept(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerDeclineJoinGroup", [](PlayerSession& s, const RpcContext& c) { s.on_party_decline(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerLeaveGroup", [](PlayerSession& s, const RpcContext& c) { s.on_party_leave(c.conn, c.now); });
    t.add(RpcScope::PlayerController, "ServerKickFromGroup", [](PlayerSession& s, const RpcContext& c) { s.on_party_kick(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerTransferGroupLeadership", [](PlayerSession& s, const RpcContext& c) { s.on_party_transfer(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerDisbandGroup", [](PlayerSession& s, const RpcContext& c) { s.on_party_disband(c.conn, c.now); });
}
