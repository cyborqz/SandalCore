// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

// the Marketplace's storefront descriptions (contentc version 52): the client's English bad-word list
func init() {
	extraDocumentedTables = append(extraDocumentedTables, BadWordTables...)
}
