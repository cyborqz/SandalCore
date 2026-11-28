// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/destiny.inc: the Destiny window's tracks (quest/destiny.h; the code is in quest/session_quests2.cpp)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// The Destiny window's answers sent, and the tracks' quests the session took (tests read them).
    size_t destiny_answers_sent() const { return destiny_answers_; }
    size_t destiny_tracks_seeded() const { return destiny_seeded_; }

private:
    /// ServerGetDestinyClientData: one ClientReceiveDestinyClientData that answers each track the client names (its quest, the guid of the track's own quest instance, the tasks done and
    /// the eight, whether the track is open).  Changes nothing; the requests are budgeted.
    void on_destiny_client_data(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Takes the own quests of the tracks the character may take now (quest::destiny_seeds): at the login and after every level gained.
    void seed_destiny(Connection& conn, Clock::time_point now);
    /// The window's answer again when what it shows changed since the client was last told (a task done raises the count: the list reads the answer, not the quest cache); only once the
    /// window has asked.
    void push_destiny(Connection& conn);
    /// The answer to a request that names `asked`, as the client is told.
    std::vector<protocol::repl::DestinyTrackReply> destiny_reply(const std::vector<int64_t>& asked) const;

    static constexpr uint32_t                  kDestinyBurst = 4;   // ServerGetDestinyClientData: the window asks when it opens
    static constexpr std::chrono::milliseconds kDestinyInterval{500};

    bool          destiny_seed_pending_ = false;   // the tracks' quests are looked at on the next quest tick (set at the login and by every level gained)
    RequestBudget destiny_budget_{kDestinyBurst, kDestinyInterval};
    size_t        destiny_answers_ = 0, destiny_refused_ = 0, destiny_seeded_ = 0, destiny_logged_ = 0;
    std::vector<int64_t>                           destiny_asked_;   // the tracks the window asked for last (empty: it has not asked)
    std::vector<protocol::repl::DestinyTrackReply> destiny_told_;    // what the client was last told of them
