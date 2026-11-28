// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// The client's English bad-word list (the Marketplace's storefront descriptions; contentc version 52): the words the game looks for in a text. The source is
// DefaultAoCBadWords.ini of the client's Config directory (its [/Script/GameSystemsPlugin.AoCBadWordsFilter] section, one "+BadWordsEnglish=<word>" line for each word), which
// `mapc export-config` writes from the game's own files. The words are game data: they live in content.db (dist\content is not in the repository) and in no
// source or test file.
//
//	bad_word   one row for each word of the list, in the file's order. word is the text as the file spells it; the game finds a word in a text whatever the case of either
//	           (the game finds a word in a text whatever the case of either), so a reader compares without regard to case.
const BadWordsDDL = `
CREATE TABLE bad_word (
    ord  INTEGER PRIMARY KEY,      -- the word's place in the file, 0 first
    word TEXT NOT NULL UNIQUE      -- the entry of the file's list, as the file spells it
);
`

// BadWordTables are the tables BadWordsDDL creates, in the DDL's order.
var BadWordTables = []string{"bad_word"}

func createBadWords(db *sql.DB) error {
	if _, err := db.Exec(BadWordsDDL); err != nil {
		return fmt.Errorf("content: create the bad-word table: %w", err)
	}
	return nil
}
