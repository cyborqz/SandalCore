// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

// The tests on the real design data read content.db from AOC_CONTENT_DB (or
// dist/content/content.db of the repo, which scripts\services.ps1 test sets) and
// skip without it.

func contentDBPath(t testing.TB) string {
	t.Helper()
	if p := os.Getenv("AOC_CONTENT_DB"); p != "" {
		return p
	}
	p, _ := filepath.Abs(filepath.Join("..", "..", "..", "dist", "content", "content.db"))
	if _, err := os.Stat(p); err != nil {
		t.Skip("no content.db (set AOC_CONTENT_DB)")
	}
	return p
}

var (
	realOnce   sync.Once
	realGraph  *Graph
	realErr    error
	realBundle map[Mode]*Bundle
	realBErr   map[Mode]error
	realMu     sync.Mutex
)

func loadReal(t testing.TB) *Graph {
	t.Helper()
	path := contentDBPath(t)
	realOnce.Do(func() { realGraph, realErr = LoadContentDB(path) })
	if realErr != nil {
		t.Fatalf("LoadContentDB: %v", realErr)
	}
	return realGraph
}

// compileReal compiles the real graph once per arithmetic mode.
func compileReal(t testing.TB, mode Mode) *Bundle {
	t.Helper()
	g := loadReal(t)
	realMu.Lock()
	defer realMu.Unlock()
	if realBundle == nil {
		realBundle, realBErr = map[Mode]*Bundle{}, map[Mode]error{}
	}
	if _, done := realBErr[mode]; !done {
		realBundle[mode], realBErr[mode] = Compile(g, Options{Mode: mode})
	}
	if err := realBErr[mode]; err != nil {
		t.Fatalf("Compile(%v): %v", mode, err)
	}
	return realBundle[mode]
}

func loadCapture(t testing.TB, name string) *capture {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	var c capture
	if err := json.Unmarshal(data, &c); err != nil {
		t.Fatal(err)
	}
	return &c
}

// built with CoA and C-Y-B

var realEx struct {
	once  sync.Once
	b     *Bundle
	res   []ExprResult
	exprs []Expr
	ids   []int64
	err   error
}

// compileRealExprs compiles the real graph with the ability expressions of the file (what contentc stores), once.
func compileRealExprs(t testing.TB) (*Bundle, []ExprResult, []Expr, []int64) {
	t.Helper()
	g := loadReal(t)
	path := contentDBPath(t)
	realEx.once.Do(func() {
		realEx.exprs, realEx.ids, realEx.err = LoadExprs(path)
		if realEx.err == nil {
			realEx.b, realEx.res, realEx.err = CompileExprs(g, Options{}, realEx.exprs)
		}
	})
	if realEx.err != nil {
		t.Fatalf("compiling the real expressions: %v", realEx.err)
	}
	if len(realEx.exprs) == 0 {
		t.Skip("the content.db has no ab_expr rows (built before contentc version 6)")
	}
	return realEx.b, realEx.res, realEx.exprs, realEx.ids
}
