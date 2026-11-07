// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/surface_move.inc: the members of the move checker's ground check (session/move_checker.h, player_movement.cpp)
//
//  The refusals are counted with the refused moves and apart from them; the 30 s position line says them; a line is logged at most once per
//  kSurfaceRefusalLogSeconds with how deep.  Included at the end of the class (session/members_registered.inc).
// ============================================================================
private:
    size_t moves_surface_refused_ = 0;   // of moves_refused_: the ground check (session/move_checker.h)
    Clock::time_point surface_refusal_log_{};
    bool surface_refusal_logged_ = false;
