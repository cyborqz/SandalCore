// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The mounts on the real design database (the shared build of real_test.go).

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

// TestRealMounts checks the mount tables against what the research found in the data and the retail captures.
func TestRealMounts(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	for key, want := range map[string]string{"typed.mount": "101", "typed.mount_stat": "250", "typed.mount_ability": "34", "typed.mount_effect": "9"} {
		if meta[key] != want {
			t.Errorf("meta %s = %q, want %q", key, meta[key], want)
		}
	}
	ms, err := db.Mounts()
	if err != nil {
		t.Fatal(err)
	}
	kinds, activation, byName := map[string]int{}, map[string]int{}, map[string]content.Mount{}
	noGroundText, blueprints := 0, map[string]bool{}
	for _, m := range ms {
		kinds[m.Kind]++
		activation[m.Kind+"/"+m.Activation]++
		byName[m.ItemName] = m
		blueprints[m.Blueprint] = true
		if m.Kind == content.MountGround && m.GroundPct == 0 {
			noGroundText++
		}
		if m.BlueprintPath == "" || m.Blueprint == "" || m.CastAbilityGuid != content.GuidSummonMount {
			t.Errorf("%s: blueprint %q %q, cast ability %d", m.ItemName, m.BlueprintPath, m.Blueprint, m.CastAbilityGuid)
		}
		if m.Kind == content.MountFlying && (m.FlyingPct != 215 || m.GroundPct != 165) {
			t.Errorf("%s: a flyer with %d%% ground and %d%% flying speed (every flyer has 165 and 215)", m.ItemName, m.GroundPct, m.FlyingPct)
		}
	}
	// the research: 101 items, 51 ground, 46 flying, 4 aquatic; 57 activate by SummonMount and 44 (the flyers) by CastAbility; 22 of the 51 ground items name no speed
	if len(ms) != 101 || kinds[content.MountGround] != 51 || kinds[content.MountFlying] != 46 || kinds[content.MountAquatic] != 4 {
		t.Errorf("mounts %d, by kind %v", len(ms), kinds)
	}
	if activation["flying/CastAbility"] != 44 || activation["flying/SummonMount"] != 2 || activation["ground/SummonMount"] != 51 || activation["aquatic/SummonMount"] != 4 {
		t.Errorf("activation by kind: %v", activation)
	}
	if noGroundText != 22 {
		t.Errorf("%d ground items name no speed, want 22", noGroundText)
	}
	if len(blueprints) != 75 {
		t.Errorf("%d distinct Blueprints, want 75", len(blueprints))
	}
	// golden items of the captures
	bird := byName["Mount_Grounded_TerrorBird_0"] // captures 911 and 959: 165%
	if bird.GroundPct != 165 || bird.Level != 20 || bird.Blueprint != "Mount_Grounded_TerrorBird_0" || bird.Activation != "SummonMount" || bird.HealthFactor() != 1 {
		t.Errorf("the terror bird = %+v", bird)
	}
	dawn := byName["Mount_Flying_Dawnbreaker_1_Alpha"] // capture 871: the item _Alpha, CastAbility, MaxHealth 24375 = 4875 x 5
	if dawn.Activation != "CastAbility" || dawn.HealthFactor() != 5 || dawn.FlyingPct != 215 || dawn.Blueprint != "Mount_Flying_Dawnbreaker_1" {
		t.Errorf("the Dawnbreaker = %+v", dawn)
	}
	shoal := byName["Mount_Aquatic_Shoalskipper_1_ALT"] // captures 898 and 899: water 230% (400 x 2.3 = 920 cm/s)
	if shoal.WaterPct != 230 || shoal.GroundPct != 0 || shoal.Kind != content.MountAquatic {
		t.Errorf("the shoalskipper = %+v", shoal)
	}
	gryphon := byName["Mount_Flying_PTR_ONLY"] // the GM Eagle Gryphon: a plain child of Mount_Flying_Base, no capture opens it
	if gryphon.Blueprint != "Mount_Flying_Gryphon_5_Armored" || gryphon.Activation != "SummonMount" || gryphon.Tradeable {
		t.Errorf("the GM Eagle Gryphon = %+v", gryphon)
	}
	if horse := byName["Mount_Grounded_Horse_StartingKaelar"]; horse.Blueprint != "Mount_Grounded_Horse_0" || horse.GroundPct != 0 {
		t.Errorf("the starting horse shares Horse_0's Blueprint and names no speed: %+v", horse)
	}
	// The health factors the item text names: 500% for 43 flyers, 1000% for the Drake of the Canopy and the two Thundering Bolt Beasts, 300% for two ground
	// items and 50% for the Scorpion Broodling.  The Blueprints' own stat lists (Mount_DrakeOfTheCanopy, Mount_ThunderingBoltBeast) say x5 for those three,
	// and so does retail's Drake in capture 883 (MaxHealth 24375 at owner level 25); the text and the milestone's spec say x10.  This table keeps the text.
	health := map[int]int{}
	for _, m := range ms {
		health[m.HealthPct]++
	}
	if health[100] != 52 || health[500] != 43 || health[1000] != 3 || health[300] != 2 || health[50] != 1 {
		t.Errorf("health percentages: %v", health)
	}
	if byName["Mount_Flying_DrakeOfTheCanopy_0"].HealthFactor() != 10 {
		t.Errorf("the Drake's factor = %v", byName["Mount_Flying_DrakeOfTheCanopy_0"].HealthFactor())
	}
	mules := 0
	for _, m := range ms {
		if m.Mule {
			mules++
			if m.CrateCapacity < 1 || m.GroundPct < 100 {
				t.Errorf("mule %+v", m)
			}
		}
	}
	if mules != 3 {
		t.Errorf("%d mules, want 3", mules)
	}

	// Mount_Base at the owner's level: the Mount_Base_HP curve (the research read 1179 at 10, 3572 at 20, 4875 at 25; a Dawnbreaker's x5 is 24375)
	byLevel := func(level int) map[string]float64 {
		st, err := db.MountStats(level)
		if err != nil {
			t.Fatal(err)
		}
		out := map[string]float64{}
		for _, s := range st {
			out[s.StatName] = s.Value
		}
		return out
	}
	for level, want := range map[int]float64{1: 500, 10: 1179, 20: 3572, 25: 4875} {
		s := byLevel(level)
		if s["MaxHealth"] != want || len(s) != 5 || s["CharacterType"] != 1.1 || s["MoveSpeedMult"] != 1 {
			t.Errorf("Mount_Base at owner level %d = %v, want MaxHealth %v", level, s, want)
		}
		// the two ratings are the same curve over 3 HP per Constitution (2 x 1.5)
		if third := want / 3; s["HealthRegenerationRating"] != third*1*1.5 || s["MitigationRating"] != third*2*2.0 {
			t.Errorf("owner level %d ratings = %v %v", level, s["HealthRegenerationRating"], s["MitigationRating"])
		}
	}
	if s := byLevel(25); s["MaxHealth"]*dawn.HealthFactor() != 24375 {
		t.Errorf("a Dawnbreaker of a level 25 owner: %v", s["MaxHealth"]*dawn.HealthFactor())
	}

	// the abilities: 33 with bUsableWhileMounted, 12 with bAutoDismount, and MountAbility_Sprint (the dash: the mount's, not usable by the rider himself)
	abilities, err := db.MountAbilities()
	if err != nil {
		t.Fatal(err)
	}
	usable, auto, roles := 0, 0, map[string]string{}
	for _, a := range abilities {
		if a.UsableWhileMounted {
			usable++
		}
		if a.AutoDismount {
			auto++
		}
		if a.Role != "" {
			roles[a.Role] = a.Name
		}
	}
	if usable != 33 || auto != 12 || len(abilities) != 34 {
		t.Errorf("%d mount abilities: %d usable mounted, %d auto-dismount", len(abilities), usable, auto)
	}
	if roles[content.MountRoleSummon] != "Summon_Mount" || roles[content.MountRoleDash] != "MountAbility_Sprint" || roles[content.MountRoleDismountE] != "General_Dismount_Elevated" ||
		roles[content.MountRoleDismountG] != "General_Dismount_Grounded" {
		t.Errorf("roles = %v", roles)
	}

	// the effects of riding: the roads T0..T4 (+0.10 .. +0.30 MoveSpeedMult, infinite), the dash (35% of the stat for 5 s), two force-dismount effects
	effects, err := db.MountEffects()
	if err != nil {
		t.Fatal(err)
	}
	roads, forced := map[int]float64{}, 0
	var dash content.MountEffect
	for _, e := range effects {
		switch e.Kind {
		case content.MountEffectRoad:
			if !e.Infinite || e.StatGuid != content.GuidMoveSpeedMult || e.Relative {
				t.Errorf("road effect %+v", e)
			}
			roads[e.Tier] = e.Value
		case content.MountEffectDash:
			dash = e
		case content.MountEffectForceDismount:
			forced++
		}
	}
	if len(roads) != 5 || roads[0] != 0.10 || roads[1] != 0.15 || roads[2] != 0.20 || roads[3] != 0.25 || roads[4] != 0.30 {
		t.Errorf("road effects = %v", roads)
	}
	if dash.Name != "MountAbility_Sprint" || dash.Value != 0.35 || !dash.Relative || dash.Duration != 5 || dash.StatGuid != content.GuidMoveSpeedMult {
		t.Errorf("the dash = %+v", dash)
	}
	if forced != 2 {
		t.Errorf("%d force-dismount effects, want 2", forced)
	}

	// the Summon_Mount montage the captures name (911, 896, 898, 871, 872)
	var net sql.NullInt64
	if err := db.SQL().QueryRow(`SELECT net_guid FROM ab_montage WHERE ability_guid = ? AND anim_index = 0`, content.GuidSummonMount).Scan(&net); err != nil || !net.Valid || uint64(net.Int64) != 10717609094785283935 {
		t.Errorf("Summon_Mount's montage NetGUID = %v, %v", net, err)
	}
}

// C Y B + C O A

// readMountSpeeds reads mapc's dist/content/mount_speeds.tsv (the repository's, or $AOC_MOUNT_SPEEDS): by Blueprint, the columns by name.
func readMountSpeeds(t *testing.T) map[string]map[string]string {
	t.Helper()
	path := os.Getenv("AOC_MOUNT_SPEEDS")
	if path == "" {
		path = filepath.Join(repoRoot(), "dist", "content", "mount_speeds.tsv")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Skipf("%s: %v (mapc mount-speeds writes it)", path, err)
	}
	lines := strings.Split(strings.TrimRight(string(raw), "\n"), "\n")
	header := strings.Split(lines[0], "\t")
	want := []string{"blueprint", "kind", "parents", "max_walk_speed", "max_run_speed", "max_fly_speed", "max_swim_speed", "defaults"}
	if strings.Join(header, ",") != strings.Join(want, ",") {
		t.Fatalf("mount_speeds.tsv header = %v, want %v", header, want)
	}
	out := map[string]map[string]string{}
	for _, l := range lines[1:] {
		f := strings.Split(l, "\t")
		if len(f) != len(header) {
			t.Fatalf("mount_speeds.tsv line %q has %d columns, want %d", l, len(f), len(header))
		}
		row := map[string]string{}
		for i, h := range header {
			row[h] = f[i]
		}
		out[f[0]] = row
	}
	return out
}

