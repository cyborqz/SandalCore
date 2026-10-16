// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"strconv"

	"aocrop/services/internal/aocexpr"
)

// The gathering reads of an ability expression (Gathering stage 2, "Gathering data"; contentc version 13): GetArtisanshipModifiedStat,
// GetRolledItemQuality, GetItemRarity, GetSourceCertificationTier and GetGatheringInteractionBaseDuration, which the Artisan equations of the data
// (the speed, rarity, quantity and experience of a harvest, the ItemVariations' Quality formulas and the Profession experience rewards) call.
//
// They are host functions (aocexpr/hostfn.go HostArtisanStat ... HostGatherBaseSecs, the engine's OpHost0..2) over numbers, like the city node reads:
// the host fills a plain gathering context and answers from it. The equations compare the quality, the rarity and the tier with whole numbers
// (`rarity == 3`, `recipeCert >= 2`), so those three answers are whole numbers; the rating and the duration are floats.

// gatherCall compiles a gathering read; ok is false when e is not one.
func (f *fnc) gatherCall(e aocexpr.Call, args []val) (v val, ok bool, err error) {
	switch e.Name {
	case "GetArtisanshipModifiedStat", "GetRolledItemQuality", "GetItemRarity", "GetSourceCertificationTier", "GetGatheringInteractionBaseDuration":
	default:
		return val{}, false, nil
	}
	v, err = f.gatherRead(e, args)
	return v, true, err
}

func (f *fnc) gatherRead(e aocexpr.Call, args []val) (val, error) {
	if e.Name == "GetArtisanshipModifiedStat" {
		if err := argc(e, 2); err != nil {
			return val{}, err
		}
		if args[0].t != tObj {
			return val{}, errAt(e.Args[0].Pos(), "%s: argument 1 is %v, not an actor", e.Name, args[0].t)
		}
		if args[1].t != tRef {
			return val{}, errAt(e.Args[1].Pos(), "%s: argument 2 is %v, not a stat", e.Name, args[1].t)
		}
		if args[1].ref.TypeID != TypeStatDef {
			return val{}, errAt(e.Args[1].Pos(), "%s: %v is not a stat", e.Name, args[1].ref)
		}
		role := val{t: tNum, n: nConst{float64(args[0].role)}}
		v, err := f.hostCall(e, aocexpr.HostArtisanStat, strconv.FormatInt(args[1].ref.Guid, 10), tNum, role)
		v.float = true
		return v, err
	}
	if err := argc(e, 0); err != nil {
		return val{}, err
	}
	fn, whole := 0, true
	switch e.Name {
	case "GetRolledItemQuality":
		fn = aocexpr.HostRolledItemQuality
	case "GetItemRarity":
		fn = aocexpr.HostItemRarity
	case "GetSourceCertificationTier":
		fn = aocexpr.HostSourceCertTier
	default: // GetGatheringInteractionBaseDuration: seconds
		fn, whole = aocexpr.HostGatherBaseSecs, false
	}
	v, err := f.hostCall(e, fn, "", tNum)
	v.float = !whole
	return v, err
}

// GatherContext is the host's model of a harvest behind the gathering reads, and the reference for the C++ host (worldd's harvest host answers from
// the same struct, server/worldd/stats/gather_host.h). The shared corpus writes `gatherhost` blocks from it and the C++ test checks its own host
// against the same answers.
//
//	GetArtisanshipModifiedStat(actor, stat)     the rating of the stat for the caster (Ratings, by the stat's guid); 0 for a stat with none and for any other actor
//	GetRolledItemQuality()                      Quality: what the harvest's rarity equation rolled (0 before one ran)
//	GetItemRarity()                             ItemRarity: the EItemRarity ordinal of the harvest's item
//	GetSourceCertificationTier()                SourceTier: the CertificationTier of the interaction (0 Novice ... 4 GrandMaster)
//	GetGatheringInteractionBaseDuration()       BaseDuration: the interaction's BaseGatheringDuration, seconds
type GatherContext struct {
	Quality      float64
	ItemRarity   int
	SourceTier   int
	BaseDuration float64
	Ratings      map[int64]float64
}

// [Cyb + C O A]

// HostFn answers a gathering read; ok is false for a function that is no gathering read. name is the stat's guid in decimal for HostArtisanStat, and
// args[0] the role of the actor.
func (c *GatherContext) HostFn(fn int, name string, args []float64) (v float64, ok bool) {
	switch fn {
	case aocexpr.HostArtisanStat:
		if len(args) >= 1 && int(args[0]) != aocexpr.RoleCaster {
			return 0, true // only the instigator, the harvesting player, has artisanship ratings
		}
		g, err := strconv.ParseInt(name, 10, 64)
		if err != nil {
			return 0, true
		}
		return c.Ratings[g], true
	case aocexpr.HostRolledItemQuality:
		return c.Quality, true
	case aocexpr.HostItemRarity:
		return float64(c.ItemRarity), true
	case aocexpr.HostSourceCertTier:
		return float64(c.SourceTier), true
	case aocexpr.HostGatherBaseSecs:
		return c.BaseDuration, true
	}
	return 0, false
}

// GatherHostQuery is a read of a GatherContext: the function, the name operand ("" for none), the role it is given (HostArtisanStat only) and the
// answer.
type GatherHostQuery struct {
	Fn   int
	Name string
	Role int
	Want float64
}

// GatherHostCase is a GatherContext with the reads the host must answer.
type GatherHostCase struct {
	Name    string
	Ctx     GatherContext
	Queries []GatherHostQuery
}

func (q GatherHostQuery) args() []float64 {
	if q.Fn == aocexpr.HostArtisanStat {
		return []float64{float64(q.Role)}
	}
	return nil
}

// check runs the queries against the model.
func (c GatherHostCase) check() error {
	for i, q := range c.Queries {
		got, ok := c.Ctx.HostFn(q.Fn, q.Name, q.args())
		if !ok || got != q.Want {
			return fmt.Errorf("gatherhost %s: query %d (fn %d %q role %d) = %v, want %v", c.Name, i, q.Fn, q.Name, q.Role, got, q.Want)
		}
	}
	return nil
}

// [C | Y | B :: C.O.A]

// gatherHostCases are the harvests of the corpus: a Novice harvest at rating 0, an Apprentice one with a rolled Uncommon and ratings (the speed
// curve's 215 and the top of the rating range 8409), a Journeyman plant with fractional ratings, and a harvest with nothing set. Every function is
// asked with a stat the context has, one it does not, and for another actor than the player.
func gatherHostCases() []GatherHostCase {
	rated := []struct {
		name string
		ctx  GatherContext
	}{
		{"gather_novice_rating_0", GatherContext{Quality: 1, ItemRarity: 0, SourceTier: 0, BaseDuration: 15, Ratings: map[int64]float64{}}},
		{"gather_apprentice_uncommon", GatherContext{Quality: 6, ItemRarity: 1, SourceTier: 1, BaseDuration: 11.25, Ratings: map[int64]float64{6064629302956261784: 215, 6064629302956327330: 8409}}},
		{"gather_journeyman_fractions", GatherContext{Quality: 31, ItemRarity: 3, SourceTier: 2, BaseDuration: 7.5, Ratings: map[int64]float64{6064629302956261784: 316.5, 6064629302956327336: 0.25}}},
		{"gather_nothing_set", GatherContext{}},
	}
	var out []GatherHostCase
	for _, r := range rated {
		c := GatherHostCase{Name: r.name, Ctx: r.ctx}
		ask := func(fn int, name string, role int) {
			v, _ := c.Ctx.HostFn(fn, name, GatherHostQuery{Fn: fn, Role: role}.args())
			c.Queries = append(c.Queries, GatherHostQuery{Fn: fn, Name: name, Role: role, Want: v})
		}
		for _, stat := range []string{"6064629302956261784", "6064629302956327330", "6064629302956327336", "424242", ""} {
			ask(aocexpr.HostArtisanStat, stat, aocexpr.RoleCaster)
		}
		ask(aocexpr.HostArtisanStat, "6064629302956261784", aocexpr.RoleTarget)
		for _, fn := range []int{aocexpr.HostRolledItemQuality, aocexpr.HostItemRarity, aocexpr.HostSourceCertTier, aocexpr.HostGatherBaseSecs} {
			ask(fn, "", 0)
		}
		out = append(out, c)
	}
	return out
}

// parseLine reads a line of a gatherhost block: the harvest's fields and the reads (see corpus.go).
func (c *GatherHostCase) parseLine(f []string) error {
	hexFloat := func(s string) (float64, error) {
		if len(s) != 16 {
			return 0, fmt.Errorf("%q is not 16 hex digits", s)
		}
		u, err := strconv.ParseUint(s, 16, 64)
		if err != nil {
			return 0, fmt.Errorf("%q is not hex", s)
		}
		return math.Float64frombits(u), nil
	}
	whole := func(s string) (int, error) {
		n, err := strconv.Atoi(s)
		if err != nil || n < 0 {
			return 0, fmt.Errorf("%q is no whole number", s)
		}
		return n, nil
	}
	var err error
	switch f[0] {
	case "quality", "duration":
		if len(f) != 2 {
			return fmt.Errorf("%s F64", f[0])
		}
		var v float64
		if v, err = hexFloat(f[1]); err != nil {
			return err
		}
		if f[0] == "quality" {
			c.Ctx.Quality = v
		} else {
			c.Ctx.BaseDuration = v
		}
	case "rarity", "tier":
		if len(f) != 2 {
			return fmt.Errorf("%s N", f[0])
		}
		var n int
		if n, err = whole(f[1]); err != nil {
			return err
		}
		if f[0] == "rarity" {
			c.Ctx.ItemRarity = n
		} else {
			c.Ctx.SourceTier = n
		}
	case "rating":
		if len(f) != 3 {
			return fmt.Errorf("rating STATGUID F64")
		}
		g, err := strconv.ParseInt(f[1], 10, 64)
		if err != nil {
			return fmt.Errorf("rating: %q is no stat guid", f[1])
		}
		v, err := hexFloat(f[2])
		if err != nil {
			return err
		}
		if c.Ctx.Ratings == nil {
			c.Ctx.Ratings = map[int64]float64{}
		}
		c.Ctx.Ratings[g] = v
	case "q":
		if len(f) != 6 || f[4] != "=" {
			return fmt.Errorf("q FN NAME ROLE = F64")
		}
		fn, err := strconv.Atoi(f[1])
		if err != nil || fn < aocexpr.HostGatherFirst || fn > aocexpr.HostGatherLast {
			return fmt.Errorf("q: %q is no gathering read (%d to %d)", f[1], aocexpr.HostGatherFirst, aocexpr.HostGatherLast)
		}
		name := f[2]
		if name == "-" {
			name = ""
		}
		role, err := whole(f[3])
		if err != nil {
			return err
		}
		want, err := hexFloat(f[5])
		if err != nil {
			return err
		}
		c.Queries = append(c.Queries, GatherHostQuery{Fn: fn, Name: name, Role: role, Want: want})
	}
	return nil
}
