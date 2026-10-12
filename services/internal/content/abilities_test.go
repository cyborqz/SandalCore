// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"reflect"
	"testing"
)

// The readers of the ability tables over rows made by hand (no content.db needed).

func openAbilityDB(t *testing.T) (*DB, *sql.DB) {
	t.Helper()
	path := newDB(t, "1")
	w, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { w.Close() })
	db, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db, w
}

func TestAbilityReaders(t *testing.T) {
	db, w := openAbilityDB(t)
	// expressions: one with terms, one compiled, one that does not compile
	exec(t, w, `INSERT INTO ab_expr (id, text, terms, program, uses_target, error) VALUES
		(1, '10 * 5.6875', '{}', 12, 0, ''),
		(2, 'GetTerm($#1:2$) * 2.0', '{"6064631035951644675":"2.75","7":"1.5"}', 13, 1, ''),
		(3, 'CalculateDamage()', '{}', -1, 0, 'unsupported function "CalculateDamage"')`)
	e, err := db.Expr(2)
	if err != nil || e.Text != "GetTerm($#1:2$) * 2.0" || e.Program != 13 || !e.UsesTarget || !reflect.DeepEqual(e.Terms, map[int64]string{6064631035951644675: "2.75", 7: "1.5"}) {
		t.Errorf("Expr(2) = %+v, %v", e, err)
	}
	if e, err = db.Expr(1); err != nil || e.Terms != nil || e.Program != 12 || e.UsesTarget {
		t.Errorf("Expr(1) = %+v, %v", e, err)
	}
	if e, err = db.Expr(3); err != nil || e.Program != -1 || e.Error == "" {
		t.Errorf("Expr(3) = %+v, %v", e, err)
	}
	if _, err := db.Expr(99); err == nil {
		t.Error("Expr(99): no error")
	}

	// a class's abilities: the ranks of its tree (plan D1's tables), the slot options' too
	exec(t, w, `INSERT INTO class (guid, name, class_enum, archetype_id, level_type_guid, level_type_name, skill_tree_guid, skill_tree_name, prefers_melee, stat_init_list_guid, stat_init_list_name) VALUES (1, 'Bard', 'ECharacterClass::Bard', 2, 3, 'Adv', 1, 'Bard', 0, 0, '')`)
	exec(t, w, `INSERT INTO skill_node (tree_guid, node_guid, ord, opt, slot_guid, name, skill_guid, skill_name, level_requirement, point_requirement, auto_granted, auto_hotbar, all_required) VALUES
		(1, 10, 0, 0, 0, 'n1', 20, 'Bard_Flourish', 3, 0, 1, 1, 0), (1, 11, 1, 0, 0, 'n2', 21, 'Bard_Passive', 0, 4, 0, 0, 0), (1, 12, 2, 0, 9, 'n3', 23, 'Bard_Song', 0, 0, 0, 0, 0)`)
	exec(t, w, `INSERT INTO skill_rank (skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, effect_duration) VALUES
		(20, 0, 30, 'r1', 100, '', 0, '', 0, 0), (21, 0, 31, 'r2', 0, '', 200, '', 1, 0), (23, 0, 33, 'r4', 101, '', 0, '', 0, 0), (23, 1, 34, 'r5', 101, '', 0, '', 0, 0)`)
	if gs, err := db.ClassAbilityGuids("Bard"); err != nil || !reflect.DeepEqual(gs, []int64{100, 101}) {
		t.Errorf("ClassAbilityGuids(Bard) = %v, %v", gs, err)
	}
	if gs, err := db.ClassAbilityGuids("Ranger"); err != nil || len(gs) != 0 {
		t.Errorf("ClassAbilityGuids(Ranger) = %v, %v", gs, err)
	}

	// an ability with a cost, an effect cost and montages (one named by a capture, with a NetGUID above 2^63)
	exec(t, w, `INSERT INTO ab_ability VALUES (100, 'Bard_Flourish', 'Flourish', '["ability.Archetype"]', 'EHeldAbilityPressBehavior::Naive', 'EAbilityActivationInput::UserConfigurable',
		'EAbilityActivationTargetRequirement::Never', 'EDefaultFallbackTargetType::SelfOnly', 1, 0, 'EAbilityWeaponRequirement::NoRequirement', 'None', 0, 0.5, 1, 1, 1, 1, 0, 1, 100.0, 5.0, 12.5,
		1, 2, 0, 0, 0, 0, '{}')`)
	a, err := db.AbilityRowByName("Bard_Flourish")
	if err != nil || a.Guid != 100 || !reflect.DeepEqual(a.Tags, []string{"ability.Archetype"}) || a.ApplicableTargets != 1 || !a.HoldAnimation || a.DisableDuringCooldown ||
		!a.UsableInAir || a.UsableWhileMounted || !a.UsableFromHotbar || a.AreaRange != 100 || a.AreaRadius != 5 || a.NetworkingMaxSpeed != 12.5 ||
		a.CooldownExpr != 1 || a.ChargesExpr != 2 || a.MaxHoldTime != 0.5 {
		t.Errorf("AbilityRowByName = %+v, %v", a, err)
	}
	if a2, err := db.AbilityRow(100); err != nil || !reflect.DeepEqual(a, a2) {
		t.Errorf("AbilityRow(100) = %+v, %v", a2, err)
	}
	if _, err := db.AbilityRow(7); err == nil {
		t.Error("AbilityRow(7): no error")
	}
	exec(t, w, `INSERT INTO ab_cost VALUES (100, 0, 0, 0, 1, 0, 5, 'Mana', 1), (100, 1, -1, 0, 0, 0, 0, '', 0)`)
	cs, err := db.AbilityCosts(100)
	if err != nil || len(cs) != 2 || cs[0].StatName != "Mana" || !cs[0].TriggersCooldown || cs[0].AmountExpr != 1 || cs[1].StatOrd != -1 || cs[1].TriggersCooldown {
		t.Errorf("AbilityCosts = %+v, %v", cs, err)
	}
	exec(t, w, `INSERT INTO ab_effect_cost VALUES (100, 1, 0, 6064632468947286527, 'GetEffect() == $#3377240744094973029:6064632468947286527$', 2)`)
	ecs, err := db.AbilityEffectCosts(100)
	if err != nil || len(ecs) != 1 || ecs[0].EffectGuid != 6064632468947286527 || ecs[0].NumExpr != 2 {
		t.Errorf("AbilityEffectCosts = %+v, %v", ecs, err)
	}
	big := uint64(16448494777319543161)
	exec(t, w, `INSERT INTO ab_montage VALUES (100, 0, '/Game/A.A', '["/Game/A.A","/Game/B.B"]', 'EAbilityDirection::NONE', 3, 'None', '[1,2]', 4, ?),
		(100, 1, '/Game/C.C', '["/Game/C.C"]', 'EAbilityDirection::NONE', 0, 'None', '[]', 0, NULL)`, int64(big))
	ms, err := db.AbilityMontages(100)
	if err != nil || len(ms) != 2 || ms[0].NetGuid == nil || *ms[0].NetGuid != big || ms[1].NetGuid != nil || !reflect.DeepEqual(ms[0].Paths, []string{"/Game/A.A", "/Game/B.B"}) ||
		!reflect.DeepEqual(ms[0].HitKeys, []int64{1, 2}) || ms[0].SelectExpr != 3 || ms[0].TargetingKey != 4 || ms[1].SoftPath != "/Game/C.C" {
		t.Errorf("AbilityMontages = %+v, %v", ms, err)
	}

	// hits, projectiles, zones, effects and mods
	exec(t, w, `INSERT INTO ab_ability_hit VALUES (100, 'hit', 1, 0, '', 0, 500), (100, 'projectile', 1, 600, 'HitsIds', 0, 501), (100, 'zone', 1, 700, 'LingeringHitsIds', 1, 502)`)
	ah, err := db.AbilityHits(100)
	if err != nil || len(ah) != 3 || ah[0].Source != "hit" || ah[1].Source != "projectile" || ah[1].OwnerGuid != 600 || ah[2].Role != "LingeringHitsIds" || ah[2].Ord != 1 {
		t.Errorf("AbilityHits = %+v, %v", ah, err)
	}
	exec(t, w, `INSERT INTO ab_hit VALUES (500, 'Flourish_Self', 'EAttackNetworkType::NET_ServerVerify', 1, 0, 'EDefaultFallbackTargetType::SelfOnly', 3, 0, 0, 0, '{}')`)
	h, err := db.HitRow(500)
	if err != nil || h.Name != "Flourish_Self" || h.ApplicableTargets != 1 || h.RequirementExpr != 3 {
		t.Errorf("HitRow = %+v, %v", h, err)
	}
	if _, err := db.HitRow(1); err == nil {
		t.Error("HitRow(1): no error")
	}
	exec(t, w, `INSERT INTO ab_hit_effect VALUES (500, 'apply', 0, 800, 1, 2, 3, '', 0), (500, 'self', 0, 801, 0, 2, 0, 'EOnHitBehavior::Stack', 3)`)
	he, err := db.HitEffects(500)
	if err != nil || len(he) != 2 || he[0].Kind != "apply" || he[0].DurationExpr != 3 || he[1].OnHitBehavior != "EOnHitBehavior::Stack" || he[1].MaxApplications != 3 {
		t.Errorf("HitEffects = %+v, %v", he, err)
	}
	exec(t, w, `INSERT INTO ab_projectile VALUES (600, 'Arrow', 'EProjectileType::PROJECTILE_Homing', 8000, 5000, 10, 1, 0, 0, 16)`)
	exec(t, w, `INSERT INTO ab_projectile_hit VALUES (600, 0, 501), (600, 1, 503)`)
	p, err := db.Projectile(600)
	if err != nil || p.Speed != 8000 || p.LifeTime != 10 || p.SphereRadius != 16 || !reflect.DeepEqual(p.Hits, []int64{501, 503}) {
		t.Errorf("Projectile = %+v, %v", p, err)
	}
	exec(t, w, `INSERT INTO ab_zone VALUES (700, 'Dance', 30, 1.5, 1, 1, 0, 'ELingeringZoneTriggerDelay::DELAY_Time', 0, 0, 0, 900)`)
	exec(t, w, `INSERT INTO ab_zone_hit VALUES (700, 'LingeringHitsIds', 0, 502), (700, 'OnBeginOverlapHitsIds', 0, 504), (700, 'LingeringHitsIds', 1, 505)`)
	z, err := db.Zone(700)
	if err != nil || z.LifeTime != 30 || z.TickRate != 1.5 || !z.UseGlobalTickRate || z.TargetingGuid != 900 ||
		!reflect.DeepEqual(z.Hits, map[string][]int64{"LingeringHitsIds": {502, 505}, "OnBeginOverlapHitsIds": {504}}) {
		t.Errorf("Zone = %+v, %v", z, err)
	}
	exec(t, w, `INSERT INTO ab_effect VALUES (800, 'Bard_Pep', 'Pep', 'EEffectCategory::Buff', '["Buff"]', '[]', 0, 4, 0, 0, 'EEffectStackingType::STACK_Override', 1, 0, 'x', 1, 0, 1, 1, 0, 0, '{}')`)
	ef, err := db.EffectRowByName("Bard_Pep")
	if err != nil || ef.Guid != 800 || ef.InfiniteDuration || ef.DurationExpr != 4 || !ef.PersistsThroughDeath || ef.PersistsRelog || !ef.Dispellable || !ef.Significant || !reflect.DeepEqual(ef.Tags, []string{"Buff"}) || len(ef.EffectTags) != 0 {
		t.Errorf("EffectRowByName = %+v, %v", ef, err)
	}
	if ef2, err := db.EffectRow(800); err != nil || !reflect.DeepEqual(ef, ef2) {
		t.Errorf("EffectRow = %+v, %v", ef2, err)
	}
	exec(t, w, `INSERT INTO ab_effect_hit VALUES (800, 'Tick', 0, 502), (800, 'Apply', 0, 501)`)
	eh, err := db.EffectHits(800)
	if err != nil || len(eh) != 2 || eh[0].Phase != "Apply" || eh[1].Phase != "Tick" {
		t.Errorf("EffectHits = %+v, %v", eh, err)
	}
	exec(t, w, `INSERT INTO ab_mod VALUES ('effect', 800, 0, 1000, 109228544644729, 'MoveSpeedMult', 2, 0), ('effect', 800, 1, 1001, 6064629069966082048, 'TempHealth', 1, 1), ('hit', 500, 0, 1002, 1, 'Mana', 1, 0)`)
	mods, err := db.Mods("effect", 800)
	if err != nil || len(mods) != 2 || mods[0].StatName != "MoveSpeedMult" || mods[0].ValueExpr != 2 || mods[0].ShieldAbsorb || !mods[1].ShieldAbsorb {
		t.Errorf("Mods(effect) = %+v, %v", mods, err)
	}
	if mods, err = db.Mods("hit", 500); err != nil || len(mods) != 1 || mods[0].StatGuid != 1 {
		t.Errorf("Mods(hit) = %+v, %v", mods, err)
	}
	if mods, err = db.Mods("effect", 1); err != nil || len(mods) != 0 {
		t.Errorf("Mods of nothing = %+v, %v", mods, err)
	}
	if _, err := db.Projectile(1); err == nil {
		t.Error("Projectile(1): no error")
	}
	if _, err := db.Zone(1); err == nil {
		t.Error("Zone(1): no error")
	}
	if _, err := db.EffectRow(1); err == nil {
		t.Error("EffectRow(1): no error")
	}
}
