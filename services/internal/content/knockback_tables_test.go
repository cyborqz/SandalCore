// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

// The creature knock-back milestone (contentc version 42): the impulse, push and teleport tables
func init() {
	extraDocumentedTables = append(extraDocumentedTables, KnockbackTables...)
}
