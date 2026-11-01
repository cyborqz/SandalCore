// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/based_move.inc: the members of PlayerSession's moves on a movement base (session/player_based_move.cpp)
//
//  The game sends a movement base with a move when the character stands on a movable thing (the mount it just got off, a corpse), and then the move's Location and
//  Acceleration are given in the base's own frame: the offset from the base, turned back by the base's rotation.  Such a move is put in the world before it is judged:
//    a base worldd can place (the root cylinder of an actor the world holds)   the actor's place plus the offset turned by the actor's yaw;
//    a base it cannot place                                                      followed from the move before on the same base (the player's place changes by what the
//                                                                                offset changed by); the first move on a base stays where the player was.
//  Included at the end of the class (session/members_registered.inc).
// ============================================================================
public:
    /// The most moves in a row that may stay in place because their base cannot be placed or followed (the first move on each base), counted from the last move whose place the
    /// server knows (a move on no base, or on a base it can place): the next one puts the player back at its last good position.
    static constexpr size_t kBasedHeldCap = 120;

    /// Moves that did not read: counted, the first and every 100th is said.
    size_t moves_unreadable() const { return moves_unreadable_; }
    /// Moves that stood on a movement base (a move the game repeats in its next packet is counted once).
    size_t moves_based() const { return moves_based_; }
    /// Of those, the ones on a base worldd cannot place.
    size_t moves_based_unplaced() const { return moves_based_unplaced_; }
    /// Of those, the ones that stayed in place (the first move on a base).
    size_t moves_based_held() const { return moves_based_held_; }
    /// The times a run of moves in place ended in a correction.
    size_t moves_based_cut() const { return moves_based_cut_; }

    /// `v` turned about the vertical axis by `yaw_degrees` (Unreal's yaw: a positive yaw turns +X towards +Y): the offset or the acceleration of a move on a base, from the base's own
    /// frame to the world's.
    static protocol::repl::Vec3d turn_by_yaw(double yaw_degrees, const protocol::repl::Vec3d& v);
    /// The counts for the position line every 30 s: ", 3 unreadable, 12 on a movement base (2 not placed)"; empty when there are none.
    std::string based_move_summary() const;

private:
    /// Where a move that stands on a movement base is in the world.
    struct BasedPlace { protocol::repl::Vec3d location, acceleration; };
    /// The move's place in the world when its base is the root cylinder of an actor worldd holds; nullopt for any other base (another component, a bone, an actor worldd does not hold).
    std::optional<BasedPlace> place_on_base(const protocol::repl::NetworkMove& m) const;
    /// Puts the packet's moves that stand on a movement base in the world (their base is cleared), the oldest first, skipping the ones judged already.  False when the run of moves
    /// in place is over: the player is put back at its last good position (the correction goes out at the end of the datagram) and the packet is not judged.
    bool stand_on_bases(protocol::repl::MovePacked& packed);

    /// Which base a move stands on (the object it names; for one named in place the component's name and the actor it belongs to).
    struct BaseKey {
        uint64_t    object = 0;
        std::string path;
        std::string bone;
        bool operator==(const BaseKey&) const = default;
    };
    /// The base worldd follows without placing it: the last move on it, the offset it gave and the place worldd put it at.
    struct BaseTrack {
        BaseKey                key;
        protocol::repl::Vec3d  offset;
        protocol::repl::Vec3d  place;
    };
    std::optional<BaseTrack> base_track_;
    size_t based_held_run_ = 0;   // the moves in place since the last move whose place the server knows
    /// The TimeStamp of the move that ended a run of moves in place: its correction goes out at the end of the datagram even when a later move of the same datagram is accepted
    /// (an accepted move cancels the correction of a refused one, because the client is then where the server has it; a move in place says nothing of where the client is).
    /// A teleport, a mount-up and a dismount drop it, as they drop pending_fix_.
    std::optional<float> based_fix_;
    /// Hands the correction of a run that was cut to the datagram's end (pending_fix_, unless a refusal after it left one); true when there was one.
    bool release_based_fix();
    size_t moves_unreadable_ = 0, moves_based_ = 0, moves_based_unplaced_ = 0, moves_based_held_ = 0, moves_based_cut_ = 0;
