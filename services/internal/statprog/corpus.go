// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bufio"
	"bytes"
	"embed"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"io"
	"math"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
)

// The shared corpus: compiled bundles, the inputs of characters, and what the
// engine must give for them, in one text file that the Go tests and the C++ test
// (server/worldd/stats/tests) both read. The Go side writes it (BuildCorpus,
// `statprog corpus`) and `statprog corpus --check` fails when the file on disk
// is not what the current compiler and engine produce.
//
// Lines (a number is decimal unless it says hex; a float is the hex of its
// IEEE bits, so nothing depends on a printer):
//
//	bundle NAME LEN             base64 lines of the bundle's LEN bytes follow, then "endbundle"
//	derive NAME OF double       a bundle made of another by switching it to ModeDouble (the header flag)
//	case NAME BUNDLE CLASS LEVEL  one character, up to "end":
//	  mod STATGUID F64              an equipment contribution
//	  tag SLOT TAG                  an equipped item's gameplay tag
//	  rounding none|halfup, item RECORD QUALITY SLOT
//	                                (plan B3) the worn items the mods and tags above were made from, when a case is made from gear,
//	                                and how their values were rounded: a reader that has the items' tables (worldd's C++ with
//	                                content.db) rebuilds the mods and tags from them and must get these; the replay ignores them
//	  state effect TYPEID GUID F64 | tag TAG F64 | skill T1 G1 T2 G2 | combat | moving | unkillable |
//	        actor F64 | prof NAME F64 | consumed TYPEID GUID F64 | npc F64 | dist INDEX F64     (TableState; dist: an NPC's stat distribution)
//	  want STATGUID F64             the value of a stat of the class (every one, in stat order)
//	  digest HEX16 N FAULTS IGNORED an FNV-1a over every output (value, base, equipment, regeneration)
//	  cap STATGUID F32 exact|near|miss   how the value compares with a captured float32
//	fxcase NAME BUNDLE CLASS LEVEL  (plan D2) a case that also has effects and ranks, and expressions to evaluate; it holds the
//	                              lines of a case and, in this order:
//	  fx TYPEID GUID STACKS         an active effect; fxtag TAG and fxmod STATGUID F64 (the value of one stack) belong to the last fx
//	  rank T1 G1 T2 G2              a skill rank held (GetSkillPurchased)
//	  actor ROLE CLASS LEVEL        (plan E1, stage 3) another actor of the hit the expressions run for (aocexpr.RoleTarget ...): its own
//	                                inputs, up to "endactor": the lines of a case (mod, tag, state, fx, fxtag, fxmod, rank); the programs
//	                                that read it (GetStat(GetTarget(), ...), OpActor) run in its context, a role with no actor reads as the caster
//	  host seed N | progress GROUP F64 | inst TYPEID GUID F64 | hittag TAG | hitflag NAME | targets F64 | xpdebt F64 | fn FN NAME F64
//	                                (fn: a named host function's answer, NAME "-" for none: the numbers it is given are added to it)
//	                                the host of the expressions: the SplitMix seed, GetAbilityProgressRatio(group),
//	                                GetEffectCountByRecordAndInstigator(record) and the hit in progress (its tags, its flags, its
//	                                target count) and the experience debt; the random source runs on through the prog lines
//	  prog INDEX F64                the value of the expression program INDEX, run against the stats of the run, in line order
//	  retail prog INDEX F64         a number of the retail captures the program INDEX (evaluated above) must give exactly
//	  retail stat GUID F64 DIGITS   a number of the retail captures the stat must have, rounded to DIGITS decimals (a retail value is printed
//	                                with four: Mitigation 0.9362, HealingMult 1.1576, MoveSpeedMult 1.3)
//	  ignoredfx N, evalfaults N     the effect mods that named no stat, and the faults of the evaluations
//	                              (the digest line holds the run's counts as for a case)
//	dprofile / dview / damage     the damage formulas (corpus_damage.go)
//	nodehost NAME                 (Nodes 1, stage 1) the city node reads of a world, up to "end" (all whole numbers, decimal):
//	  nodes N                         the registry has N nodes; node RANK TYPE LEVEL (the ENodeType and ENodeLevel ordinals) follows for each, in rank order
//	  ctx NODE BIOME ACTOR PLAYER     the evaluation's context: its node's rank (-1: none) and its biome, and the node of the expression's own actor and of the player
//	  q FN ARGS... = WANT             a read of the host: the function (aocexpr.HostNodeBiome ...), 0 to 2 arguments and the answer
//	refuse NAME BUNDLE CLASS LEVEL  running the character must fail (a valid bundle that asks too much)
//	fuzz BUNDLE SEED N ACCEPTED HASH  damage the bundle N times (FuzzParity): the count accepted and the hash
//	                              of every verdict and result must be these
//	hostile NAME FIX POS DEL HEX  the blob made of the bundle "tiny" by replacing DEL bytes at POS by the
//	                              hex bytes ("-" for none); FIX 1 then recomputes the header's length and crc;
//	                              FIX 2: HEX is the whole blob (POS and DEL are 0: a break that changes the
//	                              bundle in many places, such as a host operation in a stat formula, which needs
//	                              a program appended to the table and a stat that names it).
//	                              A reader must refuse it.
//
// The corpus is for the engine's output, not for the capture: the capture's own
// tests are in golden_test.go; the cap lines let the C++ test prove the same
// match against the capture without the content database.
// CoA : C.Y.B

//go:embed testdata/bard_l1.json testdata/ranger_l20.json
var captures embed.FS

// Corpus is a parsed corpus file.
type Corpus struct {
	Bundles  map[string][]byte
	Derived  []DerivedBundle
	Cases    []CorpusCase
	Hostiles []CorpusHostile
	Refusals []CorpusRefusal
	Fuzzes   []CorpusFuzz
	Profiles []CorpusProfile
	Views    []CorpusView
	Damages  []CorpusDamage
	// NodeHosts are the worlds the city node reads are checked against (Nodes 1, stage 1): the block `nodehost` below.
	NodeHosts []NodeHostCase
}

// CorpusFuzz is a differential fuzz: damage a bundle N times from Seed (FuzzParity); the readers must
// accept Accepted of them and fold their verdicts to Hash.
type CorpusFuzz struct {
	Bundle   string
	Seed     uint64
	N        int
	Accepted int
	Hash     uint64
}

// CorpusRefusal is a character the engine must refuse to compute (a bundle that is valid but asks too much).
type CorpusRefusal struct {
	Name, Bundle string
	Class, Level int
}

// DerivedBundle is a bundle switched to ModeDouble.
type DerivedBundle struct{ Name, Of string }

// CorpusCase is one character and its expected stats.
type CorpusCase struct {
	Name, Bundle string
	In           Inputs
	Gear         *CorpusGear // the items the inputs were made from, when they were
	State        *TableState
	Want         []CorpusWant
	Digest       uint64
	Members      int
	Faults       int
	Ignored      int
	Caps         []CorpusCap
	Fx           *CorpusFx // set for an fxcase
	Actors       []CorpusActor
}

// CorpusActor is another actor of an fxcase's hit: the inputs and state it is run with, by role.
type CorpusActor struct {
	Role  int
	In    Inputs
	State *TableState
}

// setActors runs the actors with the bundle and hands them to the evaluator.
func setActors(b *Bundle, ev *Evaluator, actors []CorpusActor) error {
	for _, a := range actors {
		var st aocexpr.State
		if a.State != nil {
			st = a.State
		}
		res, err := b.Run(a.In, st)
		if err != nil {
			return fmt.Errorf("actor %d: %w", a.Role, err)
		}
		if err := ev.SetActor(a.Role, res, a.In, st); err != nil {
			return err
		}
	}
	return nil
}

// CorpusFx is what an fxcase adds to a case: the host of its expressions and what they must give (the effects and ranks are
// in the case's Inputs).
type CorpusFx struct {
	Host       TableHostSpec
	Progs      []CorpusProg
	Retail     []CorpusRetail
	IgnoredFx  int
	EvalFaults int
}

// CorpusRetail is a number of the retail captures an fxcase must reproduce: an expression program's value exactly (Kind "prog", ID the program)
// or a stat's value rounded to Digits decimals (Kind "stat", ID the stat guid).
type CorpusRetail struct {
	Kind   string
	ID     int64
	Value  float64
	Digits int
}

