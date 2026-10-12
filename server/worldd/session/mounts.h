// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/mounts.h  (the mounts milestone, stage 2: summoning and desummoning)
//
//  One player's mount, as pure logic on the session's clock: no sockets, no Connection, no replicator.  The PlayerSession gives it what the client asked
//  (the summon trigger, the desummon request, a damaging hit) and takes what it says to send (`Effects`); the mount itself is an actor of the world's
//  registry (world/actor_registry.h, ActorKind::Mount), which every connection's ActorView opens, updates and closes: nothing here is written to a wire.
//
//  What retail does [R: the captures of research/mounts/evidence.md, sections 1 and 6; every number below is theirs unless it says "modelled"]:
//
//  A summon is `ServerItemUsed` (the mount item) and `ServerTriggerAbility(Summon_Mount, the item)` in one packet.  The item is not consumed.
//
//    +80 ms     the ability item (ReplicatedActiveAbilities: Summon_Mount, a ServerInstanceId, the montage Summon_Mount, the instigator the pawn) replaces
//               the previous one, and the PlayerController's bank-18 action bar is cleared for the Vehicle and Mount instigators and gets the mount's
//               dash on slot 0                                                                          [911, 896, 898, 871: +80 ms]
//    +104 ms    the mount actor opens, 575 cm in front of the player along its yaw (472-654 cm in 12 captured summons), on the ground, with the player's
//               yaw, bIsSummoning 1                                                                      [911 +104 ms]
//    +0.47 s    after the open: bIsSummoning 0                                                           [911 +466 ms]
//    +1.54 s    the ability item again with no montage: the cast (a 1.5 s montage) is over               [911 +1541 ms]
//
//  A refused summon (no ground at the place, a mount under lock) spawns nothing: the ability item at +100 ms and, in the same packet, a callout on the
//  PlayerController ("Cannot summon mount here.", type 1, point size 32); the item without its montage at +1.6 s; the callout is taken off 3 s after it was
//  shown                                                                                  [876: +95, +1624, +3011 ms; 870: +116, +1578 ms]
//
//  A desummon is `ServerItemUsed` and the PlayerController's `ServerDesummonMount` (a different mount item used while one is out is a desummon of the
//  old one only, and the client sends two or three while it dissolves):
//
//    +0.1 s     bIsDesummoning 1, the DissolveTimer, invulnerable and untargetable                       [874 +118 ms, 893 +72 ms]
//    +0.2 s     the mount runs forward along its yaw at its ground speed, ReplicatedMovement every 67 ms [874 +206 ms, 893 +133 ms: 810 to 1053 cm/s]
//    +3.1 s     bIsDesummoning 0 and CurrentMovementInput 0, then the close of its channel (Destroyed) at +3.2 s  [874 +3103, +3191, +3209 ms; 893 +3071,
//               +3137, +3175 ms; 898 +3094, +3161, +3196 ms: the input follows by 34 to 87 ms and the close by 34 to 40; a connection's pass is 67 ms, so
//               the input goes with the flag, 100 ms before the close, and no update is lost to the close]
//
//  Rules.  One mount per player: a summon while one is out (or being summoned) desummons it and summons nothing; one while it dissolves is ignored.  A
//  damaging hit on the player BEFORE the open (the first ~104 ms) cancels the summon (the exe's branch `MountSummon_Cancel` of Summon_Mount): nothing spawns and
//  the ability item ends at once [modelled: no capture shows a hit during a summon].  A hit NEVER desummons a mount that has appeared (the user's rule, stage 4:
//  mounts remain during fights).  The mount leaves the world with its owner's session.
//
//  Modelled, not retail: the height rule of the refusal (the exe names `MountSummonInvalidLocation` and `MaxMountSummonHeightDifference`, no value is in
//  any capture: the ground must be within 250 cm of the player's own); the lock (the item's LockExpiration is in the future); the dissolve's speed (the
//  Blueprint's run speed, mount_speeds.tsv, else 600 cm/s times the item's ground percentage); what a desummon does while the player rides (stage 3).
//
//  Stage 3 reads: `mount()`, the owner, `phase()`; and the controlled-actor seam (the rider's moves, the possession) is the session's.
// ============================================================================
#pragma once
// (C Y B) (CoA)

