// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
)

// The typed readers of the ability tables (abilities.go has the tables). A field that ends in Expr is an ab_expr id, 0 for none.

// Expr is an ab_expr row.
type Expr struct {
	ID         int64
	Text       string
	Terms      map[int64]string // a StatMod's input terms by term guid
	Program    int              // the program in the stat bundle's Exprs, -1 when there is none
	UsesTarget bool
	Error      string
}

// Expr returns the expression with the id.
func (d *DB) Expr(id int64) (Expr, error) {
	var e Expr
	var terms string
	var target int
	err := d.sql.QueryRow(`SELECT id, text, terms, program, uses_target, error FROM ab_expr WHERE id = ?`, id).Scan(&e.ID, &e.Text, &terms, &e.Program, &target, &e.Error)
	if errors.Is(err, sql.ErrNoRows) {
		return Expr{}, fmt.Errorf("content: no expression %d", id)
	}
	if err != nil {
		return Expr{}, err
	}
	e.UsesTarget = target != 0
	var m map[string]string
	if err := json.Unmarshal([]byte(terms), &m); err != nil {
		return Expr{}, fmt.Errorf("content: expression %d terms: %w", id, err)
	}
	if len(m) > 0 {
		e.Terms = make(map[int64]string, len(m))
		for k, v := range m {
			g, err := strconv.ParseInt(k, 10, 64)
			if err != nil {
				return Expr{}, fmt.Errorf("content: expression %d term %q: %w", id, k, err)
			}
			e.Terms[g] = v
		}
	}
	return e, nil
}

// ClassAbilityGuids returns the abilities the ranks of a class's tree grant (the tree the `class` row of that name names; plan D1's
// skill_node and skill_rank: the one table of ranks), in guid order. Talent slots count: every option's ranks.
func (d *DB) ClassAbilityGuids(className string) ([]int64, error) {
	rows, err := d.sql.Query(`SELECT DISTINCT r.ability_guid FROM class c JOIN skill_node n ON n.tree_guid = c.skill_tree_guid
		JOIN skill_rank r ON r.skill_guid = n.skill_guid WHERE c.name = ? AND r.ability_guid <> 0 ORDER BY r.ability_guid`, className)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			return nil, err
		}
		out = append(out, g)
	}
	return out, rows.Err()
}

// (CoA ~ C_Y_B)

// AbilityRow is an ab_ability row.
type AbilityRow struct {
	Guid                  int64
	Name, DisplayName     string
	Tags                  []string
	HeldBehavior          string
	ActivationInput       string
	TargetRequirement     string // ...::Always: a target is needed
	FallbackTarget        string
	ApplicableTargets     int // EAffectedTarget mask: Self 1, Enemy 2, Ally 4, Party 8, Raid 16
	ApplicableTypes       int
	WeaponRequirement     string
	SharedCooldownTag     string
	DisableDuringCooldown bool
	MaxHoldTime           float64
	HoldAnimation         bool
	UsableInAir           bool
	UsableWhileSwimming   bool
	UsableWhileWalking    bool
	UsableWhileMounted    bool
	UsableFromHotbar      bool
	AreaRange, AreaRadius float64
	NetworkingMaxSpeed    float64
	CooldownExpr          int64 // seconds
	ChargesExpr           int64 // the charges (none: one)
	CastExpr              int64 // seconds (none: the montage's)
	RequirementExpr       int64
	CanBreakExpr          int64
	HasProcExpr           int64
	Extra                 string // JSON: ModifyCooldownsIds, ReactionsIds, TeleportRecordsIds
}

// AbilityRow returns the ab_ability row with the guid; AbilityRowByName the one called name.
func (d *DB) AbilityRow(guid int64) (AbilityRow, error) { return d.abilityRow(`guid = ?`, guid) }

func (d *DB) AbilityRowByName(name string) (AbilityRow, error) { return d.abilityRow(`name = ?`, name) }

func (d *DB) abilityRow(where string, arg any) (AbilityRow, error) {
	var a AbilityRow
	var tags string
	var disable, hold, air, swim, walk, mounted, hotbar int
	err := d.sql.QueryRow(`SELECT guid, name, display_name, tags, held_behavior, activation_input, target_requirement, fallback_target,
		applicable_targets, applicable_types, weapon_requirement, shared_cooldown_tag, disable_during_cooldown, max_hold_time, hold_animation,
		usable_in_air, usable_while_swimming, usable_while_walking, usable_while_mounted, usable_from_hotbar, area_range, area_radius,
		networking_max_speed, cooldown_expr, charges_expr, cast_expr, requirement_expr, can_break_expr, has_proc_expr, extra
		FROM ab_ability WHERE `+where, arg).
		Scan(&a.Guid, &a.Name, &a.DisplayName, &tags, &a.HeldBehavior, &a.ActivationInput, &a.TargetRequirement, &a.FallbackTarget,
			&a.ApplicableTargets, &a.ApplicableTypes, &a.WeaponRequirement, &a.SharedCooldownTag, &disable, &a.MaxHoldTime, &hold,
			&air, &swim, &walk, &mounted, &hotbar, &a.AreaRange, &a.AreaRadius,
			&a.NetworkingMaxSpeed, &a.CooldownExpr, &a.ChargesExpr, &a.CastExpr, &a.RequirementExpr, &a.CanBreakExpr, &a.HasProcExpr, &a.Extra)
	if errors.Is(err, sql.ErrNoRows) {
		return AbilityRow{}, fmt.Errorf("content: no ab_ability for %s (%v)", where, arg)
	}
	if err != nil {
		return AbilityRow{}, err
	}
	a.DisableDuringCooldown, a.HoldAnimation, a.UsableInAir = disable != 0, hold != 0, air != 0
	a.UsableWhileSwimming, a.UsableWhileWalking, a.UsableWhileMounted, a.UsableFromHotbar = swim != 0, walk != 0, mounted != 0, hotbar != 0
	if err := json.Unmarshal([]byte(tags), &a.Tags); err != nil {
		return AbilityRow{}, fmt.Errorf("content: ability %d tags: %w", a.Guid, err)
	}
	return a, nil
}

// CostRow is an ab_cost row: one stat cost of a cost group (StatOrd -1 for a group that has none).
type CostRow struct {
	CostOrd, StatOrd int
	EnabledExpr      int64
	TriggersCooldown bool
	ChargeCostExpr   int64
	StatGuid         int64
	StatName         string
	AmountExpr       int64
}

// AbilityCosts returns the costs of an ability in order.
func (d *DB) AbilityCosts(ability int64) ([]CostRow, error) {
	rows, err := d.sql.Query(`SELECT cost_ord, stat_ord, enabled_expr, triggers_cooldown, charge_cost_expr, stat_guid, stat_name, amount_expr
		FROM ab_cost WHERE ability_guid = ? ORDER BY cost_ord, stat_ord`, ability)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []CostRow
	for rows.Next() {
		var c CostRow
		var trig int
		if err := rows.Scan(&c.CostOrd, &c.StatOrd, &c.EnabledExpr, &trig, &c.ChargeCostExpr, &c.StatGuid, &c.StatName, &c.AmountExpr); err != nil {
			return nil, err
		}
		c.TriggersCooldown = trig != 0
		out = append(out, c)
	}
	return out, rows.Err()
}

// EffectCostRow is an ab_effect_cost row: an effect the ability consumes (EffectGuid 0 for a filter of another shape).
type EffectCostRow struct {
	CostOrd, Ord int
	EffectGuid   int64
	Filter       string
	NumExpr      int64
}

// AbilityEffectCosts returns the effects an ability consumes.
func (d *DB) AbilityEffectCosts(ability int64) ([]EffectCostRow, error) {
	rows, err := d.sql.Query(`SELECT cost_ord, ord, effect_guid, filter, num_expr FROM ab_effect_cost WHERE ability_guid = ? ORDER BY cost_ord, ord`, ability)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []EffectCostRow
	for rows.Next() {
		var c EffectCostRow
		if err := rows.Scan(&c.CostOrd, &c.Ord, &c.EffectGuid, &c.Filter, &c.NumExpr); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// MontageRow is an ab_montage row: the montage of an AnimationIndex.
type MontageRow struct {
	AnimIndex    int
	SoftPath     string
	Paths        []string
	Direction    string
	SelectExpr   int64
	StartSection string
	HitKeys      []int64
	TargetingKey int64
	NetGuid      *uint64 // nil: no retail capture names the montage
}

// (CoA : C | Y | B)

// AbilityMontages returns the montages of an ability by AnimationIndex.
func (d *DB) AbilityMontages(ability int64) ([]MontageRow, error) {
	rows, err := d.sql.Query(`SELECT anim_index, soft_path, paths, direction, select_expr, start_section, hit_keys, targeting_key, net_guid
		FROM ab_montage WHERE ability_guid = ? ORDER BY anim_index`, ability)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []MontageRow
	for rows.Next() {
		var m MontageRow
		var paths, keys string
		var net sql.NullInt64
		if err := rows.Scan(&m.AnimIndex, &m.SoftPath, &paths, &m.Direction, &m.SelectExpr, &m.StartSection, &keys, &m.TargetingKey, &net); err != nil {
			return nil, err
		}
		if err := json.Unmarshal([]byte(paths), &m.Paths); err != nil {
			return nil, fmt.Errorf("content: ability %d montage %d paths: %w", ability, m.AnimIndex, err)
		}
		if err := json.Unmarshal([]byte(keys), &m.HitKeys); err != nil {
			return nil, fmt.Errorf("content: ability %d montage %d hit keys: %w", ability, m.AnimIndex, err)
		}
		if net.Valid {
			g := uint64(net.Int64)
			m.NetGuid = &g
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// EffectRow is an ab_effect row.
type EffectRow struct {
	Guid                  int64
	Name, DisplayName     string
	Category              string
	Tags, EffectTags      []string
	InfiniteDuration      bool
	DurationExpr          int64 // seconds
	TickTimer             float64
	IgnoreInitialTick     bool
	StackType             string
	GlobalStackLimit      int
	InPlaceStackLimitExpr int64
	StackDurationRefresh  string
	PersistsThroughDeath  bool
	PersistsRelog         bool
	Dispellable           bool
	Significant           bool
	RequirementExpr       int64
	CrowdControlGuid      int64
	Extra                 string
}

// EffectRow returns the ab_effect row with the guid; EffectRowByName the one called name.
func (d *DB) EffectRow(guid int64) (EffectRow, error) { return d.effectRow(`guid = ?`, guid) }

func (d *DB) EffectRowByName(name string) (EffectRow, error) { return d.effectRow(`name = ?`, name) }

func (d *DB) effectRow(where string, arg any) (EffectRow, error) {
	var e EffectRow
	var tags, etags string
	var inf, ign, pd, pr, dis, sig int
	err := d.sql.QueryRow(`SELECT guid, name, display_name, category, tags, effect_tags, infinite_duration, duration_expr, tick_timer, ignore_initial_tick,
		stack_type, global_stack_limit, in_place_stack_limit_expr, stack_duration_refresh, persists_through_death, persists_relog, dispellable, significant,
		requirement_expr, crowd_control_guid, extra FROM ab_effect WHERE `+where, arg).
		Scan(&e.Guid, &e.Name, &e.DisplayName, &e.Category, &tags, &etags, &inf, &e.DurationExpr, &e.TickTimer, &ign,
			&e.StackType, &e.GlobalStackLimit, &e.InPlaceStackLimitExpr, &e.StackDurationRefresh, &pd, &pr, &dis, &sig,
			&e.RequirementExpr, &e.CrowdControlGuid, &e.Extra)
	if errors.Is(err, sql.ErrNoRows) {
		return EffectRow{}, fmt.Errorf("content: no ab_effect for %s (%v)", where, arg)
	}
	if err != nil {
		return EffectRow{}, err
	}
	e.InfiniteDuration, e.IgnoreInitialTick, e.PersistsThroughDeath, e.PersistsRelog, e.Dispellable, e.Significant = inf != 0, ign != 0, pd != 0, pr != 0, dis != 0, sig != 0
	if err := json.Unmarshal([]byte(tags), &e.Tags); err != nil {
		return EffectRow{}, fmt.Errorf("content: effect %d tags: %w", e.Guid, err)
	}
	if err := json.Unmarshal([]byte(etags), &e.EffectTags); err != nil {
		return EffectRow{}, fmt.Errorf("content: effect %d effect tags: %w", e.Guid, err)
	}
	return e, nil
}

// ModRow is an ab_mod row: a stat mod of an effect or a hit, with the ab_expr id of its value.
type ModRow struct {
	Ord          int
	StatModGuid  int64
	StatGuid     int64
	StatName     string
	ValueExpr    int64
	ShieldAbsorb bool
}

// == C:Y:B | C | O | A ==

// Mods returns the stat mods of an effect (kind "effect") or a hit ("hit") in order.
func (d *DB) Mods(kind string, owner int64) ([]ModRow, error) {
	rows, err := d.sql.Query(`SELECT ord, stat_mod_guid, stat_guid, stat_name, value_expr, shield_absorb FROM ab_mod WHERE owner_kind = ? AND owner_guid = ? ORDER BY ord`, kind, owner)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ModRow
	for rows.Next() {
		var m ModRow
		var sh int
		if err := rows.Scan(&m.Ord, &m.StatModGuid, &m.StatGuid, &m.StatName, &m.ValueExpr, &sh); err != nil {
			return nil, err
		}
		m.ShieldAbsorb = sh != 0
		out = append(out, m)
	}
	return out, rows.Err()
}

// HitRow is an ab_hit row.
type HitRow struct {
	Guid              int64
	Name              string
	NetType           string // EAttackNetworkType: NET_ServerVerify ...
	ApplicableTargets int
	ApplicableTypes   int
	FallbackTarget    string
	RequirementExpr   int64
	DelayExpr         int64
	ThreatExpr        int64
	MaxTargetsExpr    int64
	Extra             string
}

// HitRow returns the hit with the guid.
func (d *DB) HitRow(guid int64) (HitRow, error) {
	var h HitRow
	err := d.sql.QueryRow(`SELECT guid, name, net_type, applicable_targets, applicable_types, fallback_target, requirement_expr, delay_expr, threat_expr,
		max_targets_expr, extra FROM ab_hit WHERE guid = ?`, guid).
		Scan(&h.Guid, &h.Name, &h.NetType, &h.ApplicableTargets, &h.ApplicableTypes, &h.FallbackTarget, &h.RequirementExpr, &h.DelayExpr, &h.ThreatExpr, &h.MaxTargetsExpr, &h.Extra)
	if errors.Is(err, sql.ErrNoRows) {
		return HitRow{}, fmt.Errorf("content: no ab_hit %d", guid)
	}
	return h, err
}

// HitEffectRow is an ab_hit_effect row: what a hit applies to its targets (Kind "apply") or to its caster ("self").
type HitEffectRow struct {
	Kind            string
	Ord             int
	EffectGuid      int64
	RequirementExpr int64
	StackCountExpr  int64
	DurationExpr    int64
	OnHitBehavior   string
	MaxApplications int
}

// HitEffects returns what a hit applies, in order.
func (d *DB) HitEffects(hit int64) ([]HitEffectRow, error) {
	rows, err := d.sql.Query(`SELECT kind, ord, effect_guid, requirement_expr, stack_count_expr, duration_expr, on_hit_behavior, max_applications
		FROM ab_hit_effect WHERE hit_guid = ? ORDER BY kind, ord`, hit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []HitEffectRow
	for rows.Next() {
		var h HitEffectRow
		if err := rows.Scan(&h.Kind, &h.Ord, &h.EffectGuid, &h.RequirementExpr, &h.StackCountExpr, &h.DurationExpr, &h.OnHitBehavior, &h.MaxApplications); err != nil {
			return nil, err
		}
		out = append(out, h)
	}
	return out, rows.Err()
}

// AbilityHitRow is an ab_ability_hit row: a hit the ability fires, directly (Source "hit"), through a projectile ("projectile") or a lingering zone ("zone").
type AbilityHitRow struct {
	Source    string
	Key       int64
	OwnerGuid int64
	Role      string
	Ord       int
	HitGuid   int64
}

// AbilityHits returns the hits an ability fires.
func (d *DB) AbilityHits(ability int64) ([]AbilityHitRow, error) {
	rows, err := d.sql.Query(`SELECT source, key, owner_guid, role, ord, hit_guid FROM ab_ability_hit WHERE ability_guid = ? ORDER BY source, key, owner_guid, role, ord`, ability)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AbilityHitRow
	for rows.Next() {
		var h AbilityHitRow
		if err := rows.Scan(&h.Source, &h.Key, &h.OwnerGuid, &h.Role, &h.Ord, &h.HitGuid); err != nil {
			return nil, err
		}
		out = append(out, h)
	}
	return out, rows.Err()
}

// EffectHitRow is an ab_effect_hit row: a hit an effect fires in a phase (PreApply, Apply, Tick, Consume, Remove, Expire, End).
type EffectHitRow struct {
	Phase   string
	Ord     int
	HitGuid int64
}

// EffectHits returns the hits an effect fires.
func (d *DB) EffectHits(effect int64) ([]EffectHitRow, error) {
	rows, err := d.sql.Query(`SELECT phase, ord, hit_guid FROM ab_effect_hit WHERE effect_guid = ? ORDER BY phase, ord`, effect)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []EffectHitRow
	for rows.Next() {
		var h EffectHitRow
		if err := rows.Scan(&h.Phase, &h.Ord, &h.HitGuid); err != nil {
			return nil, err
		}
		out = append(out, h)
	}
	return out, rows.Err()
}

// (Cyb | C | O | A)

// ProjectileRow is an ab_projectile row with the hits it fires on impact.
type ProjectileRow struct {
	Guid           int64
	Name           string
	ProjectileType string
	Speed, Range   float64
	LifeTime       float64
	MaxHits        int
	ChainHitCount  int
	ChainRadius    float64
	SphereRadius   float64
	Hits           []int64
}

// Projectile returns the projectile with the guid.
func (d *DB) Projectile(guid int64) (ProjectileRow, error) {
	var p ProjectileRow
	err := d.sql.QueryRow(`SELECT guid, name, projectile_type, speed, range, life_time, max_hits, chain_hit_count, chain_radius, sphere_radius FROM ab_projectile WHERE guid = ?`, guid).
		Scan(&p.Guid, &p.Name, &p.ProjectileType, &p.Speed, &p.Range, &p.LifeTime, &p.MaxHits, &p.ChainHitCount, &p.ChainRadius, &p.SphereRadius)
	if errors.Is(err, sql.ErrNoRows) {
		return ProjectileRow{}, fmt.Errorf("content: no ab_projectile %d", guid)
	}
	if err != nil {
		return ProjectileRow{}, err
	}
	p.Hits, err = d.int64s(`SELECT hit_guid FROM ab_projectile_hit WHERE projectile_guid = ? ORDER BY ord`, guid)
	return p, err
}

// ZoneRow is an ab_zone row (a lingering zone) with its hits by role.
type ZoneRow struct {
	Guid                int64
	Name                string
	LifeTime, TickRate  float64
	UseGlobalTickRate   bool
	Charges             int
	DelayTime           float64
	DelayType           string
	Speed               float64
	ExpireAfterHitsExpr int64
	ExpireIfExpr        int64
	TargetingGuid       int64
	Hits                map[string][]int64 // by role: InitialHitsIds, LingeringHitsIds, OnBeginOverlapHitsIds, OnEndOverlapHitsIds, EndHitsIds
}

// Zone returns the lingering zone with the guid.
func (d *DB) Zone(guid int64) (ZoneRow, error) {
	var z ZoneRow
	var global int
	err := d.sql.QueryRow(`SELECT guid, name, life_time, tick_rate, use_global_tick_rate, charges, delay_time, delay_type, speed, expire_after_hits_expr, expire_if_expr, targeting_guid
		FROM ab_zone WHERE guid = ?`, guid).
		Scan(&z.Guid, &z.Name, &z.LifeTime, &z.TickRate, &global, &z.Charges, &z.DelayTime, &z.DelayType, &z.Speed, &z.ExpireAfterHitsExpr, &z.ExpireIfExpr, &z.TargetingGuid)
	if errors.Is(err, sql.ErrNoRows) {
		return ZoneRow{}, fmt.Errorf("content: no ab_zone %d", guid)
	}
	if err != nil {
		return ZoneRow{}, err
	}
	z.UseGlobalTickRate = global != 0
	z.Hits = map[string][]int64{}
	rows, err := d.sql.Query(`SELECT role, hit_guid FROM ab_zone_hit WHERE zone_guid = ? ORDER BY role, ord`, guid)
	if err != nil {
		return ZoneRow{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var role string
		var h int64
		if err := rows.Scan(&role, &h); err != nil {
			return ZoneRow{}, err
		}
		z.Hits[role] = append(z.Hits[role], h)
	}
	return z, rows.Err()
}

func (d *DB) int64s(query string, args ...any) ([]int64, error) {
	rows, err := d.sql.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var v int64
		if err := rows.Scan(&v); err != nil {
			return nil, err
		}
		out = append(out, v)
	}
	return out, rows.Err()
}
