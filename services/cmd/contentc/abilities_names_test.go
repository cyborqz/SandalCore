// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

// A reference written by name ($Skill:Bard_Passive_QuickWit_Jest$) would compile as the null reference (the compiler takes a reference as its
// type and guid): contentc writes the number form before it stores and compiles the expression. Comments stay as they are, and so do names that
// are no record, names two rows share and the kinds that are not records of these tables (a stat).
func TestNamedReferencesAreWrittenAsNumbers(t *testing.T) {
	es := &exprSet{named: map[string]map[string]int64{
		"SkillTree": {"Bard": 6064629885900499588},
		"Skill":     {"Bard_Passive_QuickWit_Jest": 6064630797995540482, "Twin": 0},
		"Effect":    {"Bard_Glee": 6064630759601406637},
		"Ability":   {"Bard_Misc_Flourish": 6064630877217018486},
	}}
	in := "GetSkillPurchased(GetInstigator(), $SkillTree:Bard$, $Skill:Bard_Passive_QuickWit_Jest$) // $Skill:Bard_Passive_QuickWit_Jest$\n" +
		"&& GetEffectCount(GetInstigator(), $Effect:Bard_Glee$) > 0 /* $Effect:Bard_Glee$ */ && $Skill:Twin$ && $Skill:Nobody$ && GetStat(GetSource(), $Stat:MaxStamina$) > 0 && $#1:2$ == $Ability:Bard_Misc_Flourish$"
	want := fmt.Sprintf("GetSkillPurchased(GetInstigator(), $#%d:6064629885900499588$, $#%d:6064630797995540482$) // $Skill:Bard_Passive_QuickWit_Jest$\n", content.TypeSkillTree, content.TypeSkill) +
		fmt.Sprintf("&& GetEffectCount(GetInstigator(), $#%d:6064630759601406637$) > 0 /* $Effect:Bard_Glee$ */ && $Skill:Twin$ && $Skill:Nobody$ && GetStat(GetSource(), $Stat:MaxStamina$) > 0 && $#1:2$ == $#%d:6064630877217018486$",
			content.TypeEffect, content.TypeAbility)
	if got := es.resolve(in); got != want {
		t.Errorf("resolve:\n got %q\nwant %q", got, want)
	}
	if got := (&exprSet{}).resolve(in); got != in {
		t.Errorf("an exprSet with no names changed the text: %q", got)
	}
	if got := es.resolve("1 + 2"); got != "1 + 2" {
		t.Errorf("a text with no reference changed: %q", got)
	}
	// through id(): the stored text and the terms are the number form, and one expression written two ways is one row
	if !strings.Contains(es.resolve("$Skill:Bard_Passive_QuickWit_Jest$"), "#") {
		t.Error("the name was not resolved")
	}
}
