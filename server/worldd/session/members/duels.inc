// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  session/members/duels.inc: the members of the player's duels (session/player_duels.cpp; the duels are world/duels.h's)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// The world's duel book this session is in (tests), and the duels he fought and won here.
    const world::DuelBook* duel_book_for_test() const { return duel_book_.get(); }
    uint64_t duels_fought() const { return duels_fought_; }
    uint64_t duels_won() const { return duels_won_; }
    /// The four duel calls that passed the session's budget (tests; the calls the budget dropped are not counted).
    size_t duel_calls() const { return duel_calls_; }

private:
    /// The book of the world (at the join), and leaving it (his duel ends, System).
    void connect_duels(PlayerWorld& world);
    void leave_duels(Clock::time_point now);
    /// ServerRequestDuel(InCharacterName), ServerAcceptDuel / ServerDeclineDuel(DuelGuid), ServerForfeitDuel(DuelGuid).
    void on_duel_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_duel_answer(Connection& conn, const protocol::repl::ClientRpc& rpc, bool accept, Clock::time_point now);
    void on_duel_forfeit(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Every tick (drain_pvp): what the book says (the callouts, the end's Health), and the alignments follow the fight.
    void drain_duels(Connection& conn, Clock::time_point now);
    /// A hit he is about to take (take_hit): his duel partner's may not kill him (its Health mods leave him 1 Health, and the duel is lost); another player's harm
    /// ends his duel (System).  `to_mount`: the hit lands on his mount, so his own Health is not judged.  True when the hit was cut short (the book is told he lost).
    bool duel_filter(combat::IncomingHit& h, Clock::time_point now, bool to_mount);
    /// He dies of something else while in a duel: it ends.
    void duel_died(Clock::time_point now);
    /// A duel's callout to him (the text as a callout, quest_toast's shape).
    void duel_callout(Connection& conn, const std::string& text, Clock::time_point now);

    std::shared_ptr<world::DuelBook> duel_book_;
    RequestBudget duel_budget_{kQuietBurst, kQuietInterval};   // the four duel calls (a request and a forfeit loop would spawn and remove a banner actor for every player around)
    size_t duel_calls_ = 0;                                    // the four duel calls that passed the budget
    /// When the end's Health is given back: a moment after the end, so it follows the change of the blow that ended it (its pipeline event goes out after
    /// this tick's notices).
    std::optional<Clock::time_point> duel_heal_at_;
    uint64_t duels_fought_ = 0, duels_won_ = 0;
    size_t   duels_logged_ = 0;
private:
