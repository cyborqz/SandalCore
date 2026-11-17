// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/saga.inc: the members of PlayerSession that the Bard's Saga's ally hits added (session/player_abilities.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// Whether this player is of `actor`'s party (or is `actor`'s own pawn): what an ally hit's targeting reads of the players around its caster.
    bool in_party_with(uint64_t actor) const;
    /// A hit of another player's ability landed on this player (a heal, a blessing): its mods, put on this player's own pawn, with the part of each
    /// that took effect; the session's next tick sends the change and the confirmation to this player's client.
    void receive_player_hit(const combat::PlayerIncomingHit& hit, std::vector<double>& effective);
    /// The effects that hit put on this player, their numbers worked out for the two; `instigator` is the caster's actor id, `at` the world's clock.
    void receive_player_effects(const std::vector<effects::Applied>& effects, uint64_t instigator, double at);
    /// A shield the hit gave this player.
    void receive_player_shield(double amount, double seconds, double at);
    /// What the hit takes off this player (the blessings the other three of the Pantheon are).
    void remove_player_effects(const abilities::HitDef& hit, double at);

private:
