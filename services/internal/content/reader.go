// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"bytes"
	"compress/gzip"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"

	_ "modernc.org/sqlite" // the pure-Go SQLite driver, registered as "sqlite"

	"aocrop/services/internal/aocexpr"
)

// DB is an opened content.db.
type DB struct {
	sql      *sql.DB
	encoding string // how record.data is stored (meta record.encoding)
}

// Open opens the content.db at path for reading and checks its schema version.
// A path that does not exist is an error and is not created: SQLite would make
// an empty file of it, which a second run would then open as a database with no
// meta table.
func Open(path string) (*DB, error) {
	if _, err := os.Stat(path); err != nil {
		return nil, fmt.Errorf("content: %w", err)
	}
	db, err := sql.Open("sqlite", path+"?_pragma=query_only(1)")
	if err != nil {
		return nil, err
	}
	d := &DB{sql: db}
	meta, err := d.Meta()
	if err != nil {
		db.Close()
		return nil, fmt.Errorf("content: %s: %w", path, err)
	}
	if got := meta[MetaSchemaVersion]; got != strconv.Itoa(SchemaVersion) {
		db.Close()
		return nil, fmt.Errorf("content: %s has schema version %q, this reader wants %d", path, got, SchemaVersion)
	}
	d.encoding = meta[MetaRecordEncoding]
	return d, nil
}

// built by C.O.A with Cyb

// Close closes the database.
func (d *DB) Close() error { return d.sql.Close() }

// SQL is the underlying handle, for queries the typed ones do not cover.
func (d *DB) SQL() *sql.DB { return d.sql }

// Meta returns the meta table.
func (d *DB) Meta() (map[string]string, error) {
	rows, err := d.sql.Query(`SELECT key, value FROM meta`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]string{}
	for rows.Next() {
		var k, v string
		if err := rows.Scan(&k, &v); err != nil {
			return nil, err
		}
		out[k] = v
	}
	return out, rows.Err()
}

// RecordTable is a table of the design database and what became of it.
type RecordTable struct {
	Index   int
	TypeID  uint64
	Schema  string // "" when the table is not identified
	Rows    int    // the directory's count
	Decoded bool   // every row is in record
}

// RecordTables lists the design database's tables.
func (d *DB) RecordTables() ([]RecordTable, error) {
	rows, err := d.sql.Query(`SELECT table_index, type_id, schema, rows, decoded FROM record_table ORDER BY table_index`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []RecordTable
	for rows.Next() {
		var t RecordTable
		var tid int64
		if err := rows.Scan(&t.Index, &tid, &t.Schema, &t.Rows, &t.Decoded); err != nil {
			return nil, err
		}
		t.TypeID = uint64(tid)
		out = append(out, t)
	}
	return out, rows.Err()
}

// Record is a row of the design database.
type Record struct {
	TableIndex int
	TypeID     uint64
	Schema     string
	Guid       int64
	ParentGuid int64
	Name       string
	DevOnly    bool
	// Fields is the row's properties as JSON (see the record table).
	Fields json.RawMessage
}

// Record returns the row of type typeID with the Guid (the pair is unique: the
// record table has a constraint on it).
func (d *DB) Record(typeID uint64, guid int64) (Record, error) {
	var r Record
	var tid int64
	var data []byte
	err := d.sql.QueryRow(`SELECT table_index, type_id, schema, guid, parent_guid, name, dev_only, data
		FROM record WHERE type_id = ? AND guid = ?`, int64(typeID), guid).
		Scan(&r.TableIndex, &tid, &r.Schema, &r.Guid, &r.ParentGuid, &r.Name, &r.DevOnly, &data)
	if errors.Is(err, sql.ErrNoRows) {
		return Record{}, fmt.Errorf("content: no record %d of type %d", guid, typeID)
	}
	if err != nil {
		return Record{}, err
	}
	r.TypeID = uint64(tid)
	if r.Fields, err = DecodeData(d.encoding, data); err != nil {
		return Record{}, err
	}
	return r, nil
}

// == C-O-A | C.Y.B ==

// EncodeData is how a row's JSON is stored under an encoding.
func EncodeData(encoding string, js []byte) ([]byte, error) {
	switch encoding {
	case EncodingJSON:
		return js, nil
	case EncodingGzip:
		var b bytes.Buffer
		zw, _ := gzip.NewWriterLevel(&b, gzip.BestSpeed)
		if _, err := zw.Write(js); err != nil {
			return nil, err
		}
		if err := zw.Close(); err != nil {
			return nil, err
		}
		return b.Bytes(), nil
	}
	return nil, fmt.Errorf("content: unknown record encoding %q", encoding)
}

// DecodeData is the inverse of EncodeData.
func DecodeData(encoding string, data []byte) (json.RawMessage, error) {
	switch encoding {
	case EncodingJSON:
		return data, nil
	case EncodingGzip:
		zr, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			return nil, err
		}
		defer zr.Close()
		return io.ReadAll(zr)
	}
	return nil, fmt.Errorf("content: unknown record encoding %q", encoding)
}

