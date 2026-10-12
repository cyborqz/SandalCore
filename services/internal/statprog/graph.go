// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
)

// TypeIds of the record types a stat formula can name (the first number of a
// $#<TypeId>:<Guid>$ reference); the stat and the curve table are content's.
const (
	TypeStatDef      = content.TypeStatDef
	TypeStatCurve    = content.TypeStatCurve
	TypeStatEquation = 7180619218012719533 // StatEquationType, table of the equations EvalE reads
	TypeStatFormula  = 4547065559311335368 // StatFormulaType, EvalFormula
	TypeTerm         = 6137747221710071900 // TermType, what GetTerm names
)

// CommonLists are the StatInitializerLists every player starts from, in the order
// they apply; each class then adds its own list, Player_Base_<Class>.
var CommonLists = []string{"CombatStats", "Base_Artisan_Stats", "Player_Base"}

// StatDef is a StatTypeDef row, as the compiler needs it.
type StatDef struct {
	Guid        int64
	Name        string
	Replication int    // the audience mask
	Base        string // formulas: AoCExpression text, "" for none
	Min, Max    string
	Regen       string
	// OnSpawn is the reset behavior on spawn without its enum prefix: "Reset_None",
	// "Reset_SetMax" or "Reset_SetMin".
	OnSpawn            string
	MultiplyMods       bool // bMultiplyStatMods
	MultiplyCompliment bool // bMultiplyCompliment: with MultiplyMods, an effect's mod multiplies by (1 - value), not by value
	ClampFinal         bool // bClampStatFinalValue
	ReplicatesBuckets  bool // bReplicatesBaseAndEquipmentBuckets
	// StatType is the record type without its enum prefix: "Type_Float", "Type_Int32", "Type_Bool" or "Type_FloatArray" ("" when
	// a graph made by hand has none).
	StatType string
}

// InitItem is one entry of a StatInitializerList: Expr is the stat's next value
// ("" leaves it).
type InitItem struct {
	Stat int64
	Expr string
}

// ClassDef is a player class and the list it adds, or (Enum of NpcClassBase and more, plan E1 stage 3) an NPC profile: the whole chain of lists
// (Lists) a kind of NPC starts from, which replaces the common lists.
type ClassDef struct {
	Name  string
	Enum  int32 // ECharacterClass
	List  string
	Lists []string // an NPC profile's lists, in order (List is then empty)
}

// NpcProfiles are the NPC kinds the bundle holds (the order is the class enums NpcClassBase, NpcClassBase+1 ...). 2,123 of the game's 2,169 pawn
// Blueprints start from [CombatStats, NPC_Base, NPC_Elite_Scalar] (NPCPawn's own StatsComponent: BaseStatInitializer, StatInitializerList1 and
// StatInitializerList2); 29 more name the Guard overtuned scalar, and the other 17 name a scalar of their own (the elder dragons, a pet), which this
// profile list does not carry: they take the first profile.
var NpcProfiles = []ClassDef{
	{Name: "NPC", Lists: []string{"CombatStats", "NPC_Base", "NPC_Elite_Scalar"}},
	{Name: "NPC_Guard", Lists: []string{"CombatStats", "NPC_Base", "NPC_Elite_Scalar_Guard_Overtuned"}},
}

// listNames are the lists the class starts from, after the common ones for a player class.
func (c ClassDef) listNames() []string {
	if len(c.Lists) > 0 {
		return c.Lists
	}
	return []string{c.List}
}

// C Y B / C.O.A

// TermDef is a term's own data: DefaultValue is an expression for the value when no formula and no StatMod gives the term ("" is 0), and
// its limits bind whatever value it has. (The merge rule, for a term two sources give, is not modelled: the innermost source wins.)
type TermDef struct {
	Default string
	Clamp   Clamp
}

// Clamp is an equation's constraint: bClampMin and bClampMax with their Min and Max.
type Clamp struct {
	HasMin, HasMax bool
	Min, Max       float64
}

