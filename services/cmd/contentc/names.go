// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"aocrop/services/internal/designdb"
)

// The words the items are named with (contentc version 24, "Glint tiers"): an ItemName that is an entry of a string table of the game (a Key and the table's object path, and no text of
// its own: the six Glint kinds and six quest items) has its words in the table, a pak asset the design database does not hold. itemnames (tools/itemnames) reads the tables into
// dist/content/item_names.tsv and this puts the words into item.display_name, where the pick-up pop-up reads them ("Dim Glint"). A name the file does not hold keeps the key, as before.

const itemNamesFile = "item_names.tsv"

// itemNames is item_names.tsv: the words of each string table's entry.
type itemNames struct {
	Path    string
	SHA256  string
	Entries int
	text    map[string]map[string]string // table object path -> key -> words
	// what the item records asked for
	Resolved   int
	Unresolved int
}

// readItemNames reads item_names.tsv. A file that is missing is not an error (the caller keeps the keys and says so); a file that is damaged is.
func readItemNames(path string) (*itemNames, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256(raw)
	out := &itemNames{Path: path, SHA256: hex.EncodeToString(sum[:]), text: map[string]map[string]string{}}
	sc := bufio.NewScanner(strings.NewReader(string(raw)))
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	line := 0
	for sc.Scan() {
		line++
		text := strings.TrimRight(sc.Text(), "\r")
		if line == 1 {
			if want := "table\tkey\ttext"; text != want {
				return nil, fmt.Errorf("%s: the header is %q, want %q", path, text, want)
			}
			continue
		}
		if text == "" {
			continue
		}
		f := strings.Split(text, "\t")
		if len(f) != 3 {
			return nil, fmt.Errorf("%s:%d: %d fields, want 3", path, line, len(f))
		}
		key, err := unescapeName(f[1])
		if err != nil {
			return nil, fmt.Errorf("%s:%d: %w", path, line, err)
		}
		words, err := unescapeName(f[2])
		if err != nil {
			return nil, fmt.Errorf("%s:%d: %w", path, line, err)
		}
		if f[0] == "" || key == "" {
			return nil, fmt.Errorf("%s:%d: an entry with no table or no key", path, line)
		}
		t := out.text[f[0]]
		if t == nil {
			t = map[string]string{}
			out.text[f[0]] = t
		}
		if _, dup := t[key]; dup {
			return nil, fmt.Errorf("%s:%d: %s has the key %q twice", path, line, f[0], key)
		}
		t[key] = words
		out.Entries++
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	if out.Entries == 0 {
		return nil, fmt.Errorf("%s: no entries", path)
	}
	return out, nil
}

// unescapeName reverses itemnames' escapes: \\ is a backslash, \t a tab, \r and \n the line breaks.
func unescapeName(s string) (string, error) {
	if !strings.Contains(s, `\`) {
		return s, nil
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c != '\\' {
			b.WriteByte(c)
			continue
		}
		i++
		if i >= len(s) {
			return "", errors.New("a text ends in a lone backslash")
		}
		switch s[i] {
		case '\\':
			b.WriteByte('\\')
		case 't':
			b.WriteByte('\t')
		case 'r':
			b.WriteByte('\r')
		case 'n':
			b.WriteByte('\n')
		default:
			return "", fmt.Errorf("a text holds the unknown escape \\%c", s[i])
		}
	}
	return b.String(), nil
}

// loadItemNames reads the names file into src (the default is dist/content/item_names.tsv of the checkout) and records what it found in meta.
func loadItemNames(src *source, opt *Options, meta map[string]string) error {
	path, asked := opt.ItemNames, opt.ItemNames != ""
	if !asked {
		path = filepath.Join(repoRoot(), "dist", "content", itemNamesFile)
	}
	n, err := readItemNames(path)
	switch {
	case err == nil:
		src.itemNames = n
		meta["item_names.status"] = "applied"
		meta["item_names.path"] = n.Path
		meta["item_names.sha256"] = n.SHA256
		meta["item_names.entries"] = strconv.Itoa(n.Entries)
		opt.logf("item names: %d entries of %d string table(s) from %s", n.Entries, len(n.text), n.Path)
		return nil
	case errors.Is(err, os.ErrNotExist) && !asked:
		meta["item_names.status"] = "missing"
		meta["item_names.path"] = path
		opt.logf("WARNING: %s is not there: run `itemnames read` (scripts/extract_game_data.ps1, the names step). The names of the items that the game keeps in a string table (the six Glint kinds and six "+
			"quest items) stay the table's text keys, and the pick-up pop-up shows the key", path)
		return nil
	}
	return err
}

// textIn reads an FText as text() does, except that an entry of a string table (no text of its own, a table and a key) reads as the words the names file gives it, when it has them.
func (f *fields) textIn(name string, names *itemNames) string {
	if names != nil {
		if v, ok := f.get(name); ok {
			if t, isText := v.(designdb.Text); isText && t.Source == "" && t.Table != "" {
				if words, found := names.text[t.Table][t.Key]; found {
					names.Resolved++
					return words
				}
				names.Unresolved++
			}
		}
	}
	return f.text(name)
}
