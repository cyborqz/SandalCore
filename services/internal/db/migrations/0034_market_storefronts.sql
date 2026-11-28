-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Marketplace storefront descriptions: the text a seller gives his storefront at one Marketplace, and the count of times a Marketplace's storefront pages changed.
--
-- TWO NEW TABLES. market_storefronts holds, for each (market node, seller) that has set one, the storefront's description (1 to 50 characters, no control character); a
-- storefront with no row shows the default "<name>'s Storefront", and an empty description deletes the row. node_market_headers holds, for each node whose storefront pages
-- ever changed, how many times they changed: the page's HeaderVersion is the game's own 1843 plus this count, and the client asks for pages again only when the version it
-- holds is smaller. A description is written together with the raise of its node's count, in ONE transaction (SetStorefrontDescription).
--
-- Nothing is backfilled and no existing table, row, column or constraint is touched. A seller's rows go with the seller (deleting the character removes them).
--
-- The version is 0034: 0032 is held by the trades work (character_trades) and 0033 by the node reputation work, neither of which is in this tree. The runner (internal/db/db.go) skips a version it has recorded, applies
-- every other one in version order and allows gaps, so 0032 and 0033 still apply when they land. This ADDS TWO TABLES AND ONLY WRITES TO THEM. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0034").

SET LOCAL lock_timeout = '10s';

CREATE TABLE market_storefronts (
    node         BIGINT NOT NULL
                 CONSTRAINT market_storefronts_node CHECK (node <> 0),
    character_id TEXT   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    description  TEXT   NOT NULL
                 CONSTRAINT market_storefronts_length CHECK (char_length(description) BETWEEN 1 AND 50)
                 CONSTRAINT market_storefronts_plain CHECK (description ~ '^[^\x01-\x1f\x7f]+$'),
    updated_ms   BIGINT NOT NULL
                 CONSTRAINT market_storefronts_updated CHECK (updated_ms > 0),
    CONSTRAINT market_storefronts_pkey PRIMARY KEY (node, character_id)
);

CREATE INDEX market_storefronts_by_node ON market_storefronts (node);

CREATE TABLE node_market_headers (
    node          BIGINT PRIMARY KEY
                  CONSTRAINT node_market_headers_node CHECK (node <> 0),
    header_raises BIGINT NOT NULL DEFAULT 0
                  CONSTRAINT node_market_headers_raises CHECK (header_raises >= 0)
);

COMMENT ON TABLE market_storefronts IS 'Marketplace: the description a seller gave his storefront at one Marketplace (a node). A storefront with no row shows the default text.';
COMMENT ON COLUMN market_storefronts.node IS 'The city node whose Marketplace shows the storefront.';
COMMENT ON COLUMN market_storefronts.character_id IS 'The seller. Deleting the character removes the row.';
COMMENT ON COLUMN market_storefronts.description IS 'The text, 1 to 50 characters, with no control character and no trimming: exactly what the seller sent.';
COMMENT ON COLUMN market_storefronts.updated_ms IS 'When the text was last changed, Unix milliseconds (the database''s clock).';
COMMENT ON TABLE node_market_headers IS 'Marketplace: how many times the storefront pages of a node changed. The page version the client holds is 1843 plus this count.';
COMMENT ON COLUMN node_market_headers.node IS 'The city node whose Marketplace the count belongs to.';
COMMENT ON COLUMN node_market_headers.header_raises IS 'The count: it grows by one in the transaction of every description change.';
