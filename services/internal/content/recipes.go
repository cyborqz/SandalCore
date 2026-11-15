// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// The recipe items (contentc version 44): an item whose ItemActivationArchetype is LearnRecipes teaches the recipes its record lists
// (LearnableRecipeIds: one recipe an item in the data, and 29 of the 2,858 items name none), and its record's RequiredProfessionId and
// RequiredCertificationLevelId (0: none) are the requirement to use it.  worldd reads the table with ContentDb::item_learn; a file without
// it is an older one (no recipe item is learned).

// RecipesDDL creates item_learn.
const RecipesDDL = `
CREATE TABLE item_learn (
    item_guid          INTEGER NOT NULL,
    ord                INTEGER NOT NULL,
    recipe_guid        INTEGER NOT NULL,
    profession_guid    INTEGER NOT NULL,
    certification_guid INTEGER NOT NULL,
    PRIMARY KEY (item_guid, ord)
) WITHOUT ROWID;
`

// RecipesTables are the tables RecipesDDL creates, in the DDL's order.
var RecipesTables = []string{"item_learn"}

func createRecipes(db *sql.DB) error {
	if _, err := db.Exec(RecipesDDL); err != nil {
		return fmt.Errorf("content: create the recipe tables: %w", err)
	}
	return nil
}
