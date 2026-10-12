// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// The reward tables (plan E1, stage 3, part 2; contentc version 10): every RewardTableRecordBase of the design data, as the JSON document the
// worldd's reward evaluator (server/worldd/combat/rewards.h) reads.  What a kill hands out is a table of these (the Blueprint's Rewards:
// Base_Kill_Reward_XP+Glint gives the experience and the Glint certificates, Bandit_Base and Goblin_Base the gear and recipes of their kind), and so
// are chests and quests (the D1 scrolls' tables are also in reward_table, read the other way: item_use).
//
// data is a JSON object (every key optional; an expression is an ab_expr id, 0 or absent for none):
//
//	pred       the table's Predicate: the table gives nothing when it is false
//	algo       SubTableSelectionAlgorithm without its enum prefix: All, None, PureRandom, PerElementWeightedRandom,
//	           PerElementWeightedExpressionRandom, RollEachByPercent
//	n          NumberOfSubtablesToSelect
//	subs       the sub tables' guids
//	w          WeightsPerSubTable
//	pct        PercentagePerSubTable: expressions
//	xw         ExpressionWeightsPerSubTable: expressions
//	c          the reward containers (RewardDefContainers), each:
//	             grant       GrantMethod without its prefix: Auto (handed straight to the players: experience) or Always (into the loot)
//	             pred        GrantPredicate
//	             algo, n, w, pct, xw   the same five for the rewards of the container (NumberToSelect, WeightsPerReward,
//	                         PercentageDropRatePerReward, ExpressionWeightsPerReward)
//	             r           the rewards, each { xp: [{type, expr}], items: [{item, variation, qty}], currency: [{currency, amount}] };
//	                         type is the experience context without its prefix (Adventuring, Weapon, NodeActivity ...), qty and amount are expressions
const LootDDL = `
CREATE TABLE loot_table (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    data TEXT NOT NULL
);
-- The experience rules the kill rewards run with: the Adventuring leveling type's ExperienceDecayCurve (what a kill is worth to a player of another
-- level than the creature), an ab_expr id.
CREATE TABLE xp_rule (
    key  TEXT PRIMARY KEY,
    expr INTEGER NOT NULL
) WITHOUT ROWID;
`

func createLoot(db *sql.DB) error {
	if _, err := db.Exec(LootDDL); err != nil {
		return fmt.Errorf("content: create the loot tables: %w", err)
	}
	return nil
}
