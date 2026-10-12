// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/abilities.h
//
//  The ability runtime of one player (plan A7, spec section 7), as pure logic on a clock the caller gives: no
//  sockets, no Connection, no replicator.  It runs three abilities, Sprint, Dodge and Rest, from the numbers contentc
//  resolved into content.db (content::AbilityRules), keeps the player's Stamina and regeneration, and says what changed
//  as bootstrap::AbilityChange values (the replicated items) and ClientEndAbility calls, in events the session sends.
//
//  What it does, per ability (every number is content.db's unless a constant below says otherwise):
//
//    Sprint   Held (Smart).  Refused with less Stamina than min_stamina (1).  Adds its ability item, its effect and the
//             effect's stat mod, MoveSpeedMult + 0.2 (the retail server replicates it in the same packet: the client
//             moves at 600 cm/s times it, 720 cm/s with no talents), and takes its first charge with the activation when
//             the player moves; then, every charge_interval (0.25 s), cost_amount (1.25) Stamina while the player moves (5 a
//             second, 80 charges from a full bar: spec section 7, decided with the user).  It ends, with its items removed,
//             MoveSpeedMult back to 1.0 and a ClientEndAbility kEndAbilityDelay later (retail's 163 ms), when Stamina falls
//             below min_stamina.  A cancel (ServerNotifyHeldAbilityCancelled) ends it without ClientEndAbility (the client
//             ended its own); so does a new Sprint that replaces it.  A Rest ends it with a ClientEndAbility at once (no
//             retail capture has a Rest, and no instance may linger on the client).  A Dodge does NOT end it (retail capture
//             745: the Sprint, MoveSpeedMult and the drain run on through a Dodge).  Ending applies the exertion effect's
//             regeneration pause: the 1 s General_Stamina_DisableRegen effect, as an effect item in the same step.
//    Dodge    Costs cost_amount (50) once, at the trigger.  Refused with less Stamina, or during its cooldown (0.65 s).
//             Adds its ability item (the montage and MeshYaw the trigger's AnimationIndex and InputVector name, as the retail
//             server sends them), the Cooldowns item and the Evasion effect (0.5 s).  The Evasion effect stops regeneration
//             while it lasts, then the exertion effect pauses it for 1 s more (the DisableRegen effect item replaces the
//             Evasion in one step).  The ability item and the cooldown item go when the cooldown ends.  For the A6 move checker
//             the Dodge opens a dash window (dash_allowed): the Evasion and kDashMargin.
//    Rest     A toggle, out of combat only.  Adds its ability item; the Rest effect follows kRestEffectDelay later (the
//             sit-down animation), raising Health and Mana regeneration (stat mods from content.db).  A cancel jumps the
//             montage to its end section and, kRestEndEffectDelay later, removes the effect and puts the regeneration
//             back; the ability item stays until the next Rest replaces it, as in the capture.  A second Rest while one is
//             running replaces the first in one step (pkt#8240).  A Sprint or a Dodge ends it.
//    anything else, or a Sprint or Dodge that is refused: ClientEndAbility(ClientInstanceId) and nothing else.
//
//  Instances.  The ability item's ServerInstanceId is the server's own counter of the instances it started (0, 1, 2 ...; a
//  refused trigger takes none), not the client's ClientInstanceId (retail: a constant offset per session, 393 of 414
//  captures); ClientEndAbility and ServerNotifyHeldAbilityCancelled carry the CLIENT's id, so each running ability knows both.
//
//  The StaminaRegeneration item (the Owner array's, when the pawn has one) shows what the runtime does with it [R: retail capture 745]: 0 while the
//  regeneration is stopped (the Sprint's effect and the player moves, the Evasion, the 1 s pause), the rate otherwise; it goes out with the step that
//  changes it (sync_regen), and follows the player's moving and standing during a Sprint.
//
//  Regeneration: every stat_regen_time (1 s) Stamina rises by regen_rate (5) unless the Sprint's effect is on and the player
//  moves, the Evasion effect is on, or the exertion pause is running.  regen_rate is the content constant (the StaminaRegeneration
//  formula's 5 with nothing stopping it), not the engine's StaminaRegeneration stat (plan B3), because no gear moves that stat: the
//  formula reads effects only, and the one item stat block that names it is a potion (CanEquip 0), no set bonus does.  The Go test
//  TestNoGearMovesStaminaRegeneration (services/cmd/contentc) holds that to the design data; the day it fails, the runtime reads the
//  engine's value (and talents, food and guild effects of part 2 are state the engine does not apply yet).
//
//  Moving.  The Sprint is charged only while the player moves, which the caller says (set_moving, from the A6 moves
//  the MoveChecker accepted); a player no move has been heard from for kMovingStale counts as standing.  The player is
//  never in combat in milestone A (set_in_combat exists for the tests and for later).
//
//  Time.  Every public call takes the caller's clock in seconds (any origin, never going back) and first advances the
//  runtime to it: timers that fell due in between run in order, each at its own time, so a late tick gives the same events
//  as many small ones.  The replicated times (an effect's StartTime, a cooldown's start and end) are on the synchronized
//  clock (bootstrap::effect_clock): `world_time_at_zero` is the world time at clock 0.
// ============================================================================
#pragma once
// C O A x Cyb

