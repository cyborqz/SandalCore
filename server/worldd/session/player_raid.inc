// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_raid.inc  (social, step 8: the raid)
//
//  The raid's calls on the PlayerController: ServerConvertPartyToRaid (the convert button, the leader only), ServerAcceptConvertToRaid
//  (the leader's yes to the 9th invitation's ClientRequestConvertToRaid: the convert, then the invitation), ServerConvertRaidToParty,
//  ServerMoveRaidMember(CharacterName, NewSquadNum), ServerSetSquadLeader(CharacterNameToPromote),
//  ServerDemoteSquadLeader(CharacterNameToDemote), ServerSetSquadEnableMemberShouts(bEnableMemberShouts) and
//  ServerSetRaidShoutPrivilege(ShoutPrivilege).  The group is the party's (session/player_party.inc): its GUID, leader, master looter
//  and flat members stay; a raid adds the squads, the group type 1 and the shout privilege.
//
//  A raid holds 2 to 40 members in up to 10 squads of 8; a new member joins the lowest-numbered squad with room; the move, the squad
//  leaders and the shout settings are the raid leader's (the squad's shouts also the squad leader's); the raid shout privilege is kept and
//  sent in RaidSettings.  The frame is the party's ClientUpdateGroup with GroupType 1 and the squads; a party's one squad is built there.
// ============================================================================

void PlayerSession::party_to_raid(PartyGroup& group) {
    if (group.raid) return;
    if (group.members.size() < 2 || group.members.size() > kMaxPartyMembers) {
        send_party_line(*this, "The raid needs at least 2 members.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the convert to a raid is refused: the party holds {}", name(), group.members.size());
        return;
    }
    group.raid = true;
    group.squads.clear();
    GroupSquad squad;
    squad.number = 1;
    squad.members = group.members;
    squad.leader_guid = group.leader_guid;
    squad.member_shouts = true;
    group.squads.push_back(std::move(squad));
    group.shout_privilege = 2;
    send_party_frame(group, "the party became a raid");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: the party became a raid ({} member(s))", name(), group.members.size());
}

void PlayerSession::raid_to_party(PartyGroup& group) {
    if (!group.raid) {
        send_party_line(*this, "The group is not a raid.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a convert to a party is refused: the group is no raid", name());
        return;
    }
    if (group.members.size() > kMaxPartyMembers) {
        send_party_line(*this, "The raid has more than 8 members: it cannot become a party.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: the convert to a party is refused: the raid holds {}", name(), group.members.size());
        return;
    }
    group.raid = false;
    group.squads.clear();
    group.shout_privilege = 2;
    send_party_frame(group, "the raid became a party");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: the raid became a party ({} member(s))", name(), group.members.size());
}

void PlayerSession::on_raid_convert(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        send_party_line(*this, "Only the group leader may convert the party to a raid.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a convert to a raid while it is not the leader is refused", name());
        return;
    }
    party_to_raid(*party_group_);
}

void PlayerSession::on_raid_accept_convert(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    const std::optional<std::string> invited = party_call_name(rpc, "InvitedPlayer");
    if (!invited) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerAcceptConvertToRaid does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a convert accepted while it is not the leader changes nothing", name());
        return;
    }
    // "if you press yes you will convert it": the yes converts first, whatever the invitation then does
    if (!party_group_->raid) party_to_raid(*party_group_);
    invite_one(*invited, now, /*allow_ask=*/false);
}

void PlayerSession::on_raid_convert_to_party(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        send_party_line(*this, "Only the raid leader may convert the raid to a party.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a convert to a party while it is not the leader is refused", name());
        return;
    }
    raid_to_party(*party_group_);
}

void PlayerSession::on_raid_move_member(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !is_party_leader(*party_group_, *this)) {
        send_party_line(*this, "Only the raid leader may move a member.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a move while it is not the raid leader is refused", name());
        return;
    }
    if (!party_group_->raid) {
        send_party_line(*this, "The group is not a raid.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a move in a party is refused", name());
        return;
    }
    const std::optional<std::string> sent = party_call_name(rpc, "CharacterName");
    const std::optional<int64_t> new_squad = party_call_int(rpc, "NewSquadNum");
    if (!sent || !new_squad) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerMoveRaidMember does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    PlayerSession* target = party_member_named(*party_group_, *sent);
    if (!target || !sent->size()) {
        send_party_line(*this, party_shown(*sent) + " is not in the raid.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a move of {}, who is not in the raid, is ignored", name(), party_shown(*sent));
        return;
    }
    GroupSquad* from = squad_of(*party_group_, *target);
    if (!from) {
        send_party_line(*this, party_shown(*sent) + " is not in the raid.");
        return;
    }
    if (*new_squad < 1 || *new_squad > static_cast<int64_t>(kMaxRaidSquads)) {
        send_party_line(*this, target->name() + " cannot be moved: squad " + std::to_string(*new_squad) + " is not a squad from 1 to 10.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a move to squad {} is refused", name(), *new_squad);
        return;
    }
    if (from->number == *new_squad) {
        send_party_line(*this, target->name() + " is already in squad " + std::to_string(*new_squad) + ".");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a move of {} to its own squad is refused", name(), target->name());
        return;
    }
    GroupSquad* to = squad_with_number(*party_group_, *new_squad);
    if (to && to->members.size() >= kRaidSquadSize) {
        send_party_line(*this, target->name() + " cannot join squad " + std::to_string(*new_squad) + ": it has 8 members.");
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a move of {} to a full squad is refused", name(), target->name());
        return;
    }
    const std::string moved = target->name();
    remove_from_squad(*party_group_, *target);
    to = squad_with_number(*party_group_, *new_squad);
    if (!to) {
        party_group_->squads.push_back(GroupSquad{static_cast<int32_t>(*new_squad), {}, {}, true});
        to = &party_group_->squads.back();
    }
    to->members.push_back(target);
    send_party_frame(*party_group_, "a member was moved");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: moves {} to squad {}", name(), moved, *new_squad);
}