// roundTo rounds x to digits decimals (halves away from zero).
func roundTo(x float64, digits int) float64 {
	p := math.Pow(10, float64(digits))
	return math.Round(x*p) / p
}

// check says whether the retail number holds, given the value of the program or the stat.
func (r CorpusRetail) holds(got float64) bool {
	if r.Kind == "stat" {
		return roundTo(got, r.Digits) == r.Value
	}
	return got == r.Value
}

// TableHostSpec makes the TableHost of a case: its random seed, the progress of each group and the effects by instigator.
type TableHostSpec struct {
	Seed        uint64
	Progress    map[string]float64
	Inst        map[aocexpr.Ref]float64
	Tags, Flags map[string]bool // the hit in progress's gameplay tags and EHitFlag names
	Targets     float64
	XPDebt      float64
	Fns         map[string]float64 // HostFn answers by "FN|NAME"
}

// Host is the host a replay runs the expressions with.
func (h TableHostSpec) Host() *TableHost {
	return &TableHost{Progress: h.Progress, ByInstigator: h.Inst, Rand: SplitMix{State: h.Seed}, Tags: h.Tags, Flags: h.Flags, Targets: h.Targets, XPDebt: h.XPDebt, Fns: h.Fns}
}

// CorpusProg is the expected value of an expression program.
type CorpusProg struct {
	Index int
	Value uint64 // float64 bits
}

// CorpusGear is the worn items a case's mods and tags come from, and the rounding of their values.
type CorpusGear struct {
	Rounding Rounding
	Items    []Item
}

// CorpusWant is the expected value of a stat.
type CorpusWant struct {
	Guid  int64
	Value uint64 // float64 bits
}

// (Cyb ~ C O A)

// CorpusCap is how a value compares with a captured float32.
type CorpusCap struct {
	Guid  int64
	Bits  uint32
	Match string // exact, near or miss
}

// CorpusHostile is a blob a reader must refuse, as an edit of the "tiny" bundle.
type CorpusHostile struct {
	Name  string
	Fix   bool
	Blob  bool // the line gives the whole blob (a break that changes the bundle in many places, like a program appended to the table)
	Pos   int
	Del   int
	Ins   []byte
	Whole []byte // set by Replay: the blob made
}

// Digest is the FNV-1a 64 of a result: for every stat in order the guid, the value, the base, the
// equipment's contribution and the regeneration (as float64 bits; a stat without one counts a 0 byte).
func Digest(res *Result) uint64 {
	h := uint64(14695981039346656037)
	put := func(b []byte) {
		for _, c := range b {
			h ^= uint64(c)
			h *= 1099511628211
		}
	}
	var buf [8]byte
	u64 := func(v uint64) {
		binary.LittleEndian.PutUint64(buf[:], v)
		put(buf[:])
	}
	for _, s := range res.Stats {
		u64(uint64(s.Guid))
		u64(math.Float64bits(s.Value))
		u64(math.Float64bits(s.Base))
		u64(math.Float64bits(s.Equip))
		if s.HasRegen {
			put([]byte{1})
			u64(math.Float64bits(s.Regen))
		} else {
			put([]byte{0})
		}
	}
	return h
}

// ---- writing ----

type corpusWriter struct{ bytes.Buffer }

func (w *corpusWriter) line(format string, args ...any) { fmt.Fprintf(w, format+"\n", args...) }

func (w *corpusWriter) bundle(name string, enc []byte) {
	w.line("bundle %s %d", name, len(enc))
	s := base64.StdEncoding.EncodeToString(enc)
	for len(s) > 0 {
		n := min(len(s), 100)
		w.line("%s", s[:n])
		s = s[n:]
	}
	w.line("endbundle")
}

func f64hex(f float64) string { return fmt.Sprintf("%016x", math.Float64bits(f)) }

// Cyb : C-O-A

// caseOf writes a case: it runs the bundle (as read back from its bytes, as a reader would have
// it) and writes the inputs, every value and the digest. caps, when given, are the captured stats.
func (w *corpusWriter) caseOf(name, bundleName string, b *Bundle, in Inputs, st *TableState, caps []CorpusCap, full bool, gear ...CorpusGear) error {
	var state aocexpr.State
	if st != nil {
		state = st
	}
	res, err := b.Run(in, state)
	if err != nil {
		return fmt.Errorf("case %s: %w", name, err)
	}
	w.line("case %s %s %d %d", name, bundleName, in.Class, in.Level)
	for _, m := range in.Mods {
		w.line("mod %d %s", m.Stat, f64hex(m.Value))
	}
	for _, t := range in.Equipped {
		w.line("tag %s %s", t.Slot, t.Tag)
	}
	for _, g := range gear {
		w.line("rounding %s", [...]string{"none", "halfup"}[g.Rounding])
		for _, it := range g.Items {
			w.line("item %d %d %s", it.Record, it.Quality, it.Slot)
		}
	}
	if st != nil {
		writeState(w, st)
	}
	if full {
		for _, s := range res.Stats {
			w.line("want %d %s", s.Guid, f64hex(s.Value))
		}
	}
	w.line("digest %016x %d %d %d", Digest(res), len(res.Stats), res.Faults, res.IgnoredMods)
	if caps != nil {
		// The captured stats with how this engine's value compares: filled by the caller (needs the values).
		for _, c := range caps {
			w.line("cap %d %08x %s", c.Guid, c.Bits, c.Match)
		}
	}
	w.line("end")
	return nil
}

// fxCaseOf writes an fxcase: the run with the inputs' effects and ranks, every stat's value when full, the digest, and the value of each
// expression program in progs against the stats, with a host made from spec.
func (w *corpusWriter) fxCaseOf(name, bundleName string, b *Bundle, in Inputs, st *TableState, full bool, spec TableHostSpec, progs []int, retail ...CorpusRetail) error {
	return w.fxCaseWith(name, bundleName, b, in, st, nil, full, spec, progs, retail...)
}