#include "bootstrap/ability_replication.h"
#include "content/content_db.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd {

/// Why a Sprint, Dodge or Rest was not started.
enum class Refusal : uint8_t {
    None,
    NotEnabled,   // an ability worldd does not run (any combat ability): answered with ClientEndAbility
    Stamina,      // less Stamina than the ability needs
    Cooldown,     // the Dodge's cooldown has not ended
    InCombat,     // Rest, in combat
    Duplicate,    // the ClientInstanceId belongs to an instance that is running: the request is dropped, nothing answered
    // plan D2: the generic runtime's refusals (every one is answered with ClientEndAbility, a Duplicate is not)
    NotKnown,     // the character does not know the ability (KnownAbilities)
    NeedsTarget,  // it needs a target and the call names none
    Charges,      // out of charges: its cooldown is running
    Cost,         // it spends more of a stat or of an effect's stacks than the character has
    Busy,         // another ability's cast has not finished
    Requirement,  // the ability's own activation requirement is false (a weapon, a state)
    Unsupported,  // a shape stage 1 does not run (an enemy target, a held ability that is no toggle ...): logged
    NoFormula,    // one of its formulas does not compile
    // plan E1, stage 3: a target the call names
    BadTarget,    // it is no actor, is dead, or is no valid target of the ability (an ally it may not harm, an enemy it may not help)
    OutOfRange,   // farther than the ability reaches (the data's OutOfRangeDistance and the cast leeway)
    NoLineOfSight, // the ground between the caster and the target blocks the cast
    // plan E1, stage 3, part 2
    Dead,         // the character is dead: nothing is cast
    // the mounts milestone, stage 3 fix 2
    Mounted,      // a mounted rider has no Sprint, Dodge or Rest (General_Sprint is not bUsableWhileMounted)
};
const char* refusal_name(Refusal r);

/// One step the session sends: the change to the pawn's replicated state (maybe empty), then ClientEndAbility.
struct AbilityEvent {
    bootstrap::AbilityChange change;
    std::optional<int32_t>   end_ability;   // ClientEndAbility(ClientInstanceId)
    double                   at = 0;        // the runtime's clock when it happened
    std::string              note;          // for the log and the tests
    bool                     refusal = false;   // a trigger that was refused (its ClientEndAbility is the whole answer)
    /// plan D2, stage 2: what the server confirms of the hits this step made on the pawn (ClientHitConfirmationsReliable, sent after the change: the
    /// client draws its floating numbers from them)
    std::vector<bootstrap::HitConfirm> confirmations;
    /// plan D2, review: the lingering zones' actors this step opens, fades or closes (sent after the change)
    std::vector<bootstrap::ZoneOp> zones;
};

/// What a ServerTriggerAbility says besides the ability and the instance: the Dodge's montage and MeshYaw come from them.
struct TriggerInfo {
    int32_t  animation_index = 0;
    double   input_x = 0, input_y = 0;
    uint32_t target_count = 0;   // plan D2: how many targets the call names (an ability that needs one is aimed at the caster: nothing else is there)
    bool     item = false;       // plan D2: the call names an item (a consumable's ability): not run by the generic runtime
    /// plan E1, stage 3: the actors the call's Targets name other than the caster's own components, as the session resolved them (an NPC's ObjectId).
    std::vector<uint64_t> target_actors;
};

class AbilityRuntime {
public:
    // ---- numbers that are not content.db's, each with its evidence (the worldd README, "Settled numbers") -------------

