// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"math"
	"path/filepath"
	"testing"
)

// The town replicator sends the layout's transforms bit for bit, and retail's carry negative zeros (29 of Joeva's 85 items): a REAL column would hand them back as
// +0.0 (SQLite stores a whole-number float as an integer), so the transform columns have no type.  This test is the reason.
func TestNodeTransformColumnsKeepNegativeZero(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "content.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if err := Create(db); err != nil {
		t.Fatal(err)
	}
	negZero := math.Copysign(0, -1)
	exec(t, db, `INSERT INTO node_layout_prop (layout_guid, idx, asset_set_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz) VALUES (1, 0, 2, ?, 0.0, ?, 1.0, ?, 5.5, 0.0, 1.0, 1.0, 1.0)`, negZero, negZero, negZero)
	exec(t, db, `INSERT INTO node_layout_plot (layout_guid, level, idx, plot_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz) VALUES (1, 4, 0, 3, ?, 0.0, 0.0, 1.0, 0.0, ?, 0.0, 1.0, 1.0, 1.0)`, negZero, negZero)
	exec(t, db, `INSERT INTO node_asset_set_member (set_guid, ord, def_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz) VALUES (2, 0, 3, 0.0, 0.0, ?, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, ?)`, negZero, negZero)
	for _, c := range []struct {
		query string
		want  []bool // which of the selected values are negative zeros
	}{
		{`SELECT qx, qy, qz, tx FROM node_layout_prop`, []bool{true, false, true, true}},
		{`SELECT qx, tx, ty, sz FROM node_layout_plot`, []bool{true, false, true, false}},
		{`SELECT qz, sz, qx, tz FROM node_asset_set_member`, []bool{true, true, false, false}},
	} {
		var v [4]float64
		if err := db.QueryRow(c.query).Scan(&v[0], &v[1], &v[2], &v[3]); err != nil {
			t.Fatalf("%s: %v", c.query, err)
		}
		for i, want := range c.want {
			if got := v[i] == 0 && math.Signbit(v[i]); got != want {
				t.Errorf("%s: value %d is %v (negative zero %v), want negative zero %v", c.query, i, v[i], got, want)
			}
		}
	}
}