// TestRealMountSpeedsAgreeWithTheItemText holds the item texts' percentages to the Blueprints' own speeds (mapc mount-speeds): a ground mount's speed is its
// MaxRunSpeed = 600 x the ground percentage, a flyer's MaxFlySpeed 600 x the flying percentage and an aquatic mount's MaxSwimSpeed 400 x the water percentage.
// They agree for every item but five (listed: the two PTR Eagle Gryphons share one Blueprint; the Blueprint is what the client simulates, so it is the speed; the text is wrong or its Blueprint is shared), and
// the 22 ground items whose text names no speed have their Blueprint's.
func TestRealMountSpeedsAgreeWithTheItemText(t *testing.T) {
	speeds := readMountSpeeds(t)
	db := builtDB(t)
	ms, err := db.Mounts()
	if err != nil {
		t.Fatal(err)
	}
	num := func(row map[string]string, col string) float64 {
		v, err := strconv.ParseFloat(row[col], 64)
		if err != nil {
			t.Fatalf("%s %s = %q", row["blueprint"], col, row[col])
		}
		return v
	}
	disagree := map[string]string{}
	withText := 0
	for _, m := range ms {
		row, ok := speeds[m.Blueprint]
		if !ok || row["max_run_speed"] == "" {
			t.Errorf("%s: its Blueprint %s has no movement component in mount_speeds.tsv", m.ItemName, m.Blueprint)
			continue
		}
		if m.GroundPct != 0 {
			withText++
			if got, want := num(row, "max_run_speed"), 6*float64(m.GroundPct); got != want {
				disagree[m.ItemName] = fmt.Sprintf("ground %d%%: text %v, Blueprint run %v", m.GroundPct, want, got)
			}
		}
		if m.FlyingPct != 0 {
			if got, want := num(row, "max_fly_speed"), 6*float64(m.FlyingPct); got != want {
				disagree[m.ItemName] += fmt.Sprintf(" flying %d%%: text %v, Blueprint %v", m.FlyingPct, want, got)
			}
		}
		if m.WaterPct != 0 {
			if got, want := num(row, "max_swim_speed"), 4*float64(m.WaterPct); got != want {
				disagree[m.ItemName] += fmt.Sprintf(" water %d%%: text %v, Blueprint swim %v", m.WaterPct, want, got)
			}
		}
	}
	if len(ms) != 101 || withText != 77 {
		t.Errorf("%d mounts, %d with a ground speed in their text", len(ms), withText)
	}
	known := map[string]bool{"Mount_Flying_PTR_ONLY": true, "Mount_Flying_PTR_ONLY_Tradeable": true, "Mount_Grounded_Alligator_0": true, "Mount_Grounded_CorralesFox": true,
		"Mount_Grounded_GlimmeringGeode_0": true}
	for name, why := range disagree {
		if !known[name] {
			t.Errorf("%s: the item text and the Blueprint disagree: %s", name, why)
		}
	}
	if len(disagree) != len(known) {
		t.Errorf("%d items disagree with their Blueprint, want the %d known: %v", len(disagree), len(known), disagree)
	}
	// the GM Eagle Gryphon: the item says 165 % and 215 %, its Blueprint (a plain child of Mount_Flying_Base) 600 and 800
	if row := speeds["Mount_Flying_Gryphon_5_Armored"]; row["max_run_speed"] != "600" || row["max_fly_speed"] != "800" {
		t.Errorf("the Armored gryphon's Blueprint = %v", row)
	}
	// the golden Blueprints of the captures: the terror bird (959: 990), a Dawnbreaker (871: 1290 flying), a shoalskipper (898, 899: 920 swimming)
	if row := speeds["Mount_Grounded_TerrorBird_0"]; row["max_run_speed"] != "990" || row["max_walk_speed"] != "220" {
		t.Errorf("the terror bird's Blueprint = %v", row)
	}
	if row := speeds["Mount_Flying_Dawnbreaker_1"]; row["max_fly_speed"] != "1290" || row["kind"] != "Flying" {
		t.Errorf("the Dawnbreaker's Blueprint = %v", row)
	}
	if row := speeds["Mount_Aquatic_Shoalskipper_1_ALT"]; row["max_swim_speed"] != "920" || row["kind"] != "Aquatic" {
		t.Errorf("the shoalskipper's Blueprint = %v", row)
	}
	// the 22 ground items with no speed text have their Blueprint's: between the starting horse's 810 and the fastest mounts' 1,250
	untexted := 0
	for _, m := range ms {
		if m.Kind == content.MountGround && m.GroundPct == 0 {
			untexted++
			if run := num(speeds[m.Blueprint], "max_run_speed"); run < 600 || run > 1250 {
				t.Errorf("%s: its Blueprint's run speed %v", m.ItemName, run)
			}
		}
	}
	if untexted != 22 {
		t.Errorf("%d ground items have no speed text, want 22", untexted)
	}
}

