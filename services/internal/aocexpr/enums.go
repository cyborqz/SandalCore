// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

// The enums an expression compares a host function's answer with, by ordinal (Nodes 1, stage 1).
//
// A compiled program has only numbers on its stack, so a host function that answers with an enum (GetNodeBiome, Node_GetLevel,
// Node_GetNodeType) answers with the value's ordinal, and the compiler turns the enum literal it is compared with into the same
// ordinal. The ordinals are the client's own (the usmap order of the enum's values: docs of the nodes research, exe.md appendix E), and
// the C++ host (server/worldd/world/city_nodes.h) answers with the same numbers: contentc writes them into the node tables
// (city_node.node_type_ord, city_zoi.biome_ord ...) so that the C++ side never needs the names.
var enumValues = map[string][]string{
	"EBiomeType": {"Default", "AlpineForest", "AncientForest", "ArcticTundra", "Asian", "Badlands", "Desert", "Fjords", "Glacier", "Grasslands",
		"Iceland", "Jungle", "MagicalForest", "RedwoodForest", "Riverlands", "Swamp", "Taiga", "TabletopMountains", "Tropics", "Volcano",
		"UnderRealm", "Highlands", "cCount"},
	"ENodeType":           {"Default", "Scientific", "Divine", "Martial", "Economic", "cCount"},
	"ENodeLevel":          {"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis", "cCount"},
	"ENodeReputationRank": {"Peasant", "Commoner", "Liege", "Noble", "Aristocrat", "cCount"},
	"EBuildingState":      {"Empty", "UnderConstruction", "Operational", "Disarray", "Damaged", "Repairing", "Destroyed", "Demolition"},
	"ECharacterRace":      {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar", "cCount"},
	"ESeason":             {"Winter", "Spring", "Summer", "Fall", "cCount"},
	"EZoiType":            {"HasNode", "CanOwn", "CannotOwn", "cCount"},
}

// enumAlias are the spellings the data uses for a type: the layouts' seasons are ESeasons::Summer.
var enumAlias = map[string]string{"ESeasons": "ESeason"}

// EnumOrdinal is the ordinal of an enum literal Type::Value, and false when the type or the value is not one of the nodes' enums.
func EnumOrdinal(typ, value string) (int, bool) {
	if a, ok := enumAlias[typ]; ok {
		typ = a
	}
	for i, v := range enumValues[typ] {
		if v == value {
			return i, true
		}
	}
	return 0, false
}

// (C.Y.B + CoA)

// EnumName is the value of the enum type with the ordinal, and false for an ordinal it does not have.
func EnumName(typ string, ord int) (string, bool) {
	if a, ok := enumAlias[typ]; ok {
		typ = a
	}
	vs := enumValues[typ]
	if ord < 0 || ord >= len(vs) {
		return "", false
	}
	return vs[ord], true
}

// EnumOrdinalOf reads a data string "Type::Value" (an enum field of a record) as the ordinal of its value in the type; ok is false when
// the string has no "::" or is not one of the nodes' enums.
func EnumOrdinalOf(s string) (typ string, ord int, ok bool) {
	for i := 0; i+1 < len(s); i++ {
		if s[i] == ':' && s[i+1] == ':' {
			typ = s[:i]
			ord, ok = EnumOrdinal(typ, s[i+2:])
			if a, has := enumAlias[typ]; has {
				typ = a
			}
			return typ, ord, ok
		}
	}
	return "", 0, false
}
