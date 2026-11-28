// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The client's English bad-word list (contentc version 52): the ini's array rules, the refusals, the table the build fills, and the game's own file read as it is. The tests that
// need a list build one of their own (made-up placeholders); the words of the game's list are never written in this file: the real file is read from the player's own game
// files (AOC_CLIENT_CONFIG, else dist/content/client_config of the checkout) and compared with itself.

import (
	"context"
	"database/sql"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

const placeholderINI = "\xef\xbb\xbf[/Script/GameSystemsPlugin.AoCBadWordsFilter]\r\n+BadWordsEnglish=alphaword\r\n+BadWordsEnglish=BetaWord\r\n+BadWordsEnglish=gamma-word\r\n"

func TestParseBadWordsReadsTheArrayAsTheGameDoes(t *testing.T) {
	got, err := parseBadWords(placeholderINI, "test.ini")
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"alphaword", "BetaWord", "gamma-word"}; !reflect.DeepEqual(got, want) {
		t.Errorf("words = %q, want %q: the file's order and spelling (a byte order mark and CRLF line ends are no part of a word)", got, want)
	}
	for _, tc := range []struct {
		name, text string
		want       []string
	}{
		{"a word twice is held once (the game's + does not add it again)", "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n+BadWordsEnglish=one\n+BadWordsEnglish=two\n+BadWordsEnglish=one\n", []string{"one", "two"}},
		{"a word that differs by case is another word", "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n+BadWordsEnglish=one\n+BadWordsEnglish=ONE\n", []string{"one", "ONE"}},
		{". adds a word", "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n.BadWordsEnglish=one\n.BadWordsEnglish=two\n", []string{"one", "two"}},
		{"- takes a word out", "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n+BadWordsEnglish=one\n+BadWordsEnglish=two\n-BadWordsEnglish=one\n", []string{"two"}},
		{"! ClearArray empties it", "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n+BadWordsEnglish=one\n!BadWordsEnglish=ClearArray\n+BadWordsEnglish=two\n", []string{"two"}},
		{"comments and blank lines", "; a comment\n\n[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n; another\n+BadWordsEnglish=one\n\n", []string{"one"}},
		{"another section is not read", "[/Script/Other.Thing]\n+BadWordsEnglish=other\nBadWordsEnglish=other2\n[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n+BadWordsEnglish=one\n[/Script/After.Thing]\n+Something=else\n", []string{"one"}},
		{"the section name's case does not count", "[/script/gamesystemsplugin.aocbadwordsfilter]\n+BadWordsEnglish=one\n", []string{"one"}},
		{"a word with an equals sign keeps the rest of the line", "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n+BadWordsEnglish=a=b\n", []string{"a=b"}},
	} {
		got, err := parseBadWords(tc.text, "test.ini")
		if err != nil || !reflect.DeepEqual(got, tc.want) {
			t.Errorf("%s: %q, %v; want %q", tc.name, got, err, tc.want)
		}
	}
}

func TestParseBadWordsRefusesWhatItCannotReadAsTheGameDoes(t *testing.T) {
	const section = "[/Script/GameSystemsPlugin.AoCBadWordsFilter]\n"
	for _, tc := range []struct{ name, text, want string }{
		{"no section", "[/Script/Other.Thing]\n+BadWordsEnglish=one\n", "no [/Script/GameSystemsPlugin.AoCBadWordsFilter] section"},
		{"an empty file", "", "no [/Script/GameSystemsPlugin.AoCBadWordsFilter] section"},
		{"a section with no word", section, "holds no word"},
		{"every word taken out again", section + "+BadWordsEnglish=one\n-BadWordsEnglish=one\n", "holds no word"},
		{"a line with no =", section + "+BadWordsEnglish\n", "no '='"},
		{"a plain Key=Value line (it replaces the array in the game: not read)", section + "BadWordsEnglish=one\n", "not a +/./-/! line"},
		{"another key", section + "+SomethingElse=one\n", "not a +/./-/! line"},
		{"an empty word", section + "+BadWordsEnglish=\n", "an empty word"},
		{"a ! line that is not ClearArray", section + "!BadWordsEnglish=Other\n", "only ClearArray"},
		{"a word with a trailing space", section + "+BadWordsEnglish=one \n", "white space"},
		{"a word with a leading space", section + "+BadWordsEnglish= one\n", "white space"},
		{"a word with a tab in it", section + "+BadWordsEnglish=on\te\n", "white space"},
	} {
		if got, err := parseBadWords(tc.text, "test.ini"); err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: %q, %v; want an error holding %q", tc.name, got, err, tc.want)
		}
	}
}

func TestLoadBadWordsNeedsTheFile(t *testing.T) {
	if _, err := loadBadWords(""); err == nil || !strings.Contains(err.Error(), "DefaultAoCBadWords.ini is needed") || !strings.Contains(err.Error(), "mapc export-config") {
		t.Errorf("no Config directory: %v, want the message that names the file and the tool that writes it", err)
	}
	dir := t.TempDir()
	if _, err := loadBadWords(dir); err == nil || !strings.Contains(err.Error(), "mapc export-config") {
		t.Errorf("a Config directory without the file: %v, want an error that says where the file comes from", err)
	}
	if err := os.WriteFile(filepath.Join(dir, badWordsFile), []byte(placeholderINI), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := loadBadWords(dir)
	if err != nil || len(got) != 3 || got[1] != "BetaWord" {
		t.Errorf("the file read = %q, %v; want its three words", got, err)
	}
}

// buildBadWordsInto runs buildBadWords on an empty content database and reads the table back.
func buildBadWordsInto(t *testing.T, config string) ([]string, map[string]string) {
	t.Helper()
	sdb, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "content.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer sdb.Close()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		t.Fatal(err)
	}
	tx, err := sdb.BeginTx(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}
	meta := map[string]string{}
	if err := buildBadWords(w, &Options{Config: config, Log: io.Discard}, meta); err != nil {
		t.Fatal(err)
	}
	if err := w.close(); err != nil {
		t.Fatal(err)
	}
	rows, err := tx.Query(`SELECT word FROM bad_word ORDER BY ord`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var words []string
	for rows.Next() {
		var s string
		if err := rows.Scan(&s); err != nil {
			t.Fatal(err)
		}
		words = append(words, s)
	}
	return words, meta
}

func TestBuildBadWordsFillsTheTableInTheFilesOrder(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, badWordsFile), []byte(placeholderINI), 0o644); err != nil {
		t.Fatal(err)
	}
	words, meta := buildBadWordsInto(t, dir)
	if want := []string{"alphaword", "BetaWord", "gamma-word"}; !reflect.DeepEqual(words, want) {
		t.Errorf("bad_word = %q, want %q in the file's order", words, want)
	}
	if meta["typed.bad_word"] != "3" || !strings.Contains(meta["bad_words.source"], badWordsFile) {
		t.Errorf("meta = %v, want the count and the source", meta)
	}
}

// The game's own file, read as it is: the table holds exactly the file's words, in its order, the way the ini's rules read them, and no line of the file is skipped.
func TestTheGamesBadWordFileReadsToTheTable(t *testing.T) {
	dir := os.Getenv("AOC_CLIENT_CONFIG")
	if dir == "" {
		dir = filepath.Join(repoRoot(), "dist", "content", "client_config")
	}
	raw, err := os.ReadFile(filepath.Join(dir, badWordsFile))
	if err != nil {
		t.Fatalf("%s is not at hand (run install.bat, or set AOC_CLIENT_CONFIG): %v", badWordsFile, err)
	}
	// the expected words, worked out line by line here (only "+BadWordsEnglish=" lines the game's file holds), by a rule this test spells out itself
	var want []string
	seen := map[string]bool{}
	adds := 0
	for _, line := range strings.Split(strings.ReplaceAll(strings.TrimPrefix(string(raw), "\xef\xbb\xbf"), "\r\n", "\n"), "\n") {
		if word, ok := strings.CutPrefix(line, "+BadWordsEnglish="); ok {
			adds++
			if !seen[word] {
				seen[word] = true
				want = append(want, word)
			}
		}
	}
	if len(want) < 100 {
		t.Fatalf("the game's file gives %d words: it is not the list this reader was written for", len(want))
	}
	words, meta := buildBadWordsInto(t, dir)
	if !reflect.DeepEqual(words, want) {
		t.Errorf("bad_word holds %d words, want the file's %d (%d + lines): the table is not the file's list in the file's order", len(words), len(want), adds)
	}
	if meta["typed.bad_word"] != strconv.Itoa(len(want)) {
		t.Errorf("meta typed.bad_word = %q, want %d", meta["typed.bad_word"], len(want))
	}
	// the game finds a word whatever the case of the text: every word is its own match in its upper case and its lower case
	for _, w := range words {
		if !strings.Contains(strings.ToLower("x "+strings.ToUpper(w)+" y"), strings.ToLower(w)) {
			t.Errorf("the word %q is not found in its own upper case", w)
		}
	}
}