// TestRealMountItemKindsAgreeWithTheirBlueprints (review B I1): worldd builds a mount's open from the donor of the ITEM's kind (the tag: ground, flying, aquatic), but the
// client spawns the Blueprint mount_speeds.tsv names.  Every item's kind is its Blueprint's family but two: the aquatic Tidebreaker 1 ALT and Verdant Voyager 0, whose
// Blueprints are Mount_Grounded_* ones with swim speeds (640 and 760).  They are in the GM starter set, so a tester gets them; a new disagreement must fail here, and these two
// are listed in the backlog to be tried in game (the aquatic donor's layout may not suit a Grounded-family Blueprint).
func TestRealMountItemKindsAgreeWithTheirBlueprints(t *testing.T) {
	speeds := readMountSpeeds(t)
	db := builtDB(t)
	ms, err := db.Mounts()
	if err != nil {
		t.Fatal(err)
	}
	family := map[string]string{"Grounded": content.MountGround, "Flying": content.MountFlying, "Aquatic": content.MountAquatic}
	known := map[string]string{"Mount_Aquatic_Tidebreaker_1_ALT": "Mount_Grounded_Tidebreaker_1_ALT", "Mount_Aquatic_VerdantVoyager_0": "Mount_Grounded_VerdantVoyager_0"}
	disagree := map[string]string{}
	for _, m := range ms {
		row, ok := speeds[m.Blueprint]
		if !ok {
			t.Errorf("%s: its Blueprint %s is not in mount_speeds.tsv", m.ItemName, m.Blueprint)
			continue
		}
		want, have := family[row["kind"]]
		if !have {
			t.Errorf("%s: its Blueprint %s has the family %q (no donor for it)", m.ItemName, m.Blueprint, row["kind"])
			continue
		}
		if want != m.Kind {
			disagree[m.ItemName] = m.Blueprint + " is " + row["kind"] + ", the item is " + m.Kind
		}
	}
	for name, why := range disagree {
		if _, ok := known[name]; !ok {
			t.Errorf("%s: the item's kind and its Blueprint's family disagree (%s): the donor would not match the Blueprint", name, why)
		}
	}
	if len(disagree) != len(known) {
		t.Errorf("%d items disagree with their Blueprint's family, want the %d known: %v", len(disagree), len(known), disagree)
	}
	for name, blueprint := range known {
		if got := disagree[name]; got == "" || !strings.HasPrefix(got, blueprint+" ") {
			t.Errorf("%s: expected to disagree through %s, got %q", name, blueprint, got)
		}
	}
}
