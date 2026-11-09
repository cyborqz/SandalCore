// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
)

// Ignored names (social, step 6, migration 0021): the players and the guilds a character ignores, one row each in character_ignored_names with their place in the character's list of their kind,
// so that the order the names were added in is kept. The game client shows both lists and hides the chat lines of every name on them by itself. worldd keeps the lists while the character plays,
// sends them to the client and saves them with the progress.

const (
	// MaxIgnoredNames bounds each of the two lists of one character: the players it ignores and the guilds it ignores.
	MaxIgnoredNames = 50
	// MaxIgnoredNameLength bounds one name, in bytes. A character's name is 3 to 20 letters or digits, but a player may ignore a name nobody has (it is kept as typed), so the bound is wider.
	MaxIgnoredNameLength = 64
)

// The kinds of a row of character_ignored_names.
const (
	ignoredKindCharacter = "character"
	ignoredKindGuild     = "guild"
)

// IgnoredNames is what a character ignores: the names of the players and the names of the guilds, each list in the order the names were added.
type IgnoredNames struct {
	Characters []string
	Guilds     []string
}

// foldASCII lowercases the letters A to Z of s and nothing else. The game client finds a name on its list without regard to the case of letters, so two names that differ only in the case of
// ASCII letters are one name here; any other letter has to match exactly (folding more than the client does could refuse a name the client takes for another).
func foldASCII(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + ('a' - 'A')
		}
	}
	return string(b)
}

// validateIgnoredList checks one list: not over the cap, each name text of at most MaxIgnoredNameLength bytes with no control character and no space at either end, and no name twice (without
// regard to the case of ASCII letters).
func validateIgnoredList(what string, names []string) error {
	if len(names) > MaxIgnoredNames {
		return fmt.Errorf("%w: %d ignored %ss (at most %d)", ErrInvalid, len(names), what, MaxIgnoredNames)
	}
	seen := make(map[string]bool, len(names))
	for i, n := range names {
		switch {
		case n == "":
			return fmt.Errorf("%w: ignored %s %d is empty", ErrInvalid, what, i)
		case len(n) > MaxIgnoredNameLength:
			return fmt.Errorf("%w: ignored %s %d is %d bytes (at most %d)", ErrInvalid, what, i, len(n), MaxIgnoredNameLength)
		case !utf8.ValidString(n):
			return fmt.Errorf("%w: ignored %s %d is not valid UTF-8 text", ErrInvalid, what, i)
		case n[0] == ' ' || n[len(n)-1] == ' ':
			return fmt.Errorf("%w: ignored %s %d has a space at one end: %q", ErrInvalid, what, i, n)
		}
		for _, r := range n {
			if r < 0x20 || r == 0x7f {
				return fmt.Errorf("%w: ignored %s %d has a control character: %q", ErrInvalid, what, i, n)
			}
		}
		key := foldASCII(n)
		if seen[key] {
			return fmt.Errorf("%w: ignored %s %d: %q again", ErrInvalid, what, i, n)
		}
		seen[key] = true
	}
	return nil
}

// ValidateIgnoredNames checks the two lists of a save (validateIgnoredList says what each must be).
func ValidateIgnoredNames(n IgnoredNames) error {
	if err := validateIgnoredList("player", n.Characters); err != nil {
		return err
	}
	return validateIgnoredList("guild", n.Guilds)
}

// IgnoredNames returns the players and the guilds a character ignores, each list in the order the names were added. A character that ignores nobody has two empty lists; an unknown character is
// ErrNotFound.
func (s *Store) IgnoredNames(ctx context.Context, characterID string) (IgnoredNames, error) {
	if !idPattern.MatchString(characterID) {
		return IgnoredNames{}, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT kind, name FROM character_ignored_names WHERE character_id = $1 ORDER BY kind, position`, characterID)
	if err != nil {
		return IgnoredNames{}, err
	}
	defer rows.Close()
	var out IgnoredNames
	for rows.Next() {
		var kind, name string
		if err := rows.Scan(&kind, &name); err != nil {
			return IgnoredNames{}, err
		}
		switch kind {
		case ignoredKindCharacter:
			out.Characters = append(out.Characters, name)
		case ignoredKindGuild:
			out.Guilds = append(out.Guilds, name)
		}
	}
	if err := rows.Err(); err != nil {
		return IgnoredNames{}, err
	}
	if len(out.Characters) == 0 && len(out.Guilds) == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return IgnoredNames{}, err
		}
		if !exists {
			return IgnoredNames{}, ErrNotFound
		}
	}
	return out, nil
}

// replaceIgnoredNames makes the character's ignored players and guilds exactly n, each list in this order, in tx.
func replaceIgnoredNames(ctx context.Context, tx pgx.Tx, characterID string, n IgnoredNames) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_ignored_names WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	for _, list := range []struct {
		kind  string
		names []string
	}{{ignoredKindCharacter, n.Characters}, {ignoredKindGuild, n.Guilds}} {
		if len(list.names) == 0 {
			continue
		}
		if _, err := tx.Exec(ctx, `INSERT INTO character_ignored_names (character_id, kind, name, position)
			SELECT $1, $2, t.name, t.n - 1 FROM unnest($3::text[]) WITH ORDINALITY AS t(name, n)`, characterID, list.kind, list.names); err != nil {
			return itemsError(err)
		}
	}
	return nil
}