// fxCaseWith is fxCaseOf for a hit that has other actors.
func (w *corpusWriter) fxCaseWith(name, bundleName string, b *Bundle, in Inputs, st *TableState, actors []CorpusActor, full bool, spec TableHostSpec, progs []int, retail ...CorpusRetail) error {
	var state aocexpr.State
	if st != nil {
		state = st
	}
	res, err := b.Run(in, state)
	if err != nil {
		return fmt.Errorf("fxcase %s: %w", name, err)
	}
	w.line("fxcase %s %s %d %d", name, bundleName, in.Class, in.Level)
	for _, m := range in.Mods {
		w.line("mod %d %s", m.Stat, f64hex(m.Value))
	}
	for _, t := range in.Equipped {
		w.line("tag %s %s", t.Slot, t.Tag)
	}
	if st != nil {
		writeState(w, st)
	}
	for _, e := range in.Effects {
		w.line("fx %d %d %d", e.Effect.TypeID, e.Effect.Guid, e.Stacks)
		for _, t := range e.Tags {
			w.line("fxtag %s", t)
		}
		for _, m := range e.Mods {
			w.line("fxmod %d %s", m.Stat, f64hex(m.Value))
		}
	}
	for _, r := range in.Ranks {
		w.line("rank %d %d %d %d", r.Class.TypeID, r.Class.Guid, r.Skill.TypeID, r.Skill.Guid)
	}
	if full {
		for _, s := range res.Stats {
			w.line("want %d %s", s.Guid, f64hex(s.Value))
		}
	}
	w.line("digest %016x %d %d %d", Digest(res), len(res.Stats), res.Faults, res.IgnoredMods)
	w.line("ignoredfx %d", res.IgnoredEffectMods)
	for _, a := range actors {
		w.line("actor %d %d %d", a.Role, a.In.Class, a.In.Level)
		writeInputs(w, a.In, a.State)
		w.line("endactor")
	}
	if len(progs) > 0 {
		w.line("host seed %d", spec.Seed)
		var groups []string
		for g := range spec.Progress {
			groups = append(groups, g)
		}
		sort.Strings(groups)
		for _, g := range groups {
			name := g
			if name == "" {
				name = "-" // the form with no argument: the running ability
			}
			w.line("host progress %s %s", name, f64hex(spec.Progress[g]))
		}
		var refs []aocexpr.Ref
		for r := range spec.Inst {
			refs = append(refs, r)
		}
		sort.Slice(refs, func(i, j int) bool {
			if refs[i].TypeID != refs[j].TypeID {
				return refs[i].TypeID < refs[j].TypeID
			}
			return refs[i].Guid < refs[j].Guid
		})
		for _, r := range refs {
			w.line("host inst %d %d %s", r.TypeID, r.Guid, f64hex(spec.Inst[r]))
		}
		for _, k := range [...]struct {
			kind string
			set  map[string]bool
		}{{"hittag", spec.Tags}, {"hitflag", spec.Flags}} {
			var names []string
			for n, on := range k.set {
				if on {
					names = append(names, n)
				}
			}
			sort.Strings(names)
			for _, n := range names {
				w.line("host %s %s", k.kind, n)
			}
		}
		if spec.Targets != 0 {
			w.line("host targets %s", f64hex(spec.Targets))
		}
		if spec.XPDebt != 0 {
			w.line("host xpdebt %s", f64hex(spec.XPDebt))
		}
		var fkeys []string
		for k := range spec.Fns {
			fkeys = append(fkeys, k)
		}
		sort.Strings(fkeys)
		for _, k := range fkeys {
			fn, name, _ := strings.Cut(k, "|")
			if name == "" {
				name = "-"
			}
			w.line("host fn %s %s %s", fn, name, f64hex(spec.Fns[k]))
		}
		ev, err := b.NewEvaluator(res, in, state)
		if err != nil {
			return fmt.Errorf("fxcase %s: %w", name, err)
		}
		if err := setActors(b, ev, actors); err != nil {
			return fmt.Errorf("fxcase %s: %w", name, err)
		}
		host := spec.Host()
		got := map[int]float64{}
		for _, p := range progs {
			v, err := ev.Eval(p, host)
			if err != nil {
				return fmt.Errorf("fxcase %s: program %d: %w", name, p, err)
			}
			got[p] = v
			w.line("prog %d %s", p, f64hex(v))
		}
		w.line("evalfaults %d", ev.Faults())
		for _, r := range retail {
			if r.Kind != "prog" {
				continue
			}
			v, ok := got[int(r.ID)]
			if !ok || !r.holds(v) {
				return fmt.Errorf("fxcase %s: the retail value %v of program %d is %v here (evaluated: %v)", name, r.Value, r.ID, v, ok)
			}
			w.line("retail prog %d %s", r.ID, f64hex(r.Value))
		}
	}
	for _, r := range retail {
		if r.Kind != "stat" {
			continue
		}
		i, ok := b.StatIndex(r.ID)
		if !ok || !r.holds(res.Values[i]) {
			return fmt.Errorf("fxcase %s: the retail value %v of stat %d is %v here", name, r.Value, r.ID, res.Values[i])
		}
		w.line("retail stat %d %s %d", r.ID, f64hex(r.Value), r.Digits)
	}
	w.line("end")
	return nil
}

// writeInputs writes the lines of a character's inputs, state, effects and ranks.
func writeInputs(w *corpusWriter, in Inputs, st *TableState) {
	for _, m := range in.Mods {
		w.line("mod %d %s", m.Stat, f64hex(m.Value))
	}
	for _, t := range in.Equipped {
		w.line("tag %s %s", t.Slot, t.Tag)
	}
	if st != nil {
		writeState(w, st)
	}
	for _, e := range in.Effects {
		w.line("fx %d %d %d", e.Effect.TypeID, e.Effect.Guid, e.Stacks)
		for _, t := range e.Tags {
			w.line("fxtag %s", t)
		}
		for _, m := range e.Mods {
			w.line("fxmod %d %s", m.Stat, f64hex(m.Value))
		}
	}
	for _, r := range in.Ranks {
		w.line("rank %d %d %d %d", r.Class.TypeID, r.Class.Guid, r.Skill.TypeID, r.Skill.Guid)
	}
}

// [CoA + C | Y | B]

func writeState(w *corpusWriter, st *TableState) {
	refs := func(m map[aocexpr.Ref]float64) []aocexpr.Ref {
		var out []aocexpr.Ref
		for r := range m {
			out = append(out, r)
		}
		sort.Slice(out, func(i, j int) bool {
			if out[i].TypeID != out[j].TypeID {
				return out[i].TypeID < out[j].TypeID
			}
			return out[i].Guid < out[j].Guid
		})
		return out
	}
	for _, r := range refs(st.Effects) {
		w.line("state effect %d %d %s", r.TypeID, r.Guid, f64hex(st.Effects[r]))
	}
	var tags []string
	for t := range st.Tags {
		tags = append(tags, t)
	}
	sort.Strings(tags)
	for _, t := range tags {
		w.line("state tag %s %s", t, f64hex(st.Tags[t]))
	}
	var skills [][2]aocexpr.Ref
	for k, v := range st.Skills {
		if v {
			skills = append(skills, k)
		}
	}
	sort.Slice(skills, func(i, j int) bool {
		a, b := skills[i], skills[j]
		return fmt.Sprint(a) < fmt.Sprint(b)
	})
	for _, k := range skills {
		w.line("state skill %d %d %d %d", k[0].TypeID, k[0].Guid, k[1].TypeID, k[1].Guid)
	}
	if st.FlagCombat {
		w.line("state combat")
	}
	if st.FlagMoving {
		w.line("state moving")
	}
	if st.FlagUnkillable {
		w.line("state unkillable")
	}
	if st.Actor != 0 {
		w.line("state actor %s", f64hex(st.Actor))
	}
	var profs []string
	for p := range st.Professions {
		profs = append(profs, p)
	}
	sort.Strings(profs)
	for _, p := range profs {
		w.line("state prof %s %s", p, f64hex(st.Professions[p]))
	}
	for _, r := range refs(st.Consumed) {
		w.line("state consumed %d %d %s", r.TypeID, r.Guid, f64hex(st.Consumed[r]))
	}
	if st.EliteRank != 0 {
		w.line("state npc %s", f64hex(st.EliteRank))
	}
	for i, d := range st.NpcDist {
		if d != 0 {
			w.line("state dist %d %s", i, f64hex(d))
		}
	}
}

// diffEdit expresses blob as an edit of orig: the bytes between the common prefix and suffix.
func diffEdit(orig, blob []byte) (pos, del int, ins []byte) {
	p := 0
	for p < len(orig) && p < len(blob) && orig[p] == blob[p] {
		p++
	}
	s := 0
	for s < len(orig)-p && s < len(blob)-p && orig[len(orig)-1-s] == blob[len(blob)-1-s] {
		s++
	}
	return p, len(orig) - p - s, blob[p : len(blob)-s]
}

// ApplyEdit makes the blob of a hostile line from the bundle it edits.
func ApplyEdit(orig []byte, h CorpusHostile) ([]byte, bool) {
	if h.Blob {
		return append([]byte(nil), h.Ins...), true
	}
	if h.Pos < 0 || h.Del < 0 || h.Pos > len(orig) || h.Pos+h.Del > len(orig) {
		return nil, false
	}
	out := append(append(append([]byte(nil), orig[:h.Pos]...), h.Ins...), orig[h.Pos+h.Del:]...)
	if h.Fix && len(out) >= headerSize {
		binary.LittleEndian.PutUint32(out[8:], uint32(len(out)-headerSize))
		binary.LittleEndian.PutUint32(out[12:], crc32.ChecksumIEEE(out[headerSize:]))
	}
	return out, true
}

// maxWholeHostile is the largest blob a hostile line may carry whole.
const maxWholeHostile = 16384

// hostileLine writes a hostile blob as an edit of the tiny bundle; one that changes the bundle in many places
// (more than 512 inserted bytes) goes whole up to maxWholeHostile bytes, a bigger one is left to the Go tests.
func (w *corpusWriter) hostileLine(tiny []byte, h Hostile) bool {
	well := len(h.Blob) >= headerSize && int64(binary.LittleEndian.Uint32(h.Blob[8:])) == int64(len(h.Blob)-headerSize) &&
		binary.LittleEndian.Uint32(h.Blob[12:]) == crc32.ChecksumIEEE(h.Blob[headerSize:]) && bytes.Equal(h.Blob[:8], tiny[:8])
	var e CorpusHostile
	e.Name = strings.NewReplacer(" ", "_", ":", "").Replace(h.Name)
	if well {
		e.Fix = true
		p, d, ins := diffEdit(tiny[headerSize:], h.Blob[headerSize:])
		e.Pos, e.Del, e.Ins = headerSize+p, d, ins
	} else {
		e.Pos, e.Del, e.Ins = diffEdit(tiny, h.Blob)
	}
	if len(e.Ins) > 512 {
		// a break that changes the bundle in many places (a program appended to the table and a stat pointing at it): the line carries the whole blob
		if len(h.Blob) > maxWholeHostile {
			return false
		}
		e = CorpusHostile{Name: e.Name, Blob: true, Ins: append([]byte(nil), h.Blob...)}
	}
	if got, ok := ApplyEdit(tiny, e); !ok || !bytes.Equal(got, h.Blob) {
		return false
	}
	ins := "-"
	if len(e.Ins) > 0 {
		ins = hex.EncodeToString(e.Ins)
	}
	fix := 0
	if e.Fix {
		fix = 1
	}
	if e.Blob {
		fix = 2
	}
	w.line("hostile %s %d %d %d %s", e.Name, fix, e.Pos, e.Del, ins)
	return true
}

// C | Y | B and CoA

// BuildCorpus compiles the real graph of the content.db at dbPath and writes the corpus.
func BuildCorpus(dbPath string) ([]byte, error) {
	g, err := LoadContentDB(dbPath)
	if err != nil {
		return nil, err
	}
	gear, err := OpenGear(dbPath)
	if err != nil {
		return nil, err
	}
	defer gear.Close()

	exprs, exprIDs, err := LoadExprs(dbPath)
	if err != nil {
		return nil, err
	}
	main, mainRes, err := CompileExprs(g, Options{}, exprs)
	if err != nil {
		return nil, err
	}
	tinyF, tinyRes, err := TinyBundle(true, ModeFloat32)
	if err != nil {
		return nil, err
	}
	tinyS, tinySRes, err := TinyBundle(false, ModeSingle)
	if err != nil {
		return nil, err
	}
	// The cases run the bundles as a reader sees them.
	mainEnc, tinyEnc, tinySEnc := main.Encode(), tinyF.Encode(), tinyS.Encode()
	mustDecode := func(enc []byte) *Bundle {
		b, derr := Decode(enc)
		if derr != nil && err == nil {
			err = derr
		}
		return b
	}
	mainB, tinyB, tinySB := mustDecode(mainEnc), mustDecode(tinyEnc), mustDecode(tinySEnc)
	doubleEnc := append([]byte(nil), mainEnc...)
	binary.LittleEndian.PutUint16(doubleEnc[6:], FlagDouble)
	doubleB := mustDecode(doubleEnc)
	if err != nil {
		return nil, err
	}

	var w corpusWriter
	w.line("# statprog corpus v1, written by `go run ./internal/statprog/cmd/statprog corpus` (see internal/statprog/corpus.go).")
	w.line("# Do not edit: `corpus --check` fails when this file is not what the compiler and the engine produce.")
	w.bundle("main", mainEnc)
	w.line("derive main_double main double")
	w.bundle("tiny", tinyEnc)
	w.bundle("tiny_single", tinySEnc)
	w.bundle("abuse", AbuseBundle())
	w.line("refuse abuse_run abuse 1 1")

	classOf := classEnum
	kitItems := map[string][]Item{} // each class's kit, for the cases that were made from it
	kitInputs := func(class string, level int, rd Rounding) (Inputs, error) {
		kit, err := gear.StartingKit(class)
		if err != nil {
			return Inputs{}, err
		}
		kitItems[class] = kit
		mods, err := gear.Mods(kit, rd)
		if err != nil {
			return Inputs{}, err
		}
		tags, err := gear.Tags(kit)
		if err != nil {
			return Inputs{}, err
		}
		return Inputs{Class: classOf(class), Level: level, Mods: mods, Equipped: tags}, nil
	}

	// The captures: the engine's values and how they compare with the capture, in the default arithmetic
	// (every stat written) and in double (its digest only).
	for _, cp := range []struct {
		file, name string
		rd         Rounding
	}{{"testdata/bard_l1.json", "bard_l1", RoundNone}, {"testdata/ranger_l20.json", "ranger_l20", RoundHalfUp}} {
		raw, err := captures.ReadFile(cp.file)
		if err != nil {
			return nil, err
		}
		var c capture
		if err := json.Unmarshal(raw, &c); err != nil {
			return nil, err
		}
		items := c.gearItems()
		mods, err := gear.Mods(items, cp.rd)
		if err != nil {
			return nil, err
		}
		tags, err := gear.Tags(items)
		if err != nil {
			return nil, err
		}
		in := Inputs{Class: classOf(c.Class), Level: c.Level, Mods: mods, Equipped: tags}
		res, err := mainB.Run(in, nil)
		if err != nil {
			return nil, err
		}
		got := map[int64]float64{}
		for _, s := range res.Stats {
			got[s.Guid] = s.Value
		}
		var caps []CorpusCap
		for _, st := range c.Stats {
			m := [...]string{"exact", "near", "miss"}[compare(got[st.Guid], f32(st.Bits))]
			caps = append(caps, CorpusCap{st.Guid, st.Bits, m})
		}
		if err := w.caseOf(cp.name, "main", mainB, in, nil, caps, true, CorpusGear{Rounding: cp.rd, Items: items}); err != nil {
			return nil, err
		}
		if err := w.caseOf(cp.name+"_double", "main_double", doubleB, in, nil, nil, false); err != nil {
			return nil, err
		}
	}

	// The starting kit of each class at level 1 and 20; bare characters of other levels.
	for _, cl := range mainB.Classes {
		if int(cl.Enum) >= NpcClassBase {
			continue
		}
		cname := mainB.Strings[cl.Name]
		for _, level := range []int{1, 20} {
			in, err := kitInputs(cname, level, RoundNone)
			if err != nil {
				return nil, err
			}
			if err := w.caseOf(fmt.Sprintf("kit_%s_l%d", strings.ToLower(cname), level), "main", mainB, in, nil, nil, cname == "Tank" && level == 1,
				CorpusGear{Rounding: RoundNone, Items: kitItems[cname]}); err != nil {
				return nil, err
			}
		}
	}
	for _, bare := range []struct {
		class string
		level int
	}{{"Mage", 50}, {"Tank", 7}, {"Cleric", 33}, {"Summoner", 2}, {"Rogue", 19}, {"Fighter", 13}, {"Bard", MaxLevel}} {
		in := Inputs{Class: classOf(bare.class), Level: bare.level}
		if err := w.caseOf(fmt.Sprintf("bare_%s_l%d", strings.ToLower(bare.class), bare.level), "main", mainB, in, nil, nil, false); err != nil {
			return nil, err
		}
	}
	// State and extra mods: a Fighter in combat who has fished; mods for stats the graph does not have and repeated ones.
	fighter, err := kitInputs("Fighter", 20, RoundNone)
	if err != nil {
		return nil, err
	}
	fighter.Mods = append(fighter.Mods, Mod{Stat: 1, Value: 5}, Mod{Stat: fighter.Mods[0].Stat, Value: 2.5}, Mod{Stat: fighter.Mods[0].Stat, Value: -0.5})
	st := &TableState{FlagCombat: true, FlagMoving: true, Professions: map[string]float64{"Fishing": 6}, Actor: 7}
	if err := w.caseOf("fighter_l20_state", "main", mainB, fighter, st, nil, false); err != nil {
		return nil, err
	}

	// The tiny graph: every instruction, every state function, the faults.
	for _, tc := range []struct {
		name         string
		class, level int
		rich         bool
	}{{"tiny_ranger_l20", 1, 20, false}, {"tiny_bard_l20", 2, 20, false}, {"tiny_ranger_l5", 1, 5, false}, {"tiny_ranger_l40", 1, 40, false},
		{"tiny_bard_l11_rich", 2, 11, true}, {"tiny_ranger_l50_rich", 1, 50, true}, {"tiny_bard_l1_rich", 2, 1, true}} {
		in := tinyInputs(tc.class, tc.level, tc.rich)
		var st *TableState
		if tc.rich {
			st = richState()
		}
		if err := w.caseOf(tc.name, "tiny", tinyB, in, st, nil, true); err != nil {
			return nil, err
		}
	}
	for _, level := range []int{1, 20, 41} {
		if err := w.caseOf(fmt.Sprintf("tiny_single_bard_l%d", level), "tiny_single", tinySB, tinyInputs(2, level, true), richState(), nil, true); err != nil {
			return nil, err
		}
	}
	// Plan D2: effects, ranks and expression programs on the tiny graph.
	progsOf := func(res []ExprResult) []int {
		var out []int
		for _, r := range res {
			if r.Program >= 0 {
				out = append(out, r.Program)
			}
		}
		return out
	}
	for _, fc := range tinyFxCases() {
		b, enc, res, name := tinyB, tinyEnc, tinyRes, "tiny"
		if fc.single {
			b, enc, res, name = tinySB, tinySEnc, tinySRes, "tiny_single"
		}
		_ = enc
		if err := w.fxCaseOf(fc.name, name, b, fc.in, fc.state, true, fc.host, progsOf(res)); err != nil {
			return nil, err
		}
	}
	// The real design data: characters with the auto-granted ranks, the Sprint and the retail buffs, and every expression program of the
	// bundle evaluated against their stats.
	if len(exprs) > 0 {
		real, err := realFxCases(dbPath, mainB, mainRes, exprIDs)
		if err != nil {
			return nil, err
		}
		var progs []int
		for _, p := range mainB.Exprs {
			progs = append(progs, int(p))
		}
		for _, rc := range real {
			b := mainB
			if rc.bundle == "main_double" {
				b = doubleB
			}
			if err := w.fxCaseOf(rc.name, rc.bundle, b, rc.in, rc.state, rc.name == "real_bard_l25_sprint", rc.host, progs, rc.retail...); err != nil {
				return nil, err
			}
		}
	}
	// Plan E1, stage 3: the NPC profiles (their whole chain of lists, evaluated at the instance's level with the Blueprint's stat distribution and
	// elite rank), the other actors of a hit, and the damage formulas.
	npcState := func(rank float64, dist [8]float64) *TableState { return &TableState{EliteRank: rank, NpcDist: dist} }
	dists := [][8]float64{
		{0.25, 0.125, 0.125, 0.125, 0.125, 0.0625, 0.0625, 0.125},
		{0.5, 0.0625, 0.0625, 0.0625, 0.0625, 0.0625, 0.0625, 0.125},
		{0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125},
	}
	npcIn := map[string]Inputs{}
	npcSt := map[string]*TableState{}
	for _, cl := range mainB.Classes {
		if int(cl.Enum) < NpcClassBase {
			continue
		}
		cname := mainB.Strings[cl.Name]
		for k, level := range []int{1, 10, 25} {
			in := Inputs{Class: int(cl.Enum), Level: level}
			st := npcState(float64(k), dists[k])
			name := fmt.Sprintf("npc_%s_l%d", strings.ToLower(cname), level)
			if err := w.caseOf(name, "main", mainB, in, st, nil, k == 1); err != nil {
				return nil, err
			}
			npcIn[name], npcSt[name] = in, st
		}
	}
	npcMid := "npc_npc_l10"
	for i, fc := range tinyFxCases() {
		if i > 1 || fc.single {
			continue
		}
		actors := []CorpusActor{
			{Role: aocexpr.RoleTarget, In: tinyInputs(1, 10, true), State: richState()},
			{Role: aocexpr.RoleActiveHarm, In: tinyInputs(2, 5, false)},
		}
		if err := w.fxCaseWith(fc.name+"_actors", "tiny", tinyB, fc.in, fc.state, actors, true, fc.host, progsOf(tinyRes)); err != nil {
			return nil, err
		}
	}
	if len(exprs) > 0 {
		real, err := realFxCases(dbPath, mainB, mainRes, exprIDs)
		if err != nil {
			return nil, err
		}
		var progs []int
		for _, p := range mainB.Exprs {
			progs = append(progs, int(p))
		}
		for _, rc := range real {
			if rc.name != "real_bard_l25_sprint" && rc.name != "real_fighter_l50" {
				continue
			}
			actors := []CorpusActor{
				{Role: aocexpr.RoleTarget, In: npcIn[npcMid], State: npcSt[npcMid]},
				{Role: aocexpr.RoleActiveHarm, In: Inputs{Class: classOf("Fighter"), Level: 20}},
			}
			if err := w.fxCaseWith(rc.name+"_vs_npc", rc.bundle, mainB, rc.in, rc.state, actors, false, rc.host, progs); err != nil {
				return nil, err
			}
		}
	}
	cdb, err := content.Open(dbPath)
	if err != nil {
		return nil, err
	}
	combat, err := cdb.Combat()
	cdb.Close()
	if err != nil {
		return nil, err
	}
	profile, err := ProfileFromCombat(combat)
	if err != nil {
		return nil, err
	}
	viewOf := func(in Inputs, st *TableState) (MapView, error) {
		var state aocexpr.State
		if st != nil {
			state = st
		}
		res, err := mainB.Run(in, state)
		if err != nil {
			return nil, err
		}
		v := MapView{}
		for _, s := range res.Stats {
			v[s.Guid] = s.Value
		}
		return v, nil
	}
	fighterKit, err := kitInputs("Fighter", 20, RoundNone)
	if err != nil {
		return nil, err
	}
	mageKit, err := kitInputs("Mage", 25, RoundNone)
	if err != nil {
		return nil, err
	}
	var views []CorpusView
	for _, v := range []struct {
		name string
		in   Inputs
		st   *TableState
	}{{"fighter_l20", fighterKit, nil}, {"mage_l25", mageKit, nil}, {"npc_l10", npcIn[npcMid], npcSt[npcMid]}, {"npc_l25", npcIn["npc_npc_l25"], npcSt["npc_npc_l25"]}} {
		mv, err := viewOf(v.in, v.st)
		if err != nil {
			return nil, err
		}
		views = append(views, CorpusView{Name: v.name, V: mv})
	}
	views = append(views, CorpusView{Name: "synthetic_a", V: syntheticView(profile, 0xA11CE)}, CorpusView{Name: "synthetic_b", V: syntheticView(profile, 0xB0B)})
	w.damageCases("real", profile, views)

	// Nodes 1, stage 1: the city node reads of a few worlds, for the C++ host to answer the same.
	for _, nc := range nodeHostCases() {
		w.line("nodehost %s", nc.Name)
		w.line("nodes %d", len(nc.World.Types))
		for r := range nc.World.Types {
			w.line("node %d %d %d", r, nc.World.Types[r], nc.World.Levels[r])
		}
		w.line("ctx %d %d %d %d", nc.World.Node, nc.World.Biome, nc.World.ActorNode, nc.World.PlayerNode)
		for _, q := range nc.Queries {
			args := ""
			for _, a := range q.Args {
				args += " " + strconv.Itoa(a)
			}
			w.line("q %d%s = %d", q.Fn, args, q.Want)
		}
		w.line("end")
	}

	// The differential fuzz.
	for _, fz := range []struct {
		name string
		enc  []byte
		seed uint64
		n    int
	}{{"tiny", tinyEnc, 0x9E3779B97F4A7C15, 20000}, {"tiny_single", tinySEnc, 12345, 5000}, {"main", mainEnc, 88172645463325252, 3000}} {
		acc, h := FuzzParity(fz.enc, fz.seed, fz.n)
		w.line("fuzz %s %d %d %d %016x", fz.name, fz.seed, fz.n, acc, h)
	}

	// The hostile blobs.
	hostile, err := HostileCases()
	if err != nil {
		return nil, err
	}
	for _, h := range hostile {
		w.hostileLine(tinyEnc, h)
	}
	return w.Bytes(), nil
}

// C_Y_B captures, C-O-A code

// classEnum is ECharacterClass of a class name.
func classEnum(name string) int {
	return map[string]int{"Ranger": 1, "Bard": 2, "Cleric": 3, "Fighter": 4, "Rogue": 5, "Summoner": 6, "Tank": 7, "Mage": 8}[name]
}

// ---- reading and replaying ----

// ParseCorpus reads a corpus file.
func ParseCorpus(r io.Reader) (*Corpus, error) {
	c := &Corpus{Bundles: map[string][]byte{}}
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	var cur *CorpusCase
	var actor *CorpusActor // the actor block being read
	var dmg damageCursor
	var nh *NodeHostCase // the nodehost block being read
	var curBundle string
	var b64 strings.Builder
	var bundleLen int
	lineNo := 0
	fail := func(format string, args ...any) (*Corpus, error) {
		return nil, fmt.Errorf("corpus line %d: %s", lineNo, fmt.Sprintf(format, args...))
	}
	for sc.Scan() {
		lineNo++
		line := sc.Text()
		if line == "" || line[0] == '#' {
			continue
		}
		f := strings.Fields(line)
		if curBundle != "" {
			if f[0] == "endbundle" {
				raw, err := base64.StdEncoding.DecodeString(b64.String())
				if err != nil || len(raw) != bundleLen {
					return fail("bundle %s is not %d bytes of base64 (%v)", curBundle, bundleLen, err)
				}
				c.Bundles[curBundle] = raw
				curBundle = ""
				continue
			}
			b64.WriteString(f[0])
			continue
		}
		switch f[0] {
		case "bundle":
			if len(f) != 3 {
				return fail("bundle NAME LEN")
			}
			n, err := strconv.Atoi(f[2])
			if err != nil {
				return fail("bad length")
			}
			curBundle, bundleLen = f[1], n
			b64.Reset()
		case "derive":
			if len(f) != 4 || f[3] != "double" {
				return fail("derive NAME OF double")
			}
			c.Derived = append(c.Derived, DerivedBundle{f[1], f[2]})
		case "case":
			if len(f) != 5 {
				return fail("case NAME BUNDLE CLASS LEVEL")
			}
			cl, e1 := strconv.Atoi(f[3])
			lv, e2 := strconv.Atoi(f[4])
			if e1 != nil || e2 != nil {
				return fail("bad class or level")
			}
			c.Cases = append(c.Cases, CorpusCase{Name: f[1], Bundle: f[2], In: Inputs{Class: cl, Level: lv}})
			cur = &c.Cases[len(c.Cases)-1]
		case "fxcase":
			if len(f) != 5 {
				return fail("fxcase NAME BUNDLE CLASS LEVEL")
			}
			cl, e1 := strconv.Atoi(f[3])
			lv, e2 := strconv.Atoi(f[4])
			if e1 != nil || e2 != nil {
				return fail("bad class or level")
			}
			c.Cases = append(c.Cases, CorpusCase{Name: f[1], Bundle: f[2], In: Inputs{Class: cl, Level: lv}, Fx: &CorpusFx{}})
			cur = &c.Cases[len(c.Cases)-1]
		case "actor":
			if cur == nil || cur.Fx == nil || len(f) != 4 {
				return fail("actor ROLE CLASS LEVEL inside an fxcase")
			}
			role, e1 := strconv.Atoi(f[1])
			cl, e2 := strconv.Atoi(f[2])
			lv, e3 := strconv.Atoi(f[3])
			if e1 != nil || e2 != nil || e3 != nil {
				return fail("bad actor line")
			}
			actor = &CorpusActor{Role: role, In: Inputs{Class: cl, Level: lv}}
		case "endactor":
			if actor == nil {
				return fail("endactor outside an actor")
			}
			cur.Actors = append(cur.Actors, *actor)
			actor = nil
		case "nodehost":
			if len(f) != 2 {
				return fail("nodehost NAME")
			}
			c.NodeHosts = append(c.NodeHosts, NodeHostCase{Name: f[1]})
			nh = &c.NodeHosts[len(c.NodeHosts)-1]
		case "nodes", "node", "ctx", "q":
			if nh == nil {
				return fail("%q outside a nodehost block", f[0])
			}
			if err := nh.parseLine(f); err != nil {
				return fail("%v", err)
			}
		case "dprofile", "dp", "de", "dview", "v", "damage":
			if _, err := c.parseDamageLine(f, &dmg); err != nil {
				return fail("%v", err)
			}
		case "end":
			if nh != nil {
				nh = nil
				continue
			}
			if dmg.open() {
				if _, err := c.parseDamageLine(f, &dmg); err != nil {
					return fail("%v", err)
				}
				continue
			}
			cur = nil
		case "refuse":
			if len(f) != 5 {
				return fail("refuse NAME BUNDLE CLASS LEVEL")
			}
			cl, e1 := strconv.Atoi(f[3])
			lv, e2 := strconv.Atoi(f[4])
			if e1 != nil || e2 != nil {
				return fail("bad class or level")
			}
			c.Refusals = append(c.Refusals, CorpusRefusal{Name: f[1], Bundle: f[2], Class: cl, Level: lv})
		case "fuzz":
			if len(f) != 6 {
				return fail("fuzz BUNDLE SEED N ACCEPTED HASH")
			}
			seed, e1 := strconv.ParseUint(f[2], 10, 64)
			n, e2 := strconv.Atoi(f[3])
			acc, e3 := strconv.Atoi(f[4])
			h, e4 := strconv.ParseUint(f[5], 16, 64)
			if e1 != nil || e2 != nil || e3 != nil || e4 != nil {
				return fail("bad fuzz line")
			}
			c.Fuzzes = append(c.Fuzzes, CorpusFuzz{Bundle: f[1], Seed: seed, N: n, Accepted: acc, Hash: h})
		case "hostile":
			if len(f) != 6 {
				return fail("hostile NAME FIX POS DEL HEX")
			}
			pos, e1 := strconv.Atoi(f[3])
			del, e2 := strconv.Atoi(f[4])
			var ins []byte
			var e3 error
			if f[5] != "-" {
				ins, e3 = hex.DecodeString(f[5])
			}
			if e1 != nil || e2 != nil || e3 != nil {
				return fail("bad hostile edit")
			}
			c.Hostiles = append(c.Hostiles, CorpusHostile{Name: f[1], Fix: f[2] == "1", Blob: f[2] == "2", Pos: pos, Del: del, Ins: ins})
		default:
			if actor != nil {
				sub := CorpusCase{In: actor.In, State: actor.State}
				if err := parseCaseLine(&sub, f); err != nil {
					return fail("%v", err)
				}
				actor.In, actor.State = sub.In, sub.State
				continue
			}
			if cur == nil {
				return fail("%q outside a case", f[0])
			}
			if err := parseCaseLine(cur, f); err != nil {
				return fail("%v", err)
			}
		}
	}
	if curBundle != "" || cur != nil || actor != nil || dmg.open() || nh != nil {
		return nil, fmt.Errorf("corpus: unterminated bundle or case")
	}
	return c, sc.Err()
}

// (CoA ~ C Y B)

func parseCaseLine(c *CorpusCase, f []string) error {
	u := func(s string, base, bits int) uint64 {
		v, err := strconv.ParseUint(s, base, bits)
		if err != nil {
			panic(fmt.Errorf("bad number %q", s))
		}
		return v
	}
	var err error
	func() {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("%v in %v", r, f)
			}
		}()
		switch f[0] {
		case "mod":
			c.In.Mods = append(c.In.Mods, Mod{Stat: int64(u(f[1], 10, 64)), Value: math.Float64frombits(u(f[2], 16, 64))})
		case "tag":
			c.In.Equipped = append(c.In.Equipped, EquippedTag{Slot: f[1], Tag: f[2]})
		case "rounding":
			if c.Gear == nil {
				c.Gear = &CorpusGear{}
			}
			switch f[1] {
			case "none":
				c.Gear.Rounding = RoundNone
			case "halfup":
				c.Gear.Rounding = RoundHalfUp
			default:
				panic(fmt.Errorf("unknown rounding %q", f[1]))
			}
		case "item":
			if c.Gear == nil {
				c.Gear = &CorpusGear{}
			}
			c.Gear.Items = append(c.Gear.Items, Item{Record: int64(u(f[1], 10, 64)), Quality: int(u(f[2], 10, 32)), Slot: f[3]})
		case "state":
			if c.State == nil {
				c.State = &TableState{}
			}
			s := c.State
			switch f[1] {
			case "effect":
				if s.Effects == nil {
					s.Effects = map[aocexpr.Ref]float64{}
				}
				s.Effects[aocexpr.Ref{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}] = math.Float64frombits(u(f[4], 16, 64))
			case "tag":
				if s.Tags == nil {
					s.Tags = map[string]float64{}
				}
				s.Tags[f[2]] = math.Float64frombits(u(f[3], 16, 64))
			case "skill":
				if s.Skills == nil {
					s.Skills = map[[2]aocexpr.Ref]bool{}
				}
				s.Skills[[2]aocexpr.Ref{{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}, {TypeID: u(f[4], 10, 64), Guid: int64(u(f[5], 10, 64))}}] = true
			case "combat":
				s.FlagCombat = true
			case "moving":
				s.FlagMoving = true
			case "unkillable":
				s.FlagUnkillable = true
			case "actor":
				s.Actor = math.Float64frombits(u(f[2], 16, 64))
			case "prof":
				if s.Professions == nil {
					s.Professions = map[string]float64{}
				}
				s.Professions[f[2]] = math.Float64frombits(u(f[3], 16, 64))
			case "consumed":
				if s.Consumed == nil {
					s.Consumed = map[aocexpr.Ref]float64{}
				}
				s.Consumed[aocexpr.Ref{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}] = math.Float64frombits(u(f[4], 16, 64))
			case "npc":
				s.EliteRank = math.Float64frombits(u(f[2], 16, 64))
			case "dist":
				i := int(u(f[2], 10, 32))
				if i < 0 || i >= len(s.NpcDist) {
					panic(fmt.Errorf("state dist %d: there are 8 stats", i))
				}
				s.NpcDist[i] = math.Float64frombits(u(f[3], 16, 64))
			default:
				panic(fmt.Errorf("unknown state %q", f[1]))
			}
		case "fx":
			c.In.Effects = append(c.In.Effects, ActiveEffect{Effect: aocexpr.Ref{TypeID: u(f[1], 10, 64), Guid: int64(u(f[2], 10, 64))}, Stacks: int(u(f[3], 10, 32))})
		case "fxtag":
			e := &c.In.Effects[len(c.In.Effects)-1]
			e.Tags = append(e.Tags, f[1])
		case "fxmod":
			e := &c.In.Effects[len(c.In.Effects)-1]
			e.Mods = append(e.Mods, EffectMod{Stat: int64(u(f[1], 10, 64)), Value: math.Float64frombits(u(f[2], 16, 64))})
		case "rank":
			c.In.Ranks = append(c.In.Ranks, SkillRank{Class: aocexpr.Ref{TypeID: u(f[1], 10, 64), Guid: int64(u(f[2], 10, 64))}, Skill: aocexpr.Ref{TypeID: u(f[3], 10, 64), Guid: int64(u(f[4], 10, 64))}})
		case "ignoredfx":
			c.Fx.IgnoredFx = int(u(f[1], 10, 32))
		case "evalfaults":
			c.Fx.EvalFaults = int(u(f[1], 10, 32))
		case "host":
			h := &c.Fx.Host
			switch f[1] {
			case "seed":
				h.Seed = u(f[2], 10, 64)
			case "progress":
				if h.Progress == nil {
					h.Progress = map[string]float64{}
				}
				group := f[2]
				if group == "-" {
					group = ""
				}
				h.Progress[group] = math.Float64frombits(u(f[3], 16, 64))
			case "inst":
				if h.Inst == nil {
					h.Inst = map[aocexpr.Ref]float64{}
				}
				h.Inst[aocexpr.Ref{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}] = math.Float64frombits(u(f[4], 16, 64))
			case "hittag":
				if h.Tags == nil {
					h.Tags = map[string]bool{}
				}
				h.Tags[f[2]] = true
			case "hitflag":
				if h.Flags == nil {
					h.Flags = map[string]bool{}
				}
				h.Flags[f[2]] = true
			case "targets":
				h.Targets = math.Float64frombits(u(f[2], 16, 64))
			case "xpdebt":
				h.XPDebt = math.Float64frombits(u(f[2], 16, 64))
			case "fn":
				if h.Fns == nil {
					h.Fns = map[string]float64{}
				}
				name := f[3]
				if name == "-" {
					name = ""
				}
				h.Fns[f[2]+"|"+name] = math.Float64frombits(u(f[4], 16, 64))
			default:
				panic(fmt.Errorf("unknown host line %q", f[1]))
			}
		case "prog":
			c.Fx.Progs = append(c.Fx.Progs, CorpusProg{Index: int(u(f[1], 10, 32)), Value: u(f[2], 16, 64)})
		case "retail":
			switch f[1] {
			case "prog":
				c.Fx.Retail = append(c.Fx.Retail, CorpusRetail{Kind: "prog", ID: int64(u(f[2], 10, 64)), Value: math.Float64frombits(u(f[3], 16, 64))})
			case "stat":
				c.Fx.Retail = append(c.Fx.Retail, CorpusRetail{Kind: "stat", ID: int64(u(f[2], 10, 64)), Value: math.Float64frombits(u(f[3], 16, 64)), Digits: int(u(f[4], 10, 32))})
			default:
				panic(fmt.Errorf("unknown retail line %q", f[1]))
			}
		case "want":
			c.Want = append(c.Want, CorpusWant{Guid: int64(u(f[1], 10, 64)), Value: u(f[2], 16, 64)})
		case "digest":
			c.Digest = u(f[1], 16, 64)
			c.Members, c.Faults, c.Ignored = int(u(f[2], 10, 32)), int(u(f[3], 10, 32)), int(u(f[4], 10, 32))
		case "cap":
			c.Caps = append(c.Caps, CorpusCap{Guid: int64(u(f[1], 10, 64)), Bits: uint32(u(f[2], 16, 32)), Match: f[3]})
		default:
			panic(fmt.Errorf("unknown line %q", f[0]))
		}
	}()
	return err
}

// (C.O.A + C_Y_B)

// Replay checks the corpus the way the C++ test does: every bundle decodes, every case runs to
// its values and digest and to its capture comparisons, every hostile blob is refused.
func (c *Corpus) Replay() error {
	for _, nc := range c.NodeHosts {
		if err := nc.check(); err != nil {
			return err
		}
	}
	bundles := map[string]*Bundle{}
	for name, raw := range c.Bundles {
		b, err := Decode(raw)
		if err != nil {
			return fmt.Errorf("bundle %s: %w", name, err)
		}
		bundles[name] = b
	}
	for _, d := range c.Derived {
		raw, ok := c.Bundles[d.Of]
		if !ok {
			return fmt.Errorf("derive %s: no bundle %s", d.Name, d.Of)
		}
		raw = append([]byte(nil), raw...)
		binary.LittleEndian.PutUint16(raw[6:], FlagDouble)
		b, err := Decode(raw)
		if err != nil {
			return fmt.Errorf("derive %s: %w", d.Name, err)
		}
		bundles[d.Name] = b
	}
	for _, cs := range c.Cases {
		b, ok := bundles[cs.Bundle]
		if !ok {
			return fmt.Errorf("case %s: no bundle %s", cs.Name, cs.Bundle)
		}
		var st aocexpr.State
		if cs.State != nil {
			st = cs.State
		}
		res, err := b.Run(cs.In, st)
		if err != nil {
			return fmt.Errorf("case %s: %w", cs.Name, err)
		}
		if len(res.Stats) != cs.Members || res.Faults != cs.Faults || res.IgnoredMods != cs.Ignored || (len(cs.Want) != 0 && len(cs.Want) != cs.Members) {
			return fmt.Errorf("case %s: %d stats, %d faults, %d ignored mods; the corpus has %d, %d, %d (%d wants)", cs.Name, len(res.Stats), res.Faults, res.IgnoredMods, cs.Members, cs.Faults, cs.Ignored, len(cs.Want))
		}
		got := map[int64]float64{}
		for i, s := range res.Stats {
			if len(cs.Want) != 0 && (cs.Want[i].Guid != s.Guid || cs.Want[i].Value != math.Float64bits(s.Value)) {
				return fmt.Errorf("case %s: stat %d is %016x, the corpus has %d %016x", cs.Name, s.Guid, math.Float64bits(s.Value), cs.Want[i].Guid, cs.Want[i].Value)
			}
			got[s.Guid] = s.Value
		}
		if d := Digest(res); d != cs.Digest {
			return fmt.Errorf("case %s: digest %016x, the corpus has %016x", cs.Name, d, cs.Digest)
		}
		for _, cp := range cs.Caps {
			want := float64(math.Float32frombits(cp.Bits))
			if m := [...]string{"exact", "near", "miss"}[compare(got[cp.Guid], want)]; m != cp.Match {
				return fmt.Errorf("case %s: stat %d compares as %s with the capture, the corpus says %s", cs.Name, cp.Guid, m, cp.Match)
			}
		}
		if cs.Fx != nil {
			for _, r := range cs.Fx.Retail {
				if r.Kind != "stat" {
					continue
				}
				if i, ok := b.StatIndex(r.ID); !ok || !r.holds(res.Values[i]) {
					return fmt.Errorf("case %s: the retail value %v of stat %d is %v here", cs.Name, r.Value, r.ID, res.Values[i])
				}
			}
			if res.IgnoredEffectMods != cs.Fx.IgnoredFx {
				return fmt.Errorf("case %s: %d ignored effect mods, the corpus has %d", cs.Name, res.IgnoredEffectMods, cs.Fx.IgnoredFx)
			}
			if len(cs.Fx.Progs) > 0 {
				ev, err := b.NewEvaluator(res, cs.In, st)
				if err != nil {
					return fmt.Errorf("case %s: %w", cs.Name, err)
				}
				if err := setActors(b, ev, cs.Actors); err != nil {
					return fmt.Errorf("case %s: %w", cs.Name, err)
				}
				host := cs.Fx.Host.Host()
				progValue := map[int]float64{}
				for _, p := range cs.Fx.Progs {
					v, err := ev.Eval(p.Index, host)
					if err != nil {
						return fmt.Errorf("case %s: program %d: %w", cs.Name, p.Index, err)
					}
					if math.Float64bits(v) != p.Value {
						return fmt.Errorf("case %s: program %d gives %016x, the corpus has %016x", cs.Name, p.Index, math.Float64bits(v), p.Value)
					}
					progValue[p.Index] = v
				}
				for _, r := range cs.Fx.Retail {
					if r.Kind == "prog" {
						if v, ok := progValue[int(r.ID)]; !ok || !r.holds(v) {
							return fmt.Errorf("case %s: the retail value %v of program %d is %v here", cs.Name, r.Value, r.ID, v)
						}
					}
				}
				if ev.Faults() != cs.Fx.EvalFaults {
					return fmt.Errorf("case %s: the evaluations had %d faults, the corpus has %d", cs.Name, ev.Faults(), cs.Fx.EvalFaults)
				}
			}
		}
	}
	if err := c.replayDamage(); err != nil {
		return err
	}
	for _, r := range c.Refusals {
		b, ok := bundles[r.Bundle]
		if !ok {
			return fmt.Errorf("refuse %s: no bundle %s", r.Name, r.Bundle)
		}
		if _, err := b.Run(Inputs{Class: r.Class, Level: r.Level}, nil); err == nil {
			return fmt.Errorf("refuse %s: the run finished", r.Name)
		}
	}
	for _, fz := range c.Fuzzes {
		raw, ok := c.Bundles[fz.Bundle]
		if !ok {
			return fmt.Errorf("fuzz: no bundle %s", fz.Bundle)
		}
		if acc, h := FuzzParity(raw, fz.Seed, fz.N); acc != fz.Accepted || h != fz.Hash {
			return fmt.Errorf("fuzz %s: %d accepted with hash %016x; the corpus has %d and %016x", fz.Bundle, acc, h, fz.Accepted, fz.Hash)
		}
	}
	tiny, ok := c.Bundles["tiny"]
	if !ok && len(c.Hostiles) > 0 {
		return fmt.Errorf("hostile lines need the bundle tiny")
	}
	for _, h := range c.Hostiles {
		blob, ok := ApplyEdit(tiny, h)
		if !ok {
			return fmt.Errorf("hostile %s: the edit does not fit the bundle", h.Name)
		}
		if _, err := Decode(blob); err == nil {
			return fmt.Errorf("hostile %s was accepted", h.Name)
		}
	}
	return nil
}

// == C.O.A | C_Y_B ==

// capture is a golden file: a captured character's stats.
type capture struct {
	Class  string `json:"class"`
	Level  int    `json:"level"`
	Source string `json:"source"`
	Items  []struct {
		Slot    int   `json:"slot"`
		Record  int64 `json:"record"`
		Quality int   `json:"quality"`
	} `json:"items"`
	Stats []struct {
		Guid      int64  `json:"guid"`
		Array     string `json:"array"`
		Bits      uint32 `json:"bits"`
		BaseBits  uint32 `json:"base_bits"`
		EquipBits uint32 `json:"equip_bits"`
		HasBucket bool
	} `json:"stats"`
}

// match is how a computed value compares with a captured float32.
type match int

const (
	matchExact match = iota // equal as a float32
	matchNear               // within 1e-6 relative: an ulp or two
	matchMiss
)

func compare(got, want float64) match {
	g := float64(float32(got))
	if g == want {
		return matchExact
	}
	if math.Abs(g-want) <= 1e-6*math.Max(1, math.Abs(want)) {
		return matchNear
	}
	return matchMiss
}

func f32(bits uint32) float64 { return float64(math.Float32frombits(bits)) }

func (c *capture) gearItems() []Item {
	out := make([]Item, len(c.Items))
	for i, it := range c.Items {
		out[i] = Item{Record: it.Record, Quality: it.Quality, Slot: SlotName(it.Slot)}
	}
	return out
}

// tinyFxCase is an fxcase of the tiny graph.
type tinyFxCase struct {
	name   string
	in     Inputs
	state  *TableState
	host   TableHostSpec
	single bool
}

// tinyFxCases are the effect and expression cases of the corpus and of the Go tests: every form of effect mod, stacks, tags, ranks,
// the state, the limits, and the three hosts.
func tinyFxCases() []tinyFxCase {
	stat := func(n int) int64 { return int64(1000 + n) }
	eff := func(n int64) aocexpr.Ref { return aocexpr.Ref{TypeID: tinyEffectType, Guid: n} }
	rank := func(skill int64) SkillRank {
		return SkillRank{Class: aocexpr.Ref{TypeID: tinyClassType, Guid: 2}, Skill: aocexpr.Ref{TypeID: tinySkillType, Guid: skill}}
	}
	forms := func(in Inputs) Inputs {
		in.Effects = []ActiveEffect{
			{Effect: eff(7), Stacks: 2, Tags: []string{"pet.summon.wolf"}, Mods: []EffectMod{{stat(19), 0.25}, {stat(20), 1.05}, {stat(17), 0.1}, {stat(1), 4}}},
			{Effect: eff(8), Stacks: 1, Mods: []EffectMod{{stat(19), 0.125}, {stat(20), 1.1}, {stat(17), 0.5}, {424242, 7}}},
			{Effect: eff(9), Stacks: 0, Mods: []EffectMod{{stat(19), 100}}},
		}
		return in
	}
	hosts := tinyHosts()
	return []tinyFxCase{
		{name: "tiny_fx_none_bard_l20", in: tinyInputs(2, 20, false), host: hosts[0]},
		{name: "tiny_fx_forms_bard_l20", in: forms(tinyInputs(2, 20, false)), host: hosts[1]},
		{name: "tiny_fx_forms_ranger_l11_rich", in: func() Inputs {
			in := forms(tinyInputs(1, 11, true))
			in.Ranks = []SkillRank{rank(3), rank(5)}
			return in
		}(), state: richState(), host: hosts[2]},
		{name: "tiny_fx_ranks_bard_l41", in: func() Inputs {
			in := tinyInputs(2, 41, false)
			in.Ranks = []SkillRank{rank(3), rank(4), rank(5)}
			in.Effects = []ActiveEffect{{Effect: eff(1), Stacks: 3, Tags: []string{"pet.summon"}, Mods: []EffectMod{{stat(7), 1000}}}}
			return in
		}(), host: hosts[1]},
		{name: "tiny_fx_limits_ranger_l5", in: func() Inputs {
			in := tinyInputs(1, 5, false)
			in.Effects = []ActiveEffect{{Effect: eff(1), Stacks: 4000, Mods: []EffectMod{{stat(19), 1}, {stat(17), 0.999}, {stat(20), 0.0001}}}}
			return in
		}(), host: hosts[0]},
		{name: "tiny_fx_single_bard_l20", in: forms(tinyInputs(2, 20, true)), state: richState(), host: hosts[1], single: true},
		{name: "tiny_fx_single_ranger_l50", in: func() Inputs {
			in := forms(tinyInputs(1, 50, false))
			in.Ranks = []SkillRank{rank(4)}
			return in
		}(), host: hosts[2], single: true},
	}
}