    /// How long after the Rest ability item the Rest effect is added: the sit-down. The capture shows 1.774 s (pkt#8175 ->
    /// #8228) and 2.037 s (pkt#8240 -> #8301); the montage's section times are not in any export (the .uasset has no
    /// .uexp), so 2 s, inside both.
    static constexpr double kRestEffectDelay = 2.0;
    /// How long after the montage jump to its end section the Rest effect is removed: pkt#8318 -> #8320, 67 ms.
    static constexpr double kRestEndEffectDelay = 0.067;
    /// Where the Rest montage's end section ("SitEnd", Ability.CancelSectionName) starts: MontagePosition 5.82606792 in
    /// pkt#8318.
    static constexpr float kRestSitEndPosition = 5.82606792f;
    /// Items.Duration of an infinite effect: FLT_MAX, as in pkt#8228.
    static constexpr float kInfiniteDuration = 3.40282347e+38f;
    /// A player no accepted move has been heard from for this long counts as standing (the e2e capture's moves are 0.21 s
    /// apart, the native run's 17 ms).
    static constexpr double kMovingStale = 0.75;
    /// How long after the reply that ends an out-of-Stamina Sprint the ClientEndAbility follows: 163 ms in the only retail case
    /// (capture 745; the reply carries MoveSpeedMult 1.0 and the DisableRegen effect, the call comes after it).
    static constexpr double kEndAbilityDelay = 0.163;
    /// How long after its Evasion a Dodge still lets the client dash (dash_allowed): the dash covers 559-810 cm within 0.8 s in
    /// 31 retail Dodges, the Evasion lasts 0.5 s.
    static constexpr double kDashMargin = 0.3;

    /// `stats` are the current values by record Guid of every stat the runtime reads or sets: Stamina, MaxStamina, the Health
    /// and Mana regeneration its Rest effect raises and the maxima it scales from, and MoveSpeedMult, which the Sprint's
    /// effect raises.  A stat the rules name that is missing is an error to ok().
    AbilityRuntime(const content::AbilityRules& rules, std::map<int64_t, double> stats, double world_time_at_zero);

    bool               ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    /// Plan B3: whether the runtime holds this stat as part of its own state: MaxStamina (which Stamina is clamped to), the stats an effect
    /// raises (the Rest's regeneration, the Sprint's MoveSpeedMult) and the stats those scale from. A gear change of such a stat goes
    /// through gear_stat(); Stamina itself is the runtime's, changed only by it.
    bool tracks(int64_t record) const;

    /// Plan B3: the gear changed `record` to `value` (what the stat engine now says, with no effect on): puts it into `c` as the pawn
    /// should now show it. A stat an effect is raising keeps its raise on top of the new value (the effect ends on the new base); a
    /// MaxStamina below the Stamina clamps the Stamina to it, in `c` too. No runtime event is made: the caller applies `c` with the
    /// equip reply.
    void gear_stat(bootstrap::AbilityChange& c, int64_t record, double value);

    /// What gear_stat() changes of the runtime (its stat values and the bases an effect ends on), so that a change the pawn then refuses
    /// can be taken back: the runtime must not hold what the client was never told.
    struct GearSnapshot {
        std::map<int64_t, double> stats, base;
    };
    GearSnapshot gear_snapshot() const { return {stats_, base_}; }
    void restore_gear(GearSnapshot s) {
        stats_ = std::move(s.stats);
        base_ = std::move(s.base);
    }

    // ---- plan D2: the generic runtime's hooks (session/ability_pipeline.h): it shares the arrays, the counters and Stamina with this one ------

