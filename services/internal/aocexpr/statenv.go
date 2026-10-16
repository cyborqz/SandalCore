// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// The environment of a player's stat formulas (plan B3): the functions the
// StatTypeDef expressions, the StatInitializerList expressions and the
// equations they evaluate call, over an explicit context the stat engine
// implements.
//
// The functions, by what they need:
//
//	the stat engine         GetStat(obj, stat)  GetStatInitRunningTotal()  EvalE(equation)
//	                        EvalFormula(formula)  GetTerm(term)  GetCharacterLevel(obj)
//	objects                 GetOwner() GetInstigator() GetSource() GetSelf(): one character
//	                        in a stat formula, so all four are the same Object kinds apart
//	state of part 2         GetEffectCount  GetEffectCountByTag  GetSkillPurchased  IsInCombat
//	                        IsMoving  IsUnkillable  EquippedItemInSlotHasTag  GetActorGuid
//	                        GetPlayerProfessionLevel  GetConsumedItemStat  GetNpcEliteRank
//	                        GetNpcStatDistribution (plan E1, stage 3: an NPC's weights)
//
// The state functions are State's methods; NoState gives each the value of a
// character that has nothing yet (no effect, no skill bought, out of combat, no
// consumable in use, level 1 in every profession), which is what the captured
// level-1 Bard shows.
// A real state replaces NoState one method at a time (embed NoState).
// -- Cyb & C-O-A --

// State is what the formulas read of the character's state beyond its class,
// level and equipment: the part of the world plan B3 leaves for later plans
// (effects and talents, consumables, the guild, professions, combat).
type State interface {
	// EffectCount is GetEffectCount(obj, effect): the stacks of an effect.
	EffectCount(effect Ref) float64
	// EffectCountByTag is GetEffectCountByTag(obj, tag): the effects with a tag.
	EffectCountByTag(tag string) float64
	// SkillPurchased is GetSkillPurchased(obj, class, skill).
	SkillPurchased(class, skill Ref) bool
	// InCombat is IsInCombat(obj).
	InCombat() bool
	// Moving is IsMoving(obj).
	Moving() bool
	// Unkillable is IsUnkillable(obj): the Health stat's minimum while it is.
	Unkillable() bool
	// EquippedItemInSlotHasTag is EquippedItemInSlotHasTag(slot, tag): the item
	// worn in the slot has the gameplay tag (a tag also matches its parent).
	EquippedItemInSlotHasTag(slot Enum, tag string) bool
	// ActorGuid is GetActorGuid(obj), the id GetPlayerProfessionLevel takes.
	ActorGuid() float64
	// ProfessionLevel is GetPlayerProfessionLevel(actor, profession).
	ProfessionLevel(actor float64, profession string) float64
	// ConsumedItemStat is GetConsumedItemStat(stat): the stat's value on the
	// consumable that applied the effect being evaluated.
	ConsumedItemStat(stat Ref) float64
	// NpcEliteRank is GetNpcEliteRank(obj): 0 for anything but an NPC.
	NpcEliteRank() float64
	// NpcStatDistribution is GetNpcStatDistribution(obj, stat): the share of an NPC's stat budget one stat gets, its weight over the sum of the
	// eight (EStatDistributionStat order: Constitution, Strength, Dexterity, Mentality, Intelligence, Wisdom, MaxHealth, MaxMana); 0 for a player.
	NpcStatDistribution(index int) float64
}

// StatDistributionIndex is the number of an EStatDistributionStat value (its order in the enum), -1 for a name that is none.
func StatDistributionIndex(name string) int {
	for i, n := range []string{"Constitution", "Strength", "Dexterity", "Mentality", "Intelligence", "Wisdom", "MaxHealth", "MaxMana"} {
		if n == name {
			return i
		}
	}
	return -1
}

// NoState is the State of a character with nothing yet. Its values:
//
//	EffectCount, EffectCountByTag       0 (no effect applied)
//	SkillPurchased                      false (no skill bought)
//	InCombat, Moving, Unkillable        false
//	EquippedItemInSlotHasTag            false (the engine's own context answers it from the
//	                                    equipped items' tags; NoState knows of no item)
//	ActorGuid                           0
//	ProfessionLevel                     1 (a new character is level 1 in every profession)
//	ConsumedItemStat                    0 (nothing is being consumed)
//	NpcEliteRank                        0 (a player)
//	NpcStatDistribution                 0 (a player)
type NoState struct{}

func (NoState) EffectCount(Ref) float64                    { return 0 }
func (NoState) EffectCountByTag(string) float64            { return 0 }
func (NoState) SkillPurchased(Ref, Ref) bool               { return false }
func (NoState) InCombat() bool                             { return false }
func (NoState) Moving() bool                               { return false }
func (NoState) Unkillable() bool                           { return false }
func (NoState) EquippedItemInSlotHasTag(Enum, string) bool { return false }
func (NoState) ActorGuid() float64                         { return 0 }
func (NoState) ProfessionLevel(float64, string) float64    { return 1 }
func (NoState) ConsumedItemStat(Ref) float64               { return 0 }
func (NoState) NpcEliteRank() float64                      { return 0 }
func (NoState) NpcStatDistribution(int) float64            { return 0 }

