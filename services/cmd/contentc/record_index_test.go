// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"database/sql"
	"fmt"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

// queryer is what both a database and a transaction offer.
type queryer interface {
	Query(query string, args ...any) (*sql.Rows, error)
	QueryRow(query string, args ...any) *sql.Row
}

// planOf is the one-line plan SQLite gives a query: its steps, in order, joined by "; ".
func planOf(t *testing.T, q queryer, query string, args ...any) string {
	t.Helper()
	rows, err := q.Query("EXPLAIN QUERY PLAN "+query, args...)
	if err != nil {
		t.Fatalf("explain %s: %v", query, err)
	}
	defer rows.Close()
	var steps []string
	for rows.Next() {
		var id, parent, unused int
		var detail string
		if err := rows.Scan(&id, &parent, &unused, &detail); err != nil {
			t.Fatal(err)
		}
		steps = append(steps, detail)
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	return strings.Join(steps, "; ")
}

// recordLookups are the queries the record indexes are made for: the readers of the world server, the creature tables' generator and the data tools, and the plan each must have.
var recordLookups = []struct {
	name, query string
	args        []any
	wants       []string // every one of these is in the plan
}{
	{"a record by guid", `SELECT data FROM record WHERE guid = ?`, []any{1}, []string{"SEARCH record USING INDEX record_by_guid (guid=?)"}},
	{"a record by guid and schema", `SELECT name FROM record WHERE guid = ?1 AND schema = 'StatFormulaTypeRecordBase'`, []any{1}, []string{"SEARCH record USING INDEX record_by_schema (schema=? AND guid=?)"}},
	{"the records of a schema by guid", `SELECT guid, name FROM record WHERE schema = 'FreeholdEstateRecordBase' ORDER BY guid`, nil, []string{"SEARCH record USING INDEX record_by_schema (schema=?)"}},
	{"a join on a record's guid", `SELECT b.id, COALESCE(r.name, '') FROM probe b LEFT JOIN record r ON r.guid = b.guid ORDER BY b.id`, nil, []string{"SEARCH r USING INDEX record_by_guid (guid=?)"}},
	{"a record by type and guid", `SELECT data FROM record WHERE type_id = ? AND guid = ?`, []any{1, 1}, []string{"USING INDEX sqlite_autoindex_record_1 (type_id=? AND guid=?)"}},
}

// TestRecordIndexes: the indexes the build adds make every lookup by guid or by schema a search, no automatic (temporary) index is built for a join on a guid, they hold every row of
// record (two tables may share a guid), the rows of a shared guid come back in the order a scan gives them, and no row of any table changes.
func TestRecordIndexes(t *testing.T) {
	path := filepath.Join(t.TempDir(), "content.db")
	sdb, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer sdb.Close()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		t.Fatal(err)
	}
	if _, err := sdb.Exec(`CREATE TABLE probe (id INTEGER PRIMARY KEY, guid INTEGER NOT NULL)`); err != nil {
		t.Fatal(err)
	}
	tx, err := sdb.BeginTx(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	// 3,000 rows in 3 tables; the last row of the third table has the guid of the first row of the first (the shipped data has one such pair)
	schemas := []string{"StatFormulaTypeRecordBase", "FreeholdEstateRecordBase", "ItemRecordBase"}
	for i := 0; i < 3000; i++ {
		table := i % 3
		guid := int64(7_000_000_000 + i*13)
		if i == 2999 {
			guid = 7_000_000_000
		}
		if _, err := tx.Exec(`INSERT INTO record (table_index, type_id, schema, guid, parent_guid, name, dev_only, data) VALUES (?, ?, ?, ?, 0, ?, 0, ?)`,
			table, 100+table, schemas[table], guid, fmt.Sprintf("Row_%d", i), []byte("{}")); err != nil {
			t.Fatal(err)
		}
		if i%50 == 0 {
			if _, err := tx.Exec(`INSERT INTO probe (guid) VALUES (?)`, guid); err != nil {
				t.Fatal(err)
			}
		}
	}
	rowsBefore := dumpRecord(t, tx)

	// without the indexes every lookup by guid or schema is a scan (and the join builds a temporary index): the plans below would fail
	for _, l := range recordLookups[:4] {
		if plan := planOf(t, tx, l.query, l.args...); !strings.Contains(plan, "SCAN") && !strings.Contains(plan, "AUTOMATIC") {
			t.Errorf("%s: before the indexes the plan is %q: the test would not tell the indexes' effect", l.name, plan)
		}
	}

	if err := addRecordIndexes(tx); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}

	for _, l := range recordLookups {
		plan := planOf(t, sdb, l.query, l.args...)
		for _, want := range l.wants {
			if !strings.Contains(plan, want) {
				t.Errorf("%s: the plan is %q, it should have %q", l.name, plan, want)
			}
		}
		if strings.Contains(plan, "SCAN record") || strings.Contains(plan, "AUTOMATIC") {
			t.Errorf("%s: the plan is %q: a scan or a temporary index", l.name, plan)
		}
	}

	// the indexes hold every row, and no table's rows changed
	var total, byGuid, bySchema int
	for query, into := range map[string]*int{
		`SELECT count(*) FROM record`:                             &total,
		`SELECT count(*) FROM record INDEXED BY record_by_guid`:   &byGuid,
		`SELECT count(*) FROM record INDEXED BY record_by_schema`: &bySchema,
	} {
		if err := sdb.QueryRow(query).Scan(into); err != nil {
			t.Fatalf("%s: %v", query, err)
		}
	}
	if total != 3000 || byGuid != total || bySchema != total {
		t.Errorf("record has %d rows, record_by_guid %d, record_by_schema %d: want 3000 in each", total, byGuid, bySchema)
	}
	if after := dumpRecord(t, sdb); after != rowsBefore {
		t.Errorf("the rows of record are not the ones that were written")
	}

	// the guid that two tables share: the index returns them in rowid order, the order a scan returns them in
	scan := guidsOf(t, sdb, `SELECT rowid FROM record NOT INDEXED WHERE guid = 7000000000`)
	indexed := guidsOf(t, sdb, `SELECT rowid FROM record INDEXED BY record_by_guid WHERE guid = 7000000000`)
	if len(scan) != 2 || fmt.Sprint(scan) != fmt.Sprint(indexed) {
		t.Errorf("the rows of the shared guid: by scan %v, by index %v: want the same two", scan, indexed)
	}

	// every table's rows through its own index equal the rows through a scan, for each schema, in guid order
	for _, s := range schemas {
		scan := guidsOf(t, sdb, `SELECT guid FROM record NOT INDEXED WHERE schema = ? ORDER BY guid, rowid`, s)
		indexed := guidsOf(t, sdb, `SELECT guid FROM record WHERE schema = ? ORDER BY guid`, s)
		if len(scan) != 1000 || fmt.Sprint(scan) != fmt.Sprint(indexed) {
			t.Errorf("the guids of %s: by scan %d, by index %d: want the same 1000", s, len(scan), len(indexed))
		}
	}
}

