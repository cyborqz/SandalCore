// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"fmt"
	"strconv"
	"strings"
)

// The named host functions of an ability expression (plan D2, stage 2; plan E1, stage 3), by number. The compiler (package statprog) and the
// reference evaluator share them: a call is a function number, a name operand (a decimal guid, a tag, "" when it has none) and the numbers it is
// given; the host answers with one number. The C++ engine (server/worldd/stats) has the same numbers.
const (
	HostRemainingCharges = 1
	HostApplicableTarget = 2
	HostMovementKey      = 3
	HostStatDelta        = 4
	HostHitStatMod       = 5
	HostHitStatEffective = 6
	HostIsNeutral        = 7 // name: "A,B", the roles of the two actors
	HostIsEnemy          = 8 // name: "A,B"
	HostAngleBetween     = 9 // name: "A,B"
	HostNow              = 10
	HostProgressDuration = 11
	// Plan E1, stage 3: other actors.
	HostDistanceBetween = 12 // name: "A,B": the distance in cm between two actors
	HostIsClient        = 13 // the server runs the expression: 0
	HostRandIntRange    = 14 // args: lo, hi: a whole number from lo to hi inclusive; lo when hi < lo (UE's RandRange(Min, Max) is Min + RandHelper(Max - Min + 1): RandIntRange(1, 0) is 1, and no random number is used)
	HostSameActor       = 15 // name: "A,B": 1 when the two roles are the same actor
	HostHitTriggerCount = 16 // name: the ability's guid; args[0]: the role of the actor: how often its hits of the ability landed lately
	HostAbilityHitCount = 17 // name: the ability's guid; args[0]: the role of the actor: how often it was hit by the ability
	HostCooldownLeft    = 18 // name: the ability's guid: the seconds left of its cooldown
	HostComboCount      = 19 // GetAbilityComboCount(): the combo step the running ability is on
	// The damage formulas (DamageFormula.cpp of the game's GameSystemsPlugin): CalculateDamage() and its six companions, over the instigator's and the
	// target's stats and the hit in progress (statprog/damage.go).
	HostCalcDamage            = 20
	HostCalcDamageMultiplier  = 21
	HostCalcDamageAdditive    = 22
	HostCalcDamageMitigation  = 23
	HostCalcDamageCritical    = 24
	HostCalcDamagePenetration = 25
	HostCalcDamageDampening   = 26
	HostCalcLast              = 26
	HostComboButton           = 27 // name: the EComboButtons value ("BUTTON_Forward"): 1 when the client holds it
	HostHitContains           = 28 // name: the role of the actor: 1 when the hit in progress has it among its targets
	HostHitSourceType         = 29 // GetRecordTypeId(GetHitSourceRecord(hit)): the record type id of the hit's source (an ability, a projectile, an effect ...)
	// Plan E1, stage 3, part 2: the reward tables' (RewardTableRecordBase) reads of the group that killed the NPC.
	HostLootMaxLevel = 30  // GetPlayerLootMaxLevel(): the highest level among the players that may loot
	HostLootAvgLuck  = 31  // GetPlayerLootAvgLuck(): their average luck (0 when the game has no luck stat)
	HostPlayerLevel  = 161 // GetPlayerLevel(): the level of the player the reward or the experience is for
	HostNpcTag       = 160 // DoesNPCContainTag(tag): 1 when the NPC (the owner) carries the gameplay tag (name: the tag); past the role reads (HostRoleBase + 7*16 + 16 = 160)

	// HostRoleBase + role*16 + op: a read of an actor that only the host knows (the alignments, the effects an actor applied), by role.
	HostRoleBase = 32
)

// (C.Y.B : CoA)

