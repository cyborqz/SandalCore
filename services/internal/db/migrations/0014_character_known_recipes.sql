-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The crafting milestone, step 3, "craft": the recipes a character was taught.
--
-- One NEW table, character_known_recipes: one row per recipe a character knows although the game makes a player learn it (a recipe whose design record says Learnable). A recipe the game does
-- not make a player learn is known to everybody and has no row. The recipes are taught by a game master for now (a recipe scroll will teach them later). worldd keeps the list while a
-- character plays, sends it to the client as the character's known recipes, and saves it with the items and the wallet in the same call (CharacterService.SaveInventory with
-- replace_known_recipes).
--
-- Nothing is backfilled here: a character that was taught nothing has no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0014").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_known_recipes (
    character_id TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    recipe_guid  BIGINT      NOT NULL
                 CONSTRAINT character_known_recipes_recipe CHECK (recipe_guid > 0),
    taught_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_known_recipes_pkey PRIMARY KEY (character_id, recipe_guid)
);

COMMENT ON TABLE character_known_recipes IS 'The crafting milestone, step 3: the recipes a character was taught (the ones the game makes a player learn). worldd saves them with the items and the wallet (SaveInventory).';
COMMENT ON COLUMN character_known_recipes.recipe_guid IS 'The design database''s CraftingRecipeDef Guid.';
