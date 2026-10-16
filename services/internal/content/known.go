// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import "fmt"

// TypeIds of the design database's record types (the directory's TypeId, and
// the first number of a $#<TypeId>:<Guid>$ reference).
const (
	TypeAbility             = 1841048597116038327 // table 0, AoCAbilityRecordBase
	TypeAbilityHit          = 3672506661632452365 // table 2
	TypeStatDef             = 5365104778484440215 // table 3, StatTypeDefRecordBase
	TypeEffect              = 3377240744094973029 // table 4
	TypeCharacterClasses    = 3107803182685678074 // table 44
	TypeStatCurve           = 7037248826340794574 // table 296
	TypeStatInitializerList = 2937500229480787749 // table 303
	TypeStatMod             = 676208982154769454  // table 307

	// Plan B2: the item tables.
	TypeItem              = 2616523457551378613 // table 161, ItemRecordBase (the TypeId of an item reference too)
	TypeItemStatBlock     = 4757144276206936031 // table 172
	TypeQualityCurve      = 2763533897913176681 // table 240
	TypeStartingEquipment = 8457388432688083496 // table 289

	// Gathering stage 3, "Resource storage": the grid of a worn resource bag.
	TypeResourceBagDef = 6026413454960341687 // ResourceBagDefRecordBase (the TypeId of an item's ResourceBagDefId)

	// Plan B3: the set bonuses of the items.
	TypeSetBonus = 1330963460908541034 // table 264, SetBonusRecordBase

	// Plan D1: using an item, the level curves and the skill trees.
	TypeItemActivationArchetype = 4292131259496257600 // table 163, ItemActivationArchetypeRecordBase
	TypeActionResponse          = 1776538005552784248 // table 10, ActionResponseRecordBase
	TypeRewardTable             = 2615607320058196115 // table 255, RewardTableRecordBase
	TypeLevelingTypeDef         = 5949882024684912047 // table 177, LevelingTypeDefRecordBase (the key of the XPTracker's maps)
	TypeSkillTree               = 297887639323616934  // table 272, SkillTreeRecordBase (the key of SkillTreePurchasesFastMap)
	TypeSkillTreeNode           = 1084214640716601260 // table 273
	TypeSkill                   = 4146438674247114715 // table 268, SkillRecordBase
	TypeSkillRank               = 4847957796014348780 // table 270, SkillRankRecordBase (the ranks of a tree's list)

	// Plan D1, stage 2: the talents (what a node needs, what a rank costs, the weapon lines).
	TypeSkillRequirement = 7584189049858318272 // SkillRequirementRecordBase (a node's SkillsRequired)
	TypeSkillPointType   = 7337271797676719493 // SkillPointTypeRecordBase (what a rank costs: Combat_Archetype, Combat_Stamina, Combat_Weapon)
	TypeWeaponSkillLines = 7711002412429888222 // WeaponSkillLinesRecordBase (an item's WeaponSkillLineId)
	TypeSkillTreeSlot    = 5531284426918814767 // SkillTreeSlotRecordBase (a node's SkillSlotId: a talent choice, its SkillsIds the options)
)

// -- C-O-A x Cyb --

// Plan D1: the records worldd's leveling names.
const (
	// GuidAdventuring is the LevelingTypeDef of the character's own level (LevelCapOverride 25, bUsesExperienceDebt).
	GuidAdventuring = 108651085518458
	// GuidUniversalTree is the SkillTree every class has, whose nodes a level grants by itself (Sprint Training at 5, 20
	// and 35, Endurance Training at 10, 30 and 50, Guard and Dodge Training).
	GuidUniversalTree = 6064631673604472832
	// GuidAdventuringXPAddMult is the stat the level scrolls' XP formula and Adventuring's XPMultiplier add to 1.
	GuidAdventuringXPAddMult = 6064630809541214212
)

// GuidMaxDurability is the stat an item's durability is: 6064629444242636800 is the StatTypeDef MaxDurability (plan B2:
// a new item's Durability is its stat block's value for this stat, rounded half up).
const GuidMaxDurability = 6064629444242636800

// The containers of starting_equipment.
const (
	KitEquipment = "Equipment"
	KitBackpack  = "Backpack"
)

// Record Guids the first slice reads. Sprint, Dodge and the class Guids are
// those of DefaultAoCRecordConstants.ini; the stat Guids are the StatTypeDef
// rows of the same names (and, for Stamina, MaxStamina and StaminaRegeneration,
// the Guids the original server sent in the capture).
const (
	GuidSprint = 6064631641648332884
	GuidDodge  = 107258680465016
	// GuidRest is General_Rest, the third ability worldd runs (plan A7): DefaultAoCRecordConstants.ini does not
	// name it; the original server's replay_data.bin answers it (pkt#8175-8320) and its Ability row is "Rest".
	GuidRest = 109393746155128

	// GuidSprintStaminaDrain is the effect General_Sprint_StaminaDrain: infinite, a tick every 0.25 s, whose
	// tick hit takes 1.25 Stamina while the owner moves (2.5 in combat). No record references it (all 205,882 were
	// searched), so nothing says a Sprint applies it; plan A7 takes it as the Sprint's drain (spec section 7,
	// decided 2026-10-01) and contentc checks that its numbers agree with the Sprint cost row.
	GuidSprintStaminaDrain = 6064632350237720599

	GuidStamina             = 6064629575339934476
	GuidMaxStamina          = 6064631747701440948
	GuidStaminaRegeneration = 6064631663405498419

	GuidStrength     = 109183598614138
	GuidDexterity    = 109183598614139
	GuidConstitution = 109183598614140
	GuidIntelligence = 109183598614141
	GuidWisdom       = 109183598614142
	GuidMentality    = 109183598614143

	// GuidPlayerBase is the StatInitializerList every player starts from.
	GuidPlayerBase = 6064630101072872893
)

// PrimaryAttributes are the six stats class_stat_base holds, in Guid order.
var PrimaryAttributes = []struct {
	Name string
	Guid int64
}{
	{"Strength", GuidStrength},
	{"Dexterity", GuidDexterity},
	{"Constitution", GuidConstitution},
	{"Intelligence", GuidIntelligence},
	{"Wisdom", GuidWisdom},
	{"Mentality", GuidMentality},
}

// PlayerClasses are the eight player classes' names in archetype order: the
// character-select archetype id of a class is ArchetypeBase plus its position
// (services/internal/aocchar: Bard 17747 ... Tank 17754). The design database
// does not hold these ids; the services' wire format does.
var PlayerClasses = []string{"Bard", "Cleric", "Fighter", "Mage", "Ranger", "Rogue", "Summoner", "Tank"}

// ArchetypeBase is Bard's archetype id.
const ArchetypeBase = 17747

// ArchetypeID is the archetype id of a player class, or 0 for another name.
func ArchetypeID(class string) int64 {
	for i, c := range PlayerClasses {
		if c == class {
			return ArchetypeBase + int64(i)
		}
	}
	return 0
}

// -- CoA & C | Y | B --

// Levels is the range of levels class_stat_base covers.
const (
	MinLevel = 1
	MaxLevel = 50
)

// Stamina rule keys (table stamina_rule); contentc writes them, A7 reads them.
const (
	RuleStaminaInitial        = "stamina.initial"
	RuleMaxStaminaInitial     = "max_stamina.initial"
	RuleRegenBaseExpr         = "regen.base_expr"
	RuleRegenRateEnabled      = "regen.rate_enabled"
	RuleRegenRateDisabled     = "regen.rate_disabled"
	RuleStatRegenTime         = "stat_regen_time"
	RuleSprintCostOutOfCombat = "sprint.cost_per_charge_out_of_combat"
	RuleSprintCostInCombat    = "sprint.cost_per_charge_in_combat"
	// RuleSprintChargeInterval is the seconds between the charges of a held Sprint: the TickTimer of the
	// effect General_Sprint_StaminaDrain, the one cadence the design data has (see GuidSprintStaminaDrain).
	RuleSprintChargeInterval = "sprint.charge_interval"
	RuleDodgeCostBase        = "dodge.cost_base"
	RuleDodgeCostSkilled     = "dodge.cost_skilled"
	RuleDodgeCooldownBase    = "dodge.cooldown_base"
	RuleDodgeCooldownSkilled = "dodge.cooldown_skilled"
)

// DefaultStatRegenTime is StatRegenTime in seconds, from DefaultCombat.ini
// (Stats=(StatRegenTime=1.000000,...)); contentc reads the file when it is
// given the client's Config directory and uses this value otherwise.
const DefaultStatRegenTime = 1.0

// Audience says who a stat is replicated to, from the Replication byte of its
// StatTypeDef row, and the StatsComponent array that carries it.
//
// The byte is a mask whose bits are EReplicationRules in the mappings file
// (Rep_Autonomous 1, Rep_Simulated 2, Rep_Proxy 4); that it is that enum is an
// inference, but the capture (replay_data.bin, the StatsComponent fast arrays)
// fixes the three values it shows: the 331 stats it sends in StatRepInt32Owner
// all have 1, the 96 in StatRepInt32OwnerProxy all have 5 and the 11 in
// StatRepInt32EveryoneProxy all have 7, and no stat of another value appears.
// field is "" for the other values (0 is not replicated; 2 and 3 are in no
// stat; 6 is in two, and the capture does not show them).
func Audience(replication int) (audience, field string) {
	if replication == 0 {
		return "none", ""
	}
	bits := []struct {
		bit  int
		name string
	}{{1, "Rep_Autonomous"}, {2, "Rep_Simulated"}, {4, "Rep_Proxy"}}
	for _, b := range bits {
		if replication&b.bit != 0 {
			if audience != "" {
				audience += "|"
			}
			audience += b.name
		}
	}
	if rest := replication &^ 7; rest != 0 {
		if audience != "" {
			audience += "|"
		}
		audience += fmt.Sprintf("0x%x", rest)
	}
	switch replication {
	case 1:
		field = "StatRepInt32Owner"
	case 5:
		field = "StatRepInt32OwnerProxy"
	case 7:
		field = "StatRepInt32EveryoneProxy"
	}
	return audience, field
}
