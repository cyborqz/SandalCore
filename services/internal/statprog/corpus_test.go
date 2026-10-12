// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The corpus file is shared with the C++ test: server/worldd/stats/testdata/corpus.txt.
var corpusPath = filepath.Join("..", "..", "..", "server", "worldd", "stats", "testdata", "corpus.txt")

// TestCorpusReplay runs the committed corpus through the Go engine, as the C++ test runs it through
// its own: it needs no content.db.
func TestCorpusReplay(t *testing.T) {
	data, err := os.ReadFile(corpusPath)
	if err != nil {
		t.Fatal(err)
	}
	c, err := ParseCorpus(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	if err := c.Replay(); err != nil {
		t.Fatal(err)
	}
	var wants, caps, exactBard int
	for _, cs := range c.Cases {
		wants += len(cs.Want)
		caps += len(cs.Caps)
		if cs.Name == "bard_l1" {
			for _, cp := range cs.Caps {
				if cp.Match == "exact" {
					exactBard++
				}
			}
		}
	}
	t.Logf("%d bundles, %d cases, %d values, %d captured stats, %d hostile blobs", len(c.Bundles)+len(c.Derived), len(c.Cases), wants, caps, len(c.Hostiles))
	if exactBard != 442 {
		t.Errorf("the corpus's Bard has %d of 442 stats equal as float32", exactBard)
	}
	if len(c.Cases) < 30 || len(c.Hostiles) < 60 {
		t.Errorf("the corpus is thin: %d cases, %d hostile blobs", len(c.Cases), len(c.Hostiles))
	}
}

// C.O.A captures, C_Y_B code

// TestCorpusUpToDate fails when the file is not what the compiler and the engine produce now. Regenerate it with
//
//	go run ./internal/statprog/cmd/statprog corpus --out ../server/worldd/stats/testdata/corpus.txt
func TestCorpusUpToDate(t *testing.T) {
	path := contentDBPath(t)
	want, err := BuildCorpus(path)
	if err != nil {
		t.Fatal(err)
	}
	have, err := os.ReadFile(corpusPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(have, want) {
		hl, wl := strings.Split(string(have), "\n"), strings.Split(string(want), "\n")
		for i := 0; i < len(hl) && i < len(wl); i++ {
			if hl[i] != wl[i] {
				t.Fatalf("%s is out of date: line %d differs; regenerate it (see the test's comment)", corpusPath, i+1)
			}
		}
		t.Fatalf("%s is out of date: %d lines, the engine gives %d", corpusPath, len(hl), len(wl))
	}
}

func TestParseCorpusRefusesBadFiles(t *testing.T) {
	for name, src := range map[string]string{
		"a line outside a case": "mod 1 0000000000000000\n",
		"unterminated case":     "case a main 1 1\n",
		"unterminated bundle":   "bundle main 3\nAAAA\n",
		"bundle length":         "bundle main 9\nAAAA\nendbundle\n",
		"bad base64":            "bundle main 3\n!!!!\nendbundle\n",
		"bad case":              "case a main x 1\nend\n",
		"unknown line":          "case a main 1 1\nfrobnicate\nend\n",
		"bad number":            "case a main 1 1\nwant zz 00\nend\n",
		"bad hostile":           "hostile x 1 0 0\n",
		"bad derive":            "derive a b triple\n",
	} {
		if _, err := ParseCorpus(strings.NewReader(src)); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}