#include "bootstrap/ability_replication.h"
#include "protocol/emit/intrepid_netguid.h"
#include "spawns/mount_spawn.h"
#include "spawns/mount_state.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

class MountController {
public:
    // ---- retail's numbers (above) ---------------------------------------------------------------------------------------------------------
    static constexpr double kItemDelay = 0.080;
    static constexpr double kOpenDelay = 0.104;
    static constexpr double kSummoningEnds = 0.470;      // after the open
    static constexpr double kCastEnds = 1.541;
    static constexpr double kRefusalItemDelay = 0.105;
    static constexpr double kRefusalEnds = 1.600;
    static constexpr double kCalloutLife = 3.0;
    static constexpr double kSpawnAheadCm = 575.0;
    static constexpr double kDesummonFlags = 0.100;
    static constexpr double kDesummonRun = 0.200;
    static constexpr double kDesummonFlagsOff = 3.100;
    static constexpr double kDesummonInputOff = 3.100;
    static constexpr double kDesummonClose = 3.200;
    static constexpr double kDesummonDissolveTimer = 0.04;   // [R: 874 0.0400, 893 0.0334]
    static constexpr double kRunStepSeconds = 1.0 / 15.0;    // the server tick: ReplicatedMovement every 67 ms
    // ---- modelled ---------------------------------------------------------------------------------------------------------------------------
    /// The mount's death (stage 3: no capture shows one): it is dead (UNPCCombatInfo.bIsDead) this long before its channel closes, and its item is locked this long.
    static constexpr double kDeathClose = 1.5;
    static constexpr int64_t kLockSeconds = 300;
    static constexpr double kMaxHeightDifferenceCm = 250.0;
    static constexpr double kPlayerFootOffsetCm = 98.0;      // a player's capsule half height: its origin is this above its feet
    static constexpr double kMountFootOffsetCm = 57.0;       // 911: the mount's origin 41 cm below the player's, whose feet are 98 below its own
    static constexpr double kDefaultSpeedCm = 600.0;
    /// The refusal's callout.
    static constexpr const char* kRefusalText = "Cannot summon mount here.";
    static constexpr int32_t kCalloutType = 1;
    static constexpr int32_t kCalloutPointSize = 32;
    /// Summon_Mount's montage [R: every captured summon item names it]: the soft path and the static NetGUID its export carries.
    static constexpr const char* kMontagePath = "/Game/Characters/Animations/00_ABLT/Summon_Mount.Summon_Mount";
    static constexpr uint64_t kMontageGuid = 10717609094785283935ull;

    /// Whose the mount is.
    struct Owner {
        uint64_t                             player_id = 0;
        repl::GuidValue                      guid;          // the owner's CharacterGuid
        std::string                          name;
        protocol::emit::FIntrepidNetworkGUID pawn;
        uint32_t                             server_id = 0;
    };

    /// Where the player stands (cm, degrees): its last good position.
    struct Place {
        double x = 0, y = 0, z = 0, yaw_degrees = 0;
    };

    /// A summon the client asked for, once the session has found the item (the inventory's instance at the call's slot).
    struct Request {
        int64_t         item_record = 0;
        repl::GuidValue item_guid;
        int32_t         client_instance = 0;    // ServerTriggerAbility's ClientInstanceId (ClientEndAbility names it)
        int64_t         lock_expiration = 0;    // the item's LockExpiration (Unix seconds; 0: none)
        int64_t         now_unix = 0;
        Place           place;
        int             owner_level = 1;
        bool            dead = false;           // the player is dead: nothing is summoned
        double          world_time = 0;         // the world clock at the request (the mount's timestamps run on it)
        std::optional<double> health;           // the Health the mount starts at (stage 3: the item's saved Health; unset: its maximum)
        /// The owner's alignments as its pawn holds them now: the mount inherits them (stage 3 fix 2: never a donor's). Unset: the unflagged player's.
        std::optional<repl::AlignmentContainer> alignments;
    };

