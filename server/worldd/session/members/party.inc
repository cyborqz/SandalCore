// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/party.inc: the members of the party feature (session/player_party.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// One world's party.  Its members are the sessions in SquadMembers order (the earliest remaining first after a promotion); the
    /// leader and the master looter are GUIDs, not positions, so a transfer changes the GUID and keeps the roster as it is.  The group
    /// holds one invitation at a time: its invitee, the member who sent it and when it stops being valid.
    struct PartyGroup {
        std::array<uint32_t, 4> guid{};
        std::array<uint32_t, 4> leader_guid{};
        std::array<uint32_t, 4> master_looter_guid{};
        std::vector<PlayerSession*> members;
        std::string             pending_name;
        std::string             pending_inviter;
        Clock::time_point       pending_expires{};
    };

    /// True when the session is in a group.
    bool in_party() const { return party_group_ != nullptr; }

private:
    /// Every tick: an invitation that is five minutes old goes, and a leader still alone gets the one-member state and the empty group.
    void run_party(Connection& conn, Clock::time_point now);
    /// The client calls of the party on the PlayerController.
    void on_party_invite(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_accept(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_decline(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_leave(Connection& conn, Clock::time_point now);
    void on_party_kick(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_transfer(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_party_disband(Connection& conn, Clock::time_point now);
    /// The group a player with none gets at its first invitation: one member, itself as the leader and the master looter.
    void start_party();
    /// One member's frame: the whole group as ClientUpdateGroup carries it, queued on its connection.
    void send_party_frame(PlayerSession& to, const PartyGroup& group, const char* what);
    /// The frame of every member, the leader included.
    void send_party_frame(PartyGroup& group, const char* what);
    /// ClientUpdateGroupSettings with the defaults, once after the first two-member frame of a new party and after a leader transfer.
    void send_party_settings(const PartyGroup& group);
    /// ClientUpdateGroup of the empty group, which clears a player's frame.
    void send_empty_group(PlayerSession& to);
    /// One SYSBROADCAST line from Server on a player's connection (a refused invitation and the reason).
    void send_party_line(PlayerSession& to, const std::string& text);
    /// The load answer: the player's group, or the empty group for a player in none.
    void queue_party_or_empty(Connection& conn);
    /// Removes `who` from its group and tells the members that remain; `tell_who` also clears the removed player's frame, and a
    /// group that drops to one member sends that member the one-member state and then the empty group.
    void remove_from_party(PlayerSession& who, bool tell_who, const char* why);
    /// The invitation of `group` is over: it goes, and a leader still alone gets the one-member state and the empty group.
    void expire_party_invite(PartyGroup& group);
    /// An invitation that waits for this session ends (a lone leader gets the one-member state and the empty group).
    void clear_pending_invitations();
    /// A session that ends leaves its party at once and an invitation waiting for it ends too; the logging-out player is sent nothing.
    void leave_party_on_finish();
    /// A destroyed session is taken out of its group without a frame (the other sessions are ending too); an invitation waiting
    /// for it ends as it does on a logout.
    void forget_party();
    PartyGroup* party_group_ = nullptr;
    size_t      party_logged_ = 0, party_refused_logged_ = 0, party_unreadable_logged_ = 0;
