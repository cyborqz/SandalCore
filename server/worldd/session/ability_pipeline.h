// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_pipeline.h  (plan D2, stage 1)
//
//  The generic ability runtime of one player: any ability of content.db's ab_* tables the character knows, run from the data. It sits beside
//  plan A7's AbilityRuntime (session/abilities.h), which keeps Sprint, Dodge and Rest (their held and toggle mechanics, the Stamina pauses,
//  the dash window and the Rest montage are not in any table), and shares with it the pawn's arrays (one counter for ServerInstanceIds and
//  one for EffectIds), Stamina, the stats an effect of A7's raises, and the clock the session gives. Like AbilityRuntime it is pure logic on
//  that clock: no sockets, no Connection, no replicator; it says what changed as the same AbilityEvent values.
//
//  What a ServerTriggerAbility does here [R: the retail captures 36, 37, 38 and 44, research/d2/evidence.md and picks.json; every number below
//  is that evidence unless it says "modelled"]:
//
//    validate   the ability is known (KnownAbilities), the catalog has it, no other cast is waiting for its cost, it needs no target or the
//               call names one (nothing but the caster is in this world, so the target is the caster), a shape stage 1 runs (below), its own
//               activation requirement holds, its cooldown group has a charge left, and every cost is affordable: each stat cost (Mana, Health,
//               Stamina, a class resource) against the current value, each effect cost (Fortissimo's five stacks of Resonance) against the
//               stacks. Anything else is refused the way A7 refuses: ClientEndAbility(ClientInstanceId) and nothing changes. A formula that
//               does not compile refuses the ability (NoFormula), logged. A ClientInstanceId the pipeline has seen lately is dropped with no answer.
//    cast       one trigger makes up to four steps, each at its own time after the trigger (the montage's notifies):
//                 item       the ability item (ReplicatedActiveAbilities): the ability, a ServerInstanceId from the pawn's counter, the montage's
//                            static NetGUID by the call's AnimationIndex (ab_montage.net_guid), PlayRate 1.0, MontageGroup (DefaultGroup;
//                            Flourish EmoteGroup, the songs SongGroup), Targets the caster when the call named one; the previous item
//                            of the pipeline goes in the same update (every retail reply says "deleted 1"). About 300 ms after the trigger
//                            (299-331 ms in 12 of 14 captured casts; kItemDelay); none for an ability with no montage (Fortissimo)
//                 cost       the same packet in retail: the Cooldowns item (bCooldownTriggered, InitialCooldown, the start and end on the
//                            synchronized clock, Charges = charges LEFT: Flourish's first use 1, its second 0), the cost deltas (Mana is an absolute
//                            value in the EveryoneProxy array: 3281.125 - 56.875), the class resource and the effect costs. Also about 300 ms,
//                            later for a long montage (kCostDelay: Mesmerizing 1011 ms, Saga 906, Story_Joy 1573; modelled for the rest)
//                 effects    the effects of the ability's hits on the caster, as CurrentEffects items with the record's duration, stacks and
//                            tick, and the instant stat changes of those hits (Glee's heal): 35 ms after the cost for the ability's own hits
//                            (retail +34, +36, +32), with it for a zone's overlap hits (Nimble's Evasion arrives with the cost) and for an ability with no
//                            montage, 340 ms after it for a projectile's hits (Jest's Glee, Pep and heal came +343 after the cost)
//               The cost, the cooldown and the stat deltas are applied at the cost step, not at the trigger.
//    hits       a hit applies to the caster when its mask names Self (1) and its requirement holds: the ability's own hits (ab_ability_hit "hit"), the
//               hits of the projectiles it fires when it has a target (the caster), the first overlap of the zones it spawns (the Initial and
//               OnBeginOverlap hits; their ticks are the client's requests, and see "zones" below) and the Tick hits of an effect the caster has. An applied hit gives
//               the effects of its rows (the requirement, stack count and duration of each) and its stat mods on Health and Mana (a heal: the
//               expression's value, clamped at the maximum, and a damage that reaches the caster goes through the shields first).
//               Plan D2, stage 2: the CONFIRMATION path. The client draws its floating numbers from ClientHitConfirmationsReliable's stat mods
//               (the StatMod record, the Value the formula gave and the EffectiveValue that took effect: 0 at full Health, as retail's overheal
//               shows), so every hit that has stat mods, an effect to show (OnHitFXId or OnActivateFXId) or the Party bit in its mask is confirmed:
//                 * a hit the client asks for with RequestHitVerification_Server (an ability's hit whose key's targeting is not Default_Self: an
//                   ally or area hit, FlashCure's heal) is applied when the request comes (kConfirmDelay after it, never before the cost step plus
//                   kHitDelay) and answered; a request for a hit that finds nobody (no component of ours) gets an empty confirmation. A hit the
//                   client does not ask for within kRequestDeadline of the cost step is applied and confirmed by the server [modelled: the real
//                   client always asks; it keeps the runtime working if it did not];
//                 * a Default_Self hit is never confirmed (0 of 317 requests in the captures) and a request for one is accepted quietly;
//                 * a projectile's hit and an effect's Tick hit are server-originated: confirmed when they land.
//               A forged request (an ability that was not cast lately, a hit that is none of its own, too many, too fast) is dropped.
//    shields    a stat mod of an effect on TempHealth (a float-array stat) is a shield: the effect's instance holds its amount (Base = Value when
//               it is given), the item of the TempHealth array lists every live shield, a damage that reaches the caster takes from the oldest
//               first, and the effect's end (or the removal of the instance) takes its element out.
//    effects    an effect instance has stacks, a start and an end (the record's duration, infinite, or the hit row's). Stacking is the record's:
//               StackInPlace adds stacks up to its in-place limit (Resonance 10, Glee 1), OverrideAlways and OverrideDuration keep one instance and
//               refresh it, StackSeperate adds an instance up to the global limit; the duration refreshes always (the item's start is now), extends
//               (Glee refreshed 13.8 s into its 15 s showed 28.789: the remaining time plus the full one) or never. An instance ends at its end
//               (its item goes; the Expire and End hits that reach the caster run), ticks every TickTimer (the Tick hits that reach the caster,
//               Glee's heal every 2 s, the first tick one period after the application when the record ignores the initial one). Every change of
//               the effect state is run through the stat engine (the same effects, as the character's effect state) and only the stats whose
//               value changed are sent: Mitigation 0.9652 -> 0.9362 under Glee (x (1 - 0.03)), HealingMult x 1.05, MoveSpeedMult +0.2 under Pep.
//    class      a Bard that spends Mana gains one stack of Bard_Resonance per cast (30 s, at most 10) [R: every captured Bard cast but Fortissimo's
//               and the songs', which spend none; the reaction that does it in the game is stored and inert, so this is modelled]; Fortissimo pays
//               five stacks. The hits' own Resonance applications are left out so a cast never gains two.
//    songs      an ability that lists an infinite ticking effect (the Bard's Music_ songs, Berserk, Whirlwind, Protect) is a loop whose effects start
//               2.3 s on and repeat: stage 2. Stage 1 plays its ability item and its cost and cooldown and nothing else (logged once).
//    refused    in stage 1 and logged: an ability that needs a target and is applicable to no ally (Mock, Discordance ...), a held ability that is
//               no toggle of A7's, an ability named with an item. An ability with no required target is accepted whatever its mask: cast at the
//               air it spends its cost, starts its cooldown and plays its montage with no hits (Maddening, Misdirection).
//
//  Other actors (plan E1, stage 3).  A cast whose call names an actor other than the caster (an NPC the session resolved from the call's Targets) is validated
//  against the world (combat/hit_port.h): the target is there and alive, an enemy when the ability harms and not when it helps, in range (the data's
//  OutOfRangeDistance and the cast leeway; a weapon combo's hit reaches ThirdPartyHitDistance) and in sight (Detour's raycast); anything else is refused
//  (BadTarget, OutOfRange, NoLineOfSight). A hit that names such an actor (RequestHitVerification_Server's components, a projectile's target) is resolved
//  by the hit rules (combat/hit_resolver.h: the rolls, then each stat mod's expression run as the caster with the target's stats in the role of
//  GetTarget()), delivered to the world (Health, threat, flags, the death) and confirmed with ONE FHitInst whose targets are the actors hit, each with its
//  stat mods (Value and Effective) and its flags (Crit, Block, Miss, Lethal). The effects of the hit's rows go to the target (a shield on an NPC), the
//  "self" rows to the caster. Heals and shields on another actor go to that actor, not to the caster. The damage hits of NPCs on the caster come in as
//  receive_hit().
//
//  zones      an ability that spawns a zone with its cast (ab_ability.extra.LingeringEffectsIds, minus the zones its hits spawn: a Bard's dances)
//             is an actor on the client (AAoCAbilityLingeringEffect: the visual effect). The cast makes a Zone: its actor opens at retail's offset
//             after the trigger (ability_timings.h zone_ms; else 35 ms after the item) [R: capture 44: Maddening +362 ms, Nimble +300, Mesmerizing
//             +389, Shielding +462 and +112], the dance ends at retail's length (zone_close_ms less kZoneCloseDelay; else the zone's life, 30 s): the
//             infinite effects its overlap hits put on the caster go (Shielding's Aura), the actor fades (bFadingOut) kZoneFadeDelay later and its channel
//             is closed kZoneCloseDelay after the effects. The events carry ZoneOp values; the session opens the channel. The client's requests for the
//             zone's tick hits are taken while the actor lives.
//
//  Cooldowns. The ledger is per cooldown group (the shared tag, else the ability) with charges: using a charge starts one timer for the
//  charge being recovered; at its end a charge returns, the next timer starts while charges are missing, and the item goes when all are back.
//  The Cooldowns item keeps the first use's start through the whole chain and its end moves on by one cooldown at each return [R: Jest's return
//  item is start S, end S + 16, Charges 1: captures 39 and 44].
//  The pawn's Cooldowns array therefore always holds every ability on cooldown (a Dodge's update does not drop the others: the client's own
//  predicted cooldowns are on the server's array too).
//
//  Regeneration (absorbed from plan D1's tick). Every second of the pipeline's clock Health and Mana gain their regeneration stat (the value the
//  pawn holds: a Rest's boost counts), clamped at the maximum; the stat engine gives it five times faster out of combat and a world with no NPCs is
//  always out of combat. Stamina stays A7's tick (its own pauses are effects of A7's), with the rate set from the engine's StaminaRegeneration.
//
//  Movement. An ability whose data (NetworkingMaxSpeed) or capture says it moves the character opens a window after its trigger in which the A6
//  move checker allows its speed: Flourish slides at up to 3,659 cm/s for 0.6 s (capture 44, both casts), so 1.5 x that for 1 s; the rows
//  with a NetworkingMaxSpeed (Blink 7,000 ...) 1.5 x it for 1 s.
//
//  Modelled, not captured: the Busy refusal, every refusal, the cost step's re-check, the shared groups' items, the montage groups of the
//  abilities no capture shows, the timings of the 11 abilities no capture shows, the effect of a cancel (none: retail's reply was unchanged).
// ============================================================================
#pragma once