    enum class Outcome : uint8_t {
        Started,        // the summon runs
        Refused,        // a refusal: the ability item and the callout
        Desummoning,    // a mount was out: it is being desummoned, nothing is summoned
        Ignored,        // another summon or a desummon is in progress, or the item is no mount: nothing happens (the client's instance is ended)
    };

    /// What the session sends after a call or an advance.
    struct Callout {
        uint64_t    key = 0;
        std::string text;
    };
    /// What the item of the mount keeps when it leaves or dies (stage 3): the Health the mount had, and, when it died, the lock until which the item cannot be summoned.
    struct Persist {
        repl::GuidValue item_guid;
        float           health = 0;
        int64_t         lock_expiration = -1;   // Unix seconds; negative: the lock stays as it is
        bool            died = false;
    };
    struct Effects {
        bootstrap::AbilityChange ability;          // the Summon_Mount item, on the pawn's AbilityComponent
        bool                     bar = false;      // the bank-18 bar (bootstrap mount_bar) with the mount's dash on `bar_slot`
        int64_t                  bar_ability = 0;
        int                      bar_slot = 0;
        std::vector<Callout>     callouts;         // to add to the PlayerController
        std::vector<uint64_t>    callouts_removed;
        std::vector<int32_t>     ends;             // ClientEndAbility(ClientInstanceId)
        std::optional<Persist>   persist;          // the item's Health (and lock): the session writes it into the bag
        /// The pawn's InteractInfo.MountablePet (stage 3 fix 3): the mount's actor guid once it is open, an invalid guid once it has closed.  The client's bank-18 Mount slot
        /// (the dash) needs it [R: 871 packet 210, 898 packets 158 and 601].
        bool                     pet_changed = false;
        protocol::emit::FIntrepidNetworkGUID pet;
        bool empty() const { return ability.empty() && !bar && callouts.empty() && callouts_removed.empty() && ends.empty() && !persist && !pet_changed; }
        void merge(Effects&& o);
    };

    enum class Phase : uint8_t { Idle, Summoning, Out, Desummoning, Dying };

    struct Stats {
        size_t summons = 0, refused = 0, desummons = 0, cancelled = 0, ignored = 0, opened = 0, closed = 0, deaths = 0;
    };

    /// The ground: the height of the feet-level ground under (x, y) near `z_hint` (the world's navmesh), nullopt when there is none.
    using Ground = std::function<std::optional<double>(double x, double y, double z_hint)>;
    /// A fresh ServerInstanceId of the pawn's ability arrays (the ability runtime's counter); -1: none can be had (the item is not shown).
    using Instance = std::function<int64_t()>;

    MountController(world::ActorRegistry& registry, const spawns::MountBook& book, spawns::Minter mint, Owner owner);
// (CoA : C | Y | B)

    void set_ground(Ground g) { ground_ = std::move(g); }
    void set_instance_source(Instance i) { instance_ = std::move(i); }
    /// The speed (cm/s) a mount of Blueprint `blueprint` runs on the ground (mount_speeds.tsv's run speed): 0 asks for the item's own estimate.
    void set_speeds(std::function<double(const std::string& blueprint)> f) { speed_of_ = std::move(f); }

