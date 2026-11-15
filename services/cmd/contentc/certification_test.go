// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The certification dialogue action (contentc version 44): the profession a SetCertificationLevel choice names and whether the choice is a Promote or a Demote are kept in
// the dialogue_choice JSON, which worldd needs to move the profession.
func TestReadActionsKeepsTheCertification(t *testing.T) {
	const profession = int64(108752746862200)
	actions := list{
		st{"ActionType": "EDialogueActionType::SetCertificationLevel", "CertificationAction": "ECertificationAction::Promote", "ProfessionId": idv(profession, 0, "None")},
		st{"ActionType": "EDialogueActionType::SetCertificationLevel", "CertificationAction": "ECertificationAction::Demote", "ProfessionId": idv(profession, 0, "None")},
		st{"ActionType": "EDialogueActionType::GiveItems", "RewardIds": list{idv(5, 0, "None")}},
	}
	text, err := readActions(actions, nil)
	if err != nil {
		t.Fatalf("readActions: %v", err)
	}
	// the keys are the contract with worldd's parse_dialogue_actions ("p" the profession, "ca" 1 Promote or 2 Demote)
	const want = `[{"a":"SetCertificationLevel","p":108752746862200,"ca":1},{"a":"SetCertificationLevel","p":108752746862200,"ca":2},{"a":"GiveItems","rw":[5]}]`
	if text != want {
		t.Errorf("readActions wrote %s, want %s", text, want)
	}

	// the action response records are not involved: a bare SetCertificationLevel with no profession stays bare (the reader leaves it skipped)
	bare, err := readActions(list{st{"ActionType": "EDialogueActionType::SetCertificationLevel"}}, nil)
	if err != nil {
		t.Fatalf("readActions: %v", err)
	}
	if bare != `[{"a":"SetCertificationLevel"}]` {
		t.Errorf("a bare certification action = %s", bare)
	}
}

// The recipe items (contentc version 44): a LearnRecipes item's recipes, profession and certification are written to item_learn, one row for each recipe in the record's order;
// an item with no recipe (an empty list, or one null id) and an item that is not a LearnRecipes item write none.
func TestRecipeLearningIsWritten(t *testing.T) {
	src := progressionSource(t)
	const (
		archetype  = int64(8100000000000000200)
		twoRecipes = int64(8100000000000000201)
		noRecipe   = int64(8100000000000000202)
		unrequired = int64(8100000000000000203)
		nullOnly   = int64(8100000000000000204)
		notLearn   = int64(8100000000000000205)
		recipe     = int64(8100000000000000210)
		secondOne  = int64(8100000000000000211)
		freeRecipe = int64(8100000000000000212)
		profession = int64(108752746993277)
		cert       = int64(108906340767353)
	)
	src.rows["ItemActivationArchetypeRecordBase"] = append(src.rows["ItemActivationArchetypeRecordBase"],
		mkRow(archetype, "ConsumableRecipe", st{"ActivationAction": "EItemActivationAction::LearnRecipes", "ConsumeOnUse": true, "Cooldown": int64(0), "DisabledInCombat": false}))
	recipeItem := func(guid int64, name string, arch int64, recipes list, prof, level int64) designdb.Row {
		return mkRow(guid, name, st{"CanEquip": false, "OccupiesAllEquipSlots": false, "Level": int64(15), "DefaultQuality": int64(1), "MaxStackSize": int64(1),
			"StatBlockId": idv(0, content.TypeItemStatBlock, "None"), "QualityCurveId": idv(0, content.TypeQualityCurve, "None"),
			"AppearanceId": idv(0, 7961510888716503441, "None"), "EquipSlots": list{}, "SetBonusIds": list{}, "GameplayTags": list{},
			"ActivationArchetypeId": idv(arch, content.TypeItemActivationArchetype, "None"), "ActivationActionsIds": list{},
			"LearnableRecipeIds": recipes, "RequiredProfessionId": idv(prof, 0, "None"), "RequiredCertificationLevelId": idv(level, 0, "None")})
	}
	src.rows["ItemRecordBase"] = append(src.rows["ItemRecordBase"],
		recipeItem(twoRecipes, "Consumable_Recipe_Test", archetype, list{idv(recipe, 0, "None"), idv(0, 0, "None"), idv(secondOne, 0, "None")}, profession, cert),
		recipeItem(noRecipe, "Consumable_Recipe_NoRecipe", archetype, list{}, profession, cert),
		recipeItem(unrequired, "Consumable_Recipe_Unrequired", archetype, list{idv(freeRecipe, 0, "None")}, 0, 0),
		recipeItem(nullOnly, "Consumable_Recipe_NullOnly", archetype, list{idv(0, 0, "None")}, 0, 0),
		recipeItem(notLearn, "Not_A_Recipe_Item", synthArchKeep, list{idv(recipe, 0, "None")}, profession, cert))

	db, meta, err := buildProgressionSynthetic(t, src)
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.item_learn"] != "3" {
		t.Errorf("meta typed.item_learn = %q, want 3", meta["typed.item_learn"])
	}
	rows, err := db.SQL().Query(`SELECT item_guid, ord, recipe_guid, profession_guid, certification_guid FROM item_learn ORDER BY item_guid, ord`)
	if err != nil {
		t.Fatalf("item_learn does not read: %v", err)
	}
	defer rows.Close()
	type learnRow struct{ item, ord, recipe, profession, certification int64 }
	var got []learnRow
	for rows.Next() {
		var r learnRow
		if err := rows.Scan(&r.item, &r.ord, &r.recipe, &r.profession, &r.certification); err != nil {
			t.Fatalf("an item_learn row does not read: %v", err)
		}
		got = append(got, r)
	}
	if err := rows.Err(); err != nil {
		t.Fatalf("item_learn rows: %v", err)
	}
	// the two recipes in the record's order (the null id between them is no recipe, so the second one is ord 1); the item that asks for no profession and no certification keeps zeros
	want := []learnRow{
		{twoRecipes, 0, recipe, profession, cert},
		{twoRecipes, 1, secondOne, profession, cert},
		{unrequired, 0, freeRecipe, 0, 0},
	}
	if len(got) != len(want) {
		t.Fatalf("item_learn holds %d rows, want %d: %+v", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("item_learn row %d = %+v, want %+v", i, got[i], want[i])
		}
	}
}