// The city node reads (Nodes 1, stage 1; contentc version 12). A node is a number in a program: its rank, the index of its CityNodeRecord in the
// list of CityNodeRecordBase sorted by guid (-1: no node), because a guid does not fit a float32. An enum answer is the value's ordinal (enums.go).
// The host (worldd/world/city_nodes.h in C++, package nodehost in Go) answers them from the node tables of content.db and the nodes' live levels.
const (
	HostNodeBiome           = 170 // GetNodeBiome(): the EBiomeType ordinal of the biome of the node (the zone of influence) of the evaluation's context
	HostCurrentNode         = 171 // GetCurrentNode(actor): the rank of the node whose zone of influence holds the actor, -1 for none; args[0]: the actor (a role, or ActorPlayer)
	HostNodeRecord          = 172 // GetNodeRecord(): the rank of the node of the evaluation's context (-1: none)
	HostNodeType            = 173 // Node_GetNodeType(node): the ENodeType ordinal; args[0]: the node's rank
	HostNodeLevel           = 174 // Node_GetLevel(node): the ENodeLevel ordinal (Wilderness for no node); args[0]: the node's rank
	HostPlayerCitizen       = 175 // Node_IsPlayerCitizen(player): 1 when the player is a citizen of a node (always 0 until the citizenship milestone)
	HostPlayerCitizenOfNode = 176 // Node_IsPlayerCitizenOfNode(player, node): args[0]: the node's rank (always 0 until the citizenship milestone)
	HostPlayerMayor         = 177 // Node_IsPlayerMayor(player): 1 when the player is the mayor of a node (always 0 until the citizenship milestone)

	// ActorPlayer is the actor code of GetPlayer() in the args of HostCurrentNode (the roles of a hit are 0 to 6).
	ActorPlayer = 100
	// HostNodeLast is the last of the city node reads.
	HostNodeLast = HostPlayerMayor
)

// The gathering reads (Gathering stage 2, "Gathering data"; contentc version 13): what the Artisan equations (the speed, rarity, quantity and experience of
// a harvest) ask of the harvest in progress and of the player. They are host functions over numbers, like the city node reads: the host fills a plain
// gathering context (worldd's harvest host in C++, statprog.GatherContext in Go) and answers from it. The equations compare the answers with whole
// numbers (`rarity == 3`), so none is an enum.
const (
	HostArtisanStat       = 180 // GetArtisanshipModifiedStat(actor, stat): name: the stat's guid in decimal; args[0]: the role of the actor (the player is the caster); the rating, 0 for a stat the host has no value for
	HostRolledItemQuality = 181 // GetRolledItemQuality(): the quality the harvest's rarity equation rolled (0 before one ran)
	HostItemRarity        = 182 // GetItemRarity(): the EItemRarity ordinal of the harvest's item (Common 0 ... Artifact 6)
	HostSourceCertTier    = 183 // GetSourceCertificationTier(): the CertificationTier of the interaction that is harvested (Novice 0 ... GrandMaster 4)
	HostGatherBaseSecs    = 184 // GetGatheringInteractionBaseDuration(): the BaseGatheringDuration of the interaction, in seconds
	// HostGatherFirst and HostGatherLast bound the gathering reads (the answer of one is a quality, a tier or a rating: the numbers it is given are not added to it).
	HostGatherFirst = HostArtisanStat
	HostGatherLast  = HostGatherBaseSecs
)

// TypeCityNode is the record type id of CityNodeRecordBase: the first number of $#<TypeId>:<Guid>$ for a node.
const TypeCityNode = 8574414081702566619

// KindCityNode is the Kind of a record named instead of numbered: $CityNode:Node_Playground_Justopolis$.
const KindCityNode = "CityNode"

// NodeRanker is what a context gives that knows the city nodes: the rank of a node record (see HostNodeBiome) and the record of a rank. The
// reference evaluator needs it to turn a node value into the reference the data compares it with.
type NodeRanker interface {
	// NodeRank is the rank of the node with the guid, or of the one named name (the record name, "Verra_RVR_Joeva") when guid is 0.
	NodeRank(guid int64, name string) (rank int, ok bool)
	// NodeGuid is the guid of the node with the rank; 0 and true for rank -1 (no node).
	NodeGuid(rank int) (guid int64, ok bool)
}

// The roles an object can have in an ability expression: whose stats and effects a read of it asks about.
const (
	RoleCaster        = 0 // GetOwner(), GetInstigator(), GetSource(), GetSelf()
	RoleTarget        = 1 // GetTarget(): the actor the hit is on
	RoleActiveHarm    = 2 // GetActiveTarget(caster, Harm): the caster's selected enemy
	RoleActiveSupport = 3 // GetActiveTarget(caster, Support)
	RoleCasterOwner   = 4 // GetCreatureOwner(caster): the owner of a pet
	RoleTargetOwner   = 5 // GetCreatureOwner(target)
	RoleClosestInHit  = 6 // GetClosestTargetToActorInHit(hit, caster): the target of the hit nearest the caster
	RoleCount         = 7
	// RolePlayer is GetPlayer(): the character the expression is for (Nodes 1). It is no actor of a hit and has no role reads: only the city node
	// reads take it (an unknown role reads as the caster).
	RolePlayer = 7
)

// The reads of an actor that only the host knows: HostRoleBase + role*16 + one of these. The stats, effects, skills, level and flags of an actor are
// the engine's own (OpActor: the same operations run in the actor's context, which the caller of the evaluation supplies).
const (
	RoleOpHasAlignment    = 10 // name: the alignment's guid
	RoleOpEffectInst      = 11 // name: the effect's guid; args[0]: the role of the instigator that applied it
	RoleOpEffectRemaining = 12 // name: the effect's guid: the seconds left
)

// RoleFn is the host function number of a read of an actor by role.
func RoleFn(role, op int) int { return HostRoleBase + role*16 + op }

// RoleOf is the role of an object value (0 for every object that is no actor of a hit).
func RoleOf(o Object) int { return o.Role }

// RolePair is the name operand of a two-actor function: "A,B".
func RolePair(a, b int) string { return strconv.Itoa(a) + "," + strconv.Itoa(b) }

// ParseRolePair reads RolePair's text.
func ParseRolePair(s string) (a, b int, err error) {
	i := strings.IndexByte(s, ',')
	if i < 0 {
		return 0, 0, fmt.Errorf("%q is not two roles", s)
	}
	if a, err = strconv.Atoi(s[:i]); err != nil {
		return 0, 0, err
	}
	b, err = strconv.Atoi(s[i+1:])
	return a, b, err
}

// DamageFnNames are the damage functions by their expression names, in the order of HostCalcDamage..HostCalcLast.
var DamageFnNames = []string{"CalculateDamage", "CalculateDamageMultiplier", "CalculateDamageAdditiveMultiplier", "CalculateDamageMitigation",
	"CalculateDamageCritical", "CalculateDamagePenetration", "CalculateDamageDampening"}

// HostCaller is what an ability expression's context gives when it has a host: the named functions above.
type HostCaller interface {
	HostFn(fn int, name string, args []float64) float64
}

// TypeProjectile is GetProjectileTypeId(): the record type id of a projectile (the type a hit's source has when a projectile fired it).
const TypeProjectile = 979581256175327151

// HitSourceRef is what GetHitSourceRecord gives: no record, a marker (GetRecordTypeId of it asks the host for the source's type).
var HitSourceRef = Ref{TypeID: 0, Guid: -2}

// WeaponSkilltreeRef is what GetCurrentWeaponSkilltree gives: no record, a marker (GetSkillPurchased of it asks about the tree of the weapon in hand,
// which the state knows).
var WeaponSkilltreeRef = Ref{TypeID: 0, Guid: -1}

// RoleContexter is what a context gives when it knows the actors of a hit other than the caster: RoleContext is the stat context of the actor
// with the role (nil: unknown, read as the caster: a self hit's target is the caster).
type RoleContexter interface {
	RoleContext(role int) StatContext
}
