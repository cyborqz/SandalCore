// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Crafting and processing (the crafting milestone, step 0, "the stations stand and open"; contentc version 20): the stations, their recipe sets, lists and
// recipes, what a recipe takes and costs, and the link from a station's spawn asset to its definition. One version bump carries the data of the first four steps
// (the stations, starting a processing job, collecting it, the quick make), so the later steps change worldd only.
//
//	craft_station_def         a station definition: Processing (a job runs for a time and is collected) or FastCraft, the window title, its certification tier, the
//	                          queue, the tax tag the window's tax comes from and the recipe set the window lists
//	craft_station_asset       which station definition a population asset (the Blueprint a node's building spawns) is the station of, by name
//	craft_recipe_set          a set of recipe lists, for one profession and tier; craft_recipe_set_list the lists in order
//	craft_recipe_list         a list of recipes; craft_recipe_list_member the recipes in order
//	craft_pred                the availability predicates of a list or a recipe: the node's biome, level, the nodes, or the building modifications that unlock it
//	craft_recipe              a recipe: its profession and tier, what it takes (craft_recipe_input) and costs, its formulas and flags
//	craft_recipe_input        what a recipe takes: kind 'general' (resources by record, by tag or by a gathered-item filter, with a minimum rarity) or 'primary' (the reagent)
//	craft_cost                a money cost record (the currency tier and the amount)
//	craft_reward              the reward tables the recipes name, and the sub-tables they reach, each as the JSON of its containers (what a finished recipe yields)
//
// A recipe's cost is its StandardCurrencyCost (std_cost, a number in text) or, when that is empty, the craft_cost its cost_guid names.
const CraftingDDL = `
CREATE TABLE craft_station_def (
    guid             INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    display_name     TEXT NOT NULL,       -- the window's title ("Lumber Milling - Apprentice")
    kind             TEXT NOT NULL,       -- Type: 'Processing' or 'FastCraft'
    processing_type  TEXT NOT NULL,       -- ProcessingStationType without its prefix ('Debarker'; 'cCount' for none)
    max_tier         INTEGER NOT NULL,    -- the certification tier a recipe may need here: the tier of MaxCertificationTierId (Novice 0 ... Grand Master 4); -1 for none
    max_tier_guid    INTEGER NOT NULL,
    queue_size       INTEGER NOT NULL,
    simultaneous     INTEGER NOT NULL,    -- SimulateneousJobNum
    individual       INTEGER NOT NULL,    -- bIndividual: one queue for each player
    blacklisted      INTEGER NOT NULL,
    tax_tag          TEXT NOT NULL,       -- TaxTag.TagName, '' for none
    interaction_type INTEGER NOT NULL,    -- InteractionTypeId (the PlayerInteractionDef its use runs)
    recipe_set_guid  INTEGER NOT NULL,    -- CraftingRecipeSetsId
    min_quantity     TEXT NOT NULL,       -- MinimumBatchQuantityFormula
    max_quantity     TEXT NOT NULL,       -- MaximumBatchQuantityFormula
    window_icon      TEXT NOT NULL
);
CREATE TABLE craft_station_asset (
    asset_guid INTEGER PRIMARY KEY,       -- npc_asset
    def_guid   INTEGER NOT NULL           -- craft_station_def
);
CREATE TABLE craft_recipe_set (
    guid            INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    profession_guid INTEGER NOT NULL,
    tier_guid       INTEGER NOT NULL,
    blacklisted     INTEGER NOT NULL
);
CREATE TABLE craft_recipe_set_list (
    set_guid  INTEGER NOT NULL,
    ord       INTEGER NOT NULL,
    list_guid INTEGER NOT NULL,
    PRIMARY KEY (set_guid, ord)
) WITHOUT ROWID;
CREATE TABLE craft_recipe_list (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE craft_recipe_list_member (
    list_guid   INTEGER NOT NULL,
    ord         INTEGER NOT NULL,
    recipe_guid INTEGER NOT NULL,
    PRIMARY KEY (list_guid, ord)
) WITHOUT ROWID;
CREATE TABLE craft_pred (
    owner_kind      TEXT NOT NULL,        -- 'list' or 'recipe'
    owner_guid      INTEGER NOT NULL,
    ord             INTEGER NOT NULL,
    type            TEXT NOT NULL,        -- ERecipeUnavailableReason without its prefix: 'Biome', 'NodeLevel', 'Node', 'BuildingModification'
    node_level      TEXT NOT NULL,        -- ENodeLevel without its prefix: the least level the node must have
    node_level_n    INTEGER NOT NULL,     -- ... as a number (Wilderness 0, Crossroads 1, Encampment 2, Village 3, Town 4, City 5, Metropolis 6; -1 for a name this reader does not know)
    biomes          TEXT NOT NULL,        -- the biomes it holds in, comma separated (EBiomeType without its prefix)
    modifications   TEXT NOT NULL,        -- the node modifications it needs, comma separated guids
    nodes           TEXT NOT NULL,        -- the nodes it is for, comma separated guids
    hidden          INTEGER NOT NULL,     -- bHidden: the recipe is not shown when it fails
    hidden_in_folio INTEGER NOT NULL,     -- bHiddenInFolio
    PRIMARY KEY (owner_kind, owner_guid, ord)
) WITHOUT ROWID;
CREATE TABLE craft_recipe (
    guid             INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    profession_guid  INTEGER NOT NULL,
    tier_guid        INTEGER NOT NULL,    -- CertificationLevelId: the tier the crafter needs
    tier             INTEGER NOT NULL,    -- ... as a number (Novice 0 ... Grand Master 4); -1 for none
    category_tag     TEXT NOT NULL,
    base_duration    REAL NOT NULL,       -- seconds for one unit
    duration_formula TEXT NOT NULL,       -- '' for a crafting recipe
    quantity_formula TEXT NOT NULL,
    quality_formula  TEXT NOT NULL,
    min_quantity     TEXT NOT NULL,
    max_quantity     TEXT NOT NULL,
    std_cost         TEXT NOT NULL,       -- StandardCurrencyCost: a number, '' for none
    cost_guid        INTEGER NOT NULL,    -- CraftingCurrencyCostId (craft_cost), 0 for none
    reward_guid      INTEGER NOT NULL,    -- RewardId (craft_reward)
    distribution_guid INTEGER NOT NULL,   -- DistributionRewardId
    leveling_guid    INTEGER NOT NULL,    -- LevelingTemplateId: the leveling type the experience goes to besides the profession
    destination      TEXT NOT NULL,       -- ItemDestination: 'Inventory' or 'Equipment'
    default_quality  INTEGER NOT NULL,
    difficulty       INTEGER NOT NULL,
    fuel             INTEGER NOT NULL,
    fuel_tags        TEXT NOT NULL,       -- comma separated
    learnable        INTEGER NOT NULL,    -- true: the crafter must learn it
    can_fast_craft   INTEGER NOT NULL,
    can_gameplay     INTEGER NOT NULL,
    use_overrides    INTEGER NOT NULL,
    blacklisted      INTEGER NOT NULL
);
CREATE TABLE craft_recipe_input (
    recipe_guid INTEGER NOT NULL,
    kind        TEXT NOT NULL,            -- 'general' (GeneralResourceCost) or 'primary' (PrimaryResourceCosts)
    ord         INTEGER NOT NULL,
    item_guid   INTEGER NOT NULL,         -- the resource record, 0 when it is named by tags or a filter
    filter_guid INTEGER NOT NULL,         -- CostGatherItemFilter, 0 for none
    tags        TEXT NOT NULL,            -- MaterialTags, comma separated
    quantity    INTEGER NOT NULL,         -- for one unit
    min_rarity  INTEGER NOT NULL,         -- Common 0 ... Artifact 6
    consume     INTEGER NOT NULL,         -- bConsumeInCrafting
    PRIMARY KEY (recipe_guid, kind, ord)
) WITHOUT ROWID;
CREATE TABLE craft_cost (
    guid      INTEGER PRIMARY KEY,
    name      TEXT NOT NULL,
    tier_guid INTEGER NOT NULL,           -- CurrencyCostTierId (the currency tier)
    value     TEXT NOT NULL,              -- CurrencyCostValue: a number, in text
    base      INTEGER NOT NULL            -- BaseCurrencyCost
);
CREATE TABLE craft_reward (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    body TEXT NOT NULL                    -- JSON: the table's predicate, sub-table selection and sub-tables, and its containers with their rewards (items, experience, currency)
);
`

// CraftingTables are the tables CraftingDDL creates, in the DDL's order.
var CraftingTables = []string{"craft_station_def", "craft_station_asset", "craft_recipe_set", "craft_recipe_set_list", "craft_recipe_list", "craft_recipe_list_member", "craft_pred",
	"craft_recipe", "craft_recipe_input", "craft_cost", "craft_reward"}

func createCrafting(db *sql.DB) error {
	if _, err := db.Exec(CraftingDDL); err != nil {
		return fmt.Errorf("content: create the crafting tables: %w", err)
	}
	return nil
}
