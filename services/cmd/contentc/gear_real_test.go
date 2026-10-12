// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan B3 on the real design database (the shared build of real_test.go): the gear tables hold what the records say, and the
// stat program is the compiled graph of the file's own rows. The equality of the tables with the records (the engine's own
// reading of them) is proven in services/internal/statprog (TestGearTablesEqualTheRecords).

import (
	"bytes"
	"encoding/binary"
	"strconv"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/statprog"
)

func TestRealGearTables(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	if meta["contentc.version"] != "12" {
		t.Errorf("contentc.version = %q, want 12", meta["contentc.version"])
	}
	for table, want := range map[string]int64{"quality_curve": 14, "stat_block_stat": 18395, "set_bonus": 269, "item_set": 2513, "item_tag": 16815} {
		if n := scalarInt(t, db, `SELECT count(*) FROM `+table); n != want {
			t.Errorf("%s has %d rows, want %d", table, n, want)
		}
	}
	for key, want := range map[string]string{"typed.quality_curve": "2", "typed.stat_block_stat": "18395", "typed.set_bonus": "269", "typed.item_tag": "16815",
		"typed.stat_block_unknown_stats": "6"} {
		if meta[key] != want {
			t.Errorf("meta %s = %q, want %q", key, meta[key], want)
		}
	}
	// every item that names a curve names one that is there, and 244 name none
	if n := scalarInt(t, db, `SELECT count(*) FROM item WHERE quality_curve_guid = 0`); n != 244 {
		t.Errorf("%d items have no quality curve, want 244", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM item WHERE quality_curve_guid <> 0 AND quality_curve_guid NOT IN (SELECT guid FROM quality_curve)`); n != 0 {
		t.Errorf("%d items name a quality curve that is not in quality_curve", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM item WHERE stat_block_guid <> 0 AND stat_block_guid NOT IN (SELECT block_guid FROM stat_block_stat)`); n > 244 {
		t.Logf("%d items name a stat block with no stats", n)
	}
	// the default curve's bands, which the retail items' qualities follow
	it, err := db.Item(6064630214426886486) // Gear_Armor_Light_Starter_Chest
	if err != nil {
		t.Fatal(err)
	}
	bands, err := db.QualityCurve(it.QualityCurveGuid)
	if err != nil {
		t.Fatal(err)
	}
	for i, want := range []content.QualityBand{{X: 1, Y: 5}, {X: 6, Y: 15}, {X: 16, Y: 30}, {X: 31, Y: 50}, {X: 51, Y: 70}, {X: 71, Y: 100}, {X: 101, Y: 200}} {
		if bands[i] != want {
			t.Errorf("rarity %d of the starter chest's curve = %v, want %v", i, bands[i], want)
		}
	}
	// the starter chest: 20 of stat 109183607068294 (PhysicalMitigationRating) at every rarity but Artifact, and its durability
	block, err := db.StatBlock(it.StatBlockGuid)
	if err != nil || len(block) != 2 {
		t.Fatalf("the starter chest's stat block = %+v, %v", block, err)
	}
	for _, s := range block {
		switch s.Stat {
		case 109183607068294:
			if s.Values[0] != (content.RarityValue{Min: 20, Max: 20}) || s.Values[5] != (content.RarityValue{Min: 20, Max: 20}) || s.Values[6] != (content.RarityValue{}) {
				t.Errorf("the chest's mitigation rating = %+v", s.Values)
			}
		case content.GuidMaxDurability:
			if s.Values[0] != (content.RarityValue{Min: 11, Max: 11}) {
				t.Errorf("the chest's durability = %+v", s.Values)
			}
		default:
			t.Errorf("the starter chest's stat block has the stat %d", s.Stat)
		}
	}
	if tags, err := db.ItemTags(it.Guid); err != nil || len(tags) != 2 || tags[0] != "Item.Gear.Armor.Light" || tags[1] != "Slot.Chest" {
		t.Errorf("the starter chest's tags = %v, %v", tags, err)
	}
	// a set bonus the research read: Rosarium Guard's 2 pieces give Strength 107 at Rare
	if n := scalarInt(t, db, `SELECT count(*) FROM set_bonus WHERE pieces = 2 AND stat_guid = ? AND rare = 107`, content.GuidStrength); n < 1 {
		t.Errorf("no set bonus gives Strength 107 at Rare for 2 pieces (Rosarium Guard)")
	}
	// the shield the BlockMitigation formula asks about
	if n := scalarInt(t, db, `SELECT count(*) FROM item_tag WHERE tag = 'Item.Gear.Weapon.Shield'`); n < 1 {
		t.Errorf("no item has the tag Item.Gear.Weapon.Shield")
	}
}

// ~ C O A & Cyb ~

func TestRealStatProgram(t *testing.T) {
	db := builtDB(t)
	if err := db.RequireVersion(); err != nil {
		t.Fatal(err)
	}
	p, err := db.StatProgram()
	if err != nil {
		t.Fatal(err)
	}
	// version 3 of the format (plan E1): the eight classes and the two NPC profile classes (NPC, NPC_Guard), the 473 stats of plan B3 plus the one
	// the NPC lists name and the one the ability and reward expressions read, and the bundle holds the expressions too
	if p.Version != 3 || p.Mode != "float32" || p.Stats != 475 || p.Classes != 10 || len(p.Data) <= 77197 {
		t.Errorf("stat_program = version %d, mode %s, %d stats, %d classes, %d bytes; want 3, float32, 475, 10, more than the 77,197 of the stat graph alone",
			p.Version, p.Mode, p.Stats, p.Classes, len(p.Data))
	}
	if got := binary.LittleEndian.Uint32(p.Data[12:16]); got != p.CRC32 {
		t.Errorf("crc32 column %08x, header %08x", p.CRC32, got)
	}
	b, err := statprog.Decode(p.Data)
	if err != nil {
		t.Fatalf("the stored bundle does not verify: %v", err)
	}
	if len(b.Stats) != p.Stats || len(b.Classes) != p.Classes {
		t.Errorf("the bundle has %d stats and %d classes", len(b.Stats), len(b.Classes))
	}
	// it is the compile of the file's own graph and ability expressions
	path := built.path
	g, err := statprog.LoadContentDB(path)
	if err != nil {
		t.Fatal(err)
	}
	exprs, ids, err := statprog.LoadExprs(path)
	if err != nil {
		t.Fatal(err)
	}
	fresh, results, err := statprog.CompileExprs(g, statprog.Options{}, exprs)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(fresh.Encode(), p.Data) {
		t.Errorf("the stored bundle is not what compiling the file's graph gives")
	}
	// the stat graph alone (the player classes, the NPC profiles, the damage profile, the empty expression table) is 77,197 bytes in format 3; the
	// expressions add one stat (CrystalGolem_AntiMagicShield_Absorb, read by an NPC ability's program and named by no initializer list) and the
	// readers of the others
	bare, err := statprog.Compile(g, statprog.Options{})
	if err != nil {
		t.Fatal(err)
	}
	if len(bare.Encode()) != 77197 {
		t.Errorf("the bare stat graph is %d bytes, format 3 of plan E1 gives 77,197", len(bare.Encode()))
	}
	if len(fresh.Stats) != len(bare.Stats)+1 {
		var extra []string
		for _, s := range fresh.Stats[len(bare.Stats):] {
			extra = append(extra, fresh.Strings[s.Name])
		}
		t.Errorf("the expressions add %d stats to the bundle, want 1: %v", len(fresh.Stats)-len(bare.Stats), extra)
	} else {
		t.Logf("the expressions add the stat %s to the bundle", fresh.Strings[fresh.Stats[len(bare.Stats)].Name])
	}
	// every expression row says what the compile gave
	rows, err := db.SQL().Query(`SELECT id, program, error FROM ab_expr ORDER BY id`)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for rows.Next() {
		var id, prog int64
		var e string
		if err := rows.Scan(&id, &prog, &e); err != nil {
			t.Fatal(err)
		}
		if ids[n] != id || int(prog) != results[n].Program || e != results[n].Err {
			t.Errorf("expression %d: row (%d, %q), compile (%d, %q)", id, prog, e, results[n].Program, results[n].Err)
		}
		n++
	}
	rows.Close()
	if n != len(exprs) {
		t.Errorf("%d expression rows for %d expressions", n, len(exprs))
	}
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	for key, want := range map[string]string{"statprog.version": "3", "statprog.mode": "float32", "statprog.bytes": strconv.Itoa(len(p.Data)),
		"statprog.stats": "475", "statprog.classes": "10"} {
		if meta[key] != want {
			t.Errorf("meta %s = %q, want %q", key, meta[key], want)
		}
	}
}

// worldd's ability runtime regenerates Stamina by the content constant (stamina_rule regen.rate_enabled, the StaminaRegeneration formula's 5),
// not by the engine's StaminaRegeneration stat. That is right while no gear moves that stat, which the design data says: the one stat block that
// names it belongs to a potion (CanEquip 0, no equip slot), and no set bonus names it. This test holds the constant to that fact: the day an
// equippable item or a set bonus gives StaminaRegeneration, the A7 runtime must read the engine's stat instead.
func TestNoGearMovesStaminaRegeneration(t *testing.T) {
	db := builtDB(t)

	if n := scalarInt(t, db, `SELECT count(*) FROM stat_block_stat WHERE stat_guid = 6064631663405498419`); n < 1 {
		t.Fatalf("no stat block names StaminaRegeneration any more (%d): the test's premise is gone, check it", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM item i JOIN stat_block_stat b ON b.block_guid = i.stat_block_guid
		WHERE b.stat_guid = 6064631663405498419 AND (i.can_equip <> 0 OR i.guid IN (SELECT item_guid FROM item_equip_slot))`); n != 0 {
		t.Errorf("%d equippable items give StaminaRegeneration: worldd's Stamina regeneration must use the engine's stat, not the content constant", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM set_bonus WHERE stat_guid = 6064631663405498419`); n != 0 {
		t.Errorf("%d set bonuses give StaminaRegeneration: worldd's Stamina regeneration must use the engine's stat, not the content constant", n)
	}

}
