// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/riding.h  (the mounts milestone, stage 3: riding)
//
//  One player's ride on its mount, as logic on the session's clock: no sockets, no Connection, no replicator.  The PlayerSession gives it what happened (the
//  rider got on or off, a move was accepted, the dash was triggered, a hit reached the mount, time passed) and takes what to send; the mount itself is the
//  registry's actor (ActorKind::Mount), changed through spawns/mount_state.h, which the actor view sends to its owner on the mount's own channel.
//
//  What retail does [R: research/mounts/evidence.md sections 3, 4 and 8; captures 910, 912, 959, 906, 893]:
//
//    getting on   the rider interacts with the mount (radius 400): the PlayerController is told ClientRestart(mount) with the camera mode and the bank-18 bar, the mount
//                 gets its Owner and its rider's stats, the rider's pawn is attached at MountSocket: three packets within a millisecond (session/player_session.cpp
//                 on_interact; spawns/mount_state.h has the mount's part).  The client's TimeStamp restarts near 0: the mount has a move checker of its own.
//    riding       ServerMovePacked and ClientMoveResponsePacked run on the MOUNT's channel; the retail server sends the owner no ReplicatedMovement of a possessed mount.
//                 A ground mount's speed is its Blueprint's run speed times MoveSpeedMult (990 x 1 / 1.2 / 1.25 / 1.35 in capture 959): 1 + a road's addend (Road_T0..T4:
//                 +0.10 .. +0.30, a CurrentEffects item with no end, added and removed as the mount enters and leaves a road) + the dash's 0.35.
//    the dash     MountAbility_Sprint on the mount's AbilityComponent: 94 ms later a Cooldowns item (30 s, Charges 0, start = the reply's synchronized time), a
//                 CurrentEffects item (5 s) and MoveSpeedMult 1.35 on the mount, and the PlayerController's ClientHitConfirmationsReliable; 5 s later the effect and the
//                 multiplier go, 30 s after the start the cooldown item [R: 906 +94 ms, +5.013 s; 959 +30.0 s].
//    getting off  ServerDismountInteraction: the rider's pawn is put beside the mount at saddle height (RepDetachmentLocation: 54 cm ahead of the mount, 197 cm above
//                 its origin in capture 912), the PlayerController is restarted on its own pawn; the mount stays where it was left.
//
//  Modelled, not retail: the move checker's limits (the Blueprint's speed times the multiplier times 1.5, and the old limit for a second after the multiplier falls, because
//  the client learns of the fall a trip later); the roads' width; the dash and a road add up (1 + road + 0.35: no capture has both at once); the rider's owner stat arrays are
//  copied when it gets on and not kept in step while it rides (its Stamina does not move on a mount: nothing a mounted rider may do spends any).
// ============================================================================
#pragma once

#include "bootstrap/ability_replication.h"
#include "content/content_db.h"
#include "protocol/emit/intrepid_netguid.h"
#include "session/move_checker.h"
#include "spawns/mount_spawn.h"
#include "spawns/mount_state.h"
#include "world/actor_registry.h"
#include "world/roads.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

class Ride {
public:
    // ---- retail's numbers (above) ---------------------------------------------------------------------------------------------------
    static constexpr double  kDashReplyDelay = 0.094;
    static constexpr double  kDashCooldownSeconds = 30.0;
    static constexpr double  kDismountForwardCm = 54.0;
    static constexpr double  kDismountRiseCm = 197.0;
    /// The dash's hit record (MountAbility_Sprint's AbilityHitRecord): the confirmation names it [R: 906 packet 67, 959].
    static constexpr int64_t kDashHitRecord = 6064632423136905040ll;
    // ---- modelled -------------------------------------------------------------------------------------------------------------------------
    static constexpr double  kSpeedMargin = 1.5;
    static constexpr double  kSpeedGraceSeconds = 1.0;
    /// How far the rider may be from the mount to get on: the interaction's radius 400 and a tolerance (research: radius 400 / tolerance 500 unresolved).
    static constexpr double  kInteractRangeCm = 500.0;
    /// Stage 4: Mount_Flying_Base's MaxFlyFallSpeed, the dive's ceiling at 90 degrees down (every flying Blueprint inherits it; the dive fits 871 and 883 exactly: move_checker.h).
    static constexpr double  kMaxFlyFallSpeed = 7500.0;
    /// The speeds the item texts' percentages are over: 600 cm/s on the ground and in the air ("215 % Flying Speed" is 1,290), 400 in the water ("230 %" is 920).
    static constexpr double  kFlyBaseCm = 600.0;
    static constexpr double  kSwimBaseCm = 400.0;
    /// Stage 4, modelled: a mount left alone farther than this from its owner is desummoned (the exe names `bEnableMountDistanceDesummon` and `MountDesummonDistanceCentimeters`;
    /// the Blueprints turn it on and no data or capture has the distance).
    static constexpr double  kDesummonDistanceCm = 50000.0;
    /// Stage 4, modelled: a rider in the air this far above the ground logs back in on the ground below (a dismounted flyer itself stays in the air: fix 2).
    static constexpr double  kAirborneCm = 300.0;
// C-O-A : C.Y.B