    /// The next ServerInstanceId and EffectId: one counter each for the whole pawn, so that the generic runtime's items never share a key
    /// with Sprint's, Dodge's or Rest's.
    int64_t allocate_instance_id() { return next_instance_++; }
    int64_t allocate_effect_id() { return next_effect_id_++; }
    /// Stamina after the generic runtime spent `amount` of it (never below 0), put into `c`; false when there is less than `amount`.
    bool spend_stamina(bootstrap::AbilityChange& c, double amount);
    /// Stamina moved by `amount` (a hit's gain or loss), clamped to 0..MaxStamina and put into `c`; returns the change that took effect.
    double add_stamina(bootstrap::AbilityChange& c, double amount);
    /// The value of the stat mod of a Sprint, Dodge or Rest effect (the formula's result for the ranks the character holds: General_Sprint's
    /// MoveSpeedMult 0.2 + 0.05 for each Sprint Training rank) in place of the number the content's row carried; applied at once when the
    /// effect is on (the change goes out as an event).
    void set_effect_value(int64_t effect_guid, int64_t stat_guid, double value, double now);
    /// The Stamina regeneration per second in place of content.db's constant: the engine's StaminaRegeneration stat (5 with nothing stopping it,
    /// more under a buff such as the Epic song's). A pause or the Sprint's effect still stops it, as the formula's own effect checks do.
    /// The pawn's StaminaRegeneration item follows it: 0 while the regeneration is stopped (the Sprint's effect and the player moves, the Evasion, the
    /// exertion pause), the rate otherwise, as the retail server replicates it [R: capture 745: 0.0 with the Sprint's first charge, at its end and at a
    /// Dodge, 5.0 one second after the pause]; the client reads that item (a pawn without one is left alone).
    void set_regen_rate(double per_second, double now);
    /// The player moves or stands (from the A6 moves).
    void set_moving(bool moving, double now);
    void set_in_combat(bool in_combat, double now);
    /// The mounts milestone, stage 3 fix 2: the player gets on (or off) a mount.  A mounted rider has no Sprint and no Stamina use [R: General_Sprint is not bUsableWhileMounted, and
    /// the rider's moves are the mount's]: getting on ends a running Sprint the way a Sprint ends (its items removed, MoveSpeedMult back to 1.0, the StaminaRegeneration item,
    /// ClientEndAbility at once) but without the exertion pause, and a Rest; while mounted no Sprint, Dodge or Rest starts (each is refused with ClientEndAbility), the player
    /// counts as standing (nothing is charged) and Stamina regenerates as it does for a standing player.  Getting off changes nothing else: the Sprint does not come back.
    void set_mounted(bool mounted, double now);
    bool mounted() const { return mounted_; }
// ~ C | O | A - C.Y.B ~

    const content::AbilityRules& rules() const { return rules_; }

    /// Whether worldd runs the ability (Sprint, Dodge, Rest).
    bool enabled(int64_t ability_guid) const { return rules_.ability(ability_guid) != nullptr; }

    /// ServerTriggerAbility. The events say what happened; a refusal also queues ClientEndAbility(client_instance_id)
    /// (except for Duplicate).
    Refusal trigger(int64_t ability_guid, int32_t client_instance_id, double now, const TriggerInfo& info = {});

    /// ServerNotifyHeldAbilityCancelled: true when it released a running Sprint or Rest. An unknown or finished instance
    /// is ignored.
    bool cancel(int32_t client_instance_id, double now);

    /// Runs the timers that fell due up to `now`.
    void tick(double now);

    /// The events since the last call, oldest first.
    std::vector<AbilityEvent> take_events();

    // ---- state, for the log and the tests ------------------------------------------------------------------------
    double stamina() const { return stat(content::kGuidStamina); }
    double max_stamina() const { return stat(content::kGuidMaxStamina); }
    double stat(int64_t record) const {
        const auto it = stats_.find(record);
        return it == stats_.end() ? 0 : it->second;
    }
    bool   sprinting() const { return sprint_.has_value(); }
    bool   resting() const { return rest_.has_value() && !rest_->ended; }
    bool   rest_effect_on() const { return rest_.has_value() && rest_->effect_key.has_value(); }
    bool   evading() const { return evasion_.has_value(); }
    bool   regeneration_blocked(double now) const;
    double regen_pause_until() const { return pause_until_; }
    double dodge_cooldown_until() const { return dodge_cooldown_until_; }
    bool   moving(double now) const { return moving_ && now - moving_at_ <= kMovingStale; }
    size_t refused() const { return refused_; }
    /// The ServerInstanceId of the running (or released, for a Rest) instance the client knows as `client_instance_id`.
    std::optional<int64_t> instance_of(int32_t client_instance_id) const;
    /// Whether a client that dodged may still move at the dash's speed (the A6 checker's allowance): from a Dodge's trigger to
    /// the end of its Evasion plus kDashMargin.
    bool   dash_allowed(double now) const { return now <= dash_until_; }
    /// The next time something is due, or +infinity.
    double next_due() const;

private:
    /// Whether ending an ability tells the client: not at all, with the event (ClientEndAbility right after the update), or
    /// kEndAbilityDelay after it.
    enum class Tell : uint8_t { No, Now, Later };

