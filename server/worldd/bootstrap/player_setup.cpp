// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/player_setup.cpp
// ============================================================================
#include "bootstrap/player_setup.h"

#include "protocol/server_guid_allocator.h"

namespace aoc::worldd::bootstrap {

SessionGuids mint_session_guids(protocol::ServerGuidAllocator& a, const emit::FIntrepidNetworkGUID& game_state) {
    SessionGuids g;
    // The capture's order: PlayerState 528, PlayerController 530, GlobalGMCommands 532, (the quest and FTUE components
    // 534 and 536, which worldd does not send), the pawn 538, its components 540..566 (transcript section 4.2).
    g.player_state = a.next();
    g.player_controller = a.next();
    g.global_gm_commands = a.next();
    g.game_state = game_state;
    g.pawn = a.next();
    g.base_character_info = a.next();
    g.combat_info = a.next();
    g.owner_info = a.next();
    g.backpack = a.next();
    g.equipment = a.next();
    g.quest_storage = a.next();
    g.reward_storage = a.next();
    g.appearance = a.next();
    g.resource_storage[0] = a.next();
    g.collision_cylinder = a.next();
    g.alignment = a.next();
    g.ability = a.next();
    g.stats = a.next();
    g.xp_tracker = a.next();   // plan D1: after the pawn's components, as the capture's 578 was
    return g;   // (Resource1 and Resource2 are minted by the session when the character wears a bag for them: no other id moves)
}

}  // namespace aoc::worldd::bootstrap
