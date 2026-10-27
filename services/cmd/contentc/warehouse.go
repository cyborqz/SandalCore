// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"sort"

	"aocrop/services/internal/designdb"
)

// Storage clerks (the storage milestone, step 0; contentc version 25): warehouse_node, warehouse_def, warehouse_def_tab, warehouse_tab, warehouse_chunks_def,
// warehouse_chunks_def_chunk and warehouse_chunk (internal/content/warehouse.go says what each holds).  The expressions (when a tab or chunk is available, what it
// costs) are kept as the data's text: worldd evaluates them for the character who opens the window.

var warehouseRecordTypes = []string{"CityNodeRecordBase", "WarehouseStorageDefRecordBase", "WarehouseStorageMaterialTabRecordBase", "WarehouseStorageItemChunksDefRecordBase",
	"WarehouseStorageItemChunkRecordBase"}

// warehouseCostJSON is one cost of a tab or a chunk (WarehouseStorageCost): the currency (a record reference: its guid and its record type id), the value expression and the
// multiplier expression, both literals in the data.
type warehouseCostJSON struct {
	Currency int64  `json:"c"`
	TypeID   int64  `json:"ty"`
	Value    string `json:"v"`
	Mult     string `json:"m"`
}

// readWarehouseCosts reads a list of WarehouseStorageCost structs into JSON text ("[]" when there is none).
func readWarehouseCosts(where string, v designdb.Value) (string, error) {
	out := []warehouseCostJSON{}
	if l, ok := v.([]designdb.Value); ok {
		for i, e := range l {
			s, ok := e.(designdb.Struct)
			if !ok {
				continue
			}
			f := newFields(fmt.Sprintf("%s: cost %d", where, i), s)
			cur, _ := f.id("CurrencyDefId")
			typeID := int64(0)
			if id, ok := s["CurrencyDefId"].(designdb.Struct); ok {
				typeID = intOf(id["TypeId"])
			}
			out = append(out, warehouseCostJSON{Currency: cur, TypeID: typeID, Value: optStr(f, "ValueExpression"), Mult: optStr(f, "CostMultiplier")})
			if f.err != nil {
				return "", f.err
			}
		}
	}
	b, err := json.Marshal(out)
	return string(b), err
}

// optStr reads a text property a record may not hold (a test or guild record): "" then.
func optStr(f *fields, name string) string {
	v, _ := f.get(name)
	s, _ := v.(string)
	return s
}

// optList reads a list a record may not hold: none then.
func optList(f *fields, name string) designdb.Value {
	v, _ := f.get(name)
	return v
}

func buildWarehouse(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range warehouseRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	var nodes, defs, tabs, chunkDefs, chunks int
	insert := func(query string, args ...any) error { return w.exec(query, args...) }

	// the nodes: the three defs each names
	cityNodes := append([]designdb.Row(nil), src.rows["CityNodeRecordBase"]...)
	sort.Slice(cityNodes, func(i, j int) bool { return cityNodes[i].Guid < cityNodes[j].Guid })
	for _, r := range cityNodes {
		f := newFields(fmt.Sprintf("CityNode %s (%d)", r.Name, r.Guid), r.Fields)
		resource, _ := f.id("ResourceWarehouseStorageDefId")
		crate, _ := f.id("CrateWarehouseStorageDefId")
		chunkDef, _ := f.id("WarehouseStorageItemChunksDefId")
		if f.err != nil {
			return f.err
		}
		if resource == 0 && crate == 0 && chunkDef == 0 {
			continue
		}
		if err := insert(`INSERT INTO warehouse_node (node_guid, resource_def, crate_def, chunks_def) VALUES (?, ?, ?, ?)`, r.Guid, resource, crate, chunkDef); err != nil {
			return err
		}
		nodes++
	}

	// the defs of tabs and their tabs, in the def's order
	for _, r := range src.rows["WarehouseStorageDefRecordBase"] {
		f := newFields(fmt.Sprintf("WarehouseStorageDef %s (%d)", r.Name, r.Guid), r.Fields)
		cols := f.int("TabSlotColumns")
		rows := f.int("TabSlotRows")
		members := f.ids("MaterialTabsIds")
		if f.err != nil {
			return f.err
		}
		if err := insert(`INSERT INTO warehouse_def (guid, name, columns, rows) VALUES (?, ?, ?, ?)`, r.Guid, r.Name, cols, rows); err != nil {
			return err
		}
		for i, t := range members {
			if err := insert(`INSERT INTO warehouse_def_tab (def_guid, ord, tab_guid) VALUES (?, ?, ?)`, r.Guid, i, t); err != nil {
				return err
			}
		}
		defs++
	}
	for _, r := range src.rows["WarehouseStorageMaterialTabRecordBase"] {
		f := newFields(fmt.Sprintf("WarehouseStorageMaterialTab %s (%d)", r.Name, r.Guid), r.Fields)
		storageType := enumValue(optStr(f, "StorageType"))
		member := optBool(f, "bIsMemberStorage")
		special := optBool(f, "IsSpecializable")
		requirement := optStr(f, "AvailabilityRequirements")
		costs, err := readWarehouseCosts(f.where, optList(f, "TabCosts"))
		if err != nil {
			return err
		}
		if f.err != nil {
			return f.err
		}
		kind := 0
		if storageType == "Crates" {
			kind = 1
		}
		if err := insert(`INSERT INTO warehouse_tab (guid, name, storage_type, member, specializable, requirement, costs) VALUES (?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, b2i(member), b2i(special), requirement, costs); err != nil {
			return err
		}
		tabs++
	}

	// the defs of item chunks and their chunks, in the def's order
	for _, r := range src.rows["WarehouseStorageItemChunksDefRecordBase"] {
		f := newFields(fmt.Sprintf("WarehouseStorageItemChunksDef %s (%d)", r.Name, r.Guid), r.Fields)
		members := f.ids("ItemChunksIds")
		if f.err != nil {
			return f.err
		}
		if err := insert(`INSERT INTO warehouse_chunks_def (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for i, c := range members {
			if err := insert(`INSERT INTO warehouse_chunks_def_chunk (def_guid, ord, chunk_guid) VALUES (?, ?, ?)`, r.Guid, i, c); err != nil {
				return err
			}
		}
		chunkDefs++
	}
	for _, r := range src.rows["WarehouseStorageItemChunkRecordBase"] {
		f := newFields(fmt.Sprintf("WarehouseStorageItemChunk %s (%d)", r.Name, r.Guid), r.Fields)
		size := f.int("ChunkSize")
		level := int64(0)
		if v, ok := f.get("LevelRequirement"); ok {
			level = intOf(v)
		}
		requirement := optStr(f, "AvailabilityRequirements")
		costs, err := readWarehouseCosts(f.where, optList(f, "ChunkCosts"))
		if err != nil {
			return err
		}
		if f.err != nil {
			return f.err
		}
		if err := insert(`INSERT INTO warehouse_chunk (guid, name, size, level, requirement, costs) VALUES (?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, size, level, requirement, costs); err != nil {
			return err
		}
		chunks++
	}
	opt.logf("warehouse: %d node(s), %d tab def(s), %d tab(s), %d chunk def(s), %d chunk(s)", nodes, defs, tabs, chunkDefs, chunks)
	meta["warehouse.nodes"] = fmt.Sprint(nodes)
	meta["warehouse.chunks"] = fmt.Sprint(chunks)
	return nil
}
