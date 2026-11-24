// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_party.inc  (social, step 8: the party)
//
//  The group of two to forty: a party of two to eight, and a raid of two to forty in up to ten squads of eight.  It is world-local and
//  has no saved state: a session that ends leaves it.  The client calls ServerInviteToGroup(CharacterName), ServerAcceptJoinGroup(GroupGuid,
//  InviterName), ServerDeclineJoinGroup(GroupGuid, InviterName), ServerLeaveGroup, ServerKickFromGroup(CharacterNameToKick),
//  ServerTransferGroupLeadership(CharacterNameToPromote) and ServerDisbandGroup for the party, and the raid calls in session/player_raid.inc;
//  the server answers the invitee with ClientRequestJoinGroup(GroupGuid, InviterName, GroupType) and every member with ClientUpdateGroup(NewGroup),
//  the leader included.  A new two-member party also gets one ClientUpdateGroupSettings with the defaults, and a party's leader transfer gets
//  one after its group update.
//
//  What a group holds: its FGuid (minted once, kept for the group's life), LeaderGuid, MasterLooterGuid, the flat members in SquadMembers
//  order, and the invitations that wait (one per invited player: the invitee, the member who sent it, and the five-minute expiry).  The
//  leader is LeaderGuid and nothing else.  MasterLooterGuid is the master looter: the leader when the group is made, and it stays at a
//  transfer.  A raid also holds its squads (the number, the members, one squad leader GUID and the member-shouts setting); a party's one
//  squad is built from the flat roster and the leader when the frame is made.
//
//  The frame: the members' names, character GUIDs, classes, their online flag and their stats (the health and mana shares of their maxima, the
//  maxima, the level, the alive state, where they stand and which way they face).  The live values ride the SocialComponent's
//  PlayerStatsReplication items as well (player_party_stats.inc), and the frame itself is refreshed about every 4.5 s and on every
//  membership change.  The members are the sessions that are in the world now.
//
//  The rules: a party holds 8 members and a 9th invitation asks its leader whether to convert to a raid (ClientRequestConvertToRaid; his
//  yes ServerAcceptConvertToRaid converts and invites); a raid holds 40 members; several invitations wait at once, each five minutes; a
//  name that is not online, a player already in a party and a name that already has an invitation are refused with one SYSBROADCAST line and
//  no request.  A leave, a kick, a leader change, a disband and a logout reach every member's frame; the leader's leave or logout promotes
//  the earliest remaining member, and a group that drops to one member sends that member the one-member state and then the empty group.  A
//  disband sends every member the empty group.  An invitation that is five minutes old is gone, and a leader still alone with nothing
//  waiting gets the one-member state and the empty group; the invited player's session ending ends the invitation the same way.
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

/// The most members a raid holds, the most squads and the most members one squad holds (the game's GroupConstants).
constexpr size_t kMaxRaidMembers = 40;
constexpr size_t kMaxRaidSquads = 10;
constexpr size_t kRaidSquadSize = 8;

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

/// The invitation of `group` for `name`, or nullptr.
PlayerSession::GroupInvite* find_invite(PlayerSession::PartyGroup& group, const std::string& name) {
    for (PlayerSession::GroupInvite& invite : group.invitations) {
        if (party_same_name(invite.name, name)) return &invite;
    }
    return nullptr;
}

/// The group whose invitation names `name` and carries `guid`, or nullptr.
PlayerSession::PartyGroup* find_pending_group(const std::array<uint32_t, 4>& guid, const std::string& name) {
    for (const auto& group : party_groups()) {
        if (party_same_guid(group->guid, guid) && find_invite(*group, name)) return group.get();
    }
    return nullptr;
}

/// The group that has an invitation for `name`, or nullptr (any invitation to that player is already pending).
PlayerSession::PartyGroup* find_pending_for(const std::string& name) {
    for (const auto& group : party_groups()) {
        if (find_invite(*group, name)) return group.get();
    }
    return nullptr;
}

/// The member of `group` named `name`, or nullptr.
PlayerSession* party_member_named(PlayerSession::PartyGroup& group, const std::string& name) {
    for (PlayerSession* member : group.members) {
        if (member && party_same_name(member->name(), name)) return member;
    }
    return nullptr;
}