// StatContext is what a stat formula runs against: the stat engine's own
// functions and the State.
type StatContext interface {
	State
	// Stat is GetStat: the stat's value (a named reference, $Stat:Name$, is the
	// context's to resolve).
	Stat(stat Ref) (float64, error)
	// InitRunningTotal is GetStatInitRunningTotal(): the value of the stat the
	// initializer entry being run belongs to, as the entries before it left it.
	InitRunningTotal() (float64, error)
	// Equation is EvalE(equation): the value of a StatEquationType row.
	Equation(equation Ref) (float64, error)
	// Formula is EvalFormula(formula): the value of a StatFormulaType row.
	Formula(formula Ref) (float64, error)
	// Term is GetTerm(term): the value of an input term of the formula being
	// evaluated.
	Term(term Ref) (float64, error)
	// CharacterLevel is GetCharacterLevel(obj).
	CharacterLevel() float64
}

// (C-O-A | Cyb)

// StatFloatFuncs are the stat environment's host functions whose result is a
// float (see Env.FloatFuncs): the stats and the equations' values.
var StatFloatFuncs = map[string]bool{
	"GetStat":                 true,
	"GetStatInitRunningTotal": true,
	"EvalE":                   true,
	"EvalEquation":            true,
	"EvalFormula":             true,
	"EvalF":                   true,
	"GetTerm":                 true,
	"GetConsumedItemStat":     true,
	"GetNpcStatDistribution":  true,
}

// StatFuncNames lists the host functions StatEnv supplies (for a compiler that
// must know the same set).
func StatFuncNames() []string {
	return []string{
		"GetOwner", "GetInstigator", "GetSource", "GetSelf",
		"GetCharacterLevel", "GetNPCLevel", "GetStat", "GetStatInitRunningTotal", "EvalE", "EvalEquation", "EvalFormula", "EvalF", "GetTerm",
		"GetEffectCount", "GetEffectCountByTag", "GetSkillPurchased", "IsInCombat", "IsMoving", "IsUnkillable",
		"EquippedItemInSlotHasTag", "GetActorGuid", "GetPlayerProfessionLevel", "GetConsumedItemStat", "GetNpcEliteRank", "GetNpcStatDistribution",
	}
}

// contextOf is the context that answers the reads of an object: the actor's own when ctx knows it (RoleContexter), else the caster's.
func contextOf(ctx StatContext, o Object) StatContext {
	if o.Role != RoleCaster {
		if rc, ok := ctx.(RoleContexter); ok {
			if c := rc.RoleContext(o.Role); c != nil {
				return c
			}
		}
	}
	return ctx
}

// [C.O.A - C_Y_B]

