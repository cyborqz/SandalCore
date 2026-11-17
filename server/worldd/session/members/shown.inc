// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/shown.inc: the members of PlayerSession that keep the pawn the other players are shown in line with the player (Social, seeing other players)
//  Included at the end of the class (session/members_registered.inc); it sets its own public: and private:.  session/shown_to_others.h says where each kind of thing is shown.
// ============================================================================
public:
    /// The mount the pawn the other players are shown has out and the mount it rides, as that pawn says them now (guid.ObjectId 0: none) (tests).
    protocol::emit::FIntrepidNetworkGUID shown_mountable_pet() const { return shown_pet_; }
    protocol::emit::FIntrepidNetworkGUID shown_mount() const { return shown_mount_; }

private:
    /// Every tick, after sync_presence and run_emotes (tick_logic, so that the registry is written before the view passes): brings the pawn the other players are shown in line with the
    /// player.  Each part of seeing other players adds its own `sync_*` call here and writes through show_in_pawn (the combat stances are session/members/stance.inc).
    void run_shown(Connection& conn, Clock::time_point now);
    /// The one place a change to the pawn the other players are shown is made: `fn(WorldActor&)` runs on the registry's Player actor of this player, which counts the change (`kind`: a
    /// position alone is Change::Movement, anything else State).  False when the pawn is not in the world (yet).
    template <class F>
    bool show_in_pawn(world::Change kind, F&& fn) {
        return registry_ && world_pawn_ != 0 && registry_->modify(world_pawn_, kind, std::forward<F>(fn));
    }
    /// The mount the player has out (InteractInfo.MountablePet) and the mount it rides (the attachment and MountPtr), written to the pawn the other players are shown (session/player_shown.cpp).
    void sync_shown_mount(Clock::time_point now);
    /// While the player rides: the mount's motion for the other players (WorldActor::mount_shown) from the moves of the ride.
    void sync_shown_ride(Clock::time_point now);
    /// Social, gear and level: the held items (all 23 marked, `shown_gear_marks_` times), the appearance list and the two EveryoneProxy stat arrays, from the inventory and the player's pawn.
    void sync_shown_gear();
    /// ... the weapon set in hand (CombatInfo.bIsUsingRangedWeapon) and the stats its weapon line gives.
    void sync_shown_weapon();
    /// ... the character's level, with the level-up the players around it are shown (BroadcastPlayLevelUpEffects: the PlayerInfo announcement).
    void sync_shown_level();
    /// ... the level-up calls of every player whose announcement this session has not answered yet: one call of its own on each of those pawns' channels (the call is a multicast in the game).
    void sync_shown_level_ups(Connection& conn);
    /// ... the current Health, MaxHealth, Mana and MaxMana, from the player's pawn: every change of them is shown (damage, a heal, regeneration, an effect that
    /// changes a maximum), and no change means no update of the shown pawn.
    void sync_shown_stats();
    /// ... the death bit (CombatInfo.bIsDead), kept equal to the player's own life: written in the same tick as the Health and Mana 0 of a death, so
    /// that the watchers get them in one update, and cleared with the values of a respawn or a revive.
    void sync_shown_death();
    /// The death bit the pawn the other players are shown carries now (tests).
    bool shown_dead() const { return shown_dead_; }
    /// The player's equipment changed in an update that moved `marks` equipment slots: the pawn the others are shown takes its look, held items and stats at the next tick.
    void note_shown_gear(size_t marks) {
        shown_gear_dirty_ = true;
        shown_gear_marks_ += marks != 0 ? marks : 1;
    }
    /// What the player wears, as the inventory holds it now, goes out to the player's own client (the HeldItems and AppearanceIDs update: the 23 held items marked `marks` times) at once, or
    /// at `when`, and is noted for the pawn the other players are shown (note_shown_gear).  The one place a change of what the player wears is made, so that no way of putting something on
    /// (an item used, a crate made) leaves the other players out.
    void queue_worn_gear(Connection& conn, size_t marks, std::optional<Clock::time_point> when = std::nullopt);
    /// The player's weapon set in hand changed (a swap, or a weapon worn or taken off): the flag and the stats at the next tick.
    void note_shown_weapon() { shown_weapon_dirty_ = true; }
    /// The player gained a level: the new level and the effect the players around it are shown at the next tick.
    void note_shown_level(int level) {
        shown_level_dirty_ = true;
        shown_level_ = level;
    }
    /// The world's clock, on the mount's own clock: what its stamps are written on.
    double world_clock(Clock::time_point now) const;
    /// The mount controller said the pawn's mountable pet is `pet` (an invalid guid: none).
    void note_shown_pet(const protocol::emit::FIntrepidNetworkGUID& pet) {
        wanted_pet_ = pet;
    }
    protocol::emit::FIntrepidNetworkGUID wanted_pet_;          // what the mount controller last said (the Effects' pet)
    protocol::emit::FIntrepidNetworkGUID shown_pet_;           // ... and what the pawn the others are shown says
    protocol::emit::FIntrepidNetworkGUID shown_mount_;         // the mount that pawn is attached to (none: it walks)
    // the ride's motion, as the moves of the player's own mount gave it
    GoodPosition                         ride_at_;             // the place of the last motion written
    bool                                 ride_moving_ = false;
    Clock::time_point                    ride_moved_at_{};
    size_t                               shown_failures_ = 0;  // the pawn the others are shown did not take a value (said at most kLoggedPerKind times)
    // gear and level, marked where the player's own pawn changes and written by the syncs above
    bool                                 shown_gear_dirty_ = false;
    size_t                               shown_gear_marks_ = 0;    // the held items' marks the change owes the array
    bool                                 shown_weapon_dirty_ = false;
    bool                                 shown_level_dirty_ = false;
    int                                  shown_level_ = 0;
    bool                                 shown_dead_ = false;      // the death bit the pawn the others are shown carries
    size_t                               shown_death_logged_ = 0;
    /// The announcement serial each other player's pawn had when this session last looked at it (a pawn it never looked at is not an effect to play: its open has the level).
    std::unordered_map<uint64_t, uint64_t> shown_level_ups_;
    uint64_t                             shown_level_ups_seen_ = 0;     // the registry's count when it last looked
    uint64_t                             shown_players_seen_ = 0;       // ... and its count of the players that came in or went (a new player is looked at once before it levels)
private:
