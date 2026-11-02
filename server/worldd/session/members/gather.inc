// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/gather.inc: the members of PlayerSession that the gathering feature (session/player_gather.cpp) added after the split
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// Makes the pile of the stacks a full bag could not take, where the player stood. When the world cannot, the stacks are lost and the log says so.
    void make_gather_pile(GatherPile& pile);
    /// The end of the session for gathering: the channel is cancelled and the piles that still wait for their time are made now.
    void end_gathering(const char* why);
