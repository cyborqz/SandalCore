// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/resurrect.inc: the members of PlayerSession for another player's Resurrect (session/player_resurrect.cpp)
//  Included at the end of the class (session/members_registered.inc).
// ============================================================================
public:
    /// Another player's Resurrect landed and the client's alert has not answered yet (the tests).
    bool resurrect_offered() const { return resurrect_offered_; }

private:
    /// Another player's Resurrect landed on this dead player (the world said so at the last tick): the client's alert (ClientPromptResurrect) names him.
    void offer_resurrect(Connection& conn, const std::array<uint32_t, 4>& by);
    /// ServerPromptedResurrectResponse(Response): the alert's answer.  An accepted one raises the player where he fell (Cleric_Resurrect: 25% Health, 25% Mana, all
    /// Stamina); a declined one leaves him dead, with the respawn window.
    void on_resurrect_response(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);

    bool resurrect_offered_ = false;
private:
