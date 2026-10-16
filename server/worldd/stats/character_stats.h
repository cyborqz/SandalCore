// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/character_stats.h: the stats of one player, as the client holds them (plan B3)
//
//  What the engine computes (stat_engine.h) from a class, a level and the worn items (gear.h), kept
//  per player as float32 values, with the two things the engine cannot know:
//
//    - the current value of a stat that spawns at its maximum (Health, Mana, Stamina, a class's resource: the
//      kStatSpawnMax flag). The engine returns the maximum, which is the value at the join; after that the
//      player's current value is its own: a recompute leaves it alone unless the new maximum is below it
//      (it is clamped, and the clamp is a change like any other);
//    - which stats changed since the client was told: a recompute (the gear changed) returns only the stats
//      whose float bits differ, and for a stat that replicates base and equipment (kStatBuckets) also those
//      two. Those are the deltas of the equip reply: retail's chest piece changed 30 Owner, 14 OwnerProxy and
//      2 EveryoneProxy items.
//
//  A stat is replicated to the arrays by its audience mask (StatValue::replication): 1 Owner, 5 OwnerProxy, 7
//  EveryoneProxy, 6 ProxyOnly (other players' views: not sent to the owner), 0 not at all; a Type_FloatArray
//  stat (TempHealth, kStatFloatArray) goes to the float arrays. The client holds floats: StatValue::value is
//  the engine's double made a float32, which is what the wire carries.
//
//  Not thread safe: a player's stats belong to its session.
// ============================================================================
#pragma once

#include "stats/gear.h"
#include "stats/stat_engine.h"
#include "stats/stat_program.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aoc::worldd::stats {

/// One stat of the character as the client has it (floats).
struct StatValue {
    int64_t  guid = 0;
    uint8_t  replication = 0;   // the audience mask
    uint8_t  flags = 0;         // the bundle's kStat... flags
    float    value = 0;
    float    base = 0;          // the value before the equipment's contribution (replicated for kStatBuckets stats)
    float    equip = 0;         // the equipment's contribution (the same)
    // plan D2, stage 2: the stat's limits and regeneration per second as the last run found them (what a held stat is clamped to and what a tick adds)
    float    max = 0, min = 0, regen = 0;
    bool     has_max = false, has_min = false, has_regen = false;

    bool held() const { return (flags & (kStatSpawnMax | kStatResource)) != 0; }
    bool resource() const { return (flags & kStatResource) != 0; }
    bool buckets() const { return (flags & kStatBuckets) != 0; }
    bool float_array() const { return (flags & kStatFloatArray) != 0; }
};

/// True when the client would see no difference between the two: the float BITS of the value, and of base and equipment for a
/// stat that replicates them (+0.0 and -0.0 are two values on the wire, and a NaN is the same NaN as itself).
bool same_on_the_wire(const StatValue& a, const StatValue& b);

/// The current value a held stat keeps when its maximum becomes `max`: its own, unless the maximum is below it (a NaN current value
/// is no value: the maximum). Compares as numbers, returns the stored float untouched, so a value and its bits stay together.
float clamp_current(float current, float max);
// (C.Y.B : CoA)

/// An NPC's own numbers the stat formulas read (plan E1, stage 3): its elite rank (GetNpcEliteRank) and its Blueprint's share of the stat budget by
/// EStatDistributionStat (GetNpcStatDistribution: the eight weights over their sum). A player has none (0).
struct NpcShape {
    double                elite_rank = 0;
    std::array<double, 8> dist = {};
    bool                  operator==(const NpcShape&) const = default;
};

class CharacterStats {
public:
    /// `bundle` and `gear` must outlive this object. `class_enum` is ECharacterClass, `level` 1..kMaxLevel.
    CharacterStats(const StatBundle& bundle, const GearCatalog& gear, int class_enum, int level, Rounding rounding = Rounding::HalfUp);

    /// The join: computes the stats for the worn items; every current value starts at its maximum. False, with `err`, when the engine
    /// refuses (a class the bundle does not have, a level out of range).
    bool start(std::span<const WornItem> worn, std::string& err);

    /// The gear changed: recomputes, clamps each current value to its new maximum, makes the result the character's stats and returns
    /// in `changed` the stats whose wire value differs from before, in stat order (the new values). False, with `err` and nothing
    /// changed, when the engine refuses. Requires start().
    bool update(std::span<const WornItem> worn, std::vector<StatValue>& changed, std::string& err);

    /// The recompute of update() without committing it: what the stats would be for `worn` (next), and the stats whose wire value would
    /// differ (`changed`). Nothing of this object changes; commit(next) makes the result the character's stats. A caller that has
    /// to tell the client first (the session) commits only once the client's arrays took the change. Requires start().
    struct Next {
        std::vector<StatValue> values;
        int faults = 0, ignored_mods = 0, unknown_items = 0, unknown_sets = 0;
    };
    bool prepare(std::span<const WornItem> worn, std::vector<StatValue>& changed, Next& next, std::string& err) const;
    void commit(Next&& next);

    /// A current value (a held stat) is now `value`, as the game's own events set it (damage, regeneration, the ability runtime's
    /// Stamina): the next update clamps this value, not the one it had. A stat that is not held, or not here, is ignored.
    void set_current(int64_t guid, float value);

    /// Plan D1: the level and the class the next prepare()/update() runs with (a level-up, a class scroll). Plan D2: the effect state
    /// the same way: the passives of the ranks the character has (infinite effects with constant mods: Endurance Training's MaxStamina
    /// +10), the effects the ability runtime has on it (a buff, a song's, Resonance), and the skill ranks it holds (what GetSkillPurchased
    /// reads: purchased and auto-granted). They are effects like any other: stat_engine.h says how each changes its stat (added, multiplied,
    /// multiplied by one minus). Nothing is recomputed here: the next prepare() is the diff against what the client has.
    void set_level(int level) { level_ = level; }
    void set_class(int class_enum) { class_ = class_enum; }
    void set_passives(std::vector<ActiveEffect> passives) { passives_ = std::move(passives); }
    void set_effects(std::vector<ActiveEffect> effects) { effects_ = std::move(effects); }
    void set_ranks(std::vector<SkillRank> ranks) { ranks_ = std::move(ranks); }
    const std::vector<ActiveEffect>& passives() const { return passives_; }
    const std::vector<ActiveEffect>& effects() const { return effects_; }
    const std::vector<SkillRank>&    ranks() const { return ranks_; }
    /// Whether the character moves and whether it is in combat, as the formulas that ask read them (IsMoving, IsInCombat: the regeneration
    /// is five times faster out of combat, Stamina's stops while a Sprint is on and the player moves). Out of combat and standing by default.
    void set_moving(bool moving) { moving_ = moving; }
    void set_in_combat(bool in_combat) { in_combat_ = in_combat; }
    bool in_combat() const { return in_combat_; }
    /// Plan E1, stage 3: this is an NPC (class_enum of an NPC profile, stats/stat_program.h kNpcClassBase): its elite rank and stat distribution.
    void set_npc(const NpcShape& npc) { npc_ = npc; }
    const NpcShape& npc() const { return npc_; }
    /// Gathering stage 2: the level of each profession by name (GetPlayerProfessionLevel reads these; a profession that is not listed is level 1). Every evaluation made
    /// after this call starts with them; Evaluation::set_profession_levels changes one evaluation's.
    void set_profession_levels(std::map<std::string, int> levels) { profession_levels_ = std::move(levels); }
    const std::map<std::string, int>& profession_levels() const { return profession_levels_; }
    /// Plan D2, stage 2: the stats that are class resources (a Ranger's Focus, a Cleric's DivinePower: kStatResource): each has a current value of its
    /// own between its limits, which the next run keeps, and a regeneration program that reads it. Flags the stats it has now and every later run.
    void set_resources(std::set<int64_t> guids);

    /// The expression programs of the bundle (an ability's costs, cooldowns and the values of an effect's stat mods) run against the
    /// character as it is now: its class, level, gear, effects and ranks. `eval` runs one program (the bundle's expression program
    /// index) with a host; the values of a program that read a stat are the stats of THIS run (a stat that spawns at its maximum reads its
    /// maximum). Not thread safe, like the rest.
    class Evaluation {
    public:
        Evaluation(const StatBundle& bundle, StatInputs inputs, bool moving, bool in_combat, const NpcShape& npc = {},
                   const std::map<std::string, int>& profession_levels = {});
        ~Evaluation();
        Evaluation(const Evaluation&) = delete;
        Evaluation& operator=(const Evaluation&) = delete;
        bool ok() const { return err_.empty(); }
        const std::string& error() const { return err_; }
        bool eval(int64_t program, ExprHost& host, double& out, std::string& err);
        /// Gathering stage 2: the professions' levels the next programs read (GetPlayerProfessionLevel; a profession not listed is level 1).
        void set_profession_levels(const std::map<std::string, int>& levels);
        /// The run's settled value of a stat (0 when the class has no such stat).
        double stat(int64_t guid) const;
        int faults() const;
        /// Plan E1, stage 3: the other actors of the hit the programs run for: the reads GetStat(GetTarget(), ...) and the like run in the context of
        /// the actor with the role (stats/stat_program.h kRoleTarget ...): `other`'s run, inputs and state (it must outlive this evaluation). A role
        /// never set reads as the caster, which is what a hit on the caster itself is.
        bool set_actor(int role, const Evaluation& other, std::string& err);
        void clear_actors();
        const StatInputs&  inputs() const { return in_; }
        const StatResult&  result() const { return result_; }
        /// The state the programs of this actor read (flags and, for an NPC, its elite rank and distribution).
        const StatState&   state() const;
    private:
        struct State;
        const StatBundle*          bundle_;
        StatInputs                 in_;
        std::unique_ptr<State>     state_;
        StatResult                 result_;
        std::unique_ptr<ExprEvaluator> ev_;
        std::string                err_;
    };
    /// `live_vitals`: Health and Mana read their current values (a hit's target: GetHealthPercentage(GetTarget())) instead of the maximum a fresh run spawns at.
    std::unique_ptr<Evaluation> evaluation(std::span<const WornItem> worn, std::string& err, bool live_vitals = false) const;
// C Y B and C | O | A

    bool started() const { return started_; }
    int  class_enum() const { return class_; }
    int  level() const { return level_; }
    /// All the stats of the class, in stat order.
    const std::vector<StatValue>& values() const { return values_; }
    const StatValue* find(int64_t guid) const;

    /// What the last start() or update() found: run faults (none on the real graph), mods for stats the bundle does not have (an
    /// item's MaxDurability), worn records and sets the catalog does not know.
    int faults() const { return faults_; }
    int ignored_mods() const { return ignored_mods_; }
    int unknown_items() const { return unknown_items_; }
    int unknown_sets() const { return unknown_sets_; }

private:
    bool compute(std::span<const WornItem> worn, Next& out, std::string& err) const;

    const StatBundle*  bundle_;
    const GearCatalog* gear_;
    int                class_, level_;
    Rounding           rounding_;
    bool               started_ = false;
    std::vector<ActiveEffect> passives_;   // plan D1 (as effects since plan D2)
    std::vector<ActiveEffect> effects_;    // plan D2
    std::vector<SkillRank>    ranks_;      // plan D2
    std::set<int64_t>         resources_;  // plan D2, stage 2: the class resources (set_resources)
    std::map<std::string, int> profession_levels_;   // Gathering stage 2
    bool                      moving_ = false, in_combat_ = false;
    NpcShape                  npc_;
    std::vector<StatValue> values_;
    int faults_ = 0, ignored_mods_ = 0, unknown_items_ = 0, unknown_sets_ = 0;
};

}  // namespace aoc::worldd::stats
