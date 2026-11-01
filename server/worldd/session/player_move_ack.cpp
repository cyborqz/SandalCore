// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_move_ack.cpp
//
//  The answers to the client's good moves, at the pace of the original server (see session/members/move_ack.inc).
// ============================================================================
#include "session/player_session.h"
#include "session/player_world.h"

#include <spdlog/spdlog.h>

namespace aoc { namespace worldd {

bool PlayerSession::hold_move_ack(float time_stamp, bool on_mount) {
    if (!world_ || world_->move_ack_gap() <= Clock::duration::zero()) return false;
    // the latest good move of the frame: an older one still held is answered for by it (the client frees every saved move up to the TimeStamp)
    if (held_move_ack_) ++move_acks_held_back_;
    held_move_ack_ = HeldMoveAck{time_stamp, on_mount};
    return true;
}

bool PlayerSession::answer_move(Connection& conn, float time_stamp, bool on_mount) {
    auto ack = on_mount ? mount_ack(time_stamp) : pb_.good_move_ack(time_stamp);
    if (!ack) return false;
    if (send(conn, *ack) != Outcome::Sent) return false;   // the actor's channel is not open yet: the client resends its moves
    if (on_mount) ++mount_moves_acked_;
    if (++moves_acked_ == 1) spdlog::info("[session] {}: the first move is acknowledged (TimeStamp {})", name(), time_stamp);
    return true;
}

void PlayerSession::send_move_ack(Connection& conn, Clock::time_point now) {
    if (!held_move_ack_) return;
    const HeldMoveAck held = *held_move_ack_;
    held_move_ack_.reset();
    if (failed_ || held.on_mount != mounted()) return;   // (the player mounted or dismounted since: the client keeps its saved moves per actor, and this one is not the controlled actor's)
    const Clock::duration gap = world_ ? world_->move_ack_gap() : Clock::duration::zero();
    if (move_acked_ && move_acked_on_mount_ == held.on_mount && now - last_move_ack_ <= gap) {
        ++move_acks_held_back_;   // too soon after the last answer: dropped, the next good move brings the next
        return;
    }
    if (!answer_move(conn, held.time_stamp, held.on_mount)) return;
    move_acked_ = true;
    move_acked_on_mount_ = held.on_mount;
    last_move_ack_ = now;
}

}}  // namespace aoc::worldd
