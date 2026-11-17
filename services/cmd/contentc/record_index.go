// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"database/sql"
	"fmt"
)

// The record table holds 205,882 rows, and its own constraints serve only two lookups: UNIQUE (type_id, guid) finds a row by its table's type and guid, and record_by_table finds the
// rows of one table. A lookup by guid alone, or by the schema's name, therefore read every row (0.37 s for one guid on the shipped data), and the readers that make them add up: the
// creature tables' generator looks up about 1,800 records, and the world server looks up a few at every start and joins the plots' buildings to their records.
//
//	record_by_guid     (guid)          WHERE guid = ?, and a join on a record's guid (the plots' buildings, a reference's name)
//	record_by_schema   (schema, guid)  WHERE schema = ? [AND guid = ?] [ORDER BY guid]: the rows of one record struct in the order of their guids, and one row by schema and guid
//
// A guid names one row only together with its type: the design database has one guid in two tables. record_by_guid therefore holds every row, duplicates included, in rowid order, which
// is the order a scan returns them in. A query by schema that names no order is another matter: it now comes back in guid order, where a scan gave the order the rows were written in
// (rowid). A reader that depends on the order of its rows (the first of two equal names wins) says ORDER BY rowid, as the generators in tools/capture do.
//
// The indexes are made after every row is written (the build inserts 205,882 rows faster into a table with none) and before the commit. They add about 12 MB to the 330 MB file and
// about a second to the build; no table's rows change.
const recordIndexDDL = `
CREATE INDEX record_by_guid ON record (guid);
CREATE INDEX record_by_schema ON record (schema, guid);
`

// addRecordIndexes makes the record table's lookup indexes in tx.
func addRecordIndexes(tx *sql.Tx) error {
	if _, err := tx.Exec(recordIndexDDL); err != nil {
		return fmt.Errorf("record indexes: %w", err)
	}
	return nil
}
