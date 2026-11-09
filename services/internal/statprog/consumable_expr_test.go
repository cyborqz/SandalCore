// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import "testing"

// A potion's HealingMod reads its own Health through GetAbilityItemInstance() and GetItemInstanceStat(item, Health).  Both compile to the consumed-item stat the
// runtime already reads (GetConsumedItemStat); an item other than the ability's own is still refused.
func TestAbilityItemStatCompilesToTheConsumedOp(t *testing.T) {
	const stat = "$#5365104778484440215:108986352686712$"
	_, res, err := CompileExprs(TinyGraph(false), Options{}, []Expr{
		{What: "a potion's own Health", Text: "const item = GetAbilityItemInstance();\nGetItemInstanceStat(item, " + stat + ")"},
		{What: "an item that is not the ability's", Text: "GetItemInstanceStat(GetInstigator(), " + stat + ")"},
	})
	if err != nil {
		t.Fatalf("CompileExprs: %v", err)
	}
	if res[0].Program < 0 {
		t.Fatalf("GetItemInstanceStat(GetAbilityItemInstance(), Health) did not compile: %s", res[0].Err)
	}
	if res[1].Program >= 0 {
		t.Fatalf("GetItemInstanceStat of another item compiled (program %d), it must be refused", res[1].Program)
	}
}