// Formula is a StatFormulaType row: an equation and the terms its GetTerm reads.
type Formula struct {
	Equation int64
	Terms    map[int64]string // term guid -> the term's expression
}

// Graph is the stat formula graph the compiler reads. LoadContentDB fills it from
// content.db; tests build small ones by hand.
type Graph struct {
	Stats     map[int64]StatDef
	Lists     map[string][]InitItem
	Classes   []ClassDef // by class enum
	Curves    map[int64]*aocexpr.Curve
	Equations map[int64]string
	// Clamps are the limits of the equations that have them (a StatEquationType's Constraints, plan D2): the equation's
	// value is raised to Min and lowered to Max, as its record says. An equation with a constraint of another kind is
	// not in Equations at all.
	Clamps map[int64]Clamp
	// Terms are the TermType rows GetTerm names (plan D2): what a term is when nothing supplies it, and its limits.
	Terms    map[int64]TermDef
	Formulas map[int64]Formula
	// Nodes are the CityNodeRecords by rank (Nodes 1, contentc version 12): the index of a node in the list of CityNodeRecordBase sorted by guid.
	// A program holds a node as its rank; a graph made by hand that has none cannot compile a node read.
	Nodes []GraphNode

	byName map[string]int64
}

// StatByName is the guid of the stat called name, and false when none is, or
// when two are.
func (g *Graph) StatByName(name string) (int64, bool) {
	if g.byName == nil {
		g.byName = map[string]int64{}
		for guid, s := range g.Stats {
			if prev, dup := g.byName[s.Name]; dup && prev != guid {
				g.byName[s.Name] = 0
				continue
			}
			g.byName[s.Name] = guid
		}
	}
	guid, ok := g.byName[name]
	return guid, ok && guid != 0
}

// ~ C.O.A :: Cyb ~

