// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"

	"aocrop/services/internal/designdb"
)

// Embersprings (contentc version 26): attune_point, respawn_point and npc_instance_respawn (internal/content/respawn.go says what each holds).

// respawnRecordTypes are the record structs the builder reads.
var respawnRecordTypes = []string{"AttuneRecordBase", "RespawnRecordBase", "MasterAttuneRecordBase", "PopulationInstanceRecordBase"}

// respawnNumber reads a number the data types as a float or an integer (0 for anything else).
func respawnNumber(v designdb.Value) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case float32:
		return float64(x)
	case int64:
		return float64(x)
	case uint64:
		return float64(x)
	}
	return 0
}

// respawnVector reads an FVector (X, Y, Z) out of a struct field.
func respawnVector(s designdb.Struct, name string) (x, y, z float64) {
	v, _ := s[name].(designdb.Struct)
	return respawnNumber(v["X"]), respawnNumber(v["Y"]), respawnNumber(v["Z"])
}

// The two enums the respawn records use, by value name (the shared lookup in aocexpr knows the nodes' enums only): EPlayerRespawnType and ESiegeTeamType, as the game's own list has them.
var respawnKinds = map[string]int{"None": 0, "PlayerRespawn": 1, "AttunedRespawn": 2, "CorruptedRespawn": 3, "NodeSiegeCamp": 4, "NodeSiegeSafeHouse": 5, "GuildWar": 6, "NodeWar": 7, "ForwardOperatingBase": 8}
var siegeTeams = map[string]int{"NoTeam": 0, "Defender": 1, "Attacker": 2, "LeastPopulous": 3}

// respawnEnum reads an enum field "Type::Value" as the ordinal of its value in table; a value the table does not hold is an error (the build says so rather than storing a 0).
func respawnEnum(where string, v designdb.Value, typ string, table map[string]int) (int, error) {
	s := strOf(v)
	prefix := typ + "::"
	if len(s) <= len(prefix) || s[:len(prefix)] != prefix {
		return 0, fmt.Errorf("%s: %q is not a value of %s", where, s, typ)
	}
	n, ok := table[s[len(prefix):]]
	if !ok {
		return 0, fmt.Errorf("%s: %q is not a value this build knows of %s", where, s, typ)
	}
	return n, nil
}

// respawnText reads an FText as its source text (empty for none).
func respawnText(v designdb.Value) string {
	if t, ok := v.(designdb.Text); ok {
		return t.Source
	}
	return ""
}

func buildRespawn(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range respawnRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	// the master list: the number each place has (the index a character's saved set holds)
	index := map[int64]int64{}
	for _, r := range src.rows["MasterAttuneRecordBase"] {
		f := newFields(fmt.Sprintf("MasterAttuneRecordBase %s (%d)", r.Name, r.Guid), r.Fields)
		for _, p := range f.pairs("MasterAttuneMapIds") {
			n := intOf(p.Key)
			attune := idGuid(p.Value)
			if n <= 0 || attune == 0 {
				return fmt.Errorf("%s: an entry that is no number and place (%v, %v)", f.where, p.Key, p.Value)
			}
			if _, dup := index[attune]; dup {
				return fmt.Errorf("%s: the place %d has two numbers", f.where, attune)
			}
			index[attune] = n
		}
		if f.err != nil {
			return f.err
		}
	}
	var places, withIndex int
	for _, r := range src.rows["AttuneRecordBase"] {
		f := newFields(fmt.Sprintf("AttuneRecordBase %s (%d)", r.Name, r.Guid), r.Fields)
		data := f.strukt("AttunementData")
		if f.err != nil {
			return f.err
		}
		x, y, z := respawnVector(data, "WorldLocation")
		radius := respawnNumber(data["AutoAttuneRadius"])
		siege, err := respawnEnum(f.where, data["SiegeTeamType"], "ESiegeTeamType", siegeTeams)
		if err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO attune_point (guid, name, idx, x, y, z, radius, descriptor, level_name, siege_team) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, index[r.Guid], x, y, z, radius, respawnText(data["PlayerFacingDescriptor"]), respawnText(data["LevelName"]), siege); err != nil {
			return err
		}
		places++
		if index[r.Guid] != 0 {
			withIndex++
		}
	}
	for attune := range index {
		found := false
		for _, r := range src.rows["AttuneRecordBase"] {
			if r.Guid == attune {
				found = true
				break
			}
		}
		if !found {
			return fmt.Errorf("the master list names the place %d, which the design database does not hold", attune)
		}
	}
	var points int
	for _, r := range src.rows["RespawnRecordBase"] {
		f := newFields(fmt.Sprintf("RespawnRecordBase %s (%d)", r.Name, r.Guid), r.Fields)
		spawn := f.strukt("PlayerSpawn")
		if f.err != nil {
			return f.err
		}
		bx, by, bz := respawnVector(spawn, "BoxExtent")
		kind, err := respawnEnum(f.where, spawn["RespawnType"], "EPlayerRespawnType", respawnKinds)
		if err != nil {
			return err
		}
		team, err := respawnEnum(f.where, spawn["SiegeTeamType"], "ESiegeTeamType", siegeTeams)
		if err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO respawn_point (guid, name, kind, attune_guid, box_x, box_y, box_z, exclusion, max_range, descriptor, siege_team, unflag_combatants)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, idGuid(spawn["AttuneRecordId"]), bx, by, bz,
			respawnNumber(spawn["ExclusionRadius"]), respawnNumber(spawn["MaximumRange"]), respawnText(spawn["PlayerFacingDescriptor"]),
			team, b2i(spawn["bUnflagCombatants"] == true)); err != nil {
			return err
		}
		points++
	}
	var links int
	for _, r := range src.rows["PopulationInstanceRecordBase"] {
		respawn := idGuid(r.Fields["RespawnDataId"])
		if respawn == 0 {
			continue
		}
		if err := w.exec(`INSERT INTO npc_instance_respawn (instance_guid, respawn_guid) VALUES (?, ?)`, r.Guid, respawn); err != nil {
			return err
		}
		links++
	}
	opt.logf("respawn: %d places (%d numbered), %d respawn definitions, %d instances name one", places, withIndex, points, links)
	return nil
}