    /// What the ride needs to know of its mount.
    struct Setup {
        world::ActorRegistry*     registry = nullptr;
        const content::MountData* data = nullptr;          // the road effects and the dash's
        const content::MountRow*  row = nullptr;
        uint64_t                  actor = 0;               // the mount's ObjectId in the registry
        double                    run_speed = 600;         // cm/s at MoveSpeedMult 1: the Blueprint's
        double                    fly_speed = 0;           // cm/s at MoveSpeedMult 1 in movement mode 5 (0: the mount cannot fly)
        double                    swim_speed = 0;          // ... in mode 4 (the Blueprint's MaxSwimSpeed; the item's water % times 400 for an aquatic mount)
        double                    world_time_at_zero = 0;  // the world clock when the session clock was 0 (the effects' StartTime)
        const world::RoadMap*     roads = nullptr;         // null: none
        int64_t                   dash_ability = 0;        // the ability that is the mount's dash (MountAbility_Sprint, or its swimming twin)
        repl::GuidValue           owner;                   // the rider's CharacterGuid: the effects' Instigator and Source
        double                    dash_ready_at = -1.0;    // review A I3: the session time the dash is ready again, carried over from the rider's earlier mount (-1: ready)
        double                    gm_multiplier = 1.0;     // the GM travel tools: the GM's run speed multiplier (session/gm_travel.h), a factor of MoveSpeedMult on top of the road and the dash
    };

    explicit Ride(Setup setup);

    /// Stage 4: a mount's flight and swim speeds into `s`: its Blueprint's MaxFlySpeed and MaxSwimSpeed (mount_speeds.tsv), and, when the item's text promises more, the text's.  A flyer's
    /// flight speed is its Blueprint's, or, when the item's text promises more (the GM Eagle Gryphon's text says 215 % = 1,290 and its Blueprint, a plain child of Mount_Flying_Base,
    /// 800), that: the client's dives follow the Blueprint's numbers either way (move_checker.h), the text keeps a cruise it may claim.  An aquatic item's water speed is 400 x its
    /// percentage (the Blueprint says the same for the four).  A mount that is no flyer flies at 0 (mode 5 is refused).
    static void fill_speeds(Setup& s, const spawns::MountBook& book, const content::MountRow& row);

    uint64_t                 actor_id() const { return s_.actor; }
    const world::WorldActor* mount() const { return s_.registry ? s_.registry->find(s_.actor) : nullptr; }
    bool                     riding() const { return riding_; }
    bool                     dead() const { return dead_; }
    int64_t                  dash_ability() const { return s_.dash_ability; }

    // ---- getting on and off ---------------------------------------------------------------------------------------------------------
    /// The rider gets on at session time `t`: the mount's possession (spawns::possess). False, with `error`, when the mount does not take it (nothing changes).
    bool begin(const spawns::Possession& p, double t, std::string& error);
    /// The rider gets off: the road effect goes, the mount's possession is released and it stands at (x, y, z) facing `yaw`.  A flyer (5) or swimmer (4) stays there in that movement mode (ReplicatedMovementMode), hovering or floating.
    bool end(double t, double x, double y, double z, double yaw_degrees, uint8_t movement_mode, std::string& error);

    // ---- moving ---------------------------------------------------------------------------------------------------------------------
    /// The horizontal limit (cm/s) of a move in movement mode `mode` (1 walking, 3 falling, 4 swimming, 5 flying) at session time `t`: the Blueprint's speed for the mode times
    /// MoveSpeedMult times the margin, and the multiplier of the last second when that was larger.
    double speed_limit(uint8_t mode, double t) const;
    /// The limits a possession's checker starts with (the world's, the speed, and for a flyer the vertical ones).
    MoveLimits limits(const MoveLimits& base, double t) const;
    /// MoveSpeedMult as the move checker's 3D modes use it: now, or the last second's when that was larger (the client learns of a fall of it a trip later).
    double effective_mult(double t) const { return std::max(mult_, t < grace_until_ ? grace_mult_ : 0.0); }
    /// An accepted move: the mount stands at (x, y, z) facing `yaw` in movement mode `mode`, its rider looking `pitch16` (RemoteViewPitch16: the only thing the retail
    /// server sends on a flying mount besides the move responses [R: 871, 959, 899, 883]); the road under it decides the road effect.
    void moved(double x, double y, double z, double yaw_degrees, double t, uint16_t pitch16 = 0, uint8_t mode = 1);
    /// The movement mode of the last accepted move (5: flying).
    uint8_t mode() const { return mode_; }
    bool    can_fly() const { return s_.fly_speed > 0 && s_.row && s_.row->kind == "flying"; }
    /// MoveSpeedMult now: (1 + the road's addend + the dash's) times the GM multiplier.
    double speed_multiplier() const { return mult_; }
    /// The GM's run speed multiplier (the GM travel tools): the mount's MoveSpeedMult stat follows at once while the rider is on it, and the move checker's limits with it (speed_limit).
    void set_gm_multiplier(double m, double t) {
        s_.gm_multiplier = m;
        if (riding_) recompute(t);
    }
    double gm_multiplier() const { return s_.gm_multiplier; }
    /// The road tier the mount is on (-1: none).
    int road_tier() const { return road_tier_; }

    // ---- the dash -------------------------------------------------------------------------------------------------------------------
    enum class Dash : uint8_t { Started, OnCooldown, NotTheDash };
    /// The rider triggers `ability` at session time `t`. Started: its effects come kDashReplyDelay later (advance). OnCooldown: refused, nothing changes.
    Dash trigger_dash(int64_t ability, double t);
    bool dash_active() const { return dash_effect_ != 0; }
    bool dash_pending() const { return dash_apply_at_ >= 0; }
    double dash_ready_at() const { return cooldown_until_; }

    struct Events {
        bool dash_applied = false;   // the dash's effects were just put on the mount: the PlayerController gets the hit confirmation
        bool dash_ended = false;
    };
    /// Runs what is due at `t`.
    Events advance(double t);

    // ---- health ---------------------------------------------------------------------------------------------------------------------
    double health() const;
    double max_health() const;
    struct Hurt {
        double taken = 0;     // what came off Health
        bool   died = false;  // Health reached 0 with this hit
    };
    /// A hit of `amount` (positive) reaches the mount: its Health, never below 0.
    Hurt hurt(double amount);
// C Y B and C | O | A

    // ---- where the rider lands ------------------------------------------------------------------------------------------------------
    struct Place {
        double x = 0, y = 0, z = 0, yaw_degrees = 0;
    };
    /// Where a rider that gets off a mount at (x, y, z) facing `yaw` is put [R: 912: 54 cm ahead of the mount, 197 cm above its origin].
    static Place dismount_place(double x, double y, double z, double yaw_degrees);
    /// Where a rider is saved: standing on the ground beside the mount (a mount's origin is 57 cm above its ground, a player's 98: 41 cm up, the same 54 cm ahead), so that a relog
    /// puts it on its feet there and not in the air.
    static Place rest_place(double x, double y, double z, double yaw_degrees);

private:
    void   recompute(double t);
    void   set_road(int tier, double t);
    double clock(double t) const { return s_.world_time_at_zero + t; }
    double base_speed(uint8_t mode) const;

    Setup  s_;
    bool   riding_ = false;
    bool   dead_ = false;
    double mult_ = 1.0;
    double grace_mult_ = 1.0, grace_until_ = -1.0;
    int    road_tier_ = -1;
    uint8_t mode_ = 1;
    int32_t view_pitch_ = -1;       // the RemoteViewPitch16 the mount holds (-1: none yet)
    int64_t next_effect_id_ = 1;
    int64_t road_effect_ = 0;       // the CurrentEffects key (EffectId) of the road's item, 0: none
    int32_t road_array_id_ = 0;
    int64_t dash_effect_ = 0;
    int32_t dash_array_id_ = 0;
    double dash_apply_at_ = -1.0, dash_until_ = -1.0;
    int32_t cooldown_array_id_ = 0;
    double cooldown_until_ = -1.0;
    double cooldown_start_ = 0;
};

}  // namespace aoc::worldd