void PlayerSession::on_raid_set_squad_leader(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !party_group_->raid || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a squad leader change while it is not the raid leader is refused", name());
        return;
    }
    const std::optional<std::string> sent = party_call_name(rpc, "CharacterNameToPromote");
    if (!sent) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerSetSquadLeader does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    PlayerSession* target = party_member_named(*party_group_, *sent);
    GroupSquad* squad = target ? squad_of(*party_group_, *target) : nullptr;
    if (!target || !squad) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a squad leader change to {}, who is not in the raid, is ignored", name(), party_shown(*sent));
        return;
    }
    if (party_same_guid(squad->leader_guid, party_guid_of(*target))) return;
    squad->leader_guid = party_guid_of(*target);
    send_party_frame(*party_group_, "a squad leader was set");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: makes {} the leader of squad {}", name(), target->name(), squad->number);
}

void PlayerSession::on_raid_demote_squad_leader(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !party_group_->raid || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a squad leader demote while it is not the raid leader is refused", name());
        return;
    }
    const std::optional<std::string> sent = party_call_name(rpc, "CharacterNameToDemote");
    if (!sent) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerDemoteSquadLeader does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    PlayerSession* target = party_member_named(*party_group_, *sent);
    GroupSquad* squad = target ? squad_of(*party_group_, *target) : nullptr;
    if (!target || !squad || !party_same_guid(squad->leader_guid, party_guid_of(*target))) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a demote of {}, who is no squad leader, is ignored", name(), party_shown(*sent));
        return;
    }
    squad->leader_guid = {};
    send_party_frame(*party_group_, "a squad leader was demoted");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: demotes {} as a squad leader", name(), target->name());
}

void PlayerSession::on_raid_set_squad_shouts(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !party_group_->raid) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a squad shout setting in no raid is ignored", name());
        return;
    }
    const std::optional<bool> on = party_call_bool(rpc, "bEnableMemberShouts");
    if (!on) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerSetSquadEnableMemberShouts does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    GroupSquad* squad = squad_of(*party_group_, *this);
    if (!squad) return;
    const bool raid_leader = is_party_leader(*party_group_, *this);
    const bool squad_leader = party_same_guid(squad->leader_guid, party_guid_of(*this));
    if (!raid_leader && !squad_leader) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a squad shout setting while it is no squad leader is ignored", name());
        return;
    }
    squad->member_shouts = *on;
    send_party_frame(*party_group_, "a squad's member shouts changed");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: sets squad {}'s member shouts to {}", name(), squad->number, *on ? "on" : "off");
}

void PlayerSession::on_raid_set_raid_shout_privilege(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!party_group_ || !party_group_->raid || !is_party_leader(*party_group_, *this)) {
        spdlog::log(level_for(party_refused_logged_), "[party] {}: a raid shout privilege while it is not the raid leader is ignored", name());
        return;
    }
    const std::optional<uint64_t> value = party_call_uint(rpc, "ShoutPrivilege");
    if (!value) {
        if (++party_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[party] {}: a ServerSetRaidShoutPrivilege does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    party_group_->shout_privilege = *value;
    send_party_frame(*party_group_, "the raid's shout privilege changed");
    spdlog::log(level_for(party_logged_, 20), "[party] {}: sets the raid's shout privilege to {}", name(), *value);
}

void PlayerSession::register_raid_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerConvertPartyToRaid", [](PlayerSession& s, const RpcContext& c) { s.on_raid_convert(c.conn, c.now); });
    t.add(RpcScope::PlayerController, "ServerAcceptConvertToRaid", [](PlayerSession& s, const RpcContext& c) { s.on_raid_accept_convert(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerConvertRaidToParty", [](PlayerSession& s, const RpcContext& c) { s.on_raid_convert_to_party(c.conn, c.now); });
    t.add(RpcScope::PlayerController, "ServerMoveRaidMember", [](PlayerSession& s, const RpcContext& c) { s.on_raid_move_member(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetSquadLeader", [](PlayerSession& s, const RpcContext& c) { s.on_raid_set_squad_leader(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerDemoteSquadLeader", [](PlayerSession& s, const RpcContext& c) { s.on_raid_demote_squad_leader(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetSquadEnableMemberShouts", [](PlayerSession& s, const RpcContext& c) { s.on_raid_set_squad_shouts(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetRaidShoutPrivilege", [](PlayerSession& s, const RpcContext& c) { s.on_raid_set_raid_shout_privilege(c.conn, c.rpc, c.now); });
}
