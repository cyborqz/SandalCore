// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Town vendors (the vendors milestone, step 0, "your purse and the general merchant"; contentc version 18): which NPC opens which shop, what every listing costs,
// and the rules the selling and repair steps need. One version bump carries the data of the first three steps.
//
//	npc_vendor_link        which VendorDef the use of an NPC opens: an instance's own interaction wins over its asset's. Only an interaction that opens a shop
//	                       links; a bare vendor table id on a commodity merchant or a storage clerk feeds the crate window and links nothing
//	vendor_def_data        what the vendor_def table lacks: the tax tag, whether the def is blacklisted, whether it buys from the warehouse
//	vendor_def_modifier    the def's price modifier records, in order (vendor_modifier); vendor_def_sell_filter the sell filters (vendor_sell_filter)
//	vendor_modifier        one price modifier: the operations it applies to (contexts: 1 buy, 2 sell, 4 repair), how it changes a price, where its value comes from
//	                       (an expression, a curve at a factor, or the modifiers it holds), the items and currencies it touches (filter and currencies: JSON)
//	vendor_sell_filter     what a def buys: a whitelist of items and a tag query (JSON)
//	base_item_value        a base value record: the price expression per currency, and the one for selling when the record overrides it
//	item_base_value        which base value record an item uses
//	vendor_offer           an offer: the price expression per currency (JSON list of [currency, expression])
//	vendor_availability    the predicates of a tab (listing_ord -1) or a listing: an expression, the failure text and whether the entry hides when it fails
//	vendor_price           the price of every buyable listing, per currency, per UNIT, before the node tax: the listing's expression (the offer's when it has one), rounded up,
//	                       through the def's buy modifiers, rounded up
//	item_vendor            (contentc version 19, the selling step) per item: whether a shop may buy it (Vendorable, Blacklisted) and the range its rarity is held inside
//	item_sell_value        (version 19) per item with a base value (vendorable or not), rarity and currency: the item's base value for a sale or a repair, per unit, rounded up
//	                       (the record's own sale expressions when it overrides them, else its buy expressions); the def's modifiers are applied by the reader
//
// A tag query (filter) is {"tok": [..], "tags": [..]}: the query's token stream (version, a non-empty flag, then the expression tree: an expression type, and for a tag
// set the number of tags and their indexes into tags, for an expression set the number of sub-expressions) and the tag dictionary.
const VendorsDDL = `
CREATE TABLE npc_vendor_link (
    source_kind TEXT NOT NULL,         -- 'instance' (PopulationInstance.InteractResponseId) or 'asset' (PopulationAsset.InteractionDefId)
    source_guid INTEGER NOT NULL,
    vendor_guid INTEGER NOT NULL,      -- the VendorDef the use opens
    PRIMARY KEY (source_kind, source_guid)
) WITHOUT ROWID;
CREATE TABLE vendor_def_data (
    vendor_guid    INTEGER PRIMARY KEY,
    blacklisted    INTEGER NOT NULL,   -- Blacklisted
    tax_tag        TEXT NOT NULL,      -- TaxTag.TagName, '' for none
    from_warehouse INTEGER NOT NULL    -- bCanSellFromWarehouse
);
CREATE TABLE vendor_def_modifier (
    vendor_guid   INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    modifier_guid INTEGER NOT NULL,
    PRIMARY KEY (vendor_guid, ord)
) WITHOUT ROWID;
CREATE TABLE vendor_def_sell_filter (
    vendor_guid INTEGER NOT NULL,
    ord         INTEGER NOT NULL,
    filter_guid INTEGER NOT NULL,
    PRIMARY KEY (vendor_guid, ord)
) WITHOUT ROWID;
CREATE TABLE vendor_modifier (
    guid       INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    contexts   INTEGER NOT NULL,       -- AffectedContexts: the operations it applies to (a bit mask)
    operation  TEXT NOT NULL,          -- Add, Multiply, Set
    source     TEXT NOT NULL,          -- Expression, Curve, PriceModifierSet
    factor     TEXT NOT NULL,          -- PriceFactor: what a Curve is read at
    expression TEXT NOT NULL,          -- ValueExpression
    curve      TEXT NOT NULL,          -- JSON list of [time, value, interpolation] keys, '[]' for none
    filter     TEXT NOT NULL,          -- JSON tag query of the items it applies to, '' for all
    currencies TEXT NOT NULL,          -- JSON list of the currency tiers it applies to
    children   TEXT NOT NULL           -- JSON list of the modifiers a PriceModifierSet holds, in order
);
CREATE TABLE vendor_sell_filter (
    guid      INTEGER PRIMARY KEY,
    name      TEXT NOT NULL,
    filter    TEXT NOT NULL,           -- JSON tag query, '' for none
    whitelist TEXT NOT NULL            -- JSON list of item guids
);
CREATE TABLE base_item_value (
    guid          INTEGER PRIMARY KEY,
    name          TEXT NOT NULL,
    buy_currency  INTEGER NOT NULL,    -- BuyingCurrencyTierId
    sell_currency INTEGER NOT NULL,    -- SellingCurrencyTierId
    buy           TEXT NOT NULL,       -- JSON list of [currency tier, expression]
    override_sell INTEGER NOT NULL,    -- bUsesOverrideSellValues
    sell          TEXT NOT NULL        -- JSON list of [currency tier, expression], '[]' when sold at the modified buy value
);
CREATE TABLE item_base_value (
    item_guid       INTEGER PRIMARY KEY,
    base_value_guid INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE vendor_offer (
    guid   INTEGER PRIMARY KEY,
    name   TEXT NOT NULL,
    prices TEXT NOT NULL               -- JSON list of [currency tier, expression]
);
CREATE TABLE vendor_availability (
    inventory_guid INTEGER NOT NULL,
    listing_ord    INTEGER NOT NULL,   -- -1: the tab's own predicates
    ord            INTEGER NOT NULL,
    expression     TEXT NOT NULL,      -- AvailabilityPredicate
    failure        TEXT NOT NULL,      -- the text the window shows when it fails
    hidden         INTEGER NOT NULL,   -- bHiddenIfUnavailable of the predicate
    PRIMARY KEY (inventory_guid, listing_ord, ord)
) WITHOUT ROWID;
CREATE TABLE vendor_price (
    vendor_guid    INTEGER NOT NULL,
    inventory_guid INTEGER NOT NULL,
    ord            INTEGER NOT NULL,   -- the listing's index in its tab
    item_guid      INTEGER NOT NULL,
    variation_guid INTEGER NOT NULL,   -- ItemVariation of the listing, 0 for none
    offer_guid     INTEGER NOT NULL,   -- the offer this row prices, 0 for the listing's own price
    currency_guid  INTEGER NOT NULL,   -- the currency tier
    price          INTEGER NOT NULL,   -- per unit, before the node tax
    PRIMARY KEY (vendor_guid, inventory_guid, ord, offer_guid, currency_guid)
) WITHOUT ROWID;
CREATE INDEX vendor_price_by_item ON vendor_price (vendor_guid, item_guid);
CREATE TABLE item_vendor (
    item_guid   INTEGER PRIMARY KEY,
    vendorable  INTEGER NOT NULL,      -- Vendorable: a shop may buy the item at all
    blacklisted INTEGER NOT NULL,      -- Blacklisted
    rarity_min  INTEGER NOT NULL,      -- RarityMin as the rarity ordinal (Common 0 ... Artifact 6): the item's rarity is held inside this range
    rarity_max  INTEGER NOT NULL       -- RarityMax
) WITHOUT ROWID;
CREATE TABLE item_sell_value (
    item_guid     INTEGER NOT NULL,
    rarity        INTEGER NOT NULL,    -- the item's rarity ordinal
    currency_guid INTEGER NOT NULL,    -- the currency tier
    price         INTEGER NOT NULL,    -- the item's base value for a sale or a repair at that rarity, per unit, rounded up, before the def's modifiers
    PRIMARY KEY (item_guid, rarity, currency_guid)
) WITHOUT ROWID;
`

// VendorTables are the tables VendorsDDL creates, in the DDL's order.
var VendorTables = []string{"npc_vendor_link", "vendor_def_data", "vendor_def_modifier", "vendor_def_sell_filter", "vendor_modifier", "vendor_sell_filter",
	"base_item_value", "item_base_value", "vendor_offer", "vendor_availability", "vendor_price", "item_vendor", "item_sell_value"}

func createVendors(db *sql.DB) error {
	if _, err := db.Exec(VendorsDDL); err != nil {
		return fmt.Errorf("content: create the vendor tables: %w", err)
	}
	return nil
}
