// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/fishing.inc: the members of PlayerSession that fishing (session/player_fishing.cpp) added
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// What fishing did in this session, for the log and the tests: the casts taken, the ones refused (ClientEndAbility), the casts that came down on dry land, the bites, the results that
    /// caught a fish and the ones that caught nothing, and the casts that ended by a branch or were replaced.
    size_t fishing_casts() const { return fishing_casts_; }
    size_t fishing_refused() const { return fishing_refused_; }
    size_t fishing_failed() const { return fishing_failed_; }
    size_t fishing_bites() const { return fishing_bites_; }
    size_t fishing_catches() const { return fishing_catches_; }
    size_t fishing_empty() const { return fishing_empty_; }
    size_t fishing_ended() const { return fishing_ended_; }
    /// A cast is running (its loop, its end included).
    bool   fishing_active() const { return fishing_ && fishing_->cast.active(); }

private:
    /// The ServerTriggerAbility of the fishing ability.  True when fishing took the call (it ran the cast, refused it, or dropped a replay of it); false when the world does not fish (no water
    /// file, --no-fishing) or the call is another ability's: the generic runtime goes on as before.
    bool fishing_takes_trigger(Connection& conn, int64_t ability, int32_t client_instance, Clock::time_point now);
    /// Every tick: the cast's branches (movement, combat, the rod), the steps that fell due, and the wire of each.
    void run_fishing(Connection& conn, Clock::time_point now);
    /// The cast ends with no result (the session ends, or another cast replaces it); nothing is sent.
    void end_fishing(const char* why);
    /// One step of the cast's timeline as the pawn's items and effects (fishing/cast.h has when each falls due).
    void fishing_step(Connection& conn, Clock::time_point now, const fishing::Event& e);
    /// The result of a loop: the rewards rolled for this player and rod, and delivered like any harvest (deliver_harvest).
    void fishing_result(Connection& conn, Clock::time_point now);
    /// Applies the change to the pawn's ability items, effects and cooldowns and sends it; false when the pawn refuses it (abilities are then off for the session).
    bool fishing_apply(Connection& conn, bootstrap::AbilityChange& change, const std::string& note);

    struct FishingRun {
        fishing::Cast                cast;
        fishing::Landing             landing;
        int32_t                      client_id = 0;           // the client's ClientInstanceId (a refusal names it)
        int64_t                      instance = -1;           // the ServerInstanceId of the cast's ability item (-1 until the reply)
        int64_t                      effect_id = -1;          // the Fishing_Buff's EffectId while it is on the pawn
        int32_t                      animation_counter = 0;
        const content::GatherDefRow* def = nullptr;           // the interaction of the water the lure came down on (null: the cast fails)
        std::string                  water;                   // the water's actor, for the log
        std::string                  tool_guid;               // the rod's instance when the cast began
        std::vector<int64_t>         replaced_items;          // the item and the buff of the cast this one replaced: they go with the reply
        std::vector<int64_t>         replaced_effects;
        double                       cast_x = 0, cast_y = 0;      // where the lure came down
        double                       start_x = 0, start_y = 0;    // where the player stood: a cast is of that spot, and walking or being moved away from it ends it
        size_t                       catches = 0, empty = 0;
    };
    std::optional<FishingRun>        fishing_;
    int64_t                          fishing_item_ = -1;       // the ability item of the last cast that the pawn still lists (it is removed when the next cast replaces it)
    std::deque<int32_t>              fishing_recent_;          // the ClientInstanceIds seen lately: a trigger that comes twice is one cast
    double                           fishing_cooldown_until_ = -1;
    size_t                           fishing_logged_ = 0, fishing_done_logged_ = 0;
    size_t                           fishing_casts_ = 0, fishing_refused_ = 0, fishing_failed_ = 0, fishing_bites_ = 0, fishing_catches_ = 0, fishing_empty_ = 0, fishing_ended_ = 0;