// A LearnRecipes item record that lacks one of the three fields is an error, as a record that lacks any other field this builder reads is: the reader does not guess that the
// item teaches nothing.
func TestRecipeItemWithoutItsFieldsIsAnError(t *testing.T) {
	for _, missing := range []string{"LearnableRecipeIds", "RequiredProfessionId", "RequiredCertificationLevelId"} {
		src := progressionSource(t)
		const archetype = int64(8100000000000000200)
		src.rows["ItemActivationArchetypeRecordBase"] = append(src.rows["ItemActivationArchetypeRecordBase"],
			mkRow(archetype, "ConsumableRecipe", st{"ActivationAction": "EItemActivationAction::LearnRecipes", "ConsumeOnUse": true, "Cooldown": int64(0), "DisabledInCombat": false}))
		fields := st{"CanEquip": false, "OccupiesAllEquipSlots": false, "Level": int64(15), "DefaultQuality": int64(1), "MaxStackSize": int64(1),
			"StatBlockId": idv(0, content.TypeItemStatBlock, "None"), "QualityCurveId": idv(0, content.TypeQualityCurve, "None"),
			"AppearanceId": idv(0, 7961510888716503441, "None"), "EquipSlots": list{}, "SetBonusIds": list{}, "GameplayTags": list{},
			"ActivationArchetypeId": idv(archetype, content.TypeItemActivationArchetype, "None"), "ActivationActionsIds": list{},
			"LearnableRecipeIds": list{idv(8100000000000000210, 0, "None")}, "RequiredProfessionId": idv(108752746993277, 0, "None"),
			"RequiredCertificationLevelId": idv(108906340767353, 0, "None")}
		delete(fields, missing)
		src.rows["ItemRecordBase"] = append(src.rows["ItemRecordBase"], mkRow(8100000000000000201, "Consumable_Recipe_Test", fields))
		if _, _, err := buildProgressionSynthetic(t, src); err == nil || !strings.Contains(err.Error(), missing) {
			t.Errorf("an item record without %s: error = %v, want one that names the field", missing, err)
		}
	}
}
