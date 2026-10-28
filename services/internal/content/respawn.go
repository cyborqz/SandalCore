// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Embersprings (the respawn points a player discovers and attunes to; contentc version 26): the places, the respawn definitions and the link from a placed spawner to its definition.
//
//	attune_point         one place a player can be attuned to (AttuneRecordBase): its number in the master list (idx, 1 to 67; 0 when the list has none), where it is, how close a player
//	                     has to come to discover it (radius, cm) and the name the players see (descriptor)
//	respawn_point        one respawn definition (RespawnRecordBase): its kind (EPlayerRespawnType: 2 is an attuned respawn, an Emberspring), the place it belongs to (attune_guid; 0 for
//	                     none), the box a player comes back in (box_*, half sizes in cm), the exclusion radius, the largest distance it serves (0: no limit) and its own name
//	npc_instance_respawn the population instances that name a respawn definition (the spawner actors an Emberspring is placed by)
const RespawnDDL = `
CREATE TABLE attune_point (
    guid       INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    idx        INTEGER NOT NULL,        -- the number in MasterAttunementRecord's list (the index a character's saved set holds); 0 for none
    x          REAL NOT NULL,           -- WorldLocation, cm
    y          REAL NOT NULL,
    z          REAL NOT NULL,
    radius     REAL NOT NULL,           -- AutoAttuneRadius, cm
    descriptor TEXT NOT NULL,           -- PlayerFacingDescriptor (the name the players see)
    level_name TEXT NOT NULL,           -- LevelName (the map the place is in)
    siege_team INTEGER NOT NULL         -- ESiegeTeamType ordinal
);
CREATE TABLE respawn_point (
    guid              INTEGER PRIMARY KEY,
    name              TEXT NOT NULL,
    kind              INTEGER NOT NULL, -- EPlayerRespawnType ordinal: 1 player, 2 attuned, 3 corrupted, 4 node siege camp, 5 node siege safe house, 6 guild war, 7 node war, 8 forward base
    attune_guid       INTEGER NOT NULL, -- attune_point; 0 for none
    box_x             REAL NOT NULL,    -- BoxExtent, cm (half sizes)
    box_y             REAL NOT NULL,
    box_z             REAL NOT NULL,
    exclusion         REAL NOT NULL,    -- ExclusionRadius, cm
    max_range         REAL NOT NULL,    -- MaximumRange, cm; 0 for no limit
    descriptor        TEXT NOT NULL,    -- the respawn's own PlayerFacingDescriptor ('' for the attuned ones: the place names them)
    siege_team        INTEGER NOT NULL, -- ESiegeTeamType ordinal
    unflag_combatants INTEGER NOT NULL
);
CREATE TABLE npc_instance_respawn (
    instance_guid INTEGER PRIMARY KEY,  -- npc_instance
    respawn_guid  INTEGER NOT NULL      -- respawn_point
);
`

// RespawnTables are the tables RespawnDDL creates, in the DDL's order.
var RespawnTables = []string{"attune_point", "respawn_point", "npc_instance_respawn"}

func createRespawn(db *sql.DB) error {
	if _, err := db.Exec(RespawnDDL); err != nil {
		return fmt.Errorf("content: create the respawn tables: %w", err)
	}
	return nil
}
