// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Command contentc decodes the client's design database (CacheDB.dbc) into
// content.db, a SQLite file of the game numbers the services need (plan C1;
// spec docs/superpowers/specs/2026-09-29-native-world-state-design.md §6).
//
//	contentc [--dbc CacheDB.dbc] [--usmap game.usmap] [--config ClientConfigDir] [--out content.db]
//	contentc roads [--content content.db] [--raw roads_raw.tsv] [--out roads.tsv] [--node-level Wilderness]   (the mounts' roads: roads.go)
//
// Flags default to the environment, so the inputs, which stay outside the
// repository, are set once:
//
//	--dbc     AOC_DESIGN_DB       the client's CacheDB.dbc. The client loads the first .dbc of its
//	                              Client directory (see package designdb), so contentc warns when
//	                              that directory holds another .dbc that sorts before this one
//	--usmap   AOC_USMAP           the mappings file (5.6.0-434126+++game+jvs_game_rel-AOC_FIXED.usmap)
//	--config  AOC_CLIENT_CONFIG   the Cache.db export's AOC\Config directory, for StatRegenTime in
//	                              DefaultCombat.ini; without it the documented default (1 s) is used
//	--out     dist/content/content.db at the repository root (a build artifact, not checked in)
//
// --record-encoding says how record.data is stored: gzip+json (the default, 270 MB for
// the whole file) or json (the rows queryable with json_extract; 573 MB of JSON).
//
// The output is written beside --out and renamed over it when the run has
// succeeded. A build is deterministic: two runs on the same inputs give the same
// rows in every table. The gzip bytes of record.data are the same too when both
// runs use one Go toolchain (compress/flate's output is fixed for a Go version,
// not promised across versions); another toolchain gives the same rows and
// possibly other bytes. content.db holds (see package content for the schema):
//
//	meta                  the inputs' sha256, sizes, the .dbc's generation time and checksum
//	                      result, the decode counts per table, which mappings patches applied,
//	                      contentc's version, and a note on the 0_AoCEvolve.dbc that sat
//	                      beside CacheDB.dbc until 2026-10-01
//	record_table, record  every table of the .dbc and every row (all 369 tables, 205,882 rows),
//	                      as JSON; one table per record struct
//	stat_def, stat_curve*, stat_init, class, class_stat_base
//	ability*, effect*, hit*, owner_stat_mod, stamina_rule, stamina_regen_disable
//	ability_runtime, effect_runtime, effect_stat_runtime
//	equip_slot, item, item_equip_slot, starting_equipment                                  (plan B2)
//	quality_curve, stat_block_stat, item_set, set_bonus, item_tag, stat_program            (plan B3, version 4: what an item
//	                      adds to the stats, and the compiled stat formula graph worldd runs; the program is compiled last,
//	                      from the rows just written, decoded and re-encoded as a check)
//	ab_*                  (plan D2, version 6) every ability of every class and what it reaches: ranks, abilities with their costs and
//	                      montages (NetGUIDs from montage_ids.tsv where a capture names one), hits, projectiles, lingering zones, effects
//	                      and stat mods, and every expression once (ab_expr); the stat program is compiled with them (format version 2) and
//	                      ab_expr.program says which program each is, or why it does not compile
//	npc_*, vendor_*       (plan E1, version 7) the NPC population chain as the design data holds it (assets with the path of their
//	                      archetype, asset sets, instances, sets, world spawns, areas, patrol routes) and the vendors with their
//	                      inventories and listings
//	city_node, city_zoi, node_*, child_node_rule, vassalship_*, citizenship_*, election_def, node_layout*, building*, service_plot*
//	                      (Nodes 1, version 12) the city nodes typed: the 81 nodes by rank (the index in the guid-sorted list), the 78 zones of
//	                      influence with their biome, level details, rules, taxes, reputation, citizenship and election definitions, the layouts
//	                      with their asset sets, plots and buildings and the populations of each building state (nodes.go)
//
// The stamina_rule keys (content.Rule*) hold, with where each comes from:
// Stamina and MaxStamina's formulas and starting values, the StaminaRegeneration
// formula and what it gives with and without a disabling effect, StatRegenTime,
// Sprint's cost per charge in and out of combat and its cost and cooldown
// formulas, and Dodge's cost and cooldown with and without the skill that
// cheapens it. How often a held Sprint charges its cost is in no row it reaches:
// the one cadence the data has is the effect General_Sprint_StaminaDrain, which
// no record references (its TickTimer, 0.25 s, is the interval: spec section 7).
// The three runtime tables (plan A7) hold what worldd runs for Sprint, Dodge and
// Rest as numbers, every formula already run (package content, schema.go).
package main

