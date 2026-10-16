// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/stat_program.h: the stat program bundle (plan B3), read and verified
//
//  A bundle is the compiled stat formula graph of content.db: the stats of the
//  player, the programs of their formulas, the curves those read and the order to
//  evaluate them in. services/internal/statprog (Go) compiles it from the design
//  data; this reads it. The format and the rules a bundle must meet are in
//  statprog/README.md and stats/README.md; the Go Decode and parse_bundle apply the
//  same ones, and the shared corpus (stats/testdata/corpus.txt) holds bundles that both
//  must refuse.
//
//  parse_bundle checks everything before anything runs: after it returns true a
//  program cannot read outside its tables, overflow its stack, loop, or read a stat
//  before the stat is settled, whoever made the bytes.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::stats {

// 2 (plan D2): the opcodes Lerp, Abs and the host operations, the complement flag, the expression programs table. 3 (plan E1, stage 3): the opcodes Mod,
// NpcDist and Actor, the tangents of a curve's keys (a cubic segment is evaluated) and the NPC profiles (a class of kNpcClassBase or more: a kind of NPC's
// whole chain of initializer lists, with no common lists).
constexpr uint16_t kBundleVersion = 3;
// The first class enum of an NPC profile (ECharacterClass has eight values, 1 to 8).
constexpr int kNpcClassBase = 32;
// The actors of a hit an expression can read (aocexpr's Role...): the caster, the target, the caster's selected enemy and friend, the two owners and the
// target of the hit nearest the caster.
constexpr size_t kMaxRoles = 7;

// Limits: a bundle past any of them is refused (the same as Go's statprog).
constexpr size_t kMaxBundleBytes = 4u << 20;
constexpr size_t kMaxConsts = 65535;
constexpr size_t kMaxStrings = 65535;
constexpr size_t kMaxStringLen = 255;
constexpr size_t kMaxRefs = 65535;
constexpr size_t kMaxCurves = 4096;
constexpr size_t kMaxCurveKeys = 4096;
constexpr size_t kMaxPrograms = 65535;
constexpr size_t kMaxCodeBytes = 8192;  // also the most instructions a program can run: jumps only go forward
constexpr size_t kMaxLocals = 64;
constexpr size_t kMaxStack = 64;
constexpr size_t kMaxStats = 4096;
constexpr size_t kMaxSteps = 65535;
constexpr size_t kMaxClasses = 16;
constexpr int kMaxLevel = 255;
constexpr size_t kMaxExprs = kMaxPrograms;
// The most stacks one active effect may have and the most active effects a run may be given (see stat_engine.h).
constexpr int kMaxEffectStacks = 4096;
constexpr size_t kMaxActiveEffects = 4096;
// One run_stats may execute this many instructions in all; a valid bundle can ask for more (65,535
// initializer steps of an 8 KiB program each) and the run then fails. A run of the real graph takes about 3,000.
constexpr size_t kMaxRunInstructions = 1u << 22;

// The arithmetic of a bundle.
enum class Mode : uint8_t {
    Float32 = 0,  // values the engine stores or returns are float32; arithmetic between them is double
    Double = 1,   // every value is a double
    Single = 2,   // also every arithmetic result, constant and curve value is float32
};
// ~ CoA - C_Y_B ~