// dumpRecord is every row of record, whole, in rowid order, as one text.
func dumpRecord(t *testing.T, q queryer) string {
	t.Helper()
	rows, err := q.Query(`SELECT rowid, table_index, type_id, schema, guid, parent_guid, name, dev_only, data FROM record NOT INDEXED ORDER BY rowid`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var b strings.Builder
	for rows.Next() {
		var rowid, table, typeID, guid, parent, dev int64
		var schema, name string
		var data []byte
		if err := rows.Scan(&rowid, &table, &typeID, &schema, &guid, &parent, &name, &dev, &data); err != nil {
			t.Fatal(err)
		}
		fmt.Fprintf(&b, "%d %d %d %s %d %d %s %d %x\n", rowid, table, typeID, schema, guid, parent, name, dev, data)
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	return b.String()
}

// guidsOf is the first column of a query, as int64s, in the order it comes.
func guidsOf(t *testing.T, q queryer, query string, args ...any) []int64 {
	t.Helper()
	rows, err := q.Query(query, args...)
	if err != nil {
		t.Fatalf("%s: %v", query, err)
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var n int64
		if err := rows.Scan(&n); err != nil {
			t.Fatal(err)
		}
		out = append(out, n)
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	return out
}

// TestRealRecordIndexes: the content.db of the game's real design database has the indexes, they hold all its rows, and a lookup by guid and the plots' buildings' join use them.
func TestRealRecordIndexes(t *testing.T) {
	db := builtDB(t)
	sq := db.SQL()
	rows := scalarInt(t, db, `SELECT count(*) FROM record`)
	if rows < 100000 {
		t.Fatalf("record has only %d rows", rows)
	}
	for _, ix := range []string{"record_by_guid", "record_by_schema"} {
		if n := scalarInt(t, db, `SELECT count(*) FROM record INDEXED BY `+ix); n != rows {
			t.Errorf("%s holds %d of the %d rows", ix, n, rows)
		}
	}
	if plan := planOf(t, sq, `SELECT data FROM record WHERE guid = ?`, 1); !strings.Contains(plan, "USING INDEX record_by_guid") {
		t.Errorf("a lookup by guid: %q", plan)
	}
	if plan := planOf(t, sq, `SELECT b.plot_guid, b.building_guid, COALESCE(r.name, '') FROM service_plot_building b LEFT JOIN record r ON r.guid = b.building_guid ORDER BY b.plot_guid, b.ord`); strings.Contains(plan, "AUTOMATIC") || !strings.Contains(plan, "record_by_guid") {
		t.Errorf("the plots' buildings: %q", plan)
	}
	if plan := planOf(t, sq, `SELECT guid, name FROM record WHERE schema = 'FreeholdEstateRecordBase' ORDER BY guid`); !strings.Contains(plan, "record_by_schema") || strings.Contains(plan, "TEMP B-TREE") {
		t.Errorf("the estates: %q", plan)
	}
}
