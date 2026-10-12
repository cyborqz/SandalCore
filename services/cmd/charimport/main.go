// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// charimport carries characters from the legacy emulator's
// data/characters.json into the database for an existing account:
//
//	charimport -account cyb_account -file dist/Release/data/characters.json
//
// The file is read and checked before the database is touched. Each character
// is carried over once: one that is already there is left alone, and one whose
// id or name belongs to another character is reported, never overwritten, so
// rerunning is safe. The exit status is 1 if any character was not imported
// (a conflict or invalid data) or anything else went wrong, 0 otherwise.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"

	"aocrop/services/internal/account"
	"aocrop/services/internal/character"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/legacyimport"
)

func main() {
	username := flag.String("account", "", "username that will own the characters (must exist)")
	file := flag.String("file", "dist/Release/data/characters.json", "legacy characters.json")
	flag.Parse()
	if err := run(*username, *file); err != nil {
		fmt.Fprintln(os.Stderr, "charimport:", err)
		os.Exit(1)
	}
}

func run(username, file string) error {
	if username == "" {
		return errors.New("-account is required")
	}
	// A bad file must fail before anything else happens.
	chars, err := load(file)
	if err != nil {
		return err
	}

	ctx := context.Background()
	pool, err := db.Open(ctx, config.Load().DatabaseURL)
	if err != nil {
		return err
	}
	defer pool.Close()
	if err := db.Migrate(ctx, pool); err != nil {
		return err
	}
	acc, err := account.NewStore(pool, 0).FindByUsername(ctx, username)
	if err != nil {
		return fmt.Errorf("account %q: %w", username, err)
	}

	sum, err := legacyimport.Import(ctx, os.Stdout, character.NewStore(pool), acc.ID, chars)
	fmt.Printf("imported %d, already present %d, conflicts %d, invalid %d\n",
		sum.Imported, sum.AlreadyPresent, sum.Conflicts, sum.Invalid)
	if err != nil {
		return err
	}
	if n := sum.Conflicts + sum.Invalid; n > 0 {
		return fmt.Errorf("%d character(s) were not imported", n)
	}
	return nil
}

// C:Y:B / C | O | A

// load reads and validates the file. The account is not known until the
// database has been asked, so the characters carry none; Import sets it.
func load(file string) ([]character.New, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return legacyimport.Parse(f, 0)
}