// StatEnv returns the environment of a player's stat formulas over ctx: arithmetic
// is float64 (set Single for float32), EvalCurve resolves in curves.
//
// An object of another role than the caster (GetTarget() and what comes from it: ability expressions only) is read in the context of that actor when
// ctx knows it (RoleContexter), else as the caster: the reads the compiled code runs in the actor's context (OpActor).
func StatEnv(ctx StatContext, curves CurveSource) *Env {
	obj := func(kind string) Func {
		return func(args []Value) (Value, error) {
			if len(args) != 0 {
				return nil, fmt.Errorf("takes no arguments, got %d", len(args))
			}
			return Object{Kind: kind}, nil
		}
	}
	// argsOf checks the arguments' count and types: 'o' an Object, 'r' a Ref,
	// 's' a string, 'e' an Enum, 'n' a number.
	argsOf := func(args []Value, sig string) error {
		if len(args) != len(sig) {
			return fmt.Errorf("takes %d argument(s), got %d", len(sig), len(args))
		}
		for i, c := range sig {
			ok := false
			switch c {
			case 'o':
				_, ok = args[i].(Object)
			case 'r':
				_, ok = args[i].(Ref)
			case 's':
				_, ok = args[i].(string)
			case 'e':
				_, ok = args[i].(Enum)
			case 'n':
				_, ok = args[i].(float64)
			}
			if !ok {
				return fmt.Errorf("argument %d is %s", i+1, describeValue(args[i]))
			}
		}
		return nil
	}
	// of is the context that answers the reads of an object: the actor's own when ctx knows it, else the caster's.
	of := func(o Object) StatContext { return contextOf(ctx, o) }
	num := func(f func(args []Value) (float64, error)) Func {
		return func(args []Value) (Value, error) {
			v, err := f(args)
			if err != nil {
				return nil, err
			}
			return v, nil
		}
	}
	boolean := func(sig string, f func(args []Value) bool) Func {
		return func(args []Value) (Value, error) {
			if err := argsOf(args, sig); err != nil {
				return nil, err
			}
			return f(args), nil
		}
	}
	// roleBool is a boolean read of an object: in its context.
	roleBool := func(read func(c StatContext) bool) Func {
		return func(args []Value) (Value, error) {
			if err := argsOf(args, "o"); err != nil {
				return nil, err
			}
			return read(of(args[0].(Object))), nil
		}
	}
	level := func(a []Value) (float64, error) {
		if err := argsOf(a, "o"); err != nil {
			return 0, err
		}
		return of(a[0].(Object)).CharacterLevel(), nil
	}
	formula := func(a []Value) (float64, error) {
		if err := argsOf(a, "r"); err != nil {
			return 0, err
		}
		return ctx.Formula(a[0].(Ref))
	}
	equation := func(a []Value) (float64, error) {
		if err := argsOf(a, "r"); err != nil {
			return 0, err
		}
		return ctx.Equation(a[0].(Ref))
	}
	return &Env{
		Curves:     curves,
		FloatFuncs: StatFloatFuncs,
		Funcs: map[string]Func{
			"GetOwner":          obj("owner"),
			"GetInstigator":     obj("instigator"),
			"GetSource":         obj("source"),
			"GetSelf":           obj("self"),
			"GetCharacterLevel": num(level),
			"GetNPCLevel":       num(level),
			"GetStat": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "or"); err != nil {
					return 0, err
				}
				return of(a[0].(Object)).Stat(a[1].(Ref))
			}),
			"GetStatInitRunningTotal": num(func(a []Value) (float64, error) {
				if err := argsOf(a, ""); err != nil {
					return 0, err
				}
				return ctx.InitRunningTotal()
			}),
			"EvalE":        num(equation),
			"EvalEquation": num(equation),
			"EvalFormula":  num(formula),
			"EvalF":        num(formula),
			"GetTerm": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "r"); err != nil {
					return 0, err
				}
				return ctx.Term(a[0].(Ref))
			}),
			"GetEffectCount": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "or"); err != nil {
					return 0, err
				}
				return of(a[0].(Object)).EffectCount(a[1].(Ref)), nil
			}),
			"GetEffectCountByTag": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "os"); err != nil {
					return 0, err
				}
				return of(a[0].(Object)).EffectCountByTag(a[1].(string)), nil
			}),
			"GetSkillPurchased": func(a []Value) (Value, error) {
				if err := argsOf(a, "orr"); err != nil {
					return nil, err
				}
				// the tree of the weapon in hand (GetCurrentWeaponSkilltree) is the host's to name: the state answers the pair as the caller made it
				return of(a[0].(Object)).SkillPurchased(a[1].(Ref), a[2].(Ref)), nil
			},
			"IsInCombat":   roleBool(func(c StatContext) bool { return c.InCombat() }),
			"IsMoving":     roleBool(func(c StatContext) bool { return c.Moving() }),
			"IsUnkillable": roleBool(func(c StatContext) bool { return c.Unkillable() }),
			"EquippedItemInSlotHasTag": boolean("es", func(a []Value) bool {
				return ctx.EquippedItemInSlotHasTag(a[0].(Enum), a[1].(string))
			}),
			"GetActorGuid": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "o"); err != nil {
					return 0, err
				}
				if a[0].(Object).Role != RoleCaster {
					return 0, fmt.Errorf("the actor id of another actor is not modelled")
				}
				return ctx.ActorGuid(), nil
			}),
			"GetPlayerProfessionLevel": num(func(a []Value) (float64, error) {
				// GetPlayer() (the GM artisan scrolls' experience expressions) is the character the expression is evaluated for: the actor of the context
				if len(a) == 2 {
					if o, ok := a[0].(Object); ok && o.Role == RolePlayer {
						a = []Value{ctx.ActorGuid(), a[1]}
					}
				}
				if err := argsOf(a, "ns"); err != nil {
					return 0, err
				}
				return ctx.ProfessionLevel(a[0].(float64), a[1].(string)), nil
			}),
			"GetConsumedItemStat": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "r"); err != nil {
					return 0, err
				}
				return ctx.ConsumedItemStat(a[0].(Ref)), nil
			}),
			"GetNpcEliteRank": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "o"); err != nil {
					return 0, err
				}
				return of(a[0].(Object)).NpcEliteRank(), nil
			}),
			"GetNpcStatDistribution": num(func(a []Value) (float64, error) {
				if err := argsOf(a, "oe"); err != nil {
					return 0, err
				}
				e := a[1].(Enum)
				i := StatDistributionIndex(e.Value)
				if e.Type != "EStatDistributionStat" || i < 0 {
					return 0, fmt.Errorf("%v is not an EStatDistributionStat", e)
				}
				if a[0].(Object).Role != RoleCaster {
					return 0, fmt.Errorf("the stat distribution of another actor is not modelled")
				}
				return ctx.NpcStatDistribution(i), nil
			}),
		},
	}
}

// C-O-A x C_Y_B

// TagMatches reports whether a gameplay tag matches a wanted one (FNames: without regard to case): equal, or a
// descendant of it ("Item.Gear.Weapon.Shield.Tower" matches "Item.Gear.Weapon.Shield").
func TagMatches(tag, want string) bool {
	if len(tag) == len(want) {
		return strings.EqualFold(tag, want)
	}
	return len(tag) > len(want) && tag[len(want)] == '.' && strings.EqualFold(tag[:len(want)], want)
}

// AbilityContext is what an ability's expressions (a cost, a cooldown, a cast time, a requirement, the value of an
// effect's stat mod) run against: the stat environment and the three inputs only the host can give. The compiled
// engine (package statprog) answers them from its ExprHost; this is the reference.
//
// GetTarget() is the actor the hit is on (RoleTarget): when the hit is on the caster (a self or party hit) every read of it is the caster's
// own, and the host answers so. Reads of the target and of the other actors (hostfn.go) are the host's: HostFn.
type AbilityContext interface {
	StatContext
	HostCaller
	// AbilityProgress is GetAbilityProgressRatio(group): how far the running ability of the montage group
	// ("SongGroup") has got, 0 to 1.
	AbilityProgress(group string) float64
	// EffectCountByInstigator is GetEffectCountByRecordAndInstigator(target, effect, instigator): the stacks of the
	// effect on the target that the instigator applied (the caster's own effects on the caster).
	EffectCountByInstigator(effect Ref) float64
	// RandDouble is RandDouble(): the host's seeded source, a number in [0, 1).
	RandDouble() float64
	// HitHasTag is CheckForDamageTag(GetHitInProgress(), tag) and HitHasTag: the hit in progress (or its ability) carries the gameplay tag.
	HitHasTag(tag string) bool
	// HitHasFlag is HitFlagsContains(GetHitFlags(GetHitInProgress(), GetTarget()), EHitFlag::flag): the hit on the target has the flag
	// ("Crit" ...).
	HitHasFlag(flag string) bool
	// HitTargets is GetNumTargets(GetHitInProgress()): how many targets the hit has.
	HitTargets() float64
	// XPDebtRatio is GetXPDebtRatio(actor): the experience debt of the instigator, 0 to 1 (the actor is the caster's own).
	XPDebtRatio() float64
}

// [C_Y_B x C.O.A]

// AbilityFloatFuncs are the host functions of an ability expression that give a float (on top of StatFloatFuncs).
var AbilityFloatFuncs = map[string]bool{
	"GetAbilityProgressRatio":             true,
	"RandDouble":                          true,
	"GetXPDebtRatio":                      true,
	"GetRemainingAbilityCharges":          true,
	"GetStatDeltaOverTime":                true,
	"GetStatModValue":                     true,
	"GetStatModEffectiveValue":            true,
	"GetAngleBetween":                     true,
	"Now":                                 true,
	"GetProgressDuration":                 true,
	"GetDistanceBetween":                  true,
	"GetHealthPercentage":                 true,
	"GetEffectRemainingDuration":          true,
	"GetRemainingAbilityCooldown":         true,
	"GetHitTriggerCount":                  true,
	"GetAbilityHitCount":                  true,
	"GetAbilityComboCount":                true,
	"RandIntRange":                        true,
	"GetArtisanshipModifiedStat":          true,
	"GetGatheringInteractionBaseDuration": true,
	"CalculateDamage":                     true,
	"CalculateDamageMultiplier":           true,
	"CalculateDamageAdditiveMultiplier":   true,
	"CalculateDamageMitigation":           true,
	"CalculateDamageCritical":             true,
	"CalculateDamagePenetration":          true,
	"CalculateDamageDampening":            true,
}

// AbilityFuncNames lists the host functions AbilityEnv supplies besides StatFuncNames.
func AbilityFuncNames() []string {
	return []string{"GetTarget", "GetActiveTarget", "GetCreatureOwner", "GetAbilityProgressRatio", "GetEffectCountByRecordAndInstigator", "RandDouble",
		"GetHitInProgress", "CheckForDamageTag", "HitHasTag", "GetHitFlags", "HitFlagsContains", "GetNumTargets", "GetXPDebtRatio",
		"GetRemainingAbilityCharges", "IsApplicableTarget", "IsAnyMovementKeyPressed", "GetStatDeltaOverTime", "GetHitEvent", "GetStatModValue",
		"GetStatModEffectiveValue", "IsNeutral", "IsEnemy", "GetAngleBetween", "Now", "GetProgressDuration",
		"GetDistanceBetween", "GetHealthPercentage", "HasAlignment", "GetEffectRemainingDuration", "GetRemainingAbilityCooldown", "IsClient", "RandIntRange",
		"GetHitTriggerCount", "GetAbilityHitCount", "GetAbilityComboCount", "IsComboButtonPressed", "GetCurrentWeaponSkilltree", "DoesHitContainActor", "GetClosestTargetToActorInHit", "GetHitRecord", "GetHitSourceRecord", "GetRecordTypeId", "GetProjectileTypeId", "CalculateDamage", "CalculateDamageMultiplier", "CalculateDamageAdditiveMultiplier",
		"CalculateDamageMitigation", "CalculateDamageCritical", "CalculateDamagePenetration", "CalculateDamageDampening", "Int", "GetPlayerLootMaxLevel", "GetPlayerLootAvgLuck", "DoesNPCContainTag", "GetPlayerLevel",
		// Nodes 1, stage 1: the city node reads
		"GetPlayer", "GetPlayerGuid", "GetNodeBiome", "GetNodeRecord", "GetCurrentNode", "Node_GetNodeType", "Node_GetLevel", "Node_InLevelRangeInclusive",
		"Node_IsPlayerCitizen", "Node_IsPlayerCitizenOfNode", "Node_IsPlayerMayor", "EnumInt",
		// Gathering stage 2: the gathering reads
		"GetArtisanshipModifiedStat", "GetRolledItemQuality", "GetItemRarity", "GetSourceCertificationTier", "GetGatheringInteractionBaseDuration"}
}

// [C | O | A :: C:Y:B]

// AbilityEnv returns the environment of an ability expression over ctx: StatEnv, and GetTarget, GetAbilityProgressRatio,
// GetEffectCountByRecordAndInstigator and RandDouble.
func AbilityEnv(ctx AbilityContext, curves CurveSource) *Env {
	env := StatEnv(ctx, curves)
	env.LooseBool = true
	env.FloatFuncs = map[string]bool{}
	for k := range StatFloatFuncs {
		env.FloatFuncs[k] = true
	}
	for k := range AbilityFloatFuncs {
		env.FloatFuncs[k] = true
	}
	env.SameActor = func(a, b int) bool { return ctx.HostFn(HostSameActor, RolePair(a, b), nil) != 0 }
	env.Funcs["GetTarget"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return Object{Kind: "target", Role: RoleTarget}, nil
	}
	env.Funcs["GetActiveTarget"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		o, ok := args[0].(Object)
		e, ok2 := args[1].(Enum)
		if !ok || !ok2 || e.Type != "ETargetingIntention" || o.Role != RoleCaster {
			return nil, fmt.Errorf("takes the caster and an ETargetingIntention")
		}
		switch e.Value {
		case "Harm":
			return Object{Kind: "activetarget", Role: RoleActiveHarm}, nil
		case "Support":
			return Object{Kind: "activetarget", Role: RoleActiveSupport}, nil
		}
		return nil, fmt.Errorf("the targeting intention %v is not modelled", e)
	}
	env.Funcs["GetCreatureOwner"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		o, ok := args[0].(Object)
		if !ok {
			return nil, fmt.Errorf("takes an actor")
		}
		switch o.Role {
		case RoleCaster:
			return Object{Kind: "owner", Role: RoleCasterOwner}, nil
		case RoleTarget:
			return Object{Kind: "owner", Role: RoleTargetOwner}, nil
		}
		return nil, fmt.Errorf("the owner of that actor is not modelled")
	}
	env.Funcs["GetAbilityProgressRatio"] = func(args []Value) (Value, error) {
		if len(args) == 0 {
			return ctx.AbilityProgress(""), nil // the ability that is running
		}
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 0 or 1 argument(s), got %d", len(args))
		}
		g, ok := args[0].(string)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s", describeValue(args[0]))
		}
		return ctx.AbilityProgress(g), nil
	}
	env.Funcs["GetEffectCountByRecordAndInstigator"] = func(args []Value) (Value, error) {
		if len(args) != 3 {
			return nil, fmt.Errorf("takes 3 argument(s), got %d", len(args))
		}
		t, o1 := args[0].(Object)
		r, ok := args[1].(Ref)
		i, o2 := args[2].(Object)
		if !o1 || !ok || !o2 {
			return nil, fmt.Errorf("takes an object, a record reference and an object")
		}
		if t.Role == RoleCaster && i.Role == RoleCaster {
			return ctx.EffectCountByInstigator(r), nil
		}
		return ctx.HostFn(RoleFn(t.Role, RoleOpEffectInst), strconv.FormatInt(r.Guid, 10), []float64{float64(i.Role)}), nil
	}
	// The hit in progress: an object the hit functions take; the flags of a hit on a target: a value only HitFlagsContains reads.
	env.Funcs["GetHitInProgress"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return Object{Kind: "hit"}, nil
	}
	hitTag := func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		_, ok := args[0].(Object)
		tag, ok2 := args[1].(string)
		if !ok || !ok2 {
			return nil, fmt.Errorf("takes the hit and a tag")
		}
		return ctx.HitHasTag(tag), nil
	}
	env.Funcs["CheckForDamageTag"] = hitTag
	env.Funcs["HitHasTag"] = hitTag
	env.Funcs["GetHitFlags"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		_, o1 := args[0].(Object)
		_, o2 := args[1].(Object)
		if !o1 || !o2 {
			return nil, fmt.Errorf("takes the hit and the target")
		}
		return Object{Kind: "hitflags"}, nil
	}
	env.Funcs["HitFlagsContains"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		o, ok := args[0].(Object)
		e, ok2 := args[1].(Enum)
		if !ok || o.Kind != "hitflags" || !ok2 || e.Type != "EHitFlag" {
			return nil, fmt.Errorf("takes the flags of a hit and an EHitFlag")
		}
		return ctx.HitHasFlag(e.Value), nil
	}
	env.Funcs["GetNumTargets"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		if _, ok := args[0].(Object); !ok {
			return nil, fmt.Errorf("takes the hit")
		}
		return ctx.HitTargets(), nil
	}
	env.Funcs["GetXPDebtRatio"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		if _, ok := args[0].(float64); !ok {
			return nil, fmt.Errorf("takes an actor id")
		}
		return ctx.XPDebtRatio(), nil
	}
	env.Funcs["RandDouble"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.RandDouble(), nil
	}
	// the named host functions (plan D2, stage 2; plan E1, stage 3)
	isObj := func(v Value) bool { _, ok := v.(Object); return ok }
	env.Funcs["GetRemainingAbilityCharges"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		r, ok := args[0].(Ref)
		if !ok {
			return nil, fmt.Errorf("takes an ability reference")
		}
		return ctx.HostFn(HostRemainingCharges, fmt.Sprint(r.Guid), nil), nil
	}
	env.Funcs["GetRemainingAbilityCooldown"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		r, ok := args[0].(Ref)
		if !ok {
			return nil, fmt.Errorf("takes an ability reference")
		}
		return ctx.HostFn(HostCooldownLeft, fmt.Sprint(r.Guid), nil), nil
	}
	env.Funcs["IsApplicableTarget"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		e, ok := args[1].(Enum)
		if !isObj(args[0]) || !ok || e.Type != "EAffectedTarget" {
			return nil, fmt.Errorf("takes an actor and an EAffectedTarget")
		}
		return ctx.HostFn(HostApplicableTarget, e.Value, nil) != 0, nil
	}
	env.Funcs["IsAnyMovementKeyPressed"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostMovementKey, "", nil) != 0, nil
	}
	env.Funcs["IsClient"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostIsClient, "", nil) != 0, nil
	}
	// int(x): the compiler (statprog) makes it Floor, which is the truncation of the non-negative numbers the data applies it to (a roll, a curve's quantity)
	env.Funcs["Int"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		x, ok := args[0].(float64)
		if !ok {
			return nil, fmt.Errorf("takes a number")
		}
		return math.Floor(x), nil
	}
	env.Funcs["RandIntRange"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		lo, ok := args[0].(float64)
		hi, ok2 := args[1].(float64)
		if !ok || !ok2 {
			return nil, fmt.Errorf("takes two numbers")
		}
		return ctx.HostFn(HostRandIntRange, "", []float64{lo, hi}), nil
	}
	env.Funcs["GetStatDeltaOverTime"] = func(args []Value) (Value, error) {
		if len(args) != 3 {
			return nil, fmt.Errorf("takes 3 argument(s), got %d", len(args))
		}
		secs, ok := args[1].(float64)
		r, ok2 := args[2].(Ref)
		if !isObj(args[0]) || !ok || !ok2 {
			return nil, fmt.Errorf("takes an actor, a number of seconds and a stat")
		}
		return ctx.HostFn(HostStatDelta, fmt.Sprint(r.Guid), []float64{secs}), nil
	}
	env.Funcs["GetHitEvent"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return Object{Kind: "hitevent"}, nil
	}
	statMod := func(fn int) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			if len(args) != 2 && len(args) != 3 {
				return nil, fmt.Errorf("takes 2 or 3 arguments, got %d", len(args))
			}
			r, ok := args[len(args)-1].(Ref)
			if o, isO := args[0].(Object); !isO || o.Kind != "hitevent" || !ok || (len(args) == 3 && !isObj(args[1])) {
				return nil, fmt.Errorf("takes the hit event, an optional actor and a stat")
			}
			return ctx.HostFn(fn, fmt.Sprint(r.Guid), nil), nil
		}
	}
	env.Funcs["GetStatModValue"] = statMod(HostHitStatMod)
	env.Funcs["GetStatModEffectiveValue"] = statMod(HostHitStatEffective)
	env.Funcs["GetAbilityComboCount"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostComboCount, "", nil), nil
	}
	env.Funcs["IsComboButtonPressed"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		e, ok := args[0].(Enum)
		if !ok || e.Type != "EComboButtons" {
			return nil, fmt.Errorf("takes an EComboButtons value")
		}
		return ctx.HostFn(HostComboButton, e.Value, nil) != 0, nil
	}
	// The hit's own record and its source: GetRecordTypeId(GetHitSourceRecord(hit)) is the host's, compared with GetProjectileTypeId().
	env.Funcs["GetHitRecord"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObj(args[0]) {
			return nil, fmt.Errorf("takes the hit")
		}
		return Object{Kind: "hit"}, nil
	}
	env.Funcs["GetHitSourceRecord"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObj(args[0]) {
			return nil, fmt.Errorf("takes the hit")
		}
		return HitSourceRef, nil
	}
	env.Funcs["GetRecordTypeId"] = func(args []Value) (Value, error) {
		r, ok := args[0].(Ref)
		if len(args) != 1 || !ok {
			return nil, fmt.Errorf("takes a record reference")
		}
		if r == HitSourceRef {
			return ctx.HostFn(HostHitSourceType, "", nil), nil
		}
		return float64(r.TypeID), nil
	}
	env.Funcs["GetProjectileTypeId"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return float64(TypeProjectile), nil
	}
	env.Funcs["GetCurrentWeaponSkilltree"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObj(args[0]) {
			return nil, fmt.Errorf("takes an actor")
		}
		return WeaponSkilltreeRef, nil
	}
	env.Funcs["DoesHitContainActor"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObj(args[0]) || !isObj(args[1]) {
			return nil, fmt.Errorf("takes the hit and an actor")
		}
		return ctx.HostFn(HostHitContains, strconv.Itoa(args[1].(Object).Role), nil) != 0, nil
	}
	env.Funcs["GetClosestTargetToActorInHit"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObj(args[0]) || !isObj(args[1]) || args[1].(Object).Role != RoleCaster {
			return nil, fmt.Errorf("takes the hit and the caster")
		}
		return Object{Kind: "closest", Role: RoleClosestInHit}, nil
	}
	// two actors: the relation between the roles, and what the host knows of them
	relation := func(fn int) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			a, ok := args[0].(Object)
			b, ok2 := args[1].(Object)
			if len(args) != 2 || !ok || !ok2 {
				return nil, fmt.Errorf("takes two actors")
			}
			return ctx.HostFn(fn, RolePair(a.Role, b.Role), nil) != 0, nil
		}
	}
	env.Funcs["IsNeutral"] = relation(HostIsNeutral)
	env.Funcs["IsEnemy"] = relation(HostIsEnemy)
	env.Funcs["GetAngleBetween"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObj(args[0]) || !isObj(args[1]) {
			return nil, fmt.Errorf("takes two actors")
		}
		return ctx.HostFn(HostAngleBetween, RolePair(args[0].(Object).Role, args[1].(Object).Role), nil), nil
	}
	env.Funcs["GetDistanceBetween"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObj(args[0]) || !isObj(args[1]) {
			return nil, fmt.Errorf("takes two actors")
		}
		return ctx.HostFn(HostDistanceBetween, RolePair(args[0].(Object).Role, args[1].(Object).Role), nil), nil
	}
	// reads of one actor that have no stat formula behind them
	actorRead := func(op int, ref bool) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			name := ""
			switch {
			case ref && len(args) == 2 && isObj(args[0]):
				r, ok := args[1].(Ref)
				if !ok {
					return nil, fmt.Errorf("takes an actor and a record reference")
				}
				name = strconv.FormatInt(r.Guid, 10)
			case !ref && len(args) == 1 && isObj(args[0]):
			default:
				return nil, fmt.Errorf("takes the wrong arguments")
			}
			return ctx.HostFn(RoleFn(args[0].(Object).Role, op), name, nil), nil
		}
	}
	env.Funcs["GetHealthPercentage"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObj(args[0]) {
			return nil, fmt.Errorf("takes an actor")
		}
		c := contextOf(ctx, args[0].(Object))
		health, err := c.Stat(Ref{Kind: "Stat", Name: "Health"})
		if err != nil {
			return nil, err
		}
		max, err := c.Stat(Ref{Kind: "Stat", Name: "MaxHealth"})
		if err != nil {
			return nil, err
		}
		if max == 0 {
			return 0.0, nil // the compiled engine counts a fault and gives 0
		}
		return health / max, nil
	}
	env.Funcs["HasAlignment"] = func(args []Value) (Value, error) {
		v, err := actorRead(RoleOpHasAlignment, true)(args)
		if err != nil {
			return nil, err
		}
		return v.(float64) != 0, nil
	}
	env.Funcs["GetEffectRemainingDuration"] = actorRead(RoleOpEffectRemaining, true)
	env.Funcs["GetHitTriggerCount"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObj(args[0]) {
			return nil, fmt.Errorf("takes an actor and an ability")
		}
		r, ok := args[1].(Ref)
		if !ok {
			return nil, fmt.Errorf("takes an actor and an ability")
		}
		return ctx.HostFn(HostHitTriggerCount, strconv.FormatInt(r.Guid, 10), []float64{float64(args[0].(Object).Role)}), nil
	}
	env.Funcs["GetAbilityHitCount"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObj(args[0]) {
			return nil, fmt.Errorf("takes an actor and an ability")
		}
		r, ok := args[1].(Ref)
		if !ok {
			return nil, fmt.Errorf("takes an actor and an ability")
		}
		return ctx.HostFn(HostAbilityHitCount, strconv.FormatInt(r.Guid, 10), []float64{float64(args[0].(Object).Role)}), nil
	}
	env.Funcs["Now"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostNow, "", nil), nil
	}
	env.Funcs["GetProgressDuration"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		g, ok := args[0].(string)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s", describeValue(args[0]))
		}
		return ctx.HostFn(HostProgressDuration, g, nil), nil
	}
	// the damage formulas: the host works them out from the instigator's and the target's stats and the hit in progress
	for i, name := range DamageFnNames {
		fn := HostCalcDamage + i
		env.Funcs[name] = func(args []Value) (Value, error) {
			if len(args) != 0 {
				return nil, fmt.Errorf("takes no arguments, got %d", len(args))
			}
			return ctx.HostFn(fn, "", nil), nil
		}
	}
	addNodeFuncs(env, ctx)
	addGatherFuncs(env, ctx)
	return env
}

// CoA : C.Y.B

// addGatherFuncs adds the gathering reads (Gathering stage 2) to an ability environment: the host answers them (hostfn.go, HostArtisanStat ...).
func addGatherFuncs(env *Env, ctx AbilityContext) {
	argc := func(args []Value, n int) error {
		if len(args) != n {
			return fmt.Errorf("takes %d argument(s), got %d", n, len(args))
		}
		return nil
	}
	env.Funcs["GetArtisanshipModifiedStat"] = func(args []Value) (Value, error) {
		if err := argc(args, 2); err != nil {
			return nil, err
		}
		o, ok := args[0].(Object)
		r, ok2 := args[1].(Ref)
		if !ok || !ok2 {
			return nil, fmt.Errorf("takes an actor and a stat")
		}
		return ctx.HostFn(HostArtisanStat, fmt.Sprint(r.Guid), []float64{float64(o.Role)}), nil
	}
	for name, fn := range map[string]int{"GetRolledItemQuality": HostRolledItemQuality, "GetItemRarity": HostItemRarity, "GetSourceCertificationTier": HostSourceCertTier,
		"GetGatheringInteractionBaseDuration": HostGatherBaseSecs} {
		fn := fn
		env.Funcs[name] = func(args []Value) (Value, error) {
			if err := argc(args, 0); err != nil {
				return nil, err
			}
			return ctx.HostFn(fn, "", nil), nil
		}
	}
}

// [C O A :: C:Y:B]

// addNodeFuncs adds the city node reads (Nodes 1, stage 1) to an ability environment: the host answers them (hostfn.go, HostNodeBiome ...), and a
// node value is the reference of its record, which the context's NodeRanker turns from and to the host's rank.
func addNodeFuncs(env *Env, ctx AbilityContext) {
	ranker, _ := ctx.(NodeRanker)
	argc := func(args []Value, n int) error {
		if len(args) != n {
			return fmt.Errorf("takes %d argument(s), got %d", n, len(args))
		}
		return nil
	}
	rankOf := func(v Value) (int, error) {
		r, ok := v.(Ref)
		if !ok || (r.Kind != KindCityNode && r.TypeID != TypeCityNode) {
			return 0, fmt.Errorf("argument is %s, not a city node", describeValue(v))
		}
		if ranker == nil {
			return 0, fmt.Errorf("the context has no city nodes")
		}
		rank, ok := ranker.NodeRank(r.Guid, r.Name)
		if !ok {
			return 0, fmt.Errorf("%v is no city node", r)
		}
		return rank, nil
	}
	nodeValue := func(rank int) (Value, error) {
		if ranker == nil {
			return nil, fmt.Errorf("the context has no city nodes")
		}
		g, ok := ranker.NodeGuid(rank)
		if !ok {
			return nil, fmt.Errorf("the host answered node %d, which is no city node", rank)
		}
		return Ref{TypeID: TypeCityNode, Guid: g}, nil
	}
	enumValue := func(typ string, ord float64) (Value, error) {
		name, ok := EnumName(typ, int(ord))
		if !ok || float64(int(ord)) != ord {
			return nil, fmt.Errorf("the host answered %v, which is no %s", ord, typ)
		}
		return Enum{Type: typ, Value: name}, nil
	}
	player := func(args []Value, i int) error {
		if o, ok := args[i].(Object); !ok || o.Role != RolePlayer {
			return fmt.Errorf("argument %d is %s, not GetPlayer()", i+1, describeValue(args[i]))
		}
		return nil
	}
	// the player: the character the expression is evaluated for (a looting player, a vendor's customer)
	for _, name := range []string{"GetPlayer", "GetPlayerGuid"} {
		kind := map[string]string{"GetPlayer": "player", "GetPlayerGuid": "playerguid"}[name]
		env.Funcs[name] = func(args []Value) (Value, error) {
			if err := argc(args, 0); err != nil {
				return nil, err
			}
			return Object{Kind: kind, Role: RolePlayer}, nil
		}
	}
	env.Funcs["GetNodeBiome"] = func(args []Value) (Value, error) {
		if err := argc(args, 0); err != nil {
			return nil, err
		}
		return enumValue("EBiomeType", ctx.HostFn(HostNodeBiome, "", nil))
	}
	env.Funcs["GetNodeRecord"] = func(args []Value) (Value, error) {
		if err := argc(args, 0); err != nil {
			return nil, err
		}
		return nodeValue(int(ctx.HostFn(HostNodeRecord, "", nil)))
	}
	env.Funcs["GetCurrentNode"] = func(args []Value) (Value, error) {
		if err := argc(args, 1); err != nil {
			return nil, err
		}
		o, ok := args[0].(Object)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s, not an actor", describeValue(args[0]))
		}
		who := float64(o.Role)
		if o.Role == RolePlayer {
			who = ActorPlayer
		}
		return nodeValue(int(ctx.HostFn(HostCurrentNode, "", []float64{who})))
	}
	env.Funcs["Node_GetNodeType"] = func(args []Value) (Value, error) {
		if err := argc(args, 1); err != nil {
			return nil, err
		}
		rank, err := rankOf(args[0])
		if err != nil {
			return nil, err
		}
		return enumValue("ENodeType", ctx.HostFn(HostNodeType, "", []float64{float64(rank)}))
	}
	env.Funcs["Node_GetLevel"] = func(args []Value) (Value, error) {
		if err := argc(args, 1); err != nil {
			return nil, err
		}
		rank, err := rankOf(args[0])
		if err != nil {
			return nil, err
		}
		return enumValue("ENodeLevel", ctx.HostFn(HostNodeLevel, "", []float64{float64(rank)}))
	}
	env.Funcs["Node_InLevelRangeInclusive"] = func(args []Value) (Value, error) {
		if err := argc(args, 3); err != nil {
			return nil, err
		}
		rank, err := rankOf(args[0])
		if err != nil {
			return nil, err
		}
		var bound [2]float64
		for i := 0; i < 2; i++ {
			e, ok := args[1+i].(Enum)
			ord, known := EnumOrdinal(e.Type, e.Value)
			if !ok || e.Type != "ENodeLevel" || !known {
				return nil, fmt.Errorf("argument %d is %s, not an ENodeLevel", 2+i, describeValue(args[1+i]))
			}
			bound[i] = float64(ord)
		}
		level := ctx.HostFn(HostNodeLevel, "", []float64{float64(rank)})
		return level >= bound[0] && level <= bound[1], nil
	}
	env.Funcs["Node_IsPlayerCitizen"] = func(args []Value) (Value, error) {
		if err := argc(args, 1); err != nil {
			return nil, err
		}
		if err := player(args, 0); err != nil {
			return nil, err
		}
		return ctx.HostFn(HostPlayerCitizen, "", nil) != 0, nil
	}
	env.Funcs["Node_IsPlayerCitizenOfNode"] = func(args []Value) (Value, error) {
		if err := argc(args, 2); err != nil {
			return nil, err
		}
		if err := player(args, 0); err != nil {
			return nil, err
		}
		rank, err := rankOf(args[1])
		if err != nil {
			return nil, err
		}
		return ctx.HostFn(HostPlayerCitizenOfNode, "", []float64{float64(rank)}) != 0, nil
	}
	env.Funcs["Node_IsPlayerMayor"] = func(args []Value) (Value, error) {
		if err := argc(args, 1); err != nil {
			return nil, err
		}
		if err := player(args, 0); err != nil {
			return nil, err
		}
		return ctx.HostFn(HostPlayerMayor, "", nil) != 0, nil
	}
	// EnumInt(e): the enum value as the number it is (the data compares levels with it: EnumInt(Node_GetLevel(n)) >= EnumInt(ENodeLevel::Village))
	env.Funcs["EnumInt"] = func(args []Value) (Value, error) {
		if err := argc(args, 1); err != nil {
			return nil, err
		}
		e, ok := args[0].(Enum)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s, not an enum value", describeValue(args[0]))
		}
		ord, ok := EnumOrdinal(e.Type, e.Value)
		if !ok {
			return nil, fmt.Errorf("%v is not an enum this evaluation knows", e)
		}
		return float64(ord), nil
	}
}
