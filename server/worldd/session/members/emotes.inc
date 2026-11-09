// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/emotes.inc: the members of the emote feature (session/player_emotes.cpp)
//  Included at the end of PlayerSession (session/members_registered.inc); it sets its own public: and private:.
// ============================================================================
public:
    /// The emote calls the session looked at: the ones that played (or ended the Rest), the ones it refused (a word that is no emote, an ability the content tables do not hold, a refusal of the
    /// ability runtime, a character that cannot) and the ones dropped at the budget.
    size_t emotes_played() const { return emotes_played_; }
    size_t emotes_refused() const { return emotes_refused_; }
    size_t emotes_dropped() const { return emotes_dropped_; }

private:
    /// A burst of eight emote calls, then four a second: an accepted emote costs the client an update of the pawn.
    static constexpr uint32_t                  kEmoteBurst = 8;
    static constexpr std::chrono::milliseconds kEmoteInterval{250};
    /// Server ServerPlayEmote: the word of a typed emote command (/sit, /wave ...) plays its ability or is refused.
    void on_play_emote(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// /sit: the Rest.  Sits the character down; a second /sit, while the Rest runs, ends it.
    void emote_sit(Connection& conn, const std::string& word, Clock::time_point now);
    /// Any other word: its ability, through the generic ability runtime.
    void emote_play(Connection& conn, int64_t ability, const std::string& ability_name, const std::string& word, Clock::time_point now);
    /// Counts and logs a refusal; the client is sent nothing.
    void emote_refusal(const std::string& word, const char* why);
    /// The ClientInstanceId the server gives an ability it starts for an emote: the game client began no instance of its own, so the number is negative and never one of the client's own.
    int32_t next_emote_instance() { return emote_instance_ = emote_instance_ <= -1000000 ? -1 : emote_instance_ - 1; }
    RequestBudget emote_budget_{kEmoteBurst, kEmoteInterval};
    int32_t       emote_instance_ = 0;
    size_t        emotes_played_ = 0, emotes_refused_ = 0, emotes_dropped_ = 0;
    size_t        emote_logged_ = 0, emote_refusals_logged_ = 0, emote_unreadable_logged_ = 0, emote_shown_failures_ = 0;

    // The emotes the other players see: the emote ability items of this player's own pawn (the Rest's, Emote_Wave's ...) are written to the pawn the other players are shown (the registry's
    // actor of kind Player, session/player_presence.cpp), whose AbilityComponent every view that holds the pawn sends its client as it sends any actor's update.
    /// The emote ability items the player's own pawn holds now, by ServerInstanceId, kept from the ability changes whether or not the pawn the others are shown is in the world yet.
    std::map<int64_t, bootstrap::ActiveAbilityItem> emote_items_;
    /// ... and the ones the pawn the others are shown holds: the element of its ReplicatedActiveAbilities and the item as it was written.
    struct ShownEmote {
        int32_t                      element = 0;
        bootstrap::ActiveAbilityItem item;
    };
    std::map<int64_t, ShownEmote> emote_seen_;
    bool                          emote_dirty_ = false;   // emote_items_ changed since the other players' pawn was brought in line
    /// Keeps emote_items_ in line with an ability change that was applied to the player's own pawn (take_ability_events).
    void note_emote_items(const bootstrap::AbilityChange& change);
    /// Every tick, after sync_presence: brings the pawn the other players are shown in line with emote_items_ when they differ (tick_logic, so that the registry is written before the view passes).
    void run_emotes(Connection& conn, Clock::time_point now);
private:
