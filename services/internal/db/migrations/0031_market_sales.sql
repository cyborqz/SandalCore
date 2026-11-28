-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Marketplace sales: the money a mail carries, the sales tax of each node and the purchases that were made.
--
-- THREE NEW TABLES. character_mail_money holds the money of a mail (a sale's price; a mail has at most one row): the copper, whether the player took it into the purse,
-- and, for a sale mail, the listing it belongs to and the units it sums up. character_mail_money_open_sale lets a listing have ONE unclaimed sale mail, so a later sale of
-- the same listing is added to that mail. node_market_taxes holds, for each node that has a row, the rate its sales tax charges (0.1 to 1; a node with no row charges the
-- game's default, 0.5) and the copper its sales have paid in (the tax goes to the node; nothing spends it yet). market_purchases holds the id of every purchase that was
-- made, so that a purchase asked for again after a lost answer is answered "done" and writes nothing.
--
-- Nothing is backfilled and no existing table, row, column or constraint is touched. A listing keeps its row when it is sold, removed or expired (its status says which:
-- 1 SOLD, 2 REMOVED, 4 EXPIRED_CLAIMED), and only a listed row is read at the start.
--
-- The version is 0031. The runner (internal/db/db.go) skips a version it has recorded, applies every other one in version order and allows gaps. This ADDS THREE TABLES AND
-- ONLY WRITES TO THEM. Rolling it back is a documented block in services/README.md ("Rolling back migration 0031").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_mail_money (
    mail_id       TEXT PRIMARY KEY REFERENCES character_mails (id) ON DELETE CASCADE,
    copper        BIGINT  NOT NULL
                  CONSTRAINT character_mail_money_copper CHECK (copper > 0 AND copper <= 10000000000000),
    claimed       BOOLEAN NOT NULL DEFAULT false,
    listing_id    TEXT REFERENCES character_market_listings (id) ON DELETE SET NULL
                  CONSTRAINT character_mail_money_listing_hex CHECK (listing_id IS NULL OR listing_id ~ '^[0-9a-f]{32}$'),
    sold_quantity INTEGER NOT NULL DEFAULT 0
                  CONSTRAINT character_mail_money_sold CHECK (sold_quantity >= 0)
);

-- One unclaimed sale mail for each listing: the next sale of the listing is added to it.
CREATE UNIQUE INDEX character_mail_money_open_sale ON character_mail_money (listing_id) WHERE listing_id IS NOT NULL AND NOT claimed;

CREATE TABLE node_market_taxes (
    node             BIGINT PRIMARY KEY
                     CONSTRAINT node_market_taxes_node CHECK (node <> 0),
    rate             REAL   NOT NULL DEFAULT 0.5
                     CONSTRAINT node_market_taxes_rate CHECK (rate >= 0.1 AND rate <= 1),
    collected_copper BIGINT NOT NULL DEFAULT 0
                     CONSTRAINT node_market_taxes_collected CHECK (collected_copper >= 0)
);

CREATE TABLE market_purchases (
    purchase_id  TEXT PRIMARY KEY
                 CONSTRAINT market_purchases_id_hex CHECK (purchase_id ~ '^[0-9a-f]{32}$' AND purchase_id <> repeat('0', 32)),
    listing_id   TEXT   NOT NULL REFERENCES character_market_listings (id) ON DELETE CASCADE,
    buyer_id     TEXT   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    node         BIGINT NOT NULL
                 CONSTRAINT market_purchases_node CHECK (node <> 0),
    quantity     INTEGER NOT NULL
                 CONSTRAINT market_purchases_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    price_copper BIGINT NOT NULL
                 CONSTRAINT market_purchases_price CHECK (price_copper > 0),
    tax_copper   BIGINT NOT NULL
                 CONSTRAINT market_purchases_tax CHECK (tax_copper >= 0),
    created_ms   BIGINT NOT NULL
                 CONSTRAINT market_purchases_created CHECK (created_ms > 0)
);

CREATE INDEX market_purchases_listing ON market_purchases (listing_id);
CREATE INDEX market_purchases_buyer ON market_purchases (buyer_id);

COMMENT ON TABLE character_mail_money IS 'Marketplace: the money a mail carries (a sale''s price), whether it was taken into the purse and, for a sale mail, its listing and the units it sums up.';
COMMENT ON COLUMN character_mail_money.mail_id IS 'The mail. Deleting the mail row removes this row.';
COMMENT ON COLUMN character_mail_money.copper IS 'The money in copper, at least 1.';
COMMENT ON COLUMN character_mail_money.claimed IS 'True once the player took the money into the purse (in the same transaction that wrote the purse).';
COMMENT ON COLUMN character_mail_money.listing_id IS 'The listing a sale mail belongs to; one unclaimed sale mail for each listing. NULL: a mail that is no sale mail, or one whose listing is gone.';
COMMENT ON COLUMN character_mail_money.sold_quantity IS 'The units the sales of this mail add up to.';
COMMENT ON TABLE node_market_taxes IS 'Marketplace: the sales tax rate of a node (0.1 to 1; a node with no row charges 0.5) and the copper its sales have paid in.';
COMMENT ON COLUMN node_market_taxes.node IS 'The city node whose Marketplace charges the tax.';
COMMENT ON COLUMN node_market_taxes.rate IS 'The share of a sale the buyer pays on top of the price, 0.1 to 1.';
COMMENT ON COLUMN node_market_taxes.collected_copper IS 'The tax the node has taken in, in copper; it grows by the tax of each sale, in the transaction of the sale.';
COMMENT ON TABLE market_purchases IS 'Marketplace: one row for each purchase that was made, so the same purchase asked for again is answered done and writes nothing.';
COMMENT ON COLUMN market_purchases.purchase_id IS 'The purchase''s id: 32 lower-case hex digits, not all zero.';
COMMENT ON COLUMN market_purchases.price_copper IS 'The unit price times the units bought, in copper.';
COMMENT ON COLUMN market_purchases.tax_copper IS 'The tax the buyer paid on top, in copper.';
COMMENT ON COLUMN market_purchases.created_ms IS 'When the purchase was made, Unix milliseconds (the database''s clock).';
