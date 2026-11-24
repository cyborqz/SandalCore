// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/party.inc: the members of the party and raid feature (session/player_party.inc, session/player_raid.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// One squad of a raid: its number (1 to 10), its members in the group's order, its one squad leader (a zero GUID: none) and
    /// its member-shouts setting.  A party's one squad is built from the flat roster and the leader when the frame is made, not stored.
    struct GroupSquad {
        int32_t                    number = 1;
        std::vector<PlayerSession*> members;
        std::array<uint32_t, 4>    leader_guid{};
        bool                       member_shouts = true;
    };

    /// One invitation waiting for a player: the invited name, the member who sent it and when it stops being valid.
    struct GroupInvite {
        std::string       name;
        std::string       inviter;
        Clock::time_point expires{};
    };

    /// One world's group: a party or a raid.  Its members are the sessions in SquadMembers order (the earliest remaining first after a
    /// promotion); the leader and the master looter are GUIDs, not positions, so a transfer changes the GUID and keeps the roster as it
    /// is.  The flat `members` roster is the whole group in join order; a raid's `squads` partition it, while a party's one squad is built
    /// (a party's members are all of its one squad).  Several invitations may wait at once, one per invited player.
    struct PartyGroup {
        std::array<uint32_t, 4>     guid{};
        std::array<uint32_t, 4>     leader_guid{};
        std::array<uint32_t, 4>     master_looter_guid{};
        std::vector<PlayerSession*> members;
        std::vector<GroupInvite>    invitations;
        bool                        raid = false;
        std::vector<GroupSquad>     squads;             // a raid's non-empty squads, in number order; a party's is built
        uint64_t                    shout_privilege = 2;   // RaidSettings.ShoutPrivilege of a raid
    };

    /// True when the session is in a group.
    bool in_party() const { return party_group_ != nullptr; }
    /// True when the session's group is a raid.
    bool in_raid() const { return party_group_ != nullptr && party_group_->raid; }
    /// The key of the squad the session is in, for the combat tiers: a party's one squad is its group; a raid's is the member's squad
    /// (0 when the session is in no group or in none of its squads).
    uint64_t squad_key() const {
        if (!party_group_) return 0;
        if (!party_group_->raid) return reinterpret_cast<uintptr_t>(party_group_);
        for (const GroupSquad& squad : party_group_->squads) {
            for (const PlayerSession* member : squad.members) {
                if (member == this) return reinterpret_cast<uintptr_t>(&squad);
            }
        }
        return 0;
    }

private:
    /// Every tick: the invitations that are five minutes old go, and a leader still alone with nothing waiting gets the one-member
    /// state and the empty group.
    void run_party(Connection& conn, Clock::time_point now);
    /// The client calls of the party and the raid on the PlayerController.
    void on_party_invite(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_accept(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_decline(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_leave(Connection& conn, Clock::time_point now);
    void on_party_kick(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_transfer(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_disband(Connection& conn, Clock::time_point now);
    /// The group a player with none gets at its first invitation: one member, itself as the leader and the master looter.
    void start_party();
    /// One invitation of `sent`: the target refusals, the start of a party, a full party's ask to the leader (`allow_ask`) or its one
    /// line, a raid's full line, and the ClientRequestJoinGroup.  True when the invitation or the ask went out.
    bool invite_one(const std::string& sent, Clock::time_point now, bool allow_ask);
    /// One member's frame: the whole group as ClientUpdateGroup carries it, queued on its connection.
    void send_party_frame(PlayerSession& to, const PartyGroup& group, const char* what);
    /// The frame of every member, the leader included.
    void send_party_frame(PartyGroup& group, const char* what);
    /// ClientUpdateGroupSettings with the defaults, once after the first two-member frame of a new party and after a party's leader transfer.
    void send_party_settings(const PartyGroup& group);
    /// ClientUpdateGroup of the empty group, which clears a player's frame.
    void send_empty_group(PlayerSession& to);
    /// One SYSBROADCAST line from Server on a player's connection (a refused invitation, a refused accept and the reason).
    void send_party_line(PlayerSession& to, const std::string& text);
    /// The load answer: the player's group, or the empty group for a player in none.
    void queue_party_or_empty(Connection& conn);
    /// Removes `who` from its group and tells the members that remain; `tell_who` also clears the removed player's frame, and a group
    /// that drops to one member sends that member the one-member state and then the empty group.
    void remove_from_party(PlayerSession& who, bool tell_who, const char* why);
    /// The invitation of `group` for `name` is over: it goes, and a leader still alone with nothing waiting gets the one-member state and
    /// the empty group.
    void expire_invite(PartyGroup& group, const std::string& name);
    /// An invitation that waits for this session ends (a lone leader with nothing left gets the one-member state and the empty group).
    void clear_pending_invitations();
    /// A session that ends leaves its party at once and an invitation waiting for it ends too; the logging-out player is sent nothing.
    void leave_party_on_finish();
    /// A destroyed session is taken out of its group without a frame (the other sessions are ending too); an invitation waiting
    /// for it ends as it does on a logout.
    void forget_party();
    PartyGroup* party_group_ = nullptr;
    size_t      party_logged_ = 0, party_refused_logged_ = 0, party_unreadable_logged_ = 0;
