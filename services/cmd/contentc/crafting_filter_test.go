// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The gather-item filters of the recipes (contentc version 47): the items of each filter a recipe input names, on synthetic rows (the shape of the filter and recipe records)
// and on the real design database (TestRealFilterItems, with AOC_DESIGN_DB).

import (
	"context"
	"database/sql"
	"io"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

const (
	synthFilter        = int64(8100000000000000301)
	synthFilterUnused  = int64(8100000000000000302)
	synthFilterEmpty   = int64(8100000000000000303)
	synthFilterItemOne = int64(8100000000000000311)
	synthFilterItemTwo = int64(8100000000000000312)
	synthFilterRecipe  = int64(8100000000000000321)
	synthEmptyRecipe   = int64(8100000000000000322)
)

// buildCraftingSynthetic runs buildTyped and buildCrafting over src into a content.db.
func buildCraftingSynthetic(t *testing.T, src *source) (*content.DB, map[string]string, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	sdb, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer sdb.Close()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		t.Fatal(err)
	}
	tx, err := sdb.BeginTx(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}
	meta := map[string]string{}
	opt := &Options{Log: io.Discard}
	if err := buildTyped(w, src, opt, meta); err != nil {
		return nil, nil, err
	}
	if err := buildCrafting(w, src, opt, meta); err != nil {
		return nil, nil, err
	}
	if err := w.close(); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO meta VALUES (?, ?), (?, ?)`, content.MetaSchemaVersion, "1", content.MetaRecordEncoding, content.EncodingJSON); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	sdb.Close()
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db, meta, nil
}

// craftingSource is the synthetic source with the crafting tables empty and the rows of a filter-input recipe to be added by a test.
func craftingSource(t *testing.T) *source {
	t.Helper()
	src := syntheticSource()
	for _, schema := range craftingRecordTypes {
		src.rows[schema] = []designdb.Row{}
	}
	src.rows["GatherItemFilterRecordBase"] = []designdb.Row{}
	return src
}

// filterRecipeRow is a recipe whose one general input is named by the gather-item filter `filter` (item 0) with the plant tag, and that costs nothing.
func filterRecipeRow(guid int64, name string, filter int64) designdb.Row {
	id := func(g int64) st { return idv(g, 0, "None") }
	return mkRow(guid, name, st{
		"ProfessionId": id(0), "CertificationLevelId": id(0), "CraftingCurrencyCostId": id(0), "RewardId": id(0),
		"DistributionRewardId": id(0), "LevelingTemplateId": id(0),
		"CategoryTag":            st{"TagName": ""},
		"BaseDuration":           int64(1),
		"StandardCurrencyCost":   "",
		"ItemDestination":        "EItemDestination::Inventory",
		"GeneralResourceCost":    list{st{"Item": id(0), "CostGatherItemFilter": id(filter), "MaterialTags": list{st{"TagName": "Item.Resource.Raw.Plant"}}, "Quantity": int64(1), "MinRarity": "ERarity::Common", "bConsumeInCrafting": true}},
		"PrimaryResourceCosts":   list{},
		"FuelTags":               list{},
		"AvailabilityPredicates": list{},
		"bUseOverrides":          false,
		"MinimumQuantityFormula": "", "MaximumQuantityFormula": "",
		"DurationFormula": "", "QuantityFormula": "", "QualityFormula": "",
		"DefaultQuality": int64(0), "Difficulty": int64(0), "Fuel": int64(0), "Learnable": false,
		"bCanFastCraft": false, "bCanGameplayLayer": false, "Blacklisted": false,
	})
}

// TestFilterItemsAreWritten: a filter a recipe input names gets one table row for each item its record lists, the null id skipped and a repeated item once; a filter no
// input names gets none.  The keys are the contract with worldd's reader ("filter_guid" and "item_guid").
func TestFilterItemsAreWritten(t *testing.T) {
	src := craftingSource(t)
	src.rows["GatherItemFilterRecordBase"] = []designdb.Row{
		mkRow(synthFilter, "Test_Plants_Any", st{"ItemsIds": list{idv(synthFilterItemOne, 0, "None"), idv(0, 0, "None"), idv(synthFilterItemTwo, 0, "None"), idv(synthFilterItemOne, 0, "None")},
			"ShowText": txt("Any Plants")}),
		mkRow(synthFilterUnused, "Test_Unused_Filter", st{"ItemsIds": list{idv(synthFilterItemTwo, 0, "None")}}),
	}
	src.rows["CraftingRecipeDefRecordBase"] = []designdb.Row{filterRecipeRow(synthFilterRecipe, "Test_Filter_Recipe", synthFilter)}
	db, meta, err := buildCraftingSynthetic(t, src)
	if err != nil {
		t.Fatal(err)
	}
	if meta["crafting.filter_items"] != "2" || meta["crafting.filters"] != "1" || meta["crafting.filters_listed"] != "1" {
		t.Errorf("meta crafting.filter_items = %q, crafting.filters = %q, crafting.filters_listed = %q, want 2, 1 and 1", meta["crafting.filter_items"], meta["crafting.filters"], meta["crafting.filters_listed"])
	}
	rows, err := db.SQL().Query(`SELECT filter_guid, item_guid FROM craft_filter_item ORDER BY item_guid`)
	if err != nil {
		t.Fatalf("craft_filter_item does not read: %v", err)
	}
	defer rows.Close()
	type filterRow struct{ filter, item int64 }
	var got []filterRow
	for rows.Next() {
		var r filterRow
		if err := rows.Scan(&r.filter, &r.item); err != nil {
			t.Fatalf("a craft_filter_item row does not read: %v", err)
		}
		got = append(got, r)
	}
	if err := rows.Err(); err != nil {
		t.Fatalf("craft_filter_item rows: %v", err)
	}
	want := []filterRow{{synthFilter, synthFilterItemOne}, {synthFilter, synthFilterItemTwo}}
	if len(got) != len(want) {
		t.Fatalf("craft_filter_item holds %d rows, want %d: %+v", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("craft_filter_item row %d = %+v, want %+v", i, got[i], want[i])
		}
	}
}

// TestFilterThatListsNoItemWritesNoRow: a filter record whose ItemsIds is empty (30 of the 51 filters the real recipes name) writes no row but still counts as a filter a recipe
// input names, and the filter that lists items beside it is not disturbed: worldd falls back on the input's tags for the first and on the list for the second.
func TestFilterThatListsNoItemWritesNoRow(t *testing.T) {
	src := craftingSource(t)
	src.rows["GatherItemFilterRecordBase"] = []designdb.Row{
		mkRow(synthFilter, "Test_Plants_Any", st{"ItemsIds": list{idv(synthFilterItemOne, 0, "None")}}),
		mkRow(synthFilterEmpty, "Test_Commodity_Plants", st{"ItemsIds": list{}, "ShowText": txt("Plants from nowhere")}),
	}
	src.rows["CraftingRecipeDefRecordBase"] = []designdb.Row{
		filterRecipeRow(synthFilterRecipe, "Test_Filter_Recipe", synthFilter),
		filterRecipeRow(synthEmptyRecipe, "Test_Empty_Filter_Recipe", synthFilterEmpty),
	}
	db, meta, err := buildCraftingSynthetic(t, src)
	if err != nil {
		t.Fatal(err)
	}
	if meta["crafting.filter_items"] != "1" || meta["crafting.filters"] != "2" || meta["crafting.filters_listed"] != "1" {
		t.Errorf("meta crafting.filter_items = %q, crafting.filters = %q, crafting.filters_listed = %q, want 1, 2 and 1", meta["crafting.filter_items"], meta["crafting.filters"], meta["crafting.filters_listed"])
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM craft_filter_item WHERE filter_guid = `+strconv.FormatInt(synthFilterEmpty, 10)); n != 0 {
		t.Errorf("the filter that lists no item has %d rows, want 0", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM craft_filter_item WHERE filter_guid = `+strconv.FormatInt(synthFilter, 10)); n != 1 {
		t.Errorf("the filter that lists an item has %d rows, want 1", n)
	}
	// both filters stay on their recipe inputs, so worldd can tell "a filter with no list" from "no filter"
	if n := scalarInt(t, db, `SELECT count(*) FROM craft_recipe_input WHERE item_guid = 0 AND filter_guid != 0`); n != 2 {
		t.Errorf("%d recipe inputs name a filter, want 2", n)
	}
}

// TestFilterItemWithoutItsFieldIsAnError: a filter record without ItemsIds is an error that names the field, not a filter that admits nothing (as every other field this
// reader takes from a record is required).
func TestFilterItemWithoutItsFieldIsAnError(t *testing.T) {
	src := craftingSource(t)
	src.rows["GatherItemFilterRecordBase"] = []designdb.Row{mkRow(synthFilter, "Test_No_Items", st{})}
	src.rows["CraftingRecipeDefRecordBase"] = []designdb.Row{filterRecipeRow(synthFilterRecipe, "Test_Filter_Recipe", synthFilter)}
	if _, _, err := buildCraftingSynthetic(t, src); err == nil || !strings.Contains(err.Error(), "ItemsIds") {
		t.Errorf("a filter record without ItemsIds: error = %v, want one that names the field", err)
	}
}

// TestRealFilterItems: the filters the real recipes name, on the values the design records hold.
func TestRealFilterItems(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	if meta["crafting.filters"] != "51" || meta["crafting.filters_listed"] != "21" || meta["crafting.filter_items"] != "236" {
		t.Errorf("meta crafting.filters = %q, crafting.filters_listed = %q, crafting.filter_items = %q, want 51, 21 and 236", meta["crafting.filters"], meta["crafting.filters_listed"], meta["crafting.filter_items"])
	}
	for _, c := range []struct {
		what  string
		query string
		want  int64
	}{
		{"rows", `SELECT count(*) FROM craft_filter_item`, 236},
		{"filters that list items", `SELECT count(DISTINCT filter_guid) FROM craft_filter_item`, 21},
		{"filters recipe inputs name", `SELECT count(DISTINCT filter_guid) FROM craft_recipe_input WHERE item_guid = 0 AND filter_guid != 0`, 51},
		{"filters recipe inputs name that list no item", `SELECT count(*) FROM (SELECT DISTINCT filter_guid FROM craft_recipe_input WHERE item_guid = 0 AND filter_guid != 0 EXCEPT SELECT filter_guid FROM craft_filter_item)`, 30},
		{"rows of the Riverlands plants commodity filter (it lists no item)", `SELECT count(*) FROM craft_filter_item WHERE filter_guid = 6064633975689118054`, 0},
		{"rows of Raw_Fish_Any (it lists no item)", `SELECT count(*) FROM craft_filter_item WHERE filter_guid = 6064633967742943235`, 0},
		{"ALL_Raw_Plants_Any's items", `SELECT count(*) FROM craft_filter_item WHERE filter_guid = 6064633963143168007`, 22},
		{"Raw_Wood_Any's items", `SELECT count(*) FROM craft_filter_item WHERE filter_guid = 6064633967776563206`, 9},
		{"items that are no item", `SELECT count(*) FROM craft_filter_item WHERE item_guid NOT IN (SELECT guid FROM item)`, 0},
		{"filters that are no GatherItemFilterRecordBase", `SELECT count(*) FROM craft_filter_item WHERE filter_guid NOT IN (SELECT guid FROM record WHERE schema = 'GatherItemFilterRecordBase')`, 0},
		{"filters no recipe input names", `SELECT count(*) FROM (SELECT DISTINCT filter_guid FROM craft_filter_item EXCEPT SELECT filter_guid FROM craft_recipe_input WHERE filter_guid != 0)`, 0},
	} {
		if n := scalarInt(t, db, c.query); n != c.want {
			t.Errorf("%d %s, want %d", n, c.what, c.want)
		}
	}
	// the tag is wider than the filter: the plant tag admits Barley, the filter does not; a Daffodil is in both
	if n := scalarInt(t, db, `SELECT count(*) FROM craft_filter_item WHERE filter_guid = 6064633963143168007 AND item_guid IN (6064629184431849472, 6064630417666212190)`); n != 2 {
		t.Errorf("%d of the filter's two plants (Daffodil, Flax) are in it, want 2", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM craft_filter_item WHERE filter_guid = 6064633963143168007 AND item_guid = 6064629132834439185`); n != 0 {
		t.Errorf("Barley is in ALL_Raw_Plants_Any (%d rows), the tag admits it and the filter does not", n)
	}
}