#include "combat/hit_port.h"
#include "combat/hit_resolver.h"
#include "session/abilities.h"
#include "session/ability_catalog.h"
#include "stats/character_stats.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd {

class AbilityPipeline {
public:
    // ---- numbers that are not content.db's, each with its evidence (header) --------------------------------------------------------------
    /// The ability item and the cost step come this long after the trigger: the montage's first notify.
    static constexpr double kItemDelay = 0.300;
    static constexpr double kCostDelay = 0.300;
    /// An ability's own self hits and an ability with no montage: these long after the cost step.
    static constexpr double kHitDelay = 0.035;
    /// A projectile that reaches the caster lands this long after the cost step.
    static constexpr double kProjectileDelay = 0.340;
    /// Health and Mana regeneration: every second of the clock, at most this many ticks at once after a stall.
    static constexpr double kRegenInterval = 1.0;
    static constexpr int    kMaxRegenSteps = 5;
    /// The time a confirmation follows the client's request, at the earliest (retail: p50 80 ms, 58-355 ms in 83 pairs).
    static constexpr double kConfirmDelay = 0.080;
    /// How long after its cost step a cast takes requests for its hits, the most requests one hit takes in a cast and the least time between two
    /// of them (retail: FlashCure's three requests are 200 ms apart).
    static constexpr double kRequestWindow = 5.0;
    static constexpr int    kMaxRequestsPerHit = 8;
    static constexpr double kMinRequestGap = 0.100;
    static constexpr size_t kMaxRecords = 16;
    /// The most casts waiting for their steps at once.
    static constexpr size_t kMaxCasts = 16;
    /// A song's (or any looping ability's) own effects start this long after the trigger [R: Bard_Music_Cheerful, effects at +2321 ms in capture 44]
    /// and its loop effect re-adds the others every tick; GetAbilityProgressRatio("SongGroup") reaches 1 this long after they started [R: the HoT
    /// of Cheerful rises 3.4 of 50.9 every 2 s: +50% in 15 s].
    static constexpr double kLoopDelay = 2.3;
    static constexpr double kSongProgressSeconds = 15.0;
    /// How long a montage in the DefaultGroup is taken to run for GetAbilityProgressRatio("DefaultGroup") and GetProgressDuration [modelled: a montage's
    /// length is in no table].
    static constexpr double kCastSeconds = 3.0;
    /// A held ability (Guard, Reflect: hold_animation) ends this long after the client's ServerNotifyHeldAbilityCancelled [R: Guard's effect goes +59 ms after
    /// the cancel in capture 124].
    static constexpr double kHeldEndDelay = 0.059;
    /// The client's ServerEndAbility is answered this long after [R: 259 ms in capture 152].
    static constexpr double kEndDelay = 0.259;
    /// How many effects deep an effect's Apply hit may apply further effects.
    static constexpr int kMaxApplyDepth = 4;
    /// A movement ability's window after its trigger and the factor on its measured or data speed.
    static constexpr double kMovementWindow = 1.0;
    static constexpr double kMovementMargin = 1.5;
    /// A lingering zone's actor [R: capture 44: Maddening +362 ms after its trigger, 34 ms after its ability item (+328); Nimble +300 (item +300),
    /// Mesmerizing +389 (item +324, cost +1011), Shielding +462 (item +331)]: opened this long after the ability item.
    static constexpr double kZoneOpenDelay = 0.035;
    /// A dance ends with its montage [R: capture 41 and 44: the actors of five dances were destroyed 5,123 and 5,371 ms (Shielding), 5,383 (Maddening), 4,823
    /// (Nimble) and 2,334 ms (Mesmerizing) after their triggers, whatever else the Bard cast meanwhile; the zone's own life in the data is 30 s (3.5 s) and a
    /// montage's length is in no table, so ability_timings.h holds the captures' offset): the infinite effects its overlap hits put on the caster go, the actor
    /// starts its fade-out (bFadingOut) this long after they went [R: +61 ms] and is destroyed this long after them [R: the Aura of Shielding Dance is deleted
    /// 702 ms before its actor's close, the fade-out 641 ms before].
    static constexpr double kZoneFadeDelay = 0.06;
    static constexpr double kZoneCloseDelay = 0.70;
    /// The client's requests for a zone's tick hits go on while the actor lives: a cast takes them that much longer, and as many as the zone's life
    /// holds ticks (plus this many).
    static constexpr double kZoneRequestSlack = 1.5;
    static constexpr int    kZoneExtraRequests = 8;
    /// The most zone actors alive at once.
    static constexpr size_t kMaxZones = 16;
    /// How many ClientInstanceIds are remembered to drop a replayed one.
    static constexpr size_t kRecentIds = 256;
    /// The most effect instances at once (a hostile ability list cannot grow it without bound), and the most steps one advance runs.
    static constexpr size_t kMaxEffects = 256;
    static constexpr size_t kMaxStepsPerAdvance = 100000;
// CoA captures, C-Y-B code

