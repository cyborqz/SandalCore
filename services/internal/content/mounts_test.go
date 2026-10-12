// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"reflect"
	"testing"
)

// TestMountReaders reads back what a builder writes into the four mount tables.
func TestMountReaders(t *testing.T) {
	db := openWith(t, "11", func(raw *sql.DB) {
		exec(t, raw, `INSERT INTO mount VALUES
		(10, 'Mount_Flying_Dawnbreaker_1', 'flying', '/Game/GameBlueprints/Mounts/Mount_Flying_Dawnbreaker_1.Mount_Flying_Dawnbreaker_1_C', 'Mount_Flying_Dawnbreaker_1',
		 'CastAbility', 108436548376185, 25, 165, 215, 0, 500, 0, 0, 0, 'Flying Mount'),
		(11, 'Mount_Grounded_Horse_0', 'ground', '/Game/GameBlueprints/Mounts/Mount_Grounded_Horse_0.Mount_Grounded_Horse_0_C', 'Mount_Grounded_Horse_0',
		 'SummonMount', 108436548376185, 1, 135, 0, 0, 100, 0, 0, 1, 'Ground Mount')`)
		exec(t, raw, `INSERT INTO mount_stat VALUES (25, 5, 'MaxHealth', 4875), (25, 6, 'MoveSpeedMult', 1), (24, 5, 'MaxHealth', 4619)`)
		exec(t, raw, `INSERT INTO mount_ability VALUES (1, 'Mage_Blink', '', 1, 1), (2, 'Summon_Mount', 'summon', 1, 0)`)
		exec(t, raw, `INSERT INTO mount_effect VALUES (7, 'Road_T3_Speed_Mount', 'road', 3, 109228544644729, 0.25, 0, 1, 0, 0), (8, 'MountAbility_Sprint', 'dash', -1, 109228544644729, 0.35, 1, 0, 5, 0)`)
	})

	ms, err := db.Mounts()
	if err != nil || len(ms) != 2 {
		t.Fatalf("Mounts = %v, %v", ms, err)
	}
	if ms[0].ItemName != "Mount_Flying_Dawnbreaker_1" || ms[0].Kind != MountFlying || ms[0].HealthFactor() != 5 || ms[0].FlyingPct != 215 || ms[0].Tradeable ||
		ms[0].Blueprint != "Mount_Flying_Dawnbreaker_1" || ms[0].Activation != "CastAbility" || ms[0].CastAbilityGuid != GuidSummonMount || ms[0].Level != 25 {
		t.Errorf("flying mount = %+v", ms[0])
	}
	if ms[1].Kind != MountGround || ms[1].HealthFactor() != 1 || ms[1].GroundPct != 135 || !ms[1].Tradeable || ms[1].Activation != "SummonMount" {
		t.Errorf("ground mount = %+v", ms[1])
	}
	st, err := db.MountStats(25)
	if err != nil || !reflect.DeepEqual(st, []MountStat{{25, 5, "MaxHealth", 4875}, {25, 6, "MoveSpeedMult", 1}}) {
		t.Errorf("MountStats(25) = %v, %v", st, err)
	}
	if st, err = db.MountStats(3); err != nil || len(st) != 0 {
		t.Errorf("MountStats(3) = %v, %v: want none", st, err)
	}
	ab, err := db.MountAbilities()
	if err != nil || len(ab) != 2 || ab[0].Name != "Mage_Blink" || !ab[0].AutoDismount || !ab[0].UsableWhileMounted || ab[1].Role != MountRoleSummon || ab[1].AutoDismount {
		t.Errorf("MountAbilities = %v, %v", ab, err)
	}
	ef, err := db.MountEffects()
	if err != nil || len(ef) != 2 || ef[0].Name != "MountAbility_Sprint" || !ef[0].Relative || ef[0].Duration != 5 || ef[0].Value != 0.35 || ef[0].Infinite ||
		ef[1].Kind != MountEffectRoad || ef[1].Tier != 3 || !ef[1].Infinite || ef[1].StatGuid != GuidMoveSpeedMult || ef[1].Value != 0.25 {
		t.Errorf("MountEffects = %v, %v", ef, err)
	}
}
