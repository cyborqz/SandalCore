// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/move_ack.inc: the members of PlayerSession's move acknowledgements (session/player_move_ack.cpp)
//
//  The original server does not answer every move: a good move only sets the answer to be sent at the end of the server's frame (the latest good move's TimeStamp), and the
//  frame sends it when more than 100 ms have gone by since its last answer, otherwise it drops it; the client frees every move it saved up to the TimeStamp it is given, so a move
//  that was not answered is answered for by the next.  PlayerWorld::move_ack_gap() is that 100 ms (0: every move is answered at once, as it was before).
//  Included at the end of the class (session/members_registered.inc).
// ============================================================================
public:
    /// Good moves whose answer was not sent: dropped by the pace (a later move was answered for them, or the last answer was less than the gap ago).
    size_t move_acks_held_back() const { return move_acks_held_back_; }

private:
    /// A good move's TimeStamp: with a pace (PlayerWorld::move_ack_gap) it waits for the tick to be answered and this returns true; without one it returns false and the caller answers at once.
    bool hold_move_ack(float time_stamp, bool on_mount);
    /// Sends the answer to a good move at once: ClientMoveResponsePacked with the move's TimeStamp on the controlled actor's channel.  False when it did not go out.
    bool answer_move(Connection& conn, float time_stamp, bool on_mount);
    /// The tick's turn: the latest good move since the last tick is answered when the last answer is more than the gap old; a held answer is dropped either way.
    void send_move_ack(Connection& conn, Clock::time_point now);

    struct HeldMoveAck {
        float time_stamp = 0;
        bool  on_mount = false;
    };
    std::optional<HeldMoveAck> held_move_ack_;
    bool                       move_acked_ = false;           // an answer went out on the controlled actor ...
    bool                       move_acked_on_mount_ = false;  // ... which was the mount (the other actor of a possession change starts without a last answer)
    Clock::time_point          last_move_ack_{};
    size_t                     move_acks_held_back_ = 0;
