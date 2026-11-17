// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  session/members/party_stats.inc: the members of the party's live stats (session/player_party_stats.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// How many live-stat updates and party frames this session has sent (the log and a test of the cadence).
    size_t party_stats_sends() const { return party_stats_sends_; }
    size_t party_frame_sends() const { return party_frame_sends_; }

private:
    /// Every tick (run_party): the SocialComponent's PlayerStatsReplication items follow the party's members, at most one update a second.
    void sync_party_stats(Connection& conn, Clock::time_point now);
    /// ... and the party frame follows the members' stats and places: at once for a level, an alive state or a maximum, and with the roster's refresh otherwise.
    void sync_party_frame(Connection& conn, Clock::time_point now);
    /// What the social component's array holds now (the members' data last set on it), and when the next update may go.
    std::vector<bootstrap::GroupMember> party_stats_sent_;
    Clock::time_point                   party_stats_due_{};
    /// A party frame went out for a change of the membership (or the load's answer): it is the frame the next refresh compares against, and the roster's refresh starts from it.
    void note_party_frame_sent(std::vector<bootstrap::GroupMember> members);
    /// What the last party frame carried, and when the next one may go: the coalescing floor and the roster's refresh.
    std::vector<bootstrap::GroupMember> party_frame_sent_;
    Clock::time_point                   party_frame_due_{};
    Clock::time_point                   party_frame_refresh_due_{};
    size_t                              party_stats_sends_ = 0, party_frame_sends_ = 0, party_sync_logged_ = 0;
private:
