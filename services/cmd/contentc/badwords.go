// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// The client's English bad-word list (the Marketplace's storefront descriptions; contentc version 52): bad_word, from DefaultAoCBadWords.ini of the client's Config directory
// (its [/Script/GameSystemsPlugin.AoCBadWordsFilter] section), which `mapc export-config` writes from the game's own files. internal/content/badwords.go says
// what the table holds. The words are never in this repository: they are read from the player's own game files and kept in content.db only.

const (
	badWordsFile    = "DefaultAoCBadWords.ini"
	badWordsSection = "[/Script/GameSystemsPlugin.AoCBadWordsFilter]"
	badWordsKey     = "BadWordsEnglish"
)

// parseBadWords reads the list out of the ini's text: the lines of the BadWordsEnglish array property in the filter's section, as Unreal Engine's config reads an array:
// "+Key=value" adds the value unless the array holds it already, ".Key=value" adds it as it is, "-Key=value" takes it out and "!Key=ClearArray" empties the array. Any other
// line of the section is an error (a list read by a rule other than the game's would be a guess); a line of another section, a comment (";") and a blank line are not. The words
// come back in the file's order, each once (the game's "+" does not add a word twice, and the table's key is unique). `source` names the file in the errors.
func parseBadWords(text, source string) ([]string, error) {
	text = strings.TrimPrefix(text, "\xef\xbb\xbf")
	var words []string
	inSection, seenSection := false, false
	for n, line := range strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n") {
		line = strings.TrimRight(line, "\r")
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, ";") {
			continue
		}
		if strings.HasPrefix(trimmed, "[") {
			inSection = strings.EqualFold(trimmed, badWordsSection)
			seenSection = seenSection || inSection
			continue
		}
		if !inSection {
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			return nil, fmt.Errorf("%s line %d: no '=' in %q", source, n+1, line)
		}
		key, value := line[:eq], line[eq+1:]
		if len(key) < 2 || key[1:] != badWordsKey {
			return nil, fmt.Errorf("%s line %d: the section holds %q, which is not a +/./-/! line of %s", source, n+1, key, badWordsKey)
		}
		held := func(w string) int {
			for i, have := range words {
				if have == w {
					return i
				}
			}
			return -1
		}
		switch key[0] {
		case '+', '.':
			if value == "" {
				return nil, fmt.Errorf("%s line %d: an empty word", source, n+1)
			}
			if held(value) < 0 { // (a second equal word adds nothing to what a text is searched for, and the table's key is the word)
				words = append(words, value)
			}
		case '-':
			if i := held(value); i >= 0 {
				words = append(words[:i], words[i+1:]...)
			}
		case '!':
			if value != "ClearArray" {
				return nil, fmt.Errorf("%s line %d: !%s=%s: only ClearArray is read", source, n+1, badWordsKey, value)
			}
			words = nil
		default:
			return nil, fmt.Errorf("%s line %d: %q starts with %q, which is none of + . - !", source, n+1, key, key[:1])
		}
	}
	if !seenSection {
		return nil, fmt.Errorf("%s has no %s section", source, badWordsSection)
	}
	if len(words) == 0 {
		return nil, fmt.Errorf("%s holds no word in %s", source, badWordsSection)
	}
	for _, w := range words {
		if w != strings.TrimSpace(w) || strings.ContainsAny(w, "\r\n\t\x00") {
			return nil, fmt.Errorf("%s: the word %q has white space or a control character in it", source, w)
		}
	}
	return words, nil
}

// loadBadWords reads DefaultAoCBadWords.ini from the Config directory.
func loadBadWords(configDir string) ([]string, error) {
	if configDir == "" {
		return nil, errors.New(badWordsFile + " is needed: give the client's Config directory (--config, or AOC_CLIENT_CONFIG); scripts/extract_game_data.ps1 writes it with `mapc export-config`")
	}
	path := filepath.Join(configDir, badWordsFile)
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%w (the list is the client's own: `mapc export-config` writes it with the other client config files)", err)
	}
	return parseBadWords(string(data), badWordsFile)
}

// buildBadWords fills bad_word with the list, in the file's order.
func buildBadWords(w *writer, opt *Options, meta map[string]string) error {
	words, err := loadBadWords(opt.Config)
	if err != nil {
		return err
	}
	for i, word := range words {
		if err := w.exec(`INSERT INTO bad_word (ord, word) VALUES (?, ?)`, i, word); err != nil {
			return err
		}
	}
	opt.logf("bad words: %d words of %s", len(words), badWordsFile)
	meta["typed.bad_word"] = strconv.Itoa(len(words))
	meta["bad_words.source"] = badWordsFile + " " + badWordsSection + " +" + badWordsKey
	return nil
}