/// The member of `group` whose character GUID is `guid`, or nullptr.
PlayerSession* party_member_with_guid(PlayerSession::PartyGroup& group, const std::array<uint32_t, 4>& guid) {
    for (PlayerSession* member : group.members) {
        if (member && party_same_guid(party_guid_of(*member), guid)) return member;
    }
    return nullptr;
}

/// The squad of a raid `who` is in, or nullptr.
PlayerSession::GroupSquad* squad_of(PlayerSession::PartyGroup& group, PlayerSession& who) {
    for (PlayerSession::GroupSquad& squad : group.squads) {
        for (PlayerSession* member : squad.members) {
            if (member == &who) return &squad;
        }
    }
    return nullptr;
}

/// The squad of a raid with number `number`, or nullptr.
PlayerSession::GroupSquad* squad_with_number(PlayerSession::PartyGroup& group, int64_t number) {
    for (PlayerSession::GroupSquad& squad : group.squads) {
        if (squad.number == number) return &squad;
    }
    return nullptr;
}

/// The lowest-numbered squad with room, or a new squad at the lowest free number (a raid of fewer than ten full squads always has one).
PlayerSession::GroupSquad* squad_with_room(PlayerSession::PartyGroup& group) {
    for (int64_t number = 1; number <= static_cast<int64_t>(kMaxRaidSquads); ++number) {
        PlayerSession::GroupSquad* squad = squad_with_number(group, number);
        if (!squad) {
            group.squads.push_back(PlayerSession::GroupSquad{static_cast<int32_t>(number), {}, {}, true});
            return &group.squads.back();
        }
        if (squad->members.size() < kRaidSquadSize) return squad;
    }
    return nullptr;
}

/// Puts `who` in the lowest-numbered squad with room.
void add_to_squad(PlayerSession::PartyGroup& group, PlayerSession& who) {
    if (PlayerSession::GroupSquad* squad = squad_with_room(group)) squad->members.push_back(&who);
}