// StatDef is a StatTypeDef row.
type StatDef struct {
	Guid                int64
	Name                string
	GameplayType        string
	StatType            string
	Replication         int
	Audience            string
	RepField            string // "" when the capture does not show it
	BaseExpr            string
	MinExpr             string
	MaxExpr             string
	RegenExpr           string
	OnSpawn             string
	OnDeath             string
	OnRevive            string
	PersistValue        bool
	Hidden              bool
	ClampFinalValue     bool
	MultiplyMods        bool
	RegenWhileDead      bool
	RecalcOnCombatState bool
}

const statDefColumns = `guid, name, gameplay_type, stat_type, replication, audience, rep_field, base_expr, min_expr,
	max_expr, regen_expr, on_spawn, on_death, on_revive, persist_value, hidden, clamp_final_value, multiply_mods,
	regen_while_dead, recalc_on_combat_state`

func scanStatDef(sc interface{ Scan(...any) error }) (StatDef, error) {
	var s StatDef
	var rep sql.NullString
	err := sc.Scan(&s.Guid, &s.Name, &s.GameplayType, &s.StatType, &s.Replication, &s.Audience, &rep, &s.BaseExpr,
		&s.MinExpr, &s.MaxExpr, &s.RegenExpr, &s.OnSpawn, &s.OnDeath, &s.OnRevive, &s.PersistValue, &s.Hidden,
		&s.ClampFinalValue, &s.MultiplyMods, &s.RegenWhileDead, &s.RecalcOnCombatState)
	s.RepField = rep.String
	return s, err
}

// StatDef returns the stat with the Guid.
func (d *DB) StatDef(guid int64) (StatDef, error) {
	s, err := scanStatDef(d.sql.QueryRow(`SELECT `+statDefColumns+` FROM stat_def WHERE guid = ?`, guid))
	if errors.Is(err, sql.ErrNoRows) {
		return StatDef{}, fmt.Errorf("content: no stat %d", guid)
	}
	return s, err
}

// StatDefByName returns the stat called name; it is an error unless exactly one is.
func (d *DB) StatDefByName(name string) (StatDef, error) {
	rows, err := d.sql.Query(`SELECT `+statDefColumns+` FROM stat_def WHERE name = ?`, name)
	if err != nil {
		return StatDef{}, err
	}
	defer rows.Close()
	var out []StatDef
	for rows.Next() {
		s, err := scanStatDef(rows)
		if err != nil {
			return StatDef{}, err
		}
		out = append(out, s)
	}
	if err := rows.Err(); err != nil {
		return StatDef{}, err
	}
	if len(out) != 1 {
		return StatDef{}, fmt.Errorf("content: %d stats are called %q", len(out), name)
	}
	return out[0], nil
}

// StatInit is one entry of a StatInitializerList.
type StatInit struct {
	ListGuid   int64
	ListName   string
	Ord        int
	StatGuid   int64
	StatName   string
	Expression string
}

// StatInits returns the entries of the list called name, in order.
func (d *DB) StatInits(list string) ([]StatInit, error) {
	rows, err := d.sql.Query(`SELECT list_guid, list_name, ord, stat_guid, stat_name, expression
		FROM stat_init WHERE list_name = ? ORDER BY ord`, list)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []StatInit
	for rows.Next() {
		var s StatInit
		if err := rows.Scan(&s.ListGuid, &s.ListName, &s.Ord, &s.StatGuid, &s.StatName, &s.Expression); err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// ~ C:Y:B - C-O-A ~

// Curves loads every StatCurve with keys into a table EvalCurve can use.
func (d *DB) Curves() (*aocexpr.CurveMap, error) {
	rows, err := d.sql.Query(`SELECT guid, level, value, interp, arrive_tangent, leave_tangent FROM stat_curve ORDER BY guid, ord`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	keys := map[int64][]aocexpr.Key{}
	for rows.Next() {
		var guid int64
		var k aocexpr.Key
		var interp string
		if err := rows.Scan(&guid, &k.Time, &k.Value, &interp, &k.Arrive, &k.Leave); err != nil {
			return nil, err
		}
		if k.Interp, err = aocexpr.ParseInterp(interp); err != nil {
			return nil, err
		}
		keys[guid] = append(keys[guid], k)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	m := &aocexpr.CurveMap{TypeID: TypeStatCurve, Curves: map[int64]*aocexpr.Curve{}}
	for guid, ks := range keys {
		c, err := aocexpr.NewCurve(ks)
		if err != nil {
			return nil, fmt.Errorf("content: curve %d: %w", guid, err)
		}
		m.Curves[guid] = c
	}
	return m, nil
}

// Class is a player class.
type Class struct {
	Guid             int64
	Name             string
	ClassEnum        string
	ArchetypeID      int64
	LevelTypeGuid    int64
	LevelTypeName    string
	SkillTreeGuid    int64
	SkillTreeName    string
	PrefersMelee     bool
	StatInitListGuid int64
	StatInitListName string
}

// Classes returns the player classes in archetype order.
func (d *DB) Classes() ([]Class, error) {
	rows, err := d.sql.Query(`SELECT guid, name, class_enum, archetype_id, level_type_guid, level_type_name,
		skill_tree_guid, skill_tree_name, prefers_melee, stat_init_list_guid, stat_init_list_name
		FROM class ORDER BY archetype_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Class
	for rows.Next() {
		var c Class
		if err := rows.Scan(&c.Guid, &c.Name, &c.ClassEnum, &c.ArchetypeID, &c.LevelTypeGuid, &c.LevelTypeName,
			&c.SkillTreeGuid, &c.SkillTreeName, &c.PrefersMelee, &c.StatInitListGuid, &c.StatInitListName); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// StatValue is a computed attribute value.
type StatValue struct {
	Value float64
	// RoundingAmbiguous: evaluating the formula in float32, the engine's likely
	// arithmetic, rounds to another integer than Value does (class_stat_base's
	// rounding_ambiguous). The data does not say which the engine does.
	RoundingAmbiguous bool
}

// ClassStatBase returns the primary attributes of the class (by name) at a
// level, by stat name.
func (d *DB) ClassStatBase(class string, level int) (map[string]StatValue, error) {
	rows, err := d.sql.Query(`SELECT stat_name, value, rounding_ambiguous FROM class_stat_base WHERE class_name = ? AND level = ?`, class, level)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]StatValue{}
	for rows.Next() {
		var name string
		var v StatValue
		if err := rows.Scan(&name, &v.Value, &v.RoundingAmbiguous); err != nil {
			return nil, err
		}
		out[name] = v
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("content: no base stats for class %q at level %d", class, level)
	}
	return out, nil
}

// BaseValue is a class_stat_base row.
type BaseValue struct {
	Class string
	Level int
	Stat  string
	Value float64
}

// RoundingAmbiguous lists the class_stat_base rows whose integer depends on
// the arithmetic's width: float32 gives another value than the stored one.
func (d *DB) RoundingAmbiguous() ([]BaseValue, error) {
	rows, err := d.sql.Query(`SELECT class_name, level, stat_name, value FROM class_stat_base
		WHERE rounding_ambiguous = 1 ORDER BY class_name, stat_name, level`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []BaseValue
	for rows.Next() {
		var b BaseValue
		if err := rows.Scan(&b.Class, &b.Level, &b.Stat, &b.Value); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

// NamedGuid is a record reference with its name.
type NamedGuid struct {
	Ord  int
	Guid int64
	Name string
}

// AbilityCost is one stat cost of an ability.
type AbilityCost struct {
	CostOrd          int
	StatOrd          int
	EnabledExpr      string
	TriggersCooldown bool
	ChargeCostExpr   string
	StatGuid         int64
	StatName         string
	Expression       string
}

// Cyb with C O A

// Ability is an ability row with its costs, effects and hits.
type Ability struct {
	Guid                   int64
	Name                   string
	DisplayName            string
	Tags                   []string
	ActivationRequirements string
	CooldownExpr           string
	DisableDuringCooldown  bool
	SharedCooldownTag      string
	HeldBehavior           string
	OverrideHeldBehavior   bool
	HoldAnimation          bool
	MaxHoldTime            float64
	ComboInputs            []string
	ActivationInput        string
	UsableInAir            bool
	UsableWhileSwimming    bool
	UsableWhileWalking     bool
	UsableWhileMounted     bool
	NetworkingMaxSpeed     float64

	Costs   []AbilityCost
	Effects []NamedGuid
	Hits    []NamedGuid
}

// Ability returns the ability with the Guid.
func (d *DB) Ability(guid int64) (Ability, error) {
	return d.ability(`guid = ?`, guid)
}

// AbilityByName returns the ability called name.
func (d *DB) AbilityByName(name string) (Ability, error) {
	return d.ability(`name = ?`, name)
}

func (d *DB) ability(where string, arg any) (Ability, error) {
	var a Ability
	var tags, combo string
	err := d.sql.QueryRow(`SELECT guid, name, display_name, tags, activation_requirements, cooldown_expr,
		disable_during_cooldown, shared_cooldown_tag, held_behavior, override_held_behavior, hold_animation,
		max_hold_time, combo_inputs, activation_input, usable_in_air, usable_while_swimming, usable_while_walking,
		usable_while_mounted, networking_max_speed FROM ability WHERE `+where, arg).
		Scan(&a.Guid, &a.Name, &a.DisplayName, &tags, &a.ActivationRequirements, &a.CooldownExpr,
			&a.DisableDuringCooldown, &a.SharedCooldownTag, &a.HeldBehavior, &a.OverrideHeldBehavior, &a.HoldAnimation,
			&a.MaxHoldTime, &combo, &a.ActivationInput, &a.UsableInAir, &a.UsableWhileSwimming, &a.UsableWhileWalking,
			&a.UsableWhileMounted, &a.NetworkingMaxSpeed)
	if errors.Is(err, sql.ErrNoRows) {
		return Ability{}, fmt.Errorf("content: no ability for %s (%v)", where, arg)
	}
	if err != nil {
		return Ability{}, err
	}
	if err := json.Unmarshal([]byte(tags), &a.Tags); err != nil {
		return Ability{}, fmt.Errorf("content: ability %d tags: %w", a.Guid, err)
	}
	if err := json.Unmarshal([]byte(combo), &a.ComboInputs); err != nil {
		return Ability{}, fmt.Errorf("content: ability %d combo inputs: %w", a.Guid, err)
	}

	rows, err := d.sql.Query(`SELECT cost_ord, stat_ord, enabled_expr, triggers_cooldown, charge_cost_expr, stat_guid,
		stat_name, expression FROM ability_cost WHERE ability_guid = ? ORDER BY cost_ord, stat_ord`, a.Guid)
	if err != nil {
		return Ability{}, err
	}
	for rows.Next() {
		var c AbilityCost
		if err := rows.Scan(&c.CostOrd, &c.StatOrd, &c.EnabledExpr, &c.TriggersCooldown, &c.ChargeCostExpr,
			&c.StatGuid, &c.StatName, &c.Expression); err != nil {
			rows.Close()
			return Ability{}, err
		}
		a.Costs = append(a.Costs, c)
	}
	rows.Close()
	if a.Effects, err = d.namedGuids(`SELECT ord, effect_guid, effect_name FROM ability_effect WHERE ability_guid = ? ORDER BY ord`, a.Guid); err != nil {
		return Ability{}, err
	}
	if a.Hits, err = d.namedGuids(`SELECT ord, hit_guid, hit_name FROM ability_hit WHERE ability_guid = ? ORDER BY ord`, a.Guid); err != nil {
		return Ability{}, err
	}
	return a, nil
}

func (d *DB) namedGuids(query string, args ...any) ([]NamedGuid, error) {
	rows, err := d.sql.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []NamedGuid
	for rows.Next() {
		var n NamedGuid
		if err := rows.Scan(&n.Ord, &n.Guid, &n.Name); err != nil {
			return nil, err
		}
		out = append(out, n)
	}
	return out, rows.Err()
}

// CoA : C_Y_B

// StatMod is a StatMod row an effect or hit uses.
type StatMod struct {
	Ord         int
	StatModGuid int64
	StatModName string
	StatGuid    int64
	StatName    string
	ValueExpr   string
}

// EffectHit is a hit an effect fires in one of its phases.
type EffectHit struct {
	Phase string
	Ord   int
	Guid  int64
	Name  string
}

// Effect is an effect row with its hits and stat mods.
type Effect struct {
	Guid                    int64
	Name                    string
	DisplayName             string
	Category                string
	Tags                    []string
	EffectTags              []string
	ApplicationRequirements string
	InfiniteDuration        bool
	DurationExpr            string
	TickTimer               float64
	IgnoreInitialTick       bool
	StackType               string
	GlobalStackLimit        int
	StackDurationRefresh    string
	PersistsThroughDeath    bool
	PersistsRelog           bool
	Dispellable             bool
	Significant             bool

	Hits     []EffectHit
	StatMods []StatMod
}

// Effect returns the effect with the Guid.
func (d *DB) Effect(guid int64) (Effect, error) { return d.effect(`guid = ?`, guid) }

// EffectByName returns the effect called name.
func (d *DB) EffectByName(name string) (Effect, error) { return d.effect(`name = ?`, name) }

func (d *DB) effect(where string, arg any) (Effect, error) {
	var e Effect
	var tags, etags string
	err := d.sql.QueryRow(`SELECT guid, name, display_name, category, tags, effect_tags, application_requirements,
		infinite_duration, duration_expr, tick_timer, ignore_initial_tick, stack_type, global_stack_limit,
		stack_duration_refresh, persists_through_death, persists_relog, dispellable, significant
		FROM effect WHERE `+where, arg).
		Scan(&e.Guid, &e.Name, &e.DisplayName, &e.Category, &tags, &etags, &e.ApplicationRequirements,
			&e.InfiniteDuration, &e.DurationExpr, &e.TickTimer, &e.IgnoreInitialTick, &e.StackType, &e.GlobalStackLimit,
			&e.StackDurationRefresh, &e.PersistsThroughDeath, &e.PersistsRelog, &e.Dispellable, &e.Significant)
	if errors.Is(err, sql.ErrNoRows) {
		return Effect{}, fmt.Errorf("content: no effect for %s (%v)", where, arg)
	}
	if err != nil {
		return Effect{}, err
	}
	if err := json.Unmarshal([]byte(tags), &e.Tags); err != nil {
		return Effect{}, fmt.Errorf("content: effect %d tags: %w", e.Guid, err)
	}
	if err := json.Unmarshal([]byte(etags), &e.EffectTags); err != nil {
		return Effect{}, fmt.Errorf("content: effect %d effect tags: %w", e.Guid, err)
	}
	hits, err := d.sql.Query(`SELECT phase, ord, hit_guid, hit_name FROM effect_hit WHERE effect_guid = ?
		ORDER BY CASE phase WHEN 'PreApply' THEN 0 WHEN 'Apply' THEN 1 WHEN 'Tick' THEN 2 WHEN 'Consume' THEN 3
		WHEN 'Remove' THEN 4 WHEN 'Expire' THEN 5 ELSE 6 END, ord`, e.Guid)
	if err != nil {
		return Effect{}, err
	}
	for hits.Next() {
		var h EffectHit
		if err := hits.Scan(&h.Phase, &h.Ord, &h.Guid, &h.Name); err != nil {
			hits.Close()
			return Effect{}, err
		}
		e.Hits = append(e.Hits, h)
	}
	hits.Close()
	if e.StatMods, err = d.StatMods("effect", e.Guid); err != nil {
		return Effect{}, err
	}
	return e, nil
}

// StatMods returns the stat mods of an effect (kind "effect") or hit ("hit").
func (d *DB) StatMods(kind string, guid int64) ([]StatMod, error) {
	rows, err := d.sql.Query(`SELECT ord, stat_mod_guid, stat_mod_name, stat_guid, stat_name, value_expr
		FROM owner_stat_mod WHERE owner_kind = ? AND owner_guid = ? ORDER BY ord`, kind, guid)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []StatMod
	for rows.Next() {
		var m StatMod
		if err := rows.Scan(&m.Ord, &m.StatModGuid, &m.StatModName, &m.StatGuid, &m.StatName, &m.ValueExpr); err != nil {
			return nil, err
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// Rule is a value of the stamina_rule table with where it comes from.
type Rule struct {
	Value  string
	Source string
}

// Float parses the rule's value as a number.
func (r Rule) Float() (float64, error) { return strconv.ParseFloat(r.Value, 64) }

// StaminaRules returns the stamina_rule table by key.
func (d *DB) StaminaRules() (map[string]Rule, error) {
	rows, err := d.sql.Query(`SELECT key, value, source FROM stamina_rule`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]Rule{}
	for rows.Next() {
		var k string
		var r Rule
		if err := rows.Scan(&k, &r.Value, &r.Source); err != nil {
			return nil, err
		}
		out[k] = r
	}
	return out, rows.Err()
}

// RegenDisable is an effect that stops stamina regenerating.
type RegenDisable struct {
	Ord              int
	EffectGuid       int64
	EffectName       string
	DisablesStanding bool
	DisablesMoving   bool
}

// StaminaRegenDisables returns the effects that stop stamina regenerating.
func (d *DB) StaminaRegenDisables() ([]RegenDisable, error) {
	rows, err := d.sql.Query(`SELECT ord, effect_guid, effect_name, disables_standing, disables_moving
		FROM stamina_regen_disable ORDER BY ord`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []RegenDisable
	for rows.Next() {
		var r RegenDisable
		if err := rows.Scan(&r.Ord, &r.EffectGuid, &r.EffectName, &r.DisablesStanding, &r.DisablesMoving); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// (C:Y:B : C-O-A)

// AbilityRuntime is a row of ability_runtime: what worldd runs for an ability, as numbers (plan A7).
type AbilityRuntime struct {
	AbilityGuid           int64
	Name                  string
	HeldBehavior          string
	RequiresOutOfCombat   bool
	MinStamina            float64
	CostStatGuid          int64
	CostAmount            float64
	CostRequiresMoving    bool
	ChargeInterval        float64
	Cooldown              float64
	TriggersCooldown      bool
	DisableDuringCooldown bool
	TargetsSelf           bool
	EffectGuid            int64
	MontagePath           string
	CancelSection         string
	Source                string
}

// AbilityRuntimes returns the ability_runtime table by ability Guid.
func (d *DB) AbilityRuntimes() (map[int64]AbilityRuntime, error) {
	rows, err := d.sql.Query(`SELECT ability_guid, name, held_behavior, requires_out_of_combat, min_stamina, cost_stat_guid,
		cost_amount, cost_requires_moving, charge_interval, cooldown, triggers_cooldown, disable_during_cooldown, targets_self,
		effect_guid, montage_path, cancel_section, source FROM ability_runtime`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]AbilityRuntime{}
	for rows.Next() {
		var r AbilityRuntime
		if err := rows.Scan(&r.AbilityGuid, &r.Name, &r.HeldBehavior, &r.RequiresOutOfCombat, &r.MinStamina, &r.CostStatGuid,
			&r.CostAmount, &r.CostRequiresMoving, &r.ChargeInterval, &r.Cooldown, &r.TriggersCooldown, &r.DisableDuringCooldown,
			&r.TargetsSelf, &r.EffectGuid, &r.MontagePath, &r.CancelSection, &r.Source); err != nil {
			return nil, err
		}
		out[r.AbilityGuid] = r
	}
	return out, rows.Err()
}

// EffectRuntime is a row of effect_runtime.
type EffectRuntime struct {
	EffectGuid         int64
	Name               string
	Infinite           bool
	Duration           float64
	TickTimer          float64
	StopsRegenStanding bool
	StopsRegenMoving   bool
	EndEffectGuid      int64
	EndEffectDuration  float64
	Source             string
}

// EffectRuntimes returns the effect_runtime table by effect Guid.
func (d *DB) EffectRuntimes() (map[int64]EffectRuntime, error) {
	rows, err := d.sql.Query(`SELECT effect_guid, name, infinite, duration, tick_timer, stops_regen_standing, stops_regen_moving,
		end_effect_guid, end_effect_duration, source FROM effect_runtime`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]EffectRuntime{}
	for rows.Next() {
		var r EffectRuntime
		if err := rows.Scan(&r.EffectGuid, &r.Name, &r.Infinite, &r.Duration, &r.TickTimer, &r.StopsRegenStanding,
			&r.StopsRegenMoving, &r.EndEffectGuid, &r.EndEffectDuration, &r.Source); err != nil {
			return nil, err
		}
		out[r.EffectGuid] = r
	}
	return out, rows.Err()
}

// EffectStatRuntime is a row of effect_stat_runtime: an effect adds Factor * the owner's SourceStat + Addend to Stat
// (SourceStatGuid 0: a constant).
type EffectStatRuntime struct {
	EffectGuid     int64
	Ord            int
	StatGuid       int64
	StatName       string
	SourceStatGuid int64
	SourceStatName string
	Factor         float64
	Addend         float64
}

// EffectStatRuntimes returns the effect_stat_runtime table by effect Guid, in ord order.
func (d *DB) EffectStatRuntimes() (map[int64][]EffectStatRuntime, error) {
	rows, err := d.sql.Query(`SELECT effect_guid, ord, stat_guid, stat_name, source_stat_guid, source_stat_name, factor, addend
		FROM effect_stat_runtime ORDER BY effect_guid, ord`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64][]EffectStatRuntime{}
	for rows.Next() {
		var r EffectStatRuntime
		if err := rows.Scan(&r.EffectGuid, &r.Ord, &r.StatGuid, &r.StatName, &r.SourceStatGuid, &r.SourceStatName, &r.Factor, &r.Addend); err != nil {
			return nil, err
		}
		out[r.EffectGuid] = append(out[r.EffectGuid], r)
	}
	return out, rows.Err()
}
