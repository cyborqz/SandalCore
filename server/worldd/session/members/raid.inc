// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/raid.inc: the members of the raid feature (session/player_raid.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    // (the raid's state and its squad_key are in session/members/party.inc; register_raid_rpcs is declared by AOC_RPC_FEATURE)
private:
    /// The convert button and the yes: a party of 2 to 8 becomes a raid (the yes converts even when its invitation then fails).
    void on_raid_convert(Connection& conn, Clock::time_point now);
    void on_raid_accept_convert(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The convert back: a raid of 8 or fewer becomes the one party.
    void on_raid_convert_to_party(Connection& conn, Clock::time_point now);
    /// The move and the squad leaders: the raid leader only.
    void on_raid_move_member(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_raid_set_squad_leader(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_raid_demote_squad_leader(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The caller's own squad's member shouts (its squad leader or the raid leader); the raid's shout privilege (the raid leader).
    void on_raid_set_squad_shouts(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_raid_set_raid_shout_privilege(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The group's party-to-raid and raid-to-party conversions, with their lines and the frame.
    void party_to_raid(PartyGroup& group);
    void raid_to_party(PartyGroup& group);
private:
