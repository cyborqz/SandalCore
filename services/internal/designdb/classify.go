// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"runtime"
	"sort"
	"strings"
	"sync"
)

// Classification says what a table holds.
type Classification struct {
	Table  int
	TypeID uint64
	Rows   uint32
	// Candidates are the record structs whose walk of every row of the table
	// ends exactly on its last byte. Small tables fit several (any struct of
	// strings and numbers reads a one-row table of zeros); large ones one; and
	// a struct can fit a table that is not its own (24 fishing rows walk as
	// StatLevelGains).
	Candidates []string
	// Schema is the record struct the table is, and no other table has it (one
	// table per struct). It is, in the order the evidence is weighed:
	//
	//   - the only candidate, unless the Id structs of other tables (or
	//     KnownTypes) say the table's TypeId is another struct's, and unless a
	//     second table has that candidate too and loses (see Classify);
	//   - the candidate that the table's TypeId names, as the Id structs of other
	//     tables (or KnownTypes) have it;
	//   - for a table no struct walks, the struct the TypeId names (without
	//     Decodes);
	//   - for a table several structs fit that nothing names, the one candidate
	//     whose name stem every row's name carries (see Classify).
	//
	// It is "" when undecided.
	Schema string
	// Decodes is true when Schema is one of the Candidates: the table has been
	// walked and ends where the directory says.
	Decodes bool
	// Basis says why Schema is the table's: one of the Basis* constants; "" when
	// the table is undecided.
	Basis string
}

// What settled a table's Schema (Classification.Basis).
const (
	BasisSingleFit = "single fit" // the one struct whose walk ends on the table's last byte
	BasisTypeID    = "type id"    // an Id struct in another table's rows, or KnownTypes, names the table's TypeId
	BasisNameStem  = "name stem"  // every row is named for the struct; weaker evidence than a TypeId
)

// KnownTypes returns the record types by TypeId (a copy: the list is fixed):
// the 398 the client executable registers (exe_gen.go), which name every one
// of the directory's 369 TypeIds. Decoder.Classify starts from them; before
// they were read from the executable, StatCurve (7037248826340794574) was the
// one known, from the stat initializers' EvalCurve($#<TypeId>:<guid>$, ...)
// calls, and the rest came from the Id structs in decoded rows.
func KnownTypes() map[uint64]string {
	out := make(map[uint64]string, len(exeTypes))
	for id, s := range exeTypes {
		out[id] = s
	}
	return out
}

// [C-O-A - Cyb]

// recordSchemas lists the record structs a table can hold: the structs
// derived directly from DesignDataRecordBase. Most are named XxxRecordBase and
// have a leaf FXxxRecord below them (which the walk includes, see plan); 30
// of the tables' structs are not (DialogueNode, NarrativeQuestV2, WarRecordV2,
// FishingSizeData, ...), so the name decides nothing.
func (d *Decoder) recordSchemas() []string {
	out := append([]string(nil), d.children[recordRoot]...)
	sort.Strings(out)
	return out
}

// candidate is a record struct that walked a table, with the Id structs the
// walk met (struct name to TypeId).
type candidate struct {
	schema string
	ids    map[string]uint64
}

func (d *Decoder) candidates(t TableInfo, schemas []string) []candidate {
	if t.Rows == 0 {
		return nil // every struct "walks" an empty table; there is nothing to tell
	}
	var out []candidate
	for _, s := range schemas {
		p, err := d.plan(s, true)
		if err != nil {
			continue
		}
		ids := map[string]uint64{}
		if _, err := d.walk(t, s, p, false, ids); err == nil {
			out = append(out, candidate{s, ids})
		}
	}
	return out
}

// Candidates returns the record structs whose walk of table i ends exactly on
// its last byte (see Classification.Candidates).
func (d *Decoder) Candidates(table int) []string {
	t, ok := d.db.Table(table)
	if !ok {
		return nil
	}
	var names []string
	for _, c := range d.candidates(t, d.recordSchemas()) {
		names = append(names, c.schema)
	}
	return names
}

// typeMap is record structs by the TypeId that names them.
type typeMap map[uint64]string

// learn adds what a candidate's walk met: an Id struct XxxId carries the TypeId
// of the table of XxxRecordBase. The first of two names for one TypeId wins, the
// same way every run.
func (d *Decoder) learn(into typeMap, c candidate) {
	names := make([]string, 0, len(c.ids))
	for n := range c.ids {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, idStruct := range names {
		rec := strings.TrimSuffix(idStruct, "Id") + "RecordBase"
		if !strings.HasSuffix(idStruct, "Id") || d.m.Schema(rec) == nil {
			continue
		}
		if _, have := into[c.ids[idStruct]]; !have {
			into[c.ids[idStruct]] = rec
		}
	}
}

// schemaStem is a record struct's name without "RecordBase".
func schemaStem(schema string) string { return strings.TrimSuffix(schema, "RecordBase") }

// namesCarryStem reports whether every row of table t, read as schema, is named
// for it: the name is the struct's stem ("GroupConstants") or the stem and an
// underscore and more ("StartingStatsClass_106058593949305"). The generated
// names of most tables read that way; a few (the abilities') do not, and for
// those the stem proves nothing, so this is only a tie-break.
func (d *Decoder) namesCarryStem(t TableInfo, schema string) bool {
	p, err := d.plan(schema, true)
	if err != nil {
		return false
	}
	rows, err := d.walk(t, schema, p, true, nil)
	if err != nil || len(rows) == 0 {
		return false
	}
	stem := schemaStem(schema)
	for _, r := range rows {
		if r.Name != stem && !strings.HasPrefix(r.Name, stem+"_") {
			return false
		}
	}
	return true
}

// ~ C-O-A & Cyb ~

// Classify identifies every table's record struct, one table per struct. It
// tries each record struct on each table (the walk is cheap and fails at the
// first byte that does not fit), then settles the tables from the evidence, in
// this order:
//
//  1. A table one struct fits takes it. A struct can fit a table that is not
//     its own, so the Id structs found in the rows of the single-fit tables
//     (XxxId carries the TypeId of the table of XxxRecordBase) are held against
//     that: a single-fit table whose TypeId an Id struct gives to another struct
//     is not settled (table 126, 24 fishing rows, walks as StatLevelGains but is
//     FishingSizeData; table 157, two interaction states, walks as
//     GroupConstants).
//  2. When two single-fit tables claim one struct, the one whose TypeId an Id
//     struct names for it wins; failing that, the one whose rows are all named
//     for the struct (schemaStem); failing that, neither is settled.
//  3. Tables several structs fit take the candidate their TypeId names, and
//     settling a table teaches more Id structs, so this goes round until nothing
//     changes. A table no struct walks takes the struct its TypeId names (without
//     Decodes).
//  4. A table several structs fit that no TypeId names takes the one candidate
//     every row's name is a stem of (StartingStats, StartingStatsClass and
//     StartingStatsRace are told apart this way), if no other table has it.
//
// What the discarded tables teach (the Id structs their walks met) is not used
// after step 2.
func (d *Decoder) Classify() []Classification {
	schemas := d.recordSchemas()
	tables := d.db.Tables()
	cands := make([][]candidate, len(tables))

	var wg sync.WaitGroup
	next := make(chan int)
	for w := 0; w < runtime.NumCPU(); w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range next {
				cands[i] = d.candidates(tables[i], schemas)
			}
		}()
	}
	for i := range tables {
		next <- i
	}
	close(next)
	wg.Wait()

	// stemmed is namesCarryStem, remembered: the rows of a table are decoded once
	// per struct, however many rounds ask.
	type stemKey struct {
		table  int
		schema string
	}
	stemMemo := map[stemKey]bool{}
	stemmed := func(t TableInfo, schema string) bool {
		k := stemKey{t.Index, schema}
		v, ok := stemMemo[k]
		if !ok {
			v = d.namesCarryStem(t, schema)
			stemMemo[k] = v
		}
		return v
	}

	out := make([]Classification, len(tables))
	for i, t := range tables {
		out[i] = Classification{Table: i, TypeID: t.TypeID, Rows: t.Rows}
		for _, c := range cands[i] {
			out[i].Candidates = append(out[i].Candidates, c.schema)
		}
	}

	// Step 1: what the single-fit tables' Id structs say, held against them.
	said := typeMap{}
	for id, s := range d.known {
		said[id] = s
	}
	for i := range tables {
		if len(cands[i]) == 1 {
			d.learn(said, cands[i][0])
		}
	}
	claims := map[string][]int{} // struct -> the single-fit tables that remain
	for i, t := range tables {
		if len(cands[i]) != 1 {
			continue
		}
		if want, ok := said[t.TypeID]; ok && want != cands[i][0].schema {
			continue
		}
		claims[cands[i][0].schema] = append(claims[cands[i][0].schema], i)
	}

	// Step 2: one table per struct.
	keep := make([]bool, len(tables))
	for schema, ts := range claims {
		if i, ok := pickClaim(schema, ts, tables, said, stemmed); ok {
			keep[i] = true
		}
	}

	// Steps 3 and 4 learn only from the tables kept.
	learned := typeMap{}
	for id, s := range d.known {
		learned[id] = s
	}
	settled := make([]bool, len(tables))
	owner := map[string]int{} // struct -> its table
	settle := func(i int, schema, basis string) {
		out[i].Schema = schema
		out[i].Basis = basis
		for _, c := range cands[i] {
			if c.schema == schema {
				out[i].Decodes = true
				d.learn(learned, c)
			}
		}
		settled[i] = true
		owner[schema] = i
	}
	for i := range tables {
		if keep[i] {
			settle(i, cands[i][0].schema, BasisSingleFit)
		}
	}
	for {
		for changed := true; changed; {
			changed = false
			for i, t := range tables {
				if settled[i] {
					continue
				}
				// The TypeId decides this table: it is that struct or none.
				if want, ok := learned[t.TypeID]; ok {
					if _, taken := owner[want]; !taken && (len(cands[i]) == 0 || contains(out[i].Candidates, want)) {
						settle(i, want, BasisTypeID)
						changed = true
					}
				}
			}
		}
		// Step 4: tables several structs fit that no TypeId names, by their rows'
		// names. A struct two such tables are named for is a tie, so neither gets it.
		byName := map[string][]int{}
		for i, t := range tables {
			if settled[i] || len(cands[i]) < 2 {
				continue
			}
			if _, named := learned[t.TypeID]; named {
				continue
			}
			var carrying []string
			for _, c := range cands[i] {
				if _, taken := owner[c.schema]; !taken && stemmed(t, c.schema) {
					carrying = append(carrying, c.schema)
				}
			}
			if len(carrying) == 1 {
				byName[carrying[0]] = append(byName[carrying[0]], i)
			}
		}
		names := make([]string, 0, len(byName))
		for schema := range byName {
			names = append(names, schema)
		}
		sort.Strings(names) // what a settled table teaches does not depend on map order
		progress := false
		for _, schema := range names {
			if ts := byName[schema]; len(ts) == 1 {
				settle(ts[0], schema, BasisNameStem)
				progress = true
			}
		}
		if !progress {
			break
		}
	}
	return out
}

// pickClaim chooses, among the single-fit tables ts that claim schema, the one
// that is it (see Classify, step 2).
func pickClaim(schema string, ts []int, tables []TableInfo, said typeMap, stemmed func(TableInfo, string) bool) (int, bool) {
	if len(ts) == 1 {
		return ts[0], true
	}
	var named []int // the tables an Id struct names for the schema
	for _, i := range ts {
		if said[tables[i].TypeID] == schema {
			named = append(named, i)
		}
	}
	if len(named) == 1 {
		return named[0], true
	}
	if len(named) > 1 {
		ts = named
	}
	var carrying []int
	for _, i := range ts {
		if stemmed(tables[i], schema) {
			carrying = append(carrying, i)
		}
	}
	if len(carrying) == 1 {
		return carrying[0], true
	}
	return 0, false
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}