    /// The player asks to summon. `now` is the session's clock (seconds). Effects it makes are returned by the next advance().
    Outcome summon(const Request& r, double now);
    /// The player asks to desummon: true when a mount (or a summon in progress) was there to dismiss.
    bool desummon(double now);
    /// A hit that does damage reached the player: it cancels a summon that has not opened yet; a mount that has appeared stays (stage 4).
    void on_damage(double now);
    /// Runs what is due at `now` and returns what to send.
    Effects advance(double now);
    /// The mount died (its Health reached 0 under its rider's hits): it is dead at once, closes kDeathClose later, and its item keeps Health 0 and a lock of kLockSeconds from
    /// `now_unix`. The Effects the next advance returns say so. False when there is no mount out.
    bool die(double now, int64_t now_unix);
    /// The owner's session ends: the mount leaves the world at once (the views close it Destroyed). What its item keeps comes back (nullopt: no mount out).
    std::optional<Persist> shutdown();
    /// What the item of the mount out keeps now: its Health (nullopt: no mount).
    std::optional<Persist> persist_now() const;
    /// The speed (cm/s) a mount of `row` runs on the ground at MoveSpeedMult 1: the Blueprint's run speed, else 600 times the item's ground percentage.
    double run_speed(const content::MountRow& row) const;

    Phase phase() const { return phase_; }
    /// The mount's actor id (0: none), and the item it was summoned from.
    uint64_t actor_id() const { return actor_; }
    const world::WorldActor* mount() const { return actor_ ? registry_.find(actor_) : nullptr; }
    int64_t  item_record() const { return item_record_; }
    const Owner& owner() const { return owner_; }
    const Stats& stats() const { return stats_; }
    /// A summon (or a refusal) is in progress: its steps are pending.
    bool busy() const { return !steps_.empty(); }
    size_t pending_steps() const { return steps_.size(); }
    /// Whether a mount item is `item`: what the session asks to tell a mount item from the rest.
    bool is_mount_item(int64_t item) const { return book_.find(item) != nullptr; }
    const spawns::MountBook& book() const { return book_; }

private:
    enum class Kind : uint8_t {
        Item,            // the ability item and the bar
        Open,            // the actor
        SummoningOff,
        CastEnd,         // the item again, no montage
        RefusalItem,
        RefusalEnd,
        CalloutOff,
        DesummonFlags,
        DesummonRun,     // the run's step (repeats)
        DesummonFlagsOff,
        DesummonInputOff,
        DesummonClose,
        DeathClose,
    };
    struct Step {
        double  due = 0;
        Kind    kind = Kind::Item;
        uint64_t cast = 0;      // the cast it belongs to (a cancel drops its steps)
        uint64_t key = 0;       // a callout's
    };

    void add(double due, Kind kind, uint64_t key = 0);
    void drop_steps(uint64_t cast, bool keep_desummon);
    void run(const Step& s, double now, Effects& out);
    void run_open(double now, Effects& out);
    void begin_desummon(double now);
    void cancel_cast(double now, Effects& out, bool dismiss_mount);
    void forget_mount(Effects* out = nullptr);
    bool item_visible_effects(Effects& out, bool end);

    world::ActorRegistry&    registry_;
    const spawns::MountBook& book_;
    spawns::Minter           mint_;
    Owner                    owner_;
    Ground                   ground_;
    Instance                 instance_;
    std::function<double(const std::string&)> speed_of_;
    Phase                    phase_ = Phase::Idle;
    std::vector<Step>        steps_;
    uint64_t                 next_cast_ = 1;
    uint64_t                 cast_ = 0;            // the cast in progress
    Request                  req_;
    bool                     refusal_ = false;
    bool                     open_done_ = false;
    bool                     item_shown_ = false;  // the ability item stands with its montage
    double                   clock_offset_ = 0;    // the world clock minus the session clock, at the request: the mount's timestamps
    double                   run_end_ = 0;
    Effects                  pending_effects_;     // what a call made (a cancel) and the next advance returns
    int64_t                  item_instance_ = -1;  // the pawn's ServerInstanceId of the ability item that stands (-1: none)
    int32_t                  animation_counter_ = 1;
    uint64_t                 actor_ = 0;
    bool                     pet_set_ = false;     // the pawn's MountablePet names the mount (the Effects said so)
    int64_t                  item_record_ = 0;
    // the run-off
    double                   run_x_ = 0, run_y_ = 0, run_z_ = 0, run_yaw_ = 0, run_speed_ = 0, run_at_ = 0;
    uint64_t                 next_callout_ = 1;
    Stats                    stats_;
};

}  // namespace aoc::worldd
