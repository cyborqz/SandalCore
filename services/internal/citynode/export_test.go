// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode

// SetWatchBuffer lets the external tests shrink a watcher's buffer; it returns the function that puts it back.
func SetWatchBuffer(n int) func() {
	old := watchBuffer
	watchBuffer = n
	return func() { watchBuffer = old }
}
