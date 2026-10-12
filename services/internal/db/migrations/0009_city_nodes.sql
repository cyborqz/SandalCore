-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Nodes 1, stage 1: the city node service (citynode).
--
-- Three NEW tables; no existing table, row, column or constraint is touched.  citynode (services/internal/citynode) owns them:
--   city_node_state  the live state of each city node: its level, its XP, its vassal link (parent) and when it last changed.
--                    citynode creates a row (level 0, Wilderness) for every node it knows, when it starts; this migration inserts none.
--   city_node_clock  the node clock, one row: the node time is  real time x speed + offset_seconds  (the speed knob and the GM's skip).
--   city_node_seed   one row: when deploy/citynode_seed.json was applied (NULL: not yet; citynode applies it once, at its first start).
-- Rolling it back is a documented block in services/README.md ("Rolling back migration 0009").

SET LOCAL lock_timeout = '10s';

CREATE TABLE city_node_state (
    node_guid   BIGINT      PRIMARY KEY,                       -- the CityNodeRecordBase guid (content.db)
    level       SMALLINT    NOT NULL CHECK (level >= 0),       -- ENodeLevel: 0 Wilderness ... 4 Town (citynode bounds it by the data's MaxNodeLevel)
    experience  BIGINT      NOT NULL DEFAULT 0 CHECK (experience >= 0),
    parent_guid BIGINT      NULL REFERENCES city_node_state (node_guid) CHECK (parent_guid IS DISTINCT FROM node_guid),
    updated_at  TIMESTAMPTZ NOT NULL
);

COMMENT ON TABLE city_node_state IS 'Nodes 1: the live state of each city node (citynode owns it).  node_guid is a CityNodeRecordBase guid; parent_guid is the vassal link.';

CREATE TABLE city_node_clock (
    id             BOOLEAN          PRIMARY KEY DEFAULT TRUE CHECK (id),   -- one row
    offset_seconds BIGINT           NOT NULL DEFAULT 0,
    speed          DOUBLE PRECISION NOT NULL DEFAULT 1 CHECK (speed > 0)   -- the speed knob (1: real time)
);

INSERT INTO city_node_clock (id) VALUES (TRUE);

COMMENT ON TABLE city_node_clock IS 'Nodes 1: the node clock, one row.  Node time (unix seconds) = real time x speed + offset_seconds.';

CREATE TABLE city_node_seed (
    id         BOOLEAN     PRIMARY KEY DEFAULT TRUE CHECK (id),             -- one row
    applied_at TIMESTAMPTZ NULL
);

INSERT INTO city_node_seed (id) VALUES (TRUE);

COMMENT ON TABLE city_node_seed IS 'Nodes 1: when deploy/citynode_seed.json was applied (NULL: not yet).  citynode applies it once; nodectl seed --force applies it again.';
