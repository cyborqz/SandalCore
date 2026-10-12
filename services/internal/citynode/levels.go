// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode

import "strconv"

// The node levels, and the XP progress toward the next one.  The numbers (CityNodeExpToLevel, MaxNodeLevel,
// MinimumOperationalNodeLevel) come from the client's own config, which the design DB does not hold: the constants generator
// (tools/capture/gen_node_constants.py, checked by scripts/generators.ps1) writes them into constants_gen.go, and a test here pins
// them to the ini too.

// ENodeLevel, as the client names it.
const (
	LevelWilderness = 0
	LevelCrossroads = 1
	LevelEncampment = 2
	LevelVillage    = 3
	LevelTown       = 4
	LevelCity       = 5
	LevelMetropolis = 6
)

// MaxNodeLevel (Town), MinimumOperationalNodeLevel (Village) and ExpToLevel (CityNodeExpToLevel) are in constants_gen.go, which
// tools/capture/gen_node_constants.py writes from the ini; there is no second copy.

var levelNames = [...]string{"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis"}

// LevelName is the client's name of a level ("Town"), or "Level N" for one it does not have.
func LevelName(level int) string {
	if level >= 0 && level < len(levelNames) {
		return levelNames[level]
	}
	return "Level " + strconv.Itoa(level)
}

// Progress is where a node's XP stands in its level: the percent of the way from this level's threshold to the next
// (0-100, as the client's percent map shows it) and the XP still missing for the next level.  XP is cumulative and nothing
// here moves a level: a node whose XP is already past the next threshold shows 100 and 0 missing, and stays at its level
// (Nodes 1 has no level-up from XP).  At MaxNodeLevel, and at a level the table does not have, both are 0, as the max-level
// nodes of retail show 0.
func Progress(level int, experience int64) (percent int, toNext int64) {
	if level < 0 || level >= MaxNodeLevel || level+1 >= len(ExpToLevel) {
		return 0, 0
	}
	lo, hi := ExpToLevel[level], ExpToLevel[level+1]
	if experience <= lo {
		return 0, hi - experience
	}
	if experience >= hi {
		return 100, 0
	}
	return int((experience - lo) * 100 / (hi - lo)), hi - experience
}