// LoadContentDB reads the graph from a content.db (read only).
func LoadContentDB(path string) (*Graph, error) {
	db, err := content.Open(path)
	if err != nil {
		return nil, err
	}
	defer db.Close()
	g := &Graph{
		Stats:     map[int64]StatDef{},
		Lists:     map[string][]InitItem{},
		Equations: map[int64]string{},
		Clamps:    map[int64]Clamp{},
		Formulas:  map[int64]Formula{},
		Terms:     map[int64]TermDef{},
	}
	sq := db.SQL()

	rows, err := sq.Query(`SELECT guid, name, replication, base_expr, min_expr, max_expr, regen_expr, on_spawn,
		clamp_final_value, multiply_mods, stat_type FROM stat_def ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var s StatDef
		if err := rows.Scan(&s.Guid, &s.Name, &s.Replication, &s.Base, &s.Min, &s.Max, &s.Regen, &s.OnSpawn,
			&s.ClampFinal, &s.MultiplyMods, &s.StatType); err != nil {
			rows.Close()
			return nil, err
		}
		s.OnSpawn = s.OnSpawn[strings.LastIndex(s.OnSpawn, ":")+1:]
		s.StatType = s.StatType[strings.LastIndex(s.StatType, ":")+1:]
		g.Stats[s.Guid] = s
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	// bReplicatesBaseAndEquipmentBuckets is not a stat_def column: read it from the row.
	for guid, s := range g.Stats {
		rec, err := db.Record(TypeStatDef, guid)
		if err != nil {
			return nil, err
		}
		var f struct {
			Buckets bool `json:"bReplicatesBaseAndEquipmentBuckets"`
			Compl   bool `json:"bMultiplyCompliment"`
		}
		if err := json.Unmarshal(rec.Fields, &f); err != nil {
			return nil, fmt.Errorf("stat %d: %w", guid, err)
		}
		s.ReplicatesBuckets = f.Buckets
		s.MultiplyCompliment = f.Compl
		g.Stats[guid] = s
	}

	rows, err = sq.Query(`SELECT list_name, stat_guid, expression FROM stat_init ORDER BY list_name, ord`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var list string
		var it InitItem
		if err := rows.Scan(&list, &it.Stat, &it.Expr); err != nil {
			rows.Close()
			return nil, err
		}
		if it.Stat == 0 {
			continue // a null reference
		}
		g.Lists[list] = append(g.Lists[list], it)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}

	cm, err := db.Curves()
	if err != nil {
		return nil, err
	}
	g.Curves = cm.Curves

	classes, err := db.Classes()
	if err != nil {
		return nil, err
	}
	for _, c := range classes {
		id, ok := chardef.ClassByName(c.Name)
		if !ok {
			return nil, fmt.Errorf("class %q is not a player class of the client's enum", c.Name)
		}
		g.Classes = append(g.Classes, ClassDef{Name: c.Name, Enum: id, List: c.StatInitListName})
	}
	sort.Slice(g.Classes, func(i, j int) bool { return g.Classes[i].Enum < g.Classes[j].Enum })
	// the NPC profiles whose lists the database has
	for i, p := range NpcProfiles {
		have := true
		for _, l := range p.Lists {
			if _, ok := g.Lists[l]; !ok {
				have = false
			}
		}
		if have {
			p.Enum = int32(NpcClassBase + i)
			g.Classes = append(g.Classes, p)
		}
	}

	// The equations and formulas: every row of the two tables (a few hundred).
	rows, err = sq.Query(`SELECT guid FROM record WHERE type_id = ? ORDER BY guid`, int64(TypeStatEquation))
	if err != nil {
		return nil, err
	}
	var eqs []int64
	for rows.Next() {
		var guid int64
		if err := rows.Scan(&guid); err != nil {
			rows.Close()
			return nil, err
		}
		eqs = append(eqs, guid)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for _, guid := range eqs {
		rec, err := db.Record(TypeStatEquation, guid)
		if err != nil {
			return nil, err
		}
		var f struct {
			Equation    string `json:"Equation"`
			Constraints []struct {
				Key   string `json:"Key"`
				Value struct {
					Min      float64 `json:"Min"`
					Max      float64 `json:"Max"`
					ClampMin bool    `json:"bClampMin"`
					ClampMax bool    `json:"bClampMax"`
					Rounding string  `json:"RoundingRule"`
				} `json:"Value"`
			} `json:"Constraints"`
		}
		if err := json.Unmarshal(rec.Fields, &f); err != nil {
			return nil, fmt.Errorf("equation %d: %w", guid, err)
		}
		if len(f.Constraints) > 1 || (len(f.Constraints) == 1 && !strings.HasSuffix(f.Constraints[0].Value.Rounding, "Rounding_None")) {
			// A constraint that is more than a clamp (several, or a rounding rule) is not modelled: the equation is left
			// out, so a formula that names it fails to compile instead of ignoring the constraint.
			continue
		}
		if len(f.Constraints) == 1 {
			c := f.Constraints[0].Value
			if c.ClampMin || c.ClampMax {
				g.Clamps[guid] = Clamp{HasMin: c.ClampMin, HasMax: c.ClampMax, Min: c.Min, Max: c.Max}
			}
		}
		g.Equations[guid] = f.Equation
	}
	rows, err = sq.Query(`SELECT guid FROM record WHERE type_id = ? ORDER BY guid`, int64(TypeStatFormula))
	if err != nil {
		return nil, err
	}
	var fms []int64
	for rows.Next() {
		var guid int64
		if err := rows.Scan(&guid); err != nil {
			rows.Close()
			return nil, err
		}
		fms = append(fms, guid)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for _, guid := range fms {
		rec, err := db.Record(TypeStatFormula, guid)
		if err != nil {
			return nil, err
		}
		var f struct {
			EquationID struct {
				Guid int64 `json:"Guid"`
			} `json:"EquationId"`
			InputTerms []struct {
				TermID struct {
					Guid int64 `json:"Guid"`
				} `json:"TermId"`
				Value string `json:"Value"`
			} `json:"InputTerms"`
		}
		if err := json.Unmarshal(rec.Fields, &f); err != nil {
			return nil, fmt.Errorf("formula %d: %w", guid, err)
		}
		fm := Formula{Equation: f.EquationID.Guid, Terms: map[int64]string{}}
		for _, t := range f.InputTerms {
			fm.Terms[t.TermID.Guid] = t.Value
		}
		g.Formulas[guid] = fm
	}
	rows, err = sq.Query(`SELECT guid FROM record WHERE type_id = ? ORDER BY guid`, int64(TypeTerm))
	if err != nil {
		return nil, err
	}
	var terms []int64
	for rows.Next() {
		var guid int64
		if err := rows.Scan(&guid); err != nil {
			rows.Close()
			return nil, err
		}
		terms = append(terms, guid)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for _, guid := range terms {
		rec, err := db.Record(TypeTerm, guid)
		if err != nil {
			return nil, err
		}
		var f struct {
			Default  string `json:"DefaultValue"`
			Min      string `json:"Min"`
			Max      string `json:"Max"`
			ClampMin bool   `json:"bClampMin"`
			ClampMax bool   `json:"bClampMax"`
		}
		if err := json.Unmarshal(rec.Fields, &f); err != nil {
			return nil, fmt.Errorf("term %d: %w", guid, err)
		}
		td := TermDef{Default: f.Default}
		num := func(text string) (float64, error) {
			if strings.TrimSpace(text) == "" {
				return 0, nil
			}
			return strconv.ParseFloat(strings.TrimSpace(text), 64)
		}
		if f.ClampMin {
			if td.Clamp.Min, err = num(f.Min); err != nil {
				return nil, fmt.Errorf("term %d: Min %q: %w", guid, f.Min, err)
			}
			td.Clamp.HasMin = true
		}
		if f.ClampMax {
			if td.Clamp.Max, err = num(f.Max); err != nil {
				return nil, fmt.Errorf("term %d: Max %q: %w", guid, f.Max, err)
			}
			td.Clamp.HasMax = true
		}
		g.Terms[guid] = td
	}
	// the city nodes by rank (contentc version 12); a file before it has none, and a node read does not compile
	var hasNodes int
	if err := sq.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'city_node'`).Scan(&hasNodes); err != nil {
		return nil, err
	}
	if hasNodes == 1 {
		nodes, err := db.CityNodes()
		if err != nil {
			return nil, err
		}
		for _, n := range nodes {
			g.Nodes = append(g.Nodes, GraphNode{Guid: n.Guid, Name: n.Name})
		}
	}
	return g, nil
}

