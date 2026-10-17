// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"os"
	"path/filepath"
	"testing"

	"aocrop/services/internal/content"
)

// exportedCombatINI is the DefaultCombat.ini that scripts/extract_game_data.ps1 writes from the player's own game files (AOC_CLIENT_CONFIG, else
// dist/content/client_config of the checkout). The test skips without it.
func exportedCombatINI(t *testing.T) string {
	t.Helper()
	dir := os.Getenv("AOC_CLIENT_CONFIG")
	if dir == "" {
		dir = filepath.Join(repoRoot(), "dist", "content", "client_config")
	}
	data, err := os.ReadFile(filepath.Join(dir, "DefaultCombat.ini"))
	if err != nil {
		t.Skip("DefaultCombat.ini is not at hand (run extract_game_data.bat, or set AOC_CLIENT_CONFIG): " + err.Error())
	}
	return string(data)
}

// TestParseCombatDefault reads the game's DefaultCombat.ini.
func TestParseCombatDefault(t *testing.T) {
	cc, err := parseCombat(exportedCombatINI(t), "default")
	if err != nil {
		t.Fatal(err)
	}
	if len(cc.profile) != len(content.CombatProfileKeys) {
		t.Errorf("%d profile keys, want %d", len(cc.profile), len(content.CombatProfileKeys))
	}
	for _, k := range content.CombatProfileKeys {
		if cc.profile[k] == 0 {
			t.Errorf("no stat for %s", k)
		}
	}
	if len(cc.elements) != 11 {
		t.Errorf("%d elements, want 11 (4 physical, 7 magical)", len(cc.elements))
	}
	want := map[string]float64{
		content.CombatInCombatTimer: 6, content.CombatOutOfRange: 3500, content.CombatActivationLeeway: 1000, content.CombatProjectileHitDist: 60,
		content.CombatThirdPartyHit: 1500, content.CombatMaxHitsPerRPC: 20, content.CombatStatRegenTime: 1, content.CombatGlobalDamageMult: 1,
		content.CombatGlobalHealingMult: 1,
	}
	for k, v := range want {
		if got, ok := cc.numbers[k]; !ok || got != v {
			t.Errorf("%s = %v (%v), want %v", k, got, ok, v)
		}
	}
	// The physical aspect is not the magical one and each key has its own stat.
	if cc.profile["Physical.Damage"] != 109343399171797 || cc.profile["Magical.Penetration"] != 109343399171810 || cc.profile["Corruption"] != 6064630485796716545 {
		t.Errorf("profile: %v", cc.profile)
	}
	seen := map[int64]string{}
	for k, g := range cc.profile {
		if o, dup := seen[g]; dup {
			t.Errorf("%s and %s are the same stat %d", k, o, g)
		}
		seen[g] = k
	}
}