    struct Sprint {
        int32_t client_id;
        int64_t instance;
        int64_t effect_key;
        double  next_charge;
    };
    struct Dodge {
        int32_t client_id;
        int64_t instance;
        double  until;   // the cooldown's end: the ability item and the cooldown item go
    };
    struct Evasion {
        int64_t effect_key;
        double  until;
    };
    struct Rest {
        int32_t                 client_id;
        int64_t                 instance;
        bool                    ended = false;               // cancelled, or replaced: the item stays, the effect goes
        std::optional<double>   effect_due;                  // the Rest effect is added then
        std::optional<int64_t>  effect_key;                  // the Rest effect, once added
        std::optional<double>   remove_due;                  // and removed then
    };
    struct PendingEnd {
        int32_t client_id;
        double  due;
    };
    struct Pause {   // the DisableRegen effect item the exertion pause is replicated as
        int64_t effect_key;
        double  until;
    };

    double now_ = 0;
    void   advance(double now);
    void   run_due(double t);

    void   emit(AbilityEvent e);
    double sync(double t) const;
    void   set_stat(bootstrap::AbilityChange& c, int64_t record, double value);
    /// The effect's stat mods applied to (or taken off) the stats they name, as base + factor x source + addend.
    void   apply_mods(bootstrap::AbilityChange& c, const content::EffectRule& e);
    void   revert_mods(bootstrap::AbilityChange& c, const content::EffectRule& e);
    int64_t next_effect_id() { return next_effect_id_++; }
    bootstrap::EffectItem effect_item(const content::EffectRule& e, int64_t key, double t) const;
    bootstrap::ActiveAbilityItem ability_item(const content::AbilityRule& r, int64_t instance) const;
    void   refuse(int32_t client_instance_id, Refusal why, double t, const char* what);

    void   start_sprint(int32_t id, double t);
    void   end_sprint(double t, Tell tell, const char* why, bool exertion = true);
    void   charge_sprint(double t);
    /// Takes one charge from Stamina into `c`; true when what is left is below the Sprint's minimum.
    bool   take_charge(bootstrap::AbilityChange& c);
    void   start_dodge(int32_t id, double t, const TriggerInfo& info);
    void   end_evasion(double t);
    void   end_dodge_items(double t);
    void   start_rest(int32_t id, double t);
    void   release_rest(double t, const char* why);
    void   add_rest_effect(double t);
    void   remove_rest_effect(double t);
    void   regenerate(double t);
    /// The StaminaRegeneration item as the pawn should show it at `t`: 0 while regeneration is stopped, its rate otherwise (put into `c` when it differs
    /// from what the pawn shows; nothing when the pawn has no such item).
    void   sync_regen(bootstrap::AbilityChange& c, double t);
    /// Starts the regeneration pause `owner`'s End hit names: its DisableRegen effect item goes into `c` (replacing one that
    /// is running).
    void   pause_regeneration(bootstrap::AbilityChange& c, const content::EffectRule& owner, double t);
    void   end_pause_effect(double t);

    const content::AbilityRules rules_;
    std::optional<double>       regen_rate_override_;                       // plan D2
    std::map<std::pair<int64_t, int64_t>, double> mod_value_;            // plan D2: (effect, stat) -> the mod's value from the engine's formula
    std::map<int64_t, double>   stats_;
    std::map<int64_t, double>   base_;        // the stats an effect's mods raise (the Rest's, the Sprint's), as they are without it; and the StaminaRegeneration's rate (the pawn shows 0 while it is stopped)
    std::set<int64_t>           tracked_;     // plan B3: MaxStamina, base_'s stats and the stats the mods scale from
    double                      world_time_at_zero_;
    std::string                 error_;
// CoA captures, C_Y_B code

    bool   moving_ = false;
    bool   mounted_ = false;
    double moving_at_ = 0;
    bool   in_combat_ = false;
    int64_t next_effect_id_ = 0;
    int64_t next_instance_ = 0;
    int32_t rest_animation_counter_ = 0;
    double next_regen_ = 0;
    double pause_until_ = -1;
    double dodge_cooldown_until_ = -1;
    double dash_until_ = -1;
    std::optional<Sprint>  sprint_;
    std::optional<Dodge>   dodge_;
    std::optional<Evasion> evasion_;
    std::optional<Rest>    rest_;
    std::optional<Pause>   pause_effect_;
    std::vector<PendingEnd> pending_ends_;   // ClientEndAbility calls due later, oldest first
    size_t                 refused_ = 0;
    std::vector<AbilityEvent> events_;
};

}  // namespace aoc::worldd