    using WornFn = std::function<std::vector<stats::WornItem>()>;

    /// `catalog` may be null (a content.db without the ability tables, a test with none): only regeneration runs then. `stats` and `a7` belong to
    /// the session and outlive the pipeline; `worn` gives the equipment the stat engine runs with. `seed` seeds RandDouble.
    AbilityPipeline(const abilities::Catalog* catalog, stats::CharacterStats& stats, AbilityRuntime& a7, WornFn worn, double world_time_at_zero,
                    uint64_t seed = 1);

    /// KnownAbilities: what the character may trigger.
    void set_known(std::vector<int64_t> known);
    bool known(int64_t ability) const { return known_.count(ability) != 0; }
    /// Whether the generic runtime runs this ability (the catalog has it and A7 does not own it).
    bool handles(int64_t ability) const;

    /// Plan E1, stage 3 fix, Part C (the tooltip is the truth): the numbers the client's tooltip states for an ability, as THIS runtime evaluates them for
    /// the character as it is now (its level, gear, ranks and effects), through the very formulas a trigger() runs (resolve_formulas), with nothing cast
    /// and nothing changed. nullopt for an ability the catalog lacks; a formula that does not compile leaves `problem` set and its field at the default.
    struct StatCost {
        int64_t     stat = 0;
        std::string name;
        double      amount = 0;
    };
    struct Inspection {
        double                  cooldown = 0;           // seconds per charge
        int                     max_charges = 1;
        int                     charge_cost = 1;
        bool                    triggers_cooldown = false;
        std::string             group;                  // the cooldown group's key (the shared tag, else the ability's guid)
        std::vector<StatCost>   stat_costs;             // the enabled cost groups' stat costs (an amount of 0 is left out, as a trigger leaves it out)
        std::vector<std::pair<int64_t, int>> effect_costs;
        std::optional<double>   cast_seconds;           // the record's DynamicCastDuration (none: the montage's, a client-side time)
        bool                    requirement = true;     // the activation requirement, with no target named
        std::string             problem;
    };
    std::optional<Inspection> inspect(int64_t ability);
    /// One expression of the catalog (an effect's duration, a hit's requirement ...) evaluated for the character as it is now, with nothing in progress: what
    /// the client's tooltip evaluates for the player. False with `why` for one that does not compile.
    bool evaluate(int64_t expr, double& out, std::string& why) {
        drop_evaluation();
        return eval(expr, out, why);
    }

    /// ServerTriggerAbility. Refusal::None when the cast was accepted; every other value except Duplicate has queued ClientEndAbility.
    Refusal trigger(int64_t ability, int32_t client_instance_id, double now, const TriggerInfo& info = {});
    /// ServerNotifyHeldAbilityCancelled: a pipeline instance's cast goes on (retail's reply was unchanged); true when the instance is one of ours.
    bool cancel(int32_t client_instance_id, double now);

    /// ServerEndAbility: the client ends the instance with this ServerInstanceId (an ability of ours it started): kEndDelay later the item's montage is
    /// cleared and ClientEndAbility(its ClientInstanceId) is sent [R: Cleric_SoothingGlow, capture 152: +259 ms]. False for an instance that is none of ours
    /// or was ended already.
    bool end_request(int32_t server_instance_id, double now);

    /// RequestHitVerification_Server: what the client says it hit. `own` of the `components` are the caster's own (nothing else is in this world).
    enum class HitVerdict : uint8_t {
        Scheduled,     // a hit of the caster's: applied and confirmed kConfirmDelay on
        Quiet,         // a Default_Self hit (applied with the cast, never confirmed) or a repeat the cast does not need: accepted, nothing sent
        Empty,         // it found nobody: an empty confirmation is sent
        NoCast,        // no cast of the ability is open (or it never was)
        NotItsHit,     // the hit is none of the ability's
        TooMany,       // more requests of this hit than a cast takes, or too soon after the last
        Unsupported,   // not a hit the runtime serves
    };
    static const char* verdict_name(HitVerdict v);
    /// `targets`: the actors other than the caster's own that the components name (an NPC's ObjectId), as the session resolved them.
    HitVerdict hit_request(int64_t ability, int64_t targeting_guid, int64_t hit_guid, size_t components, size_t own, double now, const std::vector<uint64_t>& targets = {});

    /// Plan E1, stage 3, part 2: the character died (or came back): a dead character casts nothing (every trigger is refused, Refusal::Dead), the casts waiting
    /// are dropped, nothing regenerates and the hits that name another actor are not made.
    void set_dead(bool dead);
    bool dead() const { return dead_; }
// built with C | O | A and C | Y | B

    /// Plan E1, stage 3: the world's port (null: the caster is alone in its world, as in stage 2). Not owned.
    void set_combat(combat::HitPort* port) { combat_ = port; }
    /// An NPC's hit on the caster, resolved by the world: its stat mods move the character's Health through the shields, the confirmation names the NPC as the
    /// instigator and carries the flags (Lethal when the hit took the last Health). The change and the confirmation are an event.
    void receive_hit(const combat::IncomingHit& hit, double now);
    /// The mounts milestone, stage 3: the same hit, but it landed on the character's mount (the rider is on it: hits aimed at a mounted rider land on the mount's Health, and
    /// the rider's own is untouched). The session took `taken` of the mount's Health; this makes only the confirmation, naming `target` (the mount's actor) with the stat mods as
    /// the client draws them. Lethal when `lethal`. Nothing changes on the character.
    void confirm_hit_on(const combat::IncomingHit& hit, double now, const protocol::emit::FIntrepidNetworkGUID& target, const protocol::emit::FIntrepidNetworkGUID& cylinder,
                        double taken, bool lethal);
    /// Runs what fell due up to `now`: cast steps, effect ticks and expiries, cooldown timers, regeneration.
    void tick(double now);
    std::vector<AbilityEvent> take_events();
    /// The next time something is due, or +infinity.
    double next_due() const;

    /// The numbers the formulas give Sprint, Dodge and Rest for the ranks the character holds, handed to A7's runtime: the value of the Sprint effect's
    /// MoveSpeedMult mod (0.2 + 0.05 for each Sprint Training rank held: 1.3 for a level-25 Bard) and the engine's StaminaRegeneration. Called at the
    /// start and whenever the character's ranks, level or gear changed.
    void sync_a7_values(double now);

    /// Gives the caster `stacks` stacks of an effect record with the record's own duration (an item's effect, a GM's, a test's); the change is an
    /// event. False for an effect the catalog does not have.
    bool grant_effect(int64_t effect_guid, int stacks, double now);

    /// Whether the item and cost steps come at the times the retail captures show for the ability (session/ability_timings.h, the default), or all at the
    /// montage's first notify of 300 ms (what the runtime's own tests of its mechanics use).
    void set_data_timings(bool on) { data_timings_ = on; }

    /// Takes every instance of an effect record off the caster (a dispel, an item's end); true when there was one.
    bool remove_effect(int64_t effect_guid, double now);

    /// The character moves or stands, as the formulas that ask read it.
    void set_moving(bool moving);
    /// The character's effect state, ranks or level changed (a level-up, a purchase): the engine's inputs are the session's; this re-runs the
    /// stats with the effects the pipeline holds and sends the stats that changed (an event), as an equip does.
    /// Without `with_stats` the stats are not run and not sent: the caller's own run (a class change's, which diffs against what the client has)
    /// takes the effect state this leaves.
    void refresh(double now, const char* why, bool with_stats = true);

    /// Damage that reaches the character (a self hit, a fall, the tests): the shields take it first, the oldest shield first, then Health (never
    /// below 0). Returns what got through to Health. The change is an event.
    double take_damage(double amount, double now);
    /// The shields on the character, oldest first: (amount given, what is left).
    std::vector<std::pair<double, double>> shields() const;

    /// The speed in cm/s the A6 move checker allows now for a movement ability (0: none).
    double movement_allowance(double now) const;

    // ---- state, for the log and the tests --------------------------------------------------------------------------------------------------
    bool   casting() const { return !casts_.empty(); }
    size_t effect_count() const { return effects_.size(); }
    /// The stacks of the effects of a record on the character (0: none).
    int    stacks_of(int64_t effect_guid) const;
    /// The instance of an effect record: its end time on the pipeline's clock (infinity for an infinite one); nullopt when it is not on.
    std::optional<double> effect_end(int64_t effect_guid) const;
    size_t cooldowns_running() const { return groups_.size(); }
    /// Charges left in the cooldown group of an ability (-1 when it has no group running).
    int    charges_left(int64_t ability) const;
    double now() const { return now_; }
    /// The class the pipeline's class rule was resolved for (ECharacterClass; a change of the character's class needs refresh()).
    int    resource_class() const { return resource_class_; }
    size_t refused() const { return refused_; }
    size_t zones() const { return zones_.size(); }
    size_t casts_started() const { return casts_started_; }
    size_t ticks_regen() const { return regen_ticks_; }
    size_t unsupported_logged() const { return unsupported_.size(); }
    double world_time_at_zero() const { return world_time_at_zero_; }

private:
    /// Where a hit's confirmation comes from: the record type and guid of its source, whether the Source is the pawn, and whether the server may
    /// confirm it at all (a Default_Self hit never is).
    struct HitSource {
        int64_t type = 0;
        int64_t guid = 0;
        bool    actor = true;
        bool    confirm = false;
        bool    response = false;
        bool    simulated = false;   // bPredictedOnSimulated
        bool    autonomous = true;   // bPredictedOnAutonomous
        /// E1 stage 3 fix 3: the confirmation's Source is the target actor itself, one confirmation per target (the hits fired where a projectile
        /// landed: the book's Split hits; retail: Source = the target in 1,289 of 1,289, one target each)
        bool    source_is_target = false;
    };
    /// One of an open cast's hits the client may ask for.
    struct Pending {
        double at = 0;
        bool   found = true;   // the hit reached the caster (false: an empty confirmation)
        std::vector<uint64_t> targets;   // plan E1, stage 3: the other actors it names
    };
    struct HitSlot {
        const abilities::HitDef* hit = nullptr;
        int64_t                  targeting = 0;
        bool                     reaches = false;   // the hit's mask names the caster
        bool                     aims_others = false;   // plan E1, stage 3: its mask names enemies or allies other than the caster
        int                      requests = 0;
        double                   last_request = -1e9;
        std::vector<Pending>     due;          // when a request's application runs
        bool                     applied = false;   // the server applied it by itself (no request came by the deadline)
    };
    /// A cast that takes hit requests until its window ends.
    struct CastRecord {
        int32_t                      client_id = 0;
        int64_t                      ability = 0;
        double                       trigger_at = 0;
        double                       cost_at = -1;     // when its cost step ran (-1: not yet)
        double                       cost_due = 0;     // when it will
        double                       window_end = 0;
        double                       hit_delay = kHitDelay;   // how long after the cost step the cast's hits may apply (its montage's, none without one)
        int                          max_requests = kMaxRequestsPerHit;   // a zone's tick hit is asked for as often as the zone's life holds ticks
        std::vector<HitSlot>         slots;
        std::set<int64_t>            reach;            // every hit that belongs to the ability
        std::map<int64_t, std::pair<int, double>> asked;   // hit guid -> (requests, time of the last) for the hits that are no slot
    };
    struct Cast {
        int32_t                               client_id = 0;
        const abilities::AbilityDef*          def = nullptr;
        double                                trigger_at = 0;
        std::optional<double>                 item_due, cost_due, hit_due, projectile_due, loop_due, own_due;
        std::vector<const abilities::EffectDef*> own_effects;   // the infinite effects the ability puts on its caster itself (no hit applies them)
        bool                                  toggle_off = false;   // the ability is a loop that is on: this trigger ends it
        int64_t                               chain_owner = 0;      // a chain (Cleric_Mend's five projectiles that fire one hit): the one projectile this cast fires
        int64_t                               instance = -1;
        int32_t                               animation_index = 0;
        bool                                  target = false;     // the call named a target (the caster)
        bool                                  self_reachable = true;   // E1 stage 3 fix: the ability's mask names Self, Ally, Party or Raid (a projectile may land on the caster)
        std::vector<uint64_t>                 target_actors;      // plan E1, stage 3: the other actors the call named (validated)
        bool                                  loop = false;       // a song or a toggle: the item, the cost and the cooldown only
        double                                cooldown = 0;
        int                                   max_charges = 1;
        int                                   charge_cost = 1;
        std::string                           group;
        std::vector<std::pair<int64_t, double>> stat_costs;       // stat guid, amount
        std::vector<std::pair<int64_t, int>>  effect_costs;       // effect guid, stacks
        bool                                  triggers_cooldown = false;

    };
    struct EffectInst {
        int64_t                     owner = 0;                    // plan D2, stage 2: the looping ability whose effect this is (0: none)
        int64_t                     zone = 0;                     // plan D2, review: the lingering zone (instance) whose overlap hit put this on the caster: it ends with the zone
        bool                        has_shield = false;           // plan D2, stage 2: one of its mods is on TempHealth
        float                       shield_base = 0, shield_value = 0;
        uint32_t                    shield_guid[4] = {0, 0, 0, 0};
        int64_t                     key = 0;                      // the EffectId of its CurrentEffects item
        const abilities::EffectDef* def = nullptr;
        int                         stacks = 1;
        double                      start = 0, end = 0;           // end: infinity for an infinite effect
        double                      next_tick = 0;                // infinity: none
        std::vector<stats::EffectMod> mods;                       // one stack's values
    };
    /// A lingering zone the ability spawned with its cast: its actor on the client (the dance's visual effect) and the infinite effects its overlap hits
    /// put on the caster, which end with the dance.
    struct Zone {
        enum class State : uint8_t { Pending, Open, Ended };
        int64_t id = 0;
        int64_t ability = 0, zone = 0;
        int32_t client_id = 0;         // the cast: its record takes the client's tick requests while the actor lives
        double  open_at = 0;
        double  end_at = 0;            // the dance ends: the effects go (open + the zone's life, or an interrupt)
        double  fade_at = -1, close_at = -1;
        State   state = State::Pending;
    };
    struct Group {
        int                  max_charges = 1;
        int                  charges = 1;                         // left
        double               cooldown = 0;                        // seconds per charge
        double               timer_start = 0, timer_end = -1;     // the charge being recovered (timer_end < 0: none)
        /// The chain: the first use of the group started it, and every charge that returns starts the next timer from the end of the last
        /// [R: retail's Cooldowns item keeps the FIRST use's start through the whole chain and moves its end on: Jest's return item is start S, end S+16,
        /// Charges 1 (captures 44, 39); Flourish's the same]. `timers` counts the timers the chain has started.
        double               chain_start = 0;
        int                  timers = 0;
        std::set<int64_t>    items;                               // the abilities whose Cooldowns item is on the pawn
        int64_t              last_ability = 0;
    };
    /// A held ability whose own effects are on: the client's cancel ends them.
    struct Held {
        int32_t               client_id = 0;
        int64_t               ability = 0;
        std::optional<double> end_due;
    };
    bool dead_ = false;   // plan E1, stage 3, part 2
    class Host;   // the ExprHost the programs run with
// -- C-O-A . C-Y-B --

    void   advance(double now);
    void   run_due(double t);
    double sync(double t) const;
    void   emit(AbilityEvent e);
    void   refuse(int32_t client_id, Refusal why, double t, const std::string& what, bool log_always = false);

    // evaluation
    bool   eval(int64_t expr, double& out, std::string& why);
    bool   eval_bool(int64_t expr, bool none, bool& out, std::string& why);
    /// plan E1, stage 3, part 2: an ability's activation requirement for a cast aimed at another actor: the program reads the target's side (IsApplicableTarget,
    /// the distance between the two, the relation) as the hit rules do; with no target named it is eval_bool.
    bool   eval_requirement(int64_t expr, const std::vector<uint64_t>& targets, bool& out, std::string& why);
    void   drop_evaluation() { evaluation_.reset(); }
    std::string expr_problem(int64_t expr) const;

    // stats
    double stat_now(int64_t guid) const;
    void   set_held(bootstrap::AbilityChange& c, int64_t stat, double value);
    /// Runs the stat engine with the effect state as it is and puts the stats that changed into `c`.
    void   recompute(bootstrap::AbilityChange& c);
    bool   take_stat(bootstrap::AbilityChange& c, int64_t stat, double amount);

    // casting
    /// The cast's cooldown, charges, charge cost and costs from the ability's formulas for the character as it is (what trigger() and inspect() share);
    /// returns what did not compile ("its cooldown", "a cost's amount" ...) with `why`, or null.
    const char* resolve_formulas(Cast& cast, const abilities::AbilityDef& def, std::string& why);
    void   run_cast_step(Cast& cast, double t);
    void   step_item(Cast& cast, double t);
    void   step_cost(Cast& cast, double t);
    void   step_hits(Cast& cast, double t, const char* which);
    void   finish_cast_if_done(size_t index);
    std::string montage_group_of(const abilities::AbilityDef& def) const;

    // hits and effects
    void   apply_hit(bootstrap::AbilityChange& c, const abilities::HitDef& hit, double t, bool skip_resource, const HitSource& src);
    /// `raw`: the stacks are the instance's own count whatever the record's stacking says (a grant, not an application of the ability).
    void   apply_effect(bootstrap::AbilityChange& c, const abilities::EffectDef& def, int stacks, std::optional<double> duration, double t, bool raw = false,
                        int64_t owner = 0);
    void   remove_instance(bootstrap::AbilityChange& c, size_t index);
    void   remove_stacks(bootstrap::AbilityChange& c, int64_t effect_guid, int stacks);
    bootstrap::EffectItem effect_item(const EffectInst& e) const;
    void   run_effect_hits(bootstrap::AbilityChange& c, const EffectInst& e, const char* const* phases, size_t n, double t);
    void   step_loop(Cast& cast, double t);
    /// The zones of a cast's ability, spawned at its cost step; the caster's overlap hits are applied by the caller with `zone_owner_` set.
    void   spawn_zones(const Cast& cast, double open_at, double end_at);
    void   run_zone(size_t index, double t);
    double zone_due(const Zone& z) const;
    /// The infinite effects the zone's hits put on the caster go: their Expire and End hits run.
    void   end_zone_effects(bootstrap::AbilityChange& c, int64_t zone_id, double t);

    void   step_own(Cast& cast, double t);
    void   end_held(size_t index, double t);
    /// The effects some hit of the ability's closure applies (to its caster or its targets).
    std::set<int64_t> effects_applied_by_hits(const abilities::AbilityDef& def) const;
    void   end_loop(bootstrap::AbilityChange& c, int64_t ability, double t);
    /// What the hit takes off its targets (the caster): the effects of EffectsToRemoveIds, at most MaxNumEffectsToRemove of them.
    void   remove_by_hit(bootstrap::AbilityChange& c, const abilities::HitDef& hit);
    /// The engine's effect list follows effects_ at once, so that a formula run now reads the effects that were just applied.
    void   sync_engine_effects();
    // hit requests and slots
    CastRecord*  record_of(int32_t client_id);
    void   run_slot(CastRecord& rec, HitSlot& slot, double t, bool response, bool found, const std::vector<uint64_t>& targets);
    /// Plan E1, stage 3: a hit on actors other than the caster (see the header). Appends the confirmation.
    void   apply_hit_on_targets(bootstrap::AbilityChange& c, const abilities::HitDef& hit, const std::vector<uint64_t>& targets, double t, const HitSource& src,
                                const abilities::AbilityDef* ability);
    /// The caster's evaluation, made when it is not (the same one `eval` uses).
    bool   ensure_evaluation(std::string& why);
    double slot_due(const CastRecord& rec, const HitSlot& s) const;
    double record_reference(const CastRecord& rec) const { return rec.cost_at >= 0 ? rec.cost_at : rec.cost_due; }
    void   prune_records(double t);
    void   sync_shields(bootstrap::AbilityChange& c);
    std::set<int64_t> reach_of(const abilities::AbilityDef& def) const;
    int64_t targeting_of(const abilities::AbilityDef& def, int key) const;
    bool   absorb(bootstrap::AbilityChange& c, double& amount);
    /// A stat mod's value moves a stat: Health and Mana clamped to 0..maximum (a loss goes through the shields first), Stamina through A7. Returns
    /// the part that took effect (a heal at full Health: 0).
    double apply_delta(bootstrap::AbilityChange& c, int64_t stat, double amount);
    void   drop_record(int32_t client_id);
    void   effect_expires(size_t index, double t);
    void   effect_ticks(size_t index, double t);
    std::vector<stats::ActiveEffect> engine_effects() const;
    /// A hit reaches the caster when its mask names Self or Party (the caster is in its own party).
    bool   hits_caster(const content::HitRow& hit) const { return (hit.applicable_targets & (1 | 8)) != 0; }

    // cooldowns
    void   use_charge(bootstrap::AbilityChange& c, const Cast& cast, double t);
    void   group_timer_ends(const std::string& key, double t);
    bootstrap::CooldownItem cooldown_item(int64_t ability, const Group& g) const;

    // regeneration
    void   regenerate(double t);
    void   resolve_resource();

    const abilities::Catalog* catalog_;
    stats::CharacterStats&    stats_;
    AbilityRuntime&           a7_;
    WornFn                    worn_;
    double                    world_time_at_zero_;
    stats::SplitMix           rng_;
    std::unique_ptr<stats::CharacterStats::Evaluation> evaluation_;

    double                    now_ = 0;
    double                    clock_ = 0;                           // the time of the step that runs (what the host's progress and the formulas read)
    int                       apply_depth_ = 0;
    bool                      data_timings_ = true;
    bool                      moving_ = false;                      // the host's IsAnyMovementKeyPressed
    std::map<int64_t, int>    last_max_charges_;                    // the charges each ability had when last cast (GetRemainingAbilityCharges of an idle one)
    std::deque<std::pair<double, double>> losses_;                  // (time, Health lost) of the last minute: GetStatDeltaOverTime
    double remaining_charges(int64_t ability) const;
    double stat_delta_over_time(int64_t stat, double seconds) const;
    double progress_of(const std::string& group) const;
    double last_item_at_ = -1;                                      // when the ability item of the running cast came (the DefaultGroup's progress)
    double                    next_regen_ = kRegenInterval;
    std::set<int64_t>         known_;
    std::vector<Cast>         casts_;
    std::vector<EffectInst>   effects_;
    std::map<std::string, Group> groups_;
    std::deque<int32_t>       recent_ids_;
    std::optional<int64_t>    item_instance_;                       // the ability item on the pawn
    int32_t                   animation_counter_ = 0;
    std::vector<std::pair<double, double>> movement_;               // (until, speed)
    std::set<std::string>     unsupported_;                         // abilities whose unsupported shape was logged
    std::set<int64_t>         no_formula_;                          // abilities whose broken formula was logged
    std::vector<AbilityEvent> events_;
    std::vector<Zone>         zones_;
    int64_t                   next_zone_id_ = 1;
    int64_t                   zone_owner_ = 0;                      // the zone instance whose hits are being applied (the effects they create belong to it)
    std::vector<bootstrap::HitConfirm> confirmations_;             // the hits this step made, attached to its event
    std::deque<CastRecord>    records_;
    std::vector<Held>         held_;
    struct Instance {
        int64_t server_id = 0;
        int32_t client_id = 0;
        std::optional<double> end_due;
        int64_t ability = 0;
    };
    std::deque<Instance>      instances_;                           // the abilities the client may end with ServerEndAbility (the last few)
    void   end_instance(size_t index, double t);
    stats::SplitMix           guid_rng_;
    std::vector<bootstrap::ShieldElement> shields_sent_;
    size_t                    refused_ = 0, casts_started_ = 0, regen_ticks_ = 0;
    combat::HitPort*          combat_ = nullptr;                    // plan E1, stage 3: the world's port (not owned)
    const abilities::EffectDef* resource_ = nullptr;                // the class's resource effect (Bard_Resonance)
    int                       resource_class_ = 0;
};

}  // namespace aoc::worldd
