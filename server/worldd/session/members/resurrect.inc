// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/resurrect.inc: the members of the Resurrect, the revive of another player in place (Social, seeing other players: their death)
//  Included at the end of the class (session/members_registered.inc); it sets its own public: and private:.
//  The behaviour is in session/player_resurrect.inc, included by session/player_combat.cpp.
// ============================================================================
public:
    /// The ServerResurrect calls this session was given and the ones it refused (the Cleric's side).
    size_t resurrect_requests() const { return resurrect_requests_; }
    size_t resurrect_refusals() const { return resurrect_refusals_; }
    /// The resurrection prompts this session sent a dead player (the Cleric's side).
    size_t resurrect_prompts_sent() const { return resurrect_prompts_sent_; }
    /// The resurrection prompts this session was given (a dead player's side), the resurrections it took, and the ones it turned down or lost.
    size_t resurrect_prompts() const { return resurrect_prompts_; }
    size_t resurrections() const { return resurrections_; }
    size_t resurrect_declines() const { return resurrect_declines_; }
    /// Whether this session waits for an answer to a resurrection it was offered, as of its last tick (false: none, or the one it had is over: its minute ran out or its life did).
    bool resurrect_offer_pending() const { return resurrect_offer_live(last_now_); }

private:
    /// One resurrection this session was offered (another player's Resurrection): who asked, the ResurrectRecordBase's values, and when the prompt runs out.
    struct ResurrectOffer {
        uint64_t                instigator = 0;               // the Cleric's pawn (the actor the hit names)
        std::array<uint32_t, 4> instigator_character{};       // his character GUID (what ClientPromptResurrect carries)
        std::string             instigator_name;
        int64_t                 record = 0;
        double                  health_ratio = 0.25, mana_ratio = 0.25, stamina_ratio = 1.0;
        size_t                  life = 0;                     // the death it was made for (deaths_ then): a prompt does not outlive the life it asked about
        Clock::time_point       expires{};
    };
    /// Whether the prompt this session was offered waits for an answer at `now`: it is for the death he is in and its minute is not over.
    bool resurrect_offer_live(Clock::time_point now) const { return resurrect_offer_ && dead_ && resurrect_offer_->life == deaths_ && now <= resurrect_offer_->expires; }
    /// The offer of the resurrect record `record`, asked by the player of `from` (the values are the record's; the instigator is filled by the caller).
    static ResurrectOffer resurrect_offer_of(int64_t record, const bootstrap::PlayerSetup& from);
    /// ServerResurrect(CharacterName) on the Cleric's PlayerController: the client confirms the Resurrection cast it made at the dead player.
    void on_server_resurrect(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// ServerPromptedResurrectResponse(Response) on the dead player's PlayerController: his answer to the prompt.
    void on_prompted_resurrect_response(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// A Cleric's Resurrection asks this (dead) session: the prompt goes to his client, or (a record without a prompt) he is revived at once.
    void offer_resurrect(const ResurrectOffer& offer, Clock::time_point now);
    /// The revive in place with the record's values: alive, the shares of the maxima, where he fell.  The watchers' pawn follows at this session's next tick.
    void revive_in_place(const ResurrectOffer& offer, Clock::time_point now);
    /// A Resurrection hit the ability runtime landed on a dead player (his client's hit verification): that player's session is asked (or revived).
    void take_resurrect_request(Clock::time_point now);
    /// The offer whose prompt waits (and its deadline), and the counters.
    std::optional<ResurrectOffer> resurrect_offer_;
    /// The Cleric whose prompt was turned down last, and until when his asks are ignored (the cast the player declined also calls ServerResurrect and lands its hit).
    uint64_t          resurrect_declined_by_ = 0;
    Clock::time_point resurrect_declined_until_{};
    size_t resurrect_requests_ = 0, resurrect_refusals_ = 0, resurrect_prompts_sent_ = 0;
    size_t resurrect_prompts_ = 0, resurrections_ = 0, resurrect_declines_ = 0, resurrect_logged_ = 0;
private:
