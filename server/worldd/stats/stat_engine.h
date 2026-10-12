// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/stat_engine.h: the stat engine's interpreter (plan B3)
//
//  Runs a verified StatBundle for one character: the stats of its class at its
//  level with its equipment, each with the audience mask of its replication.
//  services/internal/statprog (Go) runs the same programs to the same bits.
//
//  A stat is settled in the bundle's order:
//    initial value  the initializer entries of the common lists and the class's list, in
//                   order, each seeing the value so far as GetStatInitRunningTotal();
//                   0 to start, or 1 for a stat that multiplies
//    value          initial + base (initial * base when it multiplies); plus the equipment's
//                   contribution; raised to ValueMin and lowered to ValueMax; a stat that spawns
//                   at its maximum (Health, Mana, Stamina) is its maximum
//    effects        (plan D2) the active effects' mods apply after the equipment, in the stat's own form: added
//                   (times the stacks) to an additive stat, multiplied once per stack into a multiplying one, by the mod or by
//                   one minus it for a complement stat; then the limits (see "Effect state" below)
//
//  The arithmetic is IEEE double, one operation per instruction; the values the engine
//  keeps are rounded to float32 (Mode). A division by zero, a read of a curve at NaN and a
//  value that is not finite are faults: the result is 0 and StatResult::faults counts them.
// ============================================================================
#pragma once

#include "stats/stat_program.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd::stats {

// StatMod is the equipment's contribution to a stat: the equipped items' stat blocks at their
// qualities and the bonuses of their sets, summed per stat by the caller. A guid that is not a
// stat of the bundle is ignored (StatResult::ignored_mods counts it).
struct StatMod {
    int64_t stat = 0;
    double value = 0;
};

// EquippedTag says the item worn in a slot (the EEquipSlot value's name, "Secondary") has a
// gameplay tag; give every tag of the item. A tag matches itself and its descendants
// ("Item.Gear.Weapon.Shield.Tower" matches "Item.Gear.Weapon.Shield").
struct EquippedTag {
    std::string slot;
    std::string tag;
};

// ---- Effect state (plan D2) ----------------------------------------------------------------------------------------------
//
// An active effect has stacks and the stat mods it applies, each already a number: the value of ONE stack (the host ran the
// mod's expression program with ExprEvaluator when it applied the effect). A mod changes its stat after the equipment:
//
//   an additive stat             value += mod * stacks                    (MoveSpeedMult, Evasion, MaxStamina)
//   a multiplying stat           value *= mod, once per stack             (HealingMult: 1.1025 under 1.05 is 1.157625)
//   a multiplying "compliment"   value *= (1 - mod), once per stack       (Mitigation: 0.9652 under 0.03 is 0.936244)
//
// The form is the stat's: kStatMulMods (bMultiplyStatMods) chooses between the first and the other two, kStatMulCompl
// (bMultiplyCompliment) between multiplying by the mod and by one minus it. The caller never says which. Effects apply in the
// order given, each one's mods in theirs; the stat's own limits bind the result.
//
// The same effects answer the state functions: GetEffectCount is the stacks of the effects with the record,
// GetEffectCountByTag the stacks of the effects one of whose tags matches (a tag matches itself and its descendants), and
// GetSkillPurchased is true for a held rank (purchased or auto-granted: the caller gives both). Whatever the StatState says
// besides is added.
struct EffectMod {
    int64_t stat = 0;
    double value = 0;  // one stack's
};

// The reference type ids GetEffectCount and GetSkillPurchased name: an Effect record, a skill tree, a skill and a skill rank (160 of the 171
// ability formulas that ask GetSkillPurchased name the skill; 11, a talent's upgrade such as Mage_Passive_Blink_Unyielding, name one rank).
inline constexpr uint64_t kRefTypeEffect = 3377240744094973029ull;
inline constexpr uint64_t kRefTypeSkillTree = 297887639323616934ull;
inline constexpr uint64_t kRefTypeSkill = 4146438674247114715ull;
inline constexpr uint64_t kRefTypeSkillRank = 4847957796014348780ull;
// C-O-A - C-Y-B

struct ActiveEffect {
    RecordRef effect;  // the Effect record: what GetEffectCount asks for
    int stacks = 0;    // 0 (it counts for nothing) to kMaxEffectStacks
    std::vector<std::string> tags;
    std::vector<EffectMod> mods;
};

struct SkillRank {
    RecordRef cls;    // the skill tree (GetSkillPurchased's first reference)
    RecordRef skill;
};

struct StatInputs {
    int class_enum = 0;  // ECharacterClass: Ranger 1 ... Mage 8
    int level = 1;       // 1 to kMaxLevel
    std::vector<StatMod> mods;
    std::vector<EquippedTag> equipped;
    std::vector<ActiveEffect> effects;  // plan D2
    std::vector<SkillRank> ranks;       // plan D2: the ranks held, purchased and auto-granted
    // plan D2, stage 2: the current value of the held class resources (stat guid, value): what GetStat reads of them once the run has settled
    // (the engine's own value of a resource is its initial one: a Cleric's DivinePower regenerates by its Gen stat and a Fighter's Momentum decays
    // by how much it holds, so the regeneration programs and the costs that spend "everything" (Tank_Fortify) read the held value).
    std::vector<std::pair<int64_t, double>> current;
};

// StatState is what the formulas read of the character's state beyond its class, level and
// equipment: the part of the world plan B3 leaves for later plans. The defaults are the
// "none" values of a character that has nothing yet (statprog's aocexpr.NoState):
//   effects, effects by tag 0; no skill bought; out of combat; not moving; killable;
//   actor 0; level 1 in every profession; nothing consumed; NPC elite rank 0.
class StatState {
public:
    virtual ~StatState() = default;
    virtual double effect_count(const RecordRef&) const { return 0; }
    virtual double effect_count_by_tag(const std::string&) const { return 0; }
    virtual bool skill_purchased(const RecordRef& /*cls*/, const RecordRef& /*skill*/) const { return false; }
    virtual bool in_combat() const { return false; }
    virtual bool moving() const { return false; }
    virtual bool unkillable() const { return false; }
    virtual bool equipped_item_has_tag(const std::string& /*slot*/, const std::string& /*tag*/) const { return false; }
    virtual double actor_guid() const { return 0; }
    virtual double profession_level(double /*actor*/, const std::string&) const { return 1; }
    virtual double consumed_item_stat(const RecordRef&) const { return 0; }
    virtual double npc_elite_rank() const { return 0; }
    // GetNpcStatDistribution(npc, EStatDistributionStat): an NPC's share of its stat budget for the stat (index 0 to 7), the Blueprint's weight over
    // the sum of its eight; 0 for anything that is no NPC (plan E1, stage 3).
    virtual double npc_stat_distribution(int /*index*/) const { return 0; }
};

// StatOut is one stat of the class. replication is the audience mask: 0 not replicated,
// 1 StatRepInt32Owner, 5 StatRepInt32OwnerProxy, 7 StatRepInt32EveryoneProxy, 6 ProxyOnly.
struct StatOut {
    uint32_t stat = 0;  // index in StatBundle::stats
    int64_t guid = 0;
    uint8_t replication = 0;
    uint8_t flags = 0;  // the stat's kStat... flags: kStatSpawnMax (a current value), kStatBuckets (base and equipment replicate), kStatFloatArray
    double value = 0;
    double base = 0;   // the value before the equipment's contribution
    double equip = 0;  // the equipment's contribution
    double regen = 0;  // the regeneration rate, when has_regen
    bool has_regen = false;
    // plan D2, stage 2: the stat's own limits (ValueMin, ValueMax) as the run found them, for the stats that have them: a class resource's
    // current value is its own and is held between them (a Ranger's Focus 0 to 100, a Cleric's DivinePower 0 to 100)
    double max = 0, min = 0;
    bool has_max = false, has_min = false;
};

struct StatResult {
    std::vector<StatOut> stats;  // the stats of the common lists and the class's, in stat order
    int faults = 0;
    int ignored_mods = 0;
    int ignored_effect_mods = 0;  // effect mods that name a stat the bundle does not have
    // Every stat of the bundle's settled value by index in StatBundle::stats, the stats of no list of the class too: what an
    // expression program reads.
    std::vector<double> values;
    size_t instructions = 0;  // executed, at most kMaxRunInstructions
};

// run_stats computes the stats. It returns false, and says why in err, for a class the bundle does
// not have, a level outside 1..kMaxLevel, or a bundle that needs more than kMaxRunInstructions.
bool run_stats(const StatBundle& bundle, const StatInputs& in, const StatState& state, StatResult& out, std::string& err);

// tag_matches is the tag rule: equal, or a descendant, without regard to case (FNames).
bool tag_matches(const std::string& tag, const std::string& want);

// ---- Expression programs (plan D2) ----------------------------------------------------------------------------------------

// ExprHost is what the host of an ability expression gives it: the three inputs the character's stats do not hold. The defaults
// are NoHost's: progress 0, no effect applied by anyone, every random number 0, no hit in progress, no experience debt.
class ExprHost {
public:
    virtual ~ExprHost() = default;
    // GetAbilityProgressRatio(group): how far the running ability of the montage group is, 0 to 1.
    virtual double ability_progress(const std::string& /*group*/) { return 0; }
    // GetEffectCountByRecordAndInstigator: the stacks of the effect the instigator applied (the caster: it is the target too).
    virtual double effect_count_by_instigator(const RecordRef&) { return 0; }
    // RandDouble(): a number in [0, 1) from the host's seeded source.
    virtual double rand_double() { return 0; }
    // The hit in progress (a heal or a shield formula reads it): CheckForDamageTag(GetHitInProgress(), tag), the flag of
    // HitFlagsContains(GetHitFlags(hit, target), EHitFlag::flag), GetNumTargets(hit), and the instigator's GetXPDebtRatio.
    virtual bool hit_has_tag(const std::string& /*tag*/) { return false; }
    virtual bool hit_has_flag(const std::string& /*flag*/) { return false; }
    virtual double hit_targets() { return 0; }
    virtual double xp_debt_ratio() { return 0; }
    // A named host function (statprog's Host... constants, stat_program.h's kHost...): its string operand and the n (0 to 2) numbers it takes.
    virtual double host_fn(int /*fn*/, const std::string& /*name*/, const double* /*args*/, int /*n*/) { return 0; }
};

// SplitMix is the random source of the tests and the shared corpus: SplitMix64, as statprog.SplitMix. rand_double is the top
// 53 bits of the next output over 2^53.
struct SplitMix {
    uint64_t state = 0;
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double rand_double() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};
// C.Y.B with CoA

// ExprEvaluator runs the expression programs of a bundle (StatBundle::exprs) against the stats of a finished run: an ability's
// costs, cooldowns, cast times and requirements, and the values of an effect's stat mods. It reads the run's values, the
// character's level, effects and ranks, and the host. The programs of the stat graph are not its to run. It keeps the
// bundle, the inputs and the state by reference: they must outlive it.
class ExprEvaluator {
public:
    ExprEvaluator(const StatBundle& bundle, const StatInputs& in, const StatState& state, const StatResult& result);
    ~ExprEvaluator();
    // ok() is false, and error() says why, when the run holds no stat values for this bundle or the level is not 1..kMaxLevel.
    bool ok() const { return err_.empty(); }
    const std::string& error() const { return err_; }
    // eval runs the expression program prog and returns its value (a boolean is 1 or 0). It returns false, and says why in err,
    // for a program that is no expression program. Faults (a division by zero, a curve read at NaN) count in faults() and give 0.
    bool eval(int64_t prog, ExprHost& host, double& out, std::string& err);
    int faults() const;

    // Plan E1, stage 3: the other actors of the hit the expressions run for. The reads the compiler brackets with OpActor (GetStat(GetTarget(), ...),
    // GetEffectCount(GetTarget(), ...), IsInCombat(GetTarget()), the level and the rest) run in the context of the actor with the role (kRoleTarget ...):
    // its own run's values, inputs and state, which must outlive the evaluator. A role never supplied reads as the caster, which is what a hit on
    // the caster itself is. It returns false, and says why in err, for a role outside 1..kMaxRoles-1, a run of another bundle or a level outside 1..kMaxLevel.
    bool set_actor(int role, const StatInputs& in, const StatState& state, const StatResult& result, std::string& err);
    void clear_actors();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::string err_;
};

}  // namespace aoc::worldd::stats
