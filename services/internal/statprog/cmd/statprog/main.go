// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Command statprog is the stat engine's tool: it compiles the stat formula graph of a
// content.db into a stat program bundle (internal/statprog), describes a bundle, and
// writes or checks the corpus the Go and C++ tests share.
//
//	statprog compile --db content.db --out stats.bin [--mode float32|double|single]
//	statprog dump    --in stats.bin
//	statprog corpus  --db content.db --out ../../../../server/worldd/stats/testdata/corpus.txt [--check]
//
// --db defaults to AOC_CONTENT_DB, then dist/content/content.db. `corpus --check` writes
// nothing: it exits 1 when the file is not what the current compiler and engine produce, or
// when the file fails the replay (the same checks the C++ test makes).
package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"aocrop/services/internal/statprog"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "statprog:", err)
		os.Exit(1)
	}
}

// ~ Cyb :: C-O-A ~

func defaultDB() string {
	if p := os.Getenv("AOC_CONTENT_DB"); p != "" {
		return p
	}
	return filepath.Join("..", "dist", "content", "content.db")
}

func run(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: statprog compile|dump|corpus (see the package comment)")
	}
	fs := flag.NewFlagSet(args[0], flag.ContinueOnError)
	db := fs.String("db", defaultDB(), "the content.db to read")
	out := fs.String("out", "", "the file to write")
	in := fs.String("in", "", "the bundle to read")
	mode := fs.String("mode", "float32", "arithmetic: float32, double or single")
	check := fs.Bool("check", false, "corpus: compare with the file instead of writing it")
	if err := fs.Parse(args[1:]); err != nil {
		return err
	}
	switch args[0] {
	case "compile":
		if *out == "" {
			return fmt.Errorf("compile needs --out")
		}
		var m statprog.Mode
		switch *mode {
		case "float32":
		case "double":
			m = statprog.ModeDouble
		case "single":
			m = statprog.ModeSingle
		default:
			return fmt.Errorf("--mode %q", *mode)
		}
		g, err := statprog.LoadContentDB(*db)
		if err != nil {
			return err
		}
		exprs, _, err := statprog.LoadExprs(*db) // the ability expressions of a content.db of contentc version 6 (plan D2)
		if err != nil {
			return err
		}
		b, _, err := statprog.CompileExprs(g, statprog.Options{Mode: m}, exprs)
		if err != nil {
			return err
		}
		enc := b.Encode()
		if _, err := statprog.Decode(enc); err != nil {
			return fmt.Errorf("the bundle does not verify: %w", err)
		}
		fmt.Printf("%d bytes: %s\n", len(enc), summary(b))
		return os.WriteFile(*out, enc, 0o644)
	case "dump":
		data, err := os.ReadFile(*in)
		if err != nil {
			return err
		}
		b, err := statprog.Decode(data)
		if err != nil {
			return err
		}
		fmt.Printf("%d bytes: %s\n", len(data), summary(b))
		for _, c := range b.Classes {
			fmt.Printf("class %d %s: %d steps, %d stats\n", c.Enum, b.Strings[c.Name], len(c.Steps), len(c.Members))
		}
		return nil
	case "corpus":
		if *out == "" {
			return fmt.Errorf("corpus needs --out")
		}
		got, err := statprog.BuildCorpus(*db)
		if err != nil {
			return err
		}
		if !*check {
			if err := os.WriteFile(*out, got, 0o644); err != nil {
				return err
			}
			fmt.Printf("wrote %s (%d bytes)\n", *out, len(got))
			return nil
		}
		have, err := os.ReadFile(*out)
		if err != nil {
			return err
		}
		if !bytes.Equal(have, got) {
			gl, hl := strings.Split(string(got), "\n"), strings.Split(string(have), "\n")
			for i := 0; i < len(gl) && i < len(hl); i++ {
				if gl[i] != hl[i] {
					return fmt.Errorf("%s is out of date: line %d differs (regenerate with `statprog corpus --out %s`)", *out, i+1, *out)
				}
			}
			return fmt.Errorf("%s is out of date: it has %d lines, the engine gives %d", *out, len(hl), len(gl))
		}
		c, err := statprog.ParseCorpus(bytes.NewReader(have))
		if err != nil {
			return err
		}
		if err := c.Replay(); err != nil {
			return err
		}
		fmt.Printf("%s is up to date: %d bundles, %d cases, %d hostile blobs\n", *out, len(c.Bundles)+len(c.Derived), len(c.Cases), len(c.Hostiles))
		return nil
	}
	return fmt.Errorf("unknown command %q", args[0])
}

func summary(b *statprog.Bundle) string {
	return fmt.Sprintf("%s arithmetic, %d stats, %d programs (%d of them ability expressions), %d constants, %d curves, %d references, %d classes",
		b.Mode, len(b.Stats), len(b.Progs), len(b.Exprs), len(b.Consts), len(b.Curves), len(b.Refs), len(b.Classes))
}
