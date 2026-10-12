-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The mounts milestone, stage 3: the GM starter set's test mounts.
--
-- The client's vendor window can list only 44 of the 101 mounts, so the GM starter set (AOC_GM_STARTER_SET=1) also GRANTS a selection of test mounts.
-- A character that already had the set (characters.gm_starter_set_granted_at) must get them too: the character service tops the set up at its start.  This column records WHEN a
-- top-up first gave a character something (informational: the top-up itself looks at what the character holds, so a mount thrown away is given again).
--
-- This ADDS ONE NULLABLE COLUMN: no row is rewritten, no constraint changes, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0008").

SET LOCAL lock_timeout = '10s';

ALTER TABLE characters
    ADD COLUMN gm_starter_mounts_granted_at TIMESTAMPTZ;

COMMENT ON COLUMN characters.gm_starter_mounts_granted_at IS 'The mounts milestone: when the GM starter set''s test mounts were first put into the bag (NULL: not yet).  Informational: the top-up looks at what the character holds, not at this.';
