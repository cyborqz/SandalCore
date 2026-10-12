// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"aocrop/services/internal/designdb"
)

// C-Y-B / CoA

// TestRowJSONStoresNonFiniteFloatsAsStrings: JSON has no NaN or infinity, and
// one such float used to abort the whole build ("json: unsupported value").
func TestRowJSONStoresNonFiniteFloatsAsStrings(t *testing.T) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)

	// Finite values are left alone and nothing is counted.
	fields := designdb.Struct{"A": float32(1.5), "B": []designdb.Value{float64(2), "x"}}
	js, n, err := rowJSON(enc, &buf, fields)
	if err != nil || n != 0 || string(js) != `{"A":1.5,"B":[2,"x"]}` {
		t.Errorf("finite row = %s, %d, %v", js, n, err)
	}

	// NaN at the top, +Inf in a nested struct in an array, -Inf as a map's key and
	// value, and a finite float beside them.
	fields = designdb.Struct{
		"Top": float32(math.NaN()),
		"Arr": []designdb.Value{designdb.Struct{"Deep": math.Inf(1), "Fine": float32(0.25)}, float32(7)},
		"Map": designdb.Pairs{{Key: float64(math.Inf(-1)), Value: float32(math.Inf(-1))}, {Key: "k", Value: float64(3)}},
	}
	js, n, err = rowJSON(enc, &buf, fields)
	if err != nil || n != 4 {
		t.Fatalf("row with non-finite floats = %s, %d, %v; want 4 replaced", js, n, err)
	}
	var back struct {
		Top string
		Arr []any
		Map []struct{ Key, Value any }
	}
	if err := json.Unmarshal(js, &back); err != nil {
		t.Fatalf("%s: %v", js, err)
	}
	if back.Top != "NaN" || back.Arr[0].(map[string]any)["Deep"] != "+Inf" || back.Arr[0].(map[string]any)["Fine"] != 0.25 || back.Arr[1] != 7.0 ||
		back.Map[0].Key != "-Inf" || back.Map[0].Value != "-Inf" || back.Map[1].Key != "k" || back.Map[1].Value != 3.0 {
		t.Errorf("stored as %s", js)
	}
	// The row's own fields hold the strings now, so a typed table that reads one
	// as a number fails loudly (field Top is string, want float).
	f := newFields("Row", fields)
	if f.float("Top"); f.err == nil || !strings.Contains(f.err.Error(), "field Top is string, want float") {
		t.Errorf("reading the replaced float: %v", f.err)
	}

	// Any other encoding failure is still an error.
	if _, _, err := rowJSON(enc, &buf, designdb.Struct{"C": make(chan int)}); err == nil {
		t.Error("an unencodable value was accepted")
	}
}

// TestEarlierDBCs: the client loads the first .dbc of its directory, so other
// .dbc files that sort before the one contentc reads are worth a warning.
func TestEarlierDBCs(t *testing.T) {
	dir := t.TempDir()
	for _, name := range []string{"0_AoCEvolve.dbc", "0_other.DBC", "CacheDB.dbc", "Zeta.dbc", "aaa.txt", "000.dbc.bak"} {
		if err := os.WriteFile(filepath.Join(dir, name), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "0_dir.dbc"), 0o755); err != nil { // a directory is not a file the client loads
		t.Fatal(err)
	}
	got, err := earlierDBCs(filepath.Join(dir, "CacheDB.dbc"))
	if err != nil || !reflect.DeepEqual(got, []string{"0_AoCEvolve.dbc", "0_other.DBC"}) {
		t.Errorf("earlierDBCs(CacheDB.dbc) = %v, %v", got, err)
	}
	// The first of them has none before it; the case of a name does not matter.
	if got, err := earlierDBCs(filepath.Join(dir, "0_AoCEvolve.dbc")); err != nil || len(got) != 0 {
		t.Errorf("earlierDBCs(0_AoCEvolve.dbc) = %v, %v", got, err)
	}
	if got, err := earlierDBCs(filepath.Join(dir, "cachedb.DBC")); err != nil || len(got) != 2 {
		t.Errorf("earlierDBCs(cachedb.DBC) = %v, %v", got, err)
	}
	if _, err := earlierDBCs(filepath.Join(dir, "no-such-dir", "x.dbc")); err == nil {
		t.Error("a missing directory was accepted")
	}
}
