// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Currencies and the stack sizes of the bags (the stacking milestone, step 1, "Glint tiers"; contentc version 24). One version bump carries the data of the first steps
// (the Glint kinds, the names of the items, the bags' stack sizes), so the later steps change worldd only.
//
//	currency_tier   a CurrencyTierDef: a set of currency kinds that make one another up, in order (Money: copper, silver, gold; Certificate_Glint_CurrencyTier: the six Glint kinds),
//	                and the most the wallet holds of it in units of its smallest kind
//	currency_def    a CurrencyDef: one kind of a tier. conversion_rate is how many of this kind make one of the next kind of the tier (the last kind's number is not used); item_guid is
//	                the item record that is the kind when it is carried as an item (the Glint certificates, 0 for the coins)
//	bag_stack_mod   what the stat block of an item adds to the stack size of the stacks its container holds: one row for each stat of the family Artisan_<Gathering|Processing>_..._Add_MaxStack
//	                and _Mult_MaxStack that a stat block has (the 65 resource bags), with the stat's value at each rarity as stat_block_stat holds it
const CurrencyDDL = `
CREATE TABLE currency_tier (
    guid              INTEGER PRIMARY KEY,
    name              TEXT NOT NULL,
    max_base_quantity INTEGER NOT NULL    -- MaxBaseQuantity, in units of the tier's first kind
);
CREATE TABLE currency_def (
    guid            INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    tier_guid       INTEGER NOT NULL,     -- the currency_tier whose TiersIds lists the kind, 0 when none does
    ord             INTEGER NOT NULL,     -- the kind's place in TiersIds, 0 first (the smallest kind); 0 when no tier lists it
    item_guid       INTEGER NOT NULL,     -- ItemOverrideId: the item record the kind is carried as, 0 for none
    conversion_rate INTEGER NOT NULL      -- TieredConversionRate
);
CREATE INDEX currency_def_by_item ON currency_def (item_guid);
CREATE INDEX currency_def_by_tier ON currency_def (tier_guid, ord);
CREATE TABLE bag_stack_mod (
    item_guid     INTEGER NOT NULL,       -- the item whose stat block holds the stat
    stat_guid     INTEGER NOT NULL,       -- the StatTypeDef (Artisan_Gathering_Mining_Add_MaxStack)
    stat_name     TEXT NOT NULL,
    tag           TEXT NOT NULL,          -- the gameplay tag of the items the modifier applies to, taken from the stat's name: Artisanship.Gathering.Mining; Artisanship.Gathering for
                                          --   the stat with no profession in its name (the generic bag), Artisanship.Processing for the processing one
    action        INTEGER NOT NULL,       -- 0 Add, 1 Mult (FStorageMaxStackSizeModifier's Action, from the stat's name)
    common_min    REAL NOT NULL,
    common_max    REAL NOT NULL,
    uncommon_min  REAL NOT NULL,
    uncommon_max  REAL NOT NULL,
    rare_min      REAL NOT NULL,
    rare_max      REAL NOT NULL,
    heroic_min    REAL NOT NULL,
    heroic_max    REAL NOT NULL,
    epic_min      REAL NOT NULL,
    epic_max      REAL NOT NULL,
    legendary_min REAL NOT NULL,
    legendary_max REAL NOT NULL,
    artifact_min  REAL NOT NULL,
    artifact_max  REAL NOT NULL,
    PRIMARY KEY (item_guid, stat_guid)
) WITHOUT ROWID;
`

// CurrencyTables are the tables CurrencyDDL creates, in the DDL's order.
var CurrencyTables = []string{"currency_tier", "currency_def", "bag_stack_mod"}

func createCurrency(db *sql.DB) error {
	if _, err := db.Exec(CurrencyDDL); err != nil {
		return fmt.Errorf("content: create the currency tables: %w", err)
	}
	return nil
}
