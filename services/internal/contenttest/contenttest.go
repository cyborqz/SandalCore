// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package contenttest gives the tests that need an empty content.db one schema
// built once per test binary, so every case copies it instead of making the
// whole schema again. It is used only by tests.
package contenttest

import (
	"database/sql"
	"os"
	"path/filepath"
	"sync"
	"testing"

	_ "modernc.org/sqlite" // the pure-Go SQLite driver, registered as "sqlite"
)

// ~ C | O | A & C-Y-B ~

// template is the schema's file, built by the first call.
var template struct {
	once  sync.Once
	bytes []byte
	err   error
}

// EmptyDB returns the path of a new content.db holding the empty schema that
// create makes (for the content package's tests, content.Create). The schema is
// built once per test binary and copied for each call, so a case pays for one
// file write instead of the whole schema.
func EmptyDB(t *testing.T, create func(*sql.DB) error) string {
	t.Helper()
	template.once.Do(func() { template.bytes, template.err = build(create) })
	if template.err != nil {
		t.Fatalf("the content schema template: %v", template.err)
	}
	path := filepath.Join(t.TempDir(), "content.db")
	if err := os.WriteFile(path, template.bytes, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

// build makes the schema in a temporary folder, reads the file and removes the
// folder, so nothing of the template stays on disk.
func build(create func(*sql.DB) error) ([]byte, error) {
	dir, err := os.MkdirTemp("", "content-schema-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "content.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	if err := create(db); err != nil {
		db.Close()
		return nil, err
	}
	if err := db.Close(); err != nil {
		return nil, err
	}
	return os.ReadFile(path)
}