// C-O-A x Cyb

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"aocrop/services/internal/content"
)

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}

// repoRoot finds the checkout from the working directory: the first directory
// up from it that holds services\go.mod. It falls back to the directory itself.
func repoRoot() string {
	wd, err := os.Getwd()
	if err != nil {
		return "."
	}
	for dir := wd; ; {
		if _, err := os.Stat(filepath.Join(dir, "services", "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return wd
		}
		dir = parent
	}
}

func run(args []string, stdout, stderr io.Writer) int {
	if len(args) > 0 && args[0] == "roads" {
		return runRoads(args[1:], stdout, stderr) // the roads' tiers (mounts milestone, stage 4): reads content.db and mapc's roads_raw.tsv
	}
	fs := flag.NewFlagSet("contentc", flag.ContinueOnError)
	fs.SetOutput(stderr)
	opt := Options{Log: stdout}
	fs.StringVar(&opt.DBC, "dbc", os.Getenv("AOC_DESIGN_DB"), "the design database, CacheDB.dbc (default $AOC_DESIGN_DB)")
	fs.StringVar(&opt.USMap, "usmap", os.Getenv("AOC_USMAP"), "the mappings file (default $AOC_USMAP)")
	fs.StringVar(&opt.Config, "config", os.Getenv("AOC_CLIENT_CONFIG"), "the client's Config directory, for DefaultCombat.ini (default $AOC_CLIENT_CONFIG)")
	fs.StringVar(&opt.Out, "out", filepath.Join(repoRoot(), "dist", "content", "content.db"), "the content.db to write")
	fs.StringVar(&opt.Encoding, "record-encoding", defaultEncoding, "how record.data is stored: json or gzip+json")
	if err := fs.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintf(stderr, "contentc: unexpected argument %q\n", fs.Arg(0))
		return 2
	}
	if opt.DBC == "" || opt.USMap == "" {
		fmt.Fprintln(stderr, "contentc: --dbc and --usmap are required (or set AOC_DESIGN_DB and AOC_USMAP)")
		return 2
	}
	sum, err := Build(context.Background(), opt)
	if err != nil {
		fmt.Fprintf(stderr, "contentc: %v\n", err)
		return 1
	}
	fmt.Fprintf(stdout, "contentc: %s: %d tables decoded of %d, %d rows, %d MB (record.data is %s, %d MB of JSON), %.1f s\n",
		sum.Out, sum.TablesDecoded, sum.TablesTotal, sum.Rows, sum.Size>>20, opt.Encoding, sum.JSONBytes>>20, sum.Elapsed.Seconds())
	return 0
}

// defaultEncoding is how record.data is stored unless --record-encoding says
// otherwise. Measured on CacheDB.dbc with the first 301 tables (163,258 rows,
// 357 MB of JSON): stored as text the file was 444 MB and took 9.7 s to write;
// gzipped row by row (gzip.BestSpeed) 165 MB and 20.6 s. With all 369 tables
// (205,882 rows, 573 MB of JSON) the gzipped file is 270 MB and takes about 30 s;
// the spec's 250 MB was for the first slice. The typed tables, which are what
// the services read, are unaffected; "json" makes record.data queryable with
// json_extract.
const defaultEncoding = content.EncodingGzip