// LoadExprs reads the ability expressions of a content.db (the table ab_expr, plan D2) in id order, with their ids. A file with no such table
// (made by an older contentc) has none.
func LoadExprs(path string) ([]Expr, []int64, error) {
	db, err := content.Open(path)
	if err != nil {
		return nil, nil, err
	}
	defer db.Close()
	sq := db.SQL()
	var n int
	if err := sq.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'ab_expr'`).Scan(&n); err != nil {
		return nil, nil, err
	}
	if n == 0 {
		return nil, nil, nil
	}
	rows, err := sq.Query(`SELECT id, text, terms FROM ab_expr ORDER BY id`)
	if err != nil {
		return nil, nil, err
	}
	defer rows.Close()
	var exprs []Expr
	var ids []int64
	for rows.Next() {
		var id int64
		var text, terms string
		if err := rows.Scan(&id, &text, &terms); err != nil {
			return nil, nil, err
		}
		ex := Expr{What: fmt.Sprintf("expression %d", id), Text: text}
		var m map[string]string
		if err := json.Unmarshal([]byte(terms), &m); err != nil {
			return nil, nil, fmt.Errorf("expression %d: terms: %w", id, err)
		}
		if len(m) > 0 {
			ex.Terms = make(map[int64]string, len(m))
			for k, v := range m {
				g, err := strconv.ParseInt(k, 10, 64)
				if err != nil {
					return nil, nil, fmt.Errorf("expression %d: term %q: %w", id, k, err)
				}
				ex.Terms[g] = v
			}
		}
		exprs = append(exprs, ex)
		ids = append(ids, id)
	}
	return exprs, ids, rows.Err()
}