// Opcodes. A program runs on a stack of doubles (a boolean is 1.0 or 0.0).
namespace op {
constexpr uint8_t Const = 0x01;   // u16 constant
constexpr uint8_t Load = 0x02;    // u8 local
constexpr uint8_t Store = 0x03;   // u8 local
constexpr uint8_t Stat = 0x04;    // u16 stat
constexpr uint8_t Total = 0x05;
constexpr uint8_t Level = 0x06;
constexpr uint8_t Pop = 0x07;
constexpr uint8_t Add = 0x10;
constexpr uint8_t Sub = 0x11;
constexpr uint8_t Mul = 0x12;
constexpr uint8_t Div = 0x13;
constexpr uint8_t Neg = 0x14;
constexpr uint8_t Not = 0x15;
constexpr uint8_t Lt = 0x20;
constexpr uint8_t Le = 0x21;
constexpr uint8_t Gt = 0x22;
constexpr uint8_t Ge = 0x23;
constexpr uint8_t Eq = 0x24;
constexpr uint8_t Ne = 0x25;
constexpr uint8_t Jmp = 0x30;     // u16 absolute forward target
constexpr uint8_t Jz = 0x31;      // u16
constexpr uint8_t Jnz = 0x32;     // u16
constexpr uint8_t Round = 0x40;
constexpr uint8_t Floor = 0x41;
constexpr uint8_t Ceil = 0x42;
constexpr uint8_t Min = 0x43;
constexpr uint8_t Max = 0x44;
constexpr uint8_t Sel = 0x45;
constexpr uint8_t Curve = 0x46;   // u16 curve
constexpr uint8_t Lerp = 0x47;    // a b t -> a + t * (b - a), each operation rounded as a stat formula's are
constexpr uint8_t Abs = 0x48;
constexpr uint8_t Mod = 0x49;     // b a -> a % b: both truncated to whole numbers, 0 and a fault when b is 0
constexpr uint8_t Effect = 0x50;  // u16 ref
constexpr uint8_t EffectTag = 0x51;  // u16 string
constexpr uint8_t Skill = 0x52;   // u16 ref, u16 ref
constexpr uint8_t Combat = 0x53;
constexpr uint8_t Moving = 0x54;
constexpr uint8_t Unkillable = 0x55;
constexpr uint8_t EquipTag = 0x56;   // u16 string (slot), u16 string (tag)
constexpr uint8_t Profession = 0x57; // u16 string
constexpr uint8_t Consumed = 0x58;   // u16 ref
constexpr uint8_t NpcRank = 0x59;
constexpr uint8_t NpcDist = 0x65;    // u16 index (0 to 7, EStatDistributionStat): the NPC's share of its stat budget for the stat
constexpr uint8_t Actor = 0x66;      // u8 role: the reads that follow are of the actor of the hit with that role (expression programs only)
// Host operations (plan D2): inputs only the host of an ability expression has. A program that uses one is an
// expression program (StatBundle::exprs) and no formula of the stat graph.
constexpr uint8_t Progress = 0x5A;    // u16 string (a montage group): the progress ratio of the running ability of the group
constexpr uint8_t EffectInst = 0x5B;  // u16 ref: the stacks of the effect that the instigator applied
constexpr uint8_t Rand = 0x5C;        // the host's next random number in [0, 1)
constexpr uint8_t HitTag = 0x5D;      // u16 string (a gameplay tag): 1 if the hit in progress (or its ability) has it
constexpr uint8_t HitFlag = 0x5E;     // u16 string (an EHitFlag name, "Crit"): 1 if the hit on the target has the flag
constexpr uint8_t XpDebt = 0x5F;      // the instigator's experience debt ratio
constexpr uint8_t HitTargets = 0x60;  // how many targets the hit in progress has
// A named host function (plan D2, stage 2): u16 function (the kHost... constants), u16 string (its name operand); the 0, 1 or 2 numbers it takes are
// popped, the host answers (statprog's OpHost0..2).
constexpr uint8_t Host0 = 0x61;
constexpr uint8_t Host1 = 0x62;
constexpr uint8_t Host2 = 0x63;
constexpr uint8_t Ret = 0xFF;
}  // namespace op

// The host functions (the Host... constants of statprog's format.go).
constexpr int kHostRemainingCharges = 1;   // GetRemainingAbilityCharges(ability): name = the ability's guid in decimal
constexpr int kHostApplicableTarget = 2;   // IsApplicableTarget(target, EAffectedTarget::X): name = "TARGET_Self" ...
constexpr int kHostMovementKey = 3;        // IsAnyMovementKeyPressed()
constexpr int kHostStatDelta = 4;          // GetStatDeltaOverTime(actor, seconds, stat): name = the stat's guid, one argument
constexpr int kHostHitStatMod = 5;         // GetStatModValue(GetHitEvent(), [actor,] stat)
constexpr int kHostHitStatEffective = 6;   // GetStatModEffectiveValue(GetHitEvent(), [actor,] stat)
constexpr int kHostIsNeutral = 7;
constexpr int kHostIsEnemy = 8;
constexpr int kHostAngleBetween = 9;
constexpr int kHostNow = 10;
constexpr int kHostProgressDuration = 11;  // GetProgressDuration(group)
// Plan E1, stage 3: other actors, the hit's targets and the damage formulas.
constexpr int kHostDistanceBetween = 12;   // name "A,B": the distance in cm between two actors (the roles)
constexpr int kHostIsClient = 13;          // the server runs the expression: 0
constexpr int kHostRandIntRange = 14;      // args lo, hi: a whole number from lo to hi inclusive; lo when hi < lo (no random number used)
constexpr int kHostSameActor = 15;         // name "A,B": 1 when the two roles are the same actor
constexpr int kHostHitTriggerCount = 16;   // name: the ability's guid; args[0]: the role of the actor
constexpr int kHostAbilityHitCount = 17;   // name: the ability's guid; args[0]: the role of the actor
constexpr int kHostCooldownLeft = 18;      // name: the ability's guid: the seconds left of its cooldown
constexpr int kHostComboCount = 19;        // GetAbilityComboCount()
constexpr int kHostCalcDamage = 20;        // CalculateDamage() and its six companions (stats/damage.h)
constexpr int kHostCalcDamageMultiplier = 21;
constexpr int kHostCalcDamageAdditive = 22;
constexpr int kHostCalcDamageMitigation = 23;
constexpr int kHostCalcDamagePenetration = 25;
constexpr int kHostCalcDamageCritical = 24;
constexpr int kHostCalcDamageDampening = 26;
constexpr int kHostCalcLast = 26;
constexpr int kHostHitSourceType = 29;     // GetRecordTypeId(GetHitSourceRecord(hit)): the record type id of the hit's source
constexpr int kHostComboButton = 27;       // name: the EComboButtons value ("BUTTON_Forward"): 1 when the client holds it
constexpr int kHostHitContains = 28;       // name: the role of the actor: 1 when the hit in progress has it among its targets
// plan E1, stage 3, part 2: the reward tables' reads (aocexpr/hostfn.go)
constexpr int kHostLootMaxLevel = 30;     // GetPlayerLootMaxLevel(): the highest level among the players that may loot
constexpr int kHostLootAvgLuck = 31;      // GetPlayerLootAvgLuck(): their average luck
constexpr int kHostPlayerLevel = 161;    // GetPlayerLevel(): the level of the player the reward or the experience is for
constexpr int kHostNpcTag = 160;          // DoesNPCContainTag(tag): name = the tag; 1 when the NPC (the owner) carries it
// Nodes 1, stage 1 (aocexpr/hostfn.go HostNodeBiome ...): the city node reads.  A node is a number in a program, its RANK (the index of its
// CityNodeRecord in the list sorted by guid, -1 for none), and an enum answer is the value's ordinal: world/city_nodes.h answers them.
constexpr int kHostNodeBiome = 170;            // GetNodeBiome(): the EBiomeType of the context's zone of influence
constexpr int kHostCurrentNode = 171;          // GetCurrentNode(actor): args[0] is a role, or kActorPlayer for GetPlayer()
constexpr int kHostNodeRecord = 172;           // GetNodeRecord(): the node of the context
constexpr int kHostNodeType = 173;             // Node_GetNodeType(node): args[0] is the rank; the ENodeType
constexpr int kHostNodeLevel = 174;            // Node_GetLevel(node): args[0] is the rank; the ENodeLevel
constexpr int kHostPlayerCitizen = 175;        // Node_IsPlayerCitizen(player): 0 until the citizenship milestone
constexpr int kHostPlayerCitizenOfNode = 176;  // Node_IsPlayerCitizenOfNode(player, node): args[0] is the rank
constexpr int kHostPlayerMayor = 177;          // Node_IsPlayerMayor(player)
constexpr int kHostNodeLast = 177;
// Gathering stage 2 (aocexpr/hostfn.go HostArtisanStat ...): what the Artisan equations ask of the harvest in progress and of the player.  All numbers; the equations compare the
// quality, the rarity and the tier with whole numbers (stats/gather_host.h answers them from a GatherContext).
constexpr int kHostArtisanStat = 180;        // GetArtisanshipModifiedStat(actor, stat): name = the stat's guid in decimal, args[0] = the role of the actor (the player is the caster)
constexpr int kHostRolledItemQuality = 181;  // GetRolledItemQuality(): the quality the harvest's rarity equation rolled
constexpr int kHostItemRarity = 182;         // GetItemRarity(): the EItemRarity ordinal of the harvest's item (Common 0 ... Artifact 6)
constexpr int kHostSourceCertTier = 183;     // GetSourceCertificationTier(): the CertificationTier of the harvested interaction (Novice 0 ... GrandMaster 4)
constexpr int kHostGatherBaseSecs = 184;     // GetGatheringInteractionBaseDuration(): the interaction's BaseGatheringDuration, seconds
constexpr int kHostGatherFirst = 180;
constexpr int kHostGatherLast = 184;
constexpr int kActorPlayer = 100;              // the actor code of GetPlayer() in the args of kHostCurrentNode (the roles of a hit are 0 to 6)
// kHostRoleBase + role * 16 + op: a read of an actor that only the host knows, by role.
constexpr int kHostRoleBase = 32;
constexpr int kRoleOpHasAlignment = 10;    // name: the alignment's guid
constexpr int kRoleOpEffectInst = 11;      // name: the effect's guid; args[0]: the role of the instigator that applied it
constexpr int kRoleOpEffectRemaining = 12; // name: the effect's guid: the seconds left
constexpr int role_fn(int role, int op) { return kHostRoleBase + role * 16 + op; }
// -- C-Y-B x CoA --

// The roles of the actors of a hit.
constexpr int kRoleCaster = 0;
constexpr int kRoleTarget = 1;
constexpr int kRoleActiveHarm = 2;
constexpr int kRoleActiveSupport = 3;
constexpr int kRoleCasterOwner = 4;
constexpr int kRoleTargetOwner = 5;
constexpr int kRoleClosestInHit = 6;

// Stat flags.
constexpr uint8_t kStatMulMods = 1 << 0;
constexpr uint8_t kStatClampFinal = 1 << 1;
constexpr uint8_t kStatSpawnMax = 1 << 2;
constexpr uint8_t kStatSpawnMin = 1 << 3;
constexpr uint8_t kStatBuckets = 1 << 4;
constexpr uint8_t kStatFloatArray = 1 << 5;  // Type_FloatArray (TempHealth): the float arrays carry it, not the Int32 ones
// bMultiplyCompliment, with kStatMulMods: the stat's effect mods multiply it by (1 - value) instead of by value.
constexpr uint8_t kStatMulCompl = 1 << 6;
// Plan D2, stage 2 (never in a bundle: CharacterStats sets it): a class resource, a stat with a regeneration program that does not spawn at its
// maximum (DivinePower, Focus, Momentum, Essence, Courage): its current value is its own state like Health's, between its limits.
constexpr uint8_t kStatResource = 1 << 7;

// Node kinds of the evaluation order (stat << 1 | kind).
constexpr uint32_t kNodeInit = 0;
constexpr uint32_t kNodeValue = 1;

// Curve key interpolation.
constexpr uint8_t kInterpLinear = 0;
constexpr uint8_t kInterpConstant = 1;
constexpr uint8_t kInterpCubic = 2;  // a Bezier through the first key's leave tangent and the next key's arrive tangent (cubic_segment)

struct RecordRef {
    uint64_t type_id = 0;
    int64_t guid = 0;
    bool operator==(const RecordRef& o) const { return type_id == o.type_id && guid == o.guid; }
};

struct CurveKey {
    double time = 0, value = 0;
    uint8_t interp = 0;
    double arrive = 0, leave = 0;  // the tangents of a cubic segment (zero for the others)
};
struct Curve {
    std::vector<CurveKey> keys;
};
struct Program {
    uint16_t locals = 0;
    std::vector<uint8_t> code;
    // It reads the running total (op::Total): an initializer entry that sees the stat's value so far, Base included (stat_engine.cpp, run_stats).
    // A static property of the code (a read behind a branch counts), set by parse_bundle.
    bool reads_total = false;
};
struct Stat {
    int64_t guid = 0;
    uint32_t name = 0;  // index into strings
    uint8_t replication = 0;
    uint8_t flags = 0;
    int32_t base = -1, min = -1, max = -1, regen = -1;  // program indexes, -1 for none
};
struct Step {
    uint32_t stat = 0;
    uint32_t prog = 0;
};
struct ClassDef {
    uint8_t enum_value = 0;  // ECharacterClass
    uint32_t name = 0;
    std::vector<Step> steps;
    std::vector<uint32_t> members;
};

struct StatBundle {
    Mode mode = Mode::Float32;
    std::vector<double> consts;
    std::vector<std::string> strings;
    std::vector<RecordRef> refs;
    std::vector<Curve> curves;
    std::vector<Program> programs;
    std::vector<Stat> stats;
    std::vector<Step> common_steps;
    std::vector<uint32_t> common_members;
    std::vector<ClassDef> classes;
    std::vector<uint32_t> order;
    // The programs a host runs for an ability's expressions, by index in programs, strictly increasing: they read the stats of a
    // finished run and may use the host operations; no formula of the stat graph may.
    std::vector<uint32_t> exprs;
    uint32_t crc32 = 0;  // the header's checksum of the body (parse_bundle verified it): what a stat_program row says it is

    // by_guid maps a stat guid to its index (built by parse_bundle).
    std::unordered_map<int64_t, uint32_t> by_guid;

    const std::string& stat_name(size_t i) const { return strings[stats[i].name]; }
    const ClassDef* class_by_enum(int e) const;
    // is_expr says the program is an expression program.
    bool is_expr(int64_t prog) const;
};

// parse_bundle reads and verifies a bundle. It returns false, and says why in err, for
// anything wrong; out is then unspecified.
bool parse_bundle(const uint8_t* data, size_t size, StatBundle& out, std::string& err);
inline bool parse_bundle(const std::vector<uint8_t>& v, StatBundle& out, std::string& err) {
    return parse_bundle(v.data(), v.size(), out, err);
}

}  // namespace aoc::worldd::stats
