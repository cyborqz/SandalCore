// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import "aocrop/services/internal/aocexpr"

// TableState is an aocexpr.State read from tables: a character with some effects,
// skills, flags and so on, for tests and the shared corpus (which gives the C++
// engine the same state). Every field left empty is NoState's value.
type TableState struct {
	Effects                                map[aocexpr.Ref]float64 // stacks by effect
	Tags                                   map[string]float64      // effects by tag
	Skills                                 map[[2]aocexpr.Ref]bool // bought skills by (class, skill)
	FlagCombat, FlagMoving, FlagUnkillable bool
	Actor                                  float64
	Professions                            map[string]float64 // levels; a profession not named is level 1
	Consumed                               map[aocexpr.Ref]float64
	EliteRank                              float64
	// NpcDist is an NPC's share of its stat budget by EStatDistributionStat (aocexpr.StatDistributionIndex), the weights over their sum.
	NpcDist [8]float64
}

func (s *TableState) EffectCount(r aocexpr.Ref) float64    { return s.Effects[r] }
func (s *TableState) EffectCountByTag(t string) float64    { return s.Tags[t] }
func (s *TableState) SkillPurchased(c, k aocexpr.Ref) bool { return s.Skills[[2]aocexpr.Ref{c, k}] }
func (s *TableState) InCombat() bool                       { return s.FlagCombat }
func (s *TableState) Moving() bool                         { return s.FlagMoving }
func (s *TableState) Unkillable() bool                     { return s.FlagUnkillable }
func (s *TableState) EquippedItemInSlotHasTag(aocexpr.Enum, string) bool {
	return false
}
func (s *TableState) ActorGuid() float64 { return s.Actor }
func (s *TableState) ProfessionLevel(_ float64, p string) float64 {
	if v, ok := s.Professions[p]; ok {
		return v
	}
	return 1
}
func (s *TableState) ConsumedItemStat(r aocexpr.Ref) float64 { return s.Consumed[r] }
func (s *TableState) NpcEliteRank() float64                  { return s.EliteRank }
func (s *TableState) NpcStatDistribution(i int) float64 {
	if i < 0 || i >= len(s.NpcDist) {
		return 0
	}
	return s.NpcDist[i]
}