/// Takes `who` out of its squad; a squad leader stops being one, and an emptied squad goes.
void remove_from_squad(PlayerSession::PartyGroup& group, PlayerSession& who) {
    const std::array<uint32_t, 4> guid = party_guid_of(who);
    for (size_t i = 0; i < group.squads.size();) {
        PlayerSession::GroupSquad& squad = group.squads[i];
        const auto at = std::find(squad.members.begin(), squad.members.end(), &who);
        if (at == squad.members.end()) {
            ++i;
            continue;
        }
        squad.members.erase(at);
        if (party_same_guid(squad.leader_guid, guid)) squad.leader_guid = {};
        if (squad.members.empty()) {
            group.squads.erase(group.squads.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

/// One member of the frame as the client reads it: the name, the character GUID and the class, and the stats the party window draws.  The health and mana shares are the
/// current value over its maximum; the alive state is 0 alive and 1 dead (the game's 2-bit values beyond that are not in the read data); the place is the last good
/// position in whole centimetres and the heading its yaw in degrees.
bootstrap::GroupMember party_member_of(const PlayerSession& member) {
    bootstrap::GroupMember shown;
    shown.name = member.name();
    shown.guid = party_guid_of(member);
    shown.primary_class = member.class_id();
    const stats::CharacterStats* cs = member.character_stats();
    const auto value = [cs](int64_t record) {
        const stats::StatValue* v = cs ? cs->find(record) : nullptr;
        return v ? v->value : 0.0f;
    };
    shown.max_health = value(content::kGuidMaxHealth);
    shown.max_mana = value(content::kGuidMaxMana);
    shown.health_percent = shown.max_health > 0 ? value(content::kGuidHealth) / shown.max_health : 0.0f;
    shown.mana_percent = shown.max_mana > 0 ? value(content::kGuidMana) / shown.max_mana : 0.0f;
    shown.level = member.progress().level;
    shown.alive_status = member.dead() ? 1 : 0;
    const GoodPosition at = member.movement().good();
    shown.location = repl::quantize_vector(1, at.x, at.y, at.z);   // (FVector_NetQuantize: whole centimetres)
    shown.heading = static_cast<float>(at.yaw_degrees);
    return shown;
}

/// The group frame's data: the GUIDs, the flat roster, the group type, the raid settings and the squads (a party's one squad is built here).
bootstrap::GroupClientData party_frame_data(const PlayerSession::PartyGroup& group) {
    bootstrap::GroupClientData data;
    data.group_guid = group.guid;
    data.leader_guid = group.leader_guid;
    data.master_looter_guid = group.master_looter_guid;
    data.group_type = group.raid ? 1 : 0;
    data.shout_privilege = group.shout_privilege;
    for (PlayerSession* member : group.members) {
        if (!member) continue;
        data.members.push_back(party_member_of(*member));
    }
    if (!group.raid) {
        bootstrap::GroupSquadData squad;
        squad.number = 1;
        squad.members = data.members;
        squad.leaders.push_back(group.leader_guid);
        squad.member_shouts = true;
        data.squads.push_back(std::move(squad));
    } else {
        for (const PlayerSession::GroupSquad& held : group.squads) {
            bootstrap::GroupSquadData squad;
            squad.number = held.number;
            for (PlayerSession* member : held.members) {
                if (!member) continue;
                squad.members.push_back(party_member_of(*member));
            }
            if (held.leader_guid != std::array<uint32_t, 4>{}) squad.leaders.push_back(held.leader_guid);
            squad.member_shouts = held.member_shouts;
            data.squads.push_back(std::move(squad));
        }
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

/// The int32 at `path` of a client call; nullopt when the call does not decode or the number is not there.
std::optional<int64_t> party_call_int(const repl::ClientRpc& rpc, const char* path) {
    if (!rpc.call.decoded) return std::nullopt;
    return repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), 0);
}

/// The enum number at `path` of a client call; nullopt when the call does not decode or the number is not there.
std::optional<uint64_t> party_call_uint(const repl::ClientRpc& rpc, const char* path) {
    if (!rpc.call.decoded) return std::nullopt;
    return repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), 0);
}

/// The bool at `path` of a client call; nullopt when the call does not decode or the flag is not there.
std::optional<bool> party_call_bool(const repl::ClientRpc& rpc, const char* path) {
    if (!rpc.call.decoded) return std::nullopt;
    return repl::rpcf::bool_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path), false);
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

bool PlayerSession::invite_one(const std::string& sent, Clock::time_point now, bool allow_ask) {
    if (sent.empty()) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an invitation with no name is refused", name());
        return false;
    }
    if (party_same_name(sent, name())) {
        send_party_line(*this, sent + " cannot join: that is your own name.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an invitation of its own name is refused", name());
        return false;
    }
    ChatEntry* entry = ChatEntry::find(world_, sent);
    if (!entry || !entry->self) {
        send_party_line(*this, sent + " is not online.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: not online", name(), party_shown(sent));
        return false;
    }
    PlayerSession& target = *entry->self;
    if (target.party_group_) {
        send_party_line(*this, target.name() + " is already in a party.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: already in a party", name(), target.name());
        return false;
    }
    if (find_pending_for(target.name())) {
        send_party_line(*this, target.name() + " already has a party invitation pending.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: an invitation is pending", name(), target.name());
        return false;
    }
    if (!party_group_) {
        start_party();
        send_party_frame(*party_group_, "the party is created");
        spdlog::info("[party] {}: the party is created with its leader alone", name());
    }
    PartyGroup& group = *party_group_;
    Connection* target_conn = target.chat_entry_.connection;
    if (!target_conn) {
        send_party_line(*this, target.name() + " is not online.");
        return false;
    }
    if (!group.raid && group.members.size() >= kMaxPartyMembers) {
        if (allow_ask && is_party_leader(group, *this)) {
            Connection* leader_conn = chat_entry_.connection;
            if (!leader_conn) return false;
            const std::string invited = target.name();
            queue("ClientRequestConvertToRaid", [invited](bootstrap::PlayerBootstrap& pb) { return pb.convert_to_raid_request(invited); }, *leader_conn);
            spdlog::log(level_for(party_logged_, 20), "[party] {}: the 9th invitation of {} asks the leader to convert to a raid", name(), target.name());
            return true;
        }
        send_party_line(*this, target.name() + " cannot join: the party is full.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: the party holds {}", name(), target.name(), group.members.size());
        return false;
    }
    if (group.raid && group.members.size() >= kMaxRaidMembers) {
        send_party_line(*this, target.name() + " cannot join: the raid is full.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the invitation of {} is refused: the raid holds {}", name(), target.name(), group.members.size());
        return false;
    }
    group.invitations.push_back(GroupInvite{target.name(), name(), now + kPartyInviteLifetime});
    const std::array<uint32_t, 4> group_guid = group.guid;
    const std::string inviter = name();
    const uint64_t group_type = group.raid ? 1 : 0;
    target.queue("ClientRequestJoinGroup",
                 [group_guid, inviter, group_type](bootstrap::PlayerBootstrap& pb) { return pb.group_invite(group_guid, inviter, group_type); }, *target_conn);
    spdlog::log(level_for(party_logged_, 20), "[party] {}: invites {} ({} member(s) in the group)", name(), target.name(), group.members.size());
    return true;
}

