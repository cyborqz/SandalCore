// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package standin tells the data-free build of the continuous-integration job apart from a normal one.
//
// Some Go files are made from the player's own game install (services/internal/citynode/constants_gen.go, written by
// scripts/extract_game_data.ps1) and are not in the repository. A build with -tags aoc_standin swaps them for placeholder
// files, so the services compile and the tests that need no game data run on a machine that has none. A test that needs
// the real data calls Skip and is skipped in that build only; in a normal build it runs, and fails when its data is missing.
package standin

import "testing"

// Tag is the build tag of the data-free build.
const Tag = "aoc_standin"

// Skip skips the test in the data-free build (-tags aoc_standin) and does nothing in a normal build.
func Skip(t testing.TB, needs string) {
	t.Helper()
	if Active {
		t.Skipf("data-free build (-tags %s): needs %s", Tag, needs)
	}
}