void PlayerSession::send_party_frame(PlayerSession& to, const PartyGroup& group, const char* what) {
    Connection* conn = to.chat_entry_.connection;
    if (!conn || to.finished_ || to.failed_) return;
    const bootstrap::GroupClientData data = party_frame_data(group);
    to.note_party_frame_sent(data.members);   // (the frame the next refresh compares against)
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
        note_party_frame_sent(data.members);
        queue("ClientUpdateGroup", [data](bootstrap::PlayerBootstrap& pb) { return pb.group_update(data); }, conn);
    } else {
        party_frame_sent_.clear();
        queue("ClientUpdateGroup", [](bootstrap::PlayerBootstrap& pb) { return pb.empty_group(); }, conn);
    }
}

void PlayerSession::expire_invite(PartyGroup& group, const std::string& name) {
    const auto at = std::find_if(group.invitations.begin(), group.invitations.end(), [&name](const GroupInvite& invite) { return party_same_name(invite.name, name); });
    if (at == group.invitations.end()) return;
    group.invitations.erase(at);
    if (!group.invitations.empty() || group.members.size() != 1) return;
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
    if (group->raid) remove_from_squad(*group, who);
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
        if (group->raid) {
            group->raid = false;
            group->squads.clear();
            group->shout_privilege = 2;
        }
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
    // an invitation that waits for this session ends with it; a lone leader with nothing left then gets the empty group
    std::vector<PartyGroup*> waiting;
    for (const auto& group : party_groups()) {
        if (find_invite(*group, name())) waiting.push_back(group.get());
    }
    for (PartyGroup* group : waiting) {
        if (!find_invite(*group, name())) continue;
        spdlog::log(level_for(party_logged_, 20), "[party] {}: the invited player's session ending ends the invitation",
                    group->members.empty() ? name() : group->members.front()->name());
        expire_invite(*group, name());
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
    if (group->raid) remove_from_squad(*group, *this);
    if (group->members.empty()) {
        erase_group(group);
        return;
    }
    if (was_leader) {
        group->leader_guid = party_guid_of(*group->members.front());
    }
    if (was_master_looter) group->master_looter_guid = group->leader_guid;
    if (group->members.size() == 1 && group->raid) {
        group->raid = false;
        group->squads.clear();
        group->shout_privilege = 2;
    }
}

void PlayerSession::run_party(Connection& conn, Clock::time_point now) {
    std::vector<PartyGroup*> due;
    for (const auto& group : party_groups()) {
        for (const GroupInvite& invite : group->invitations) {
            if (now >= invite.expires) {
                due.push_back(group.get());
                break;
            }
        }
    }
    for (PartyGroup* group : due) {
        bool removed = false;
        for (auto it = group->invitations.begin(); it != group->invitations.end();) {
            if (now < it->expires) {
                ++it;
                continue;
            }
            spdlog::log(level_for(party_logged_, 20), "[party] {}: the invitation of {} expired", group->members.empty() ? name() : group->members.front()->name(), it->name);
            it = group->invitations.erase(it);
            removed = true;
        }
        if (removed && group->invitations.empty() && group->members.size() == 1) {
            send_party_frame(*group, "the invitation expired");
            PlayerSession* last = group->members.front();
            send_empty_group(*last);
            last->party_group_ = nullptr;
            erase_group(group);
        }
    }
    // the group's live stats: the social component's array follows the members' values, and the frame's stats and places follow them at their own cadence
    sync_party_stats(conn, now);
    sync_party_frame(conn, now);
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
    invite_one(*sent, now, /*allow_ask=*/true);
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
    GroupInvite* invite = group ? find_invite(*group, name()) : nullptr;
    if (!group || !invite || !party_same_name(invite->inviter, *inviter)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept of a group it was not invited to is refused", name());
        return;
    }
    if (now >= invite->expires) {
        expire_invite(*group, name());
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept after the invitation expired is refused", name());
        return;
    }
    if (party_group_) {
        expire_invite(*group, name());
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept while it is in a group is refused", name());
        return;
    }
    const bool raid = group->raid;
    if ((!raid && group->members.size() >= kMaxPartyMembers) || (raid && group->members.size() >= kMaxRaidMembers)) {
        const std::string full = raid ? "the raid is full." : "the party is full.";
        send_party_line(*this, full);
        PlayerSession* leader = party_member_with_guid(*group, group->leader_guid);
        if (leader) send_party_line(*leader, name() + (raid ? " cannot join: the raid is full." : " cannot join: the party is full."));
        expire_invite(*group, name());
        spdlog::log(level_for(party_refused_logged_), "[party] {}: an accept of a full {} is refused", name(), raid ? "raid" : "party");
        return;
    }
    const bool first_two_members = group->members.size() == 1;
    group->invitations.erase(std::remove_if(group->invitations.begin(), group->invitations.end(),
                                            [this](const GroupInvite& held) { return party_same_name(held.name, name()); }),
                             group->invitations.end());
    group->members.push_back(this);
    party_group_ = group;
    if (raid) add_to_squad(*group, *this);
    send_party_frame(*group, "a member joined the party");
    if (first_two_members) send_party_settings(*group);
    spdlog::log(level_for(party_logged_, 20), "[party] {}: joins the group ({} member(s))", name(), group->members.size());
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
    GroupInvite* invite = group ? find_invite(*group, name()) : nullptr;
    if (!group || !invite || !party_same_name(invite->inviter, *inviter)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a decline of a group it was not invited to is ignored", name());
        return;
    }
    if (now >= invite->expires) {
        expire_invite(*group, name());
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a decline after the invitation expired is ignored", name());
        return;
    }
    expire_invite(*group, name());
    spdlog::log(level_for(party_logged_, 20), "[party] {}: declines the invitation", name());
}

void PlayerSession::on_party_leave(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a leave while it is in no group is ignored", name());
        return;
    }
    remove_from_party(*this, /*tell_who=*/true, "a member left the party");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: leaves its group", name());
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
    PlayerSession* target = party_member_named(*party_group_, *sent);
    if (!target || target == this) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a kick of {}, who is not in the group, is ignored", name(), party_shown(*sent));
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
    PlayerSession* target = party_member_named(*party_group_, *sent);
    if (!target || target == this) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a leader change to {}, who is not in the group, is ignored", name(), party_shown(*sent));
        return;
    }
    party_group_->leader_guid = party_guid_of(*target);
    // a party's one squad is built from the leader, so there is no squad leader to move; a raid's squad marks stay as they are
    send_party_frame(*party_group_, "the leader changed");
    if (!party_group_->raid) send_party_settings(*party_group_);
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
    spdlog::log(level_for(party_logged_, 20), "[party] {}: disbands the group ({} member(s))", name(), members.size());
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

// The party's live stats (social, live Health and Mana): the social component's items and the frame's refresh, in a file of their own.
#include "session/player_party_stats.inc"

// The raid's calls and the squads (the converts, the move, the squad leaders, the shouts), in a file of their own.
#include "session/player_raid.inc"
