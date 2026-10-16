// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
	"aocrop/services/internal/usmap"
)

// Version is contentc's version, recorded in meta (contentc.version). Raise it
// when a change alters what content.db holds.
const Version = "16"

// Options say what Build reads and writes.
type Options struct {
	DBC    string // the design database (CacheDB.dbc)
	USMap  string // the mappings file
	Config string // the client's Config directory (DefaultCombat.ini); "" uses the documented defaults
	Out    string // the content.db to write

	// Encoding is how record.data is stored: content.EncodingJSON or
	// content.EncodingGzip.
	Encoding string

	// ArtisanCurves is mapc's artisan_curves.tsv: the profession level-up curves of the paks (the design database holds a placeholder). "" is dist/content/artisan_curves.tsv
	// of the checkout; a file that is not there leaves the placeholder, and the build says so (and meta artisan_curves.status).
	ArtisanCurves string

	// Patches are the changes to the mappings the walk needs; nil is
	// designdb.Patches(). Build refuses a patch that fits nothing (its struct or
	// anchor is not in the mappings): the mappings are not the ones it was
	// written for.
	Patches []designdb.Patch

	// Now stands in for the clock (tests); nil is time.Now.
	Now func() time.Time
	// Log gets one line per step; nil is silent.
	Log io.Writer
}

// Summary says what Build wrote.
type Summary struct {
	Out           string
	Size          int64
	TablesTotal   int
	TablesDecoded int
	Rows          int
	JSONBytes     int64 // the size of all the rows' JSON before any compression
	NonFinite     int   // floats that were NaN or infinite, stored in record as strings
	Elapsed       time.Duration
}

// (C | Y | B : C.O.A)

func (o *Options) logf(format string, args ...any) {
	if o.Log != nil {
		fmt.Fprintf(o.Log, format+"\n", args...)
	}
}

func (o *Options) now() time.Time {
	if o.Now != nil {
		return o.Now()
	}
	return time.Now()
}

// fileInfo is a hashed input.
type fileInfo struct {
	Path   string
	SHA256 string
	Size   int64
}

func hashFile(path string) (fileInfo, error) {
	f, err := os.Open(path)
	if err != nil {
		return fileInfo{}, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return fileInfo{}, err
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		abs = path
	}
	return fileInfo{Path: abs, SHA256: hex.EncodeToString(h.Sum(nil)), Size: n}, nil
}

// source holds the rows of the tables the typed tables read, by record struct.
type source struct {
	rows    map[string][]designdb.Row
	typeIDs map[string]uint64
	// what buildTyped read, for the builders after it (plan D1 reads the StatCurve keys and the stat names)
	curves    *aocexpr.CurveMap
	statNames map[int64]string
	// artisan are the profession level-up curves of artisan_curves.tsv, nil when the file is not there (gather.go)
	artisan *artisanCurves
}

// keep lists the record structs the typed tables read.
var keep = map[string]bool{
	"StatTypeDefRecordBase":         true,
	"StatCurveRecordBase":           true,
	"StatInitializerListRecordBase": true,
	"CharacterClassesRecordBase":    true,
	"AoCAbilityRecordBase":          true,
	"AbilityHitRecordBase":          true,
	"EffectRecordBase":              true,
	"StatModRecordBase":             true,
	// plan B2: the item tables
	"ItemRecordBase":              true,
	"ItemStatBlockRecordBase":     true,
	"QualityCurveRecordBase":      true,
	"StartingEquipmentRecordBase": true,
	"ResourceBagDefRecordBase":    true, // Gathering stage 3 (version 14): the grids of the resource bags
	// plan B3: what an item adds to the stats
	"SetBonusRecordBase": true,
	// plan D1: using an item, the level curves and the ranks a level grants
	"ItemActivationArchetypeRecordBase": true,
	"ActionResponseRecordBase":          true,
	"RewardTableRecordBase":             true,
	"LevelingTypeDefRecordBase":         true,
	"SkillTreeRecordBase":               true,
	"SkillTreeNodeRecordBase":           true,
	"SkillRecordBase":                   true,
	"SkillRankRecordBase":               true,
	// plan D1, stage 2: the talents
	"SkillRequirementRecordBase": true,
	"SkillPointTypeRecordBase":   true,
	"WeaponSkillLinesRecordBase": true,
	"SkillTreeSlotRecordBase":    true,
	// plan D2: the ability tables
	"ProjectileRecordBase":       true,
	"LingeringEffectRecordBase":  true,
	"AbilityTargetingRecordBase": true,
	// plan E1: the NPC population chain and the vendors
	"PopulationAssetRecordBase":    true,
	"AssetSetRecordBase":           true,
	"PopulationInstanceRecordBase": true,
	"PopulationSetRecordBase":      true,
	"WorldSpawnRecordBase":         true,
	"PopulationAreaRecordBase":     true,
	"PathingRouteRecordBase":       true,
	"VendorDefRecordBase":          true,
	"VendorInventoryDefRecordBase": true,
	// plan E1, stage 3 (version 9): what decides whether a population point spawns now
	"FilteringOverrideRecordBase":  true,
	"NarrativeObjectiveV2":         true,
	"NarrativeQuestV2":             true,
	"EventTriggerDefRecordBase":    true,
	"StoryArcPhaseRecordBase":      true,
	"BuildingLevelSpawnRecordBase": true,
	"LayoutRecordBase":             true,
	"CannonizationStateRecordBase": true,
	"EventObjectiveDefRecordBase":  true,
	"EventStageDefRecordBase":      true,
	"EventDefRecordBase":           true,
	// plan E1, stage 3: who fights whom
	"CharacterAlignmentRecordBase":    true,
	"CharacterRelationshipRecordBase": true,
	// mounts (version 11): the equations the Mount_Base list reads
	"StatEquationTypeRecordBase": true,
	// Nodes 1 (version 12): the city nodes, their zones of influence, rules, taxes, layouts, plots and buildings (nodes.go; LayoutRecordBase and
	// BuildingLevelSpawnRecordBase are kept above)
	"CityNodeRecordBase":                   true,
	"ZOIRecordBase":                        true,
	"NodeLevelDetailsRecordBase":           true,
	"NodeModificationRecordBase":           true,
	"NodeSizeRecordBase":                   true,
	"ChildNodeRulesRecordBase":             true,
	"ChildNodeDesiredLevelRulesRecordBase": true,
	"VassalshipDefRecordBase":              true,
	"TaxDetailsRecordBase":                 true,
	"NodeGlobalTaxTableRecordBase":         true,
	"NodeToPlayerReputationTierRecordBase": true,
	"CitizenshipNodeRecordBase":            true,
	"CitizenshipGenericRecordBase":         true,
	"QuantityScalarDefRecordBase":          true,
	"CitizenshipDuesDefRecordBase":         true,
	"PropertyTaxDuesDefRecordBase":         true,
	"CityNodeElectionDefRecordBase":        true,
	"ElectionConstantsRecordBase":          true,
	"CityNodeConstantsRecordBase":          true,
	"NodeAssetSetRecordBase":               true,
	"NodeAssetDefinitionRecordBase":        true,
	"ServiceBuildingPlotRecordBase":        true,
	"BuildingRecordBase":                   true,
	"BuildingNodeAssetSetsRecordBase":      true,
	// Gathering stage 2 (version 13): the gathering tables, the professions and the certification levels (gather.go; ItemRecordBase and ActionResponseRecordBase are kept above)
	"GatherableActorDefRecordBase":       true,
	"GatheringFoliageDataRecordBase":     true,
	"GatheringFoliageStateRecordBase":    true, // Gathering stage 3 (version 15): the states of the painted foliage
	"PlayerInteractionDefRecordBase":     true,
	"GatheringInteractionDataRecordBase": true,
	"GatheringDynamicDefRecordBase":      true,
	"GatheringToolDataRecordBase":        true,
	"CertificationLevelRecordBase":       true,
	"CharacterProfessionsRecordBase":     true,
	"ItemVariationRecordBase":            true,
}

// -- C Y B x C.O.A --

func (s *source) need(schema string) ([]designdb.Row, error) {
	rows, ok := s.rows[schema]
	if !ok {
		return nil, fmt.Errorf("the design database has no decoded %s table", schema)
	}
	return rows, nil
}

// Build decodes the design database and writes content.db to opt.Out. It
// writes a temporary file beside it and renames it over the output when
// everything has succeeded, so a failed run leaves the previous file alone.
func Build(ctx context.Context, opt Options) (*Summary, error) {
	t0 := opt.now()
	switch opt.Encoding {
	case content.EncodingJSON, content.EncodingGzip:
	default:
		return nil, fmt.Errorf("unknown record encoding %q", opt.Encoding)
	}

	opt.logf("hashing the inputs")
	dbcInfo, err := hashFile(opt.DBC)
	if err != nil {
		return nil, fmt.Errorf("design database: %w", err)
	}
	mapInfo, err := hashFile(opt.USMap)
	if err != nil {
		return nil, fmt.Errorf("mappings file: %w", err)
	}

	opt.logf("opening %s (%d bytes)", opt.DBC, dbcInfo.Size)
	db, err := designdb.Open(opt.DBC)
	if err != nil {
		return nil, err
	}
	if err := db.Verify(); err != nil {
		return nil, fmt.Errorf("design database fails its own checksums: %w", err)
	}
	m, err := usmap.Load(opt.USMap)
	if err != nil {
		return nil, err
	}
	patches := opt.Patches
	if patches == nil {
		patches = designdb.Patches()
	}
	dec := db.DecoderWith(m, designdb.Options{Patches: patches, KnownTypes: designdb.KnownTypes()})
	if err := dec.CheckPatches(); err != nil {
		return nil, err
	}
	earlier, err := earlierDBCs(opt.DBC)
	if err != nil {
		return nil, err
	}
	if len(earlier) > 0 {
		opt.logf("warning: %s holds %s, which sorts before %s: the client loads the first .dbc of its directory, so it would not load %[3]s",
			filepath.Dir(opt.DBC), strings.Join(earlier, ", "), filepath.Base(opt.DBC))
	}
	opt.logf("classifying %d tables", len(db.Tables()))
	classes := dec.Classify()

	if err := os.MkdirAll(filepath.Dir(opt.Out), 0o755); err != nil {
		return nil, err
	}
	tmp := opt.Out + ".tmp"
	_ = os.Remove(tmp)
	sdb, err := sql.Open("sqlite", tmp+"?_pragma=journal_mode(OFF)&_pragma=synchronous(OFF)&_pragma=temp_store(MEMORY)")
	if err != nil {
		return nil, err
	}
	finished := false
	defer func() {
		sdb.Close()
		if !finished {
			_ = os.Remove(tmp)
		}
	}()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		return nil, err
	}
	tx, err := sdb.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}

	// record: every row of every table that decoded.
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	sum := &Summary{Out: opt.Out, TablesTotal: len(classes)}
	tableRows := map[string]int{}
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	for _, c := range classes {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if c.Decodes {
			rows, err := dec.Decode(c.Table, c.Schema)
			if err != nil {
				return nil, fmt.Errorf("table %d: %w", c.Table, err)
			}
			if len(rows) != int(c.Rows) {
				return nil, fmt.Errorf("table %d (%s): %d rows decoded, the directory says %d", c.Table, c.Schema, len(rows), c.Rows)
			}
			if c.TypeID > 1<<63-1 {
				return nil, fmt.Errorf("table %d: TypeId %d does not fit an int64", c.Table, c.TypeID)
			}
			for i := range rows {
				r := &rows[i]
				js, nonFinite, err := rowJSON(enc, &buf, r.Fields)
				if err != nil {
					return nil, fmt.Errorf("table %d (%s) row %q: %w", c.Table, c.Schema, r.Name, err)
				}
				sum.NonFinite += nonFinite
				sum.JSONBytes += int64(len(js))
				var data any
				if opt.Encoding == content.EncodingJSON {
					data = string(js)
				} else {
					data, err = content.EncodeData(opt.Encoding, js)
					if err != nil {
						return nil, err
					}
				}
				if err := w.exec(`INSERT INTO record (table_index, type_id, schema, guid, parent_guid, name, dev_only, data)
					VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, c.Table, int64(c.TypeID), c.Schema, r.Guid, r.ParentGuid, r.Name, b2i(r.DevOnly), data); err != nil {
					return nil, fmt.Errorf("table %d (%s) row %q (Guid %d): %w", c.Table, c.Schema, r.Name, r.Guid, err)
				}
			}
			sum.TablesDecoded++
			sum.Rows += len(rows)
			tableRows[strconv.Itoa(c.Table)] = len(rows)
			if keep[c.Schema] {
				src.rows[c.Schema] = rows
				src.typeIDs[c.Schema] = c.TypeID
			}
		}
		if err := w.exec(`INSERT INTO record_table (table_index, type_id, schema, rows, decoded) VALUES (?, ?, ?, ?, ?)`,
			c.Table, int64(c.TypeID), c.Schema, c.Rows, b2i(c.Decodes)); err != nil {
			return nil, err
		}
	}
	opt.logf("record: %d rows of %d tables, %d MB of JSON", sum.Rows, sum.TablesDecoded, sum.JSONBytes>>20)
	if sum.NonFinite > 0 {
		opt.logf("warning: %d floats were NaN or infinite and are stored in record as the strings \"NaN\", \"+Inf\" and \"-Inf\"", sum.NonFinite)
	}

	// The typed tables.
	meta := map[string]string{}
	if err := loadArtisanCurves(src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildTyped(w, src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildItems(w, src, m, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildGear(w, src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildProgression(w, src, src.curves, src.statNames, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildTalents(w, src, src.statNames, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildAbilities(w, src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildPopulation(w, src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildCombat(w, src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildMounts(w, src, &opt, meta); err != nil {
		return nil, err
	}
	if err := buildNodes(w, src, &opt, meta); err != nil {
		return nil, err
	}

	// meta.
	h := db.Header()
	meta[content.MetaSchemaVersion] = strconv.Itoa(content.SchemaVersion)
	meta[content.MetaRecordEncoding] = opt.Encoding
	meta["contentc.version"] = Version
	meta["source.dbc.path"] = dbcInfo.Path
	meta["source.dbc.name"] = filepath.Base(dbcInfo.Path)
	meta["source.dbc.sha256"] = dbcInfo.SHA256
	meta["source.dbc.size"] = strconv.FormatInt(dbcInfo.Size, 10)
	meta["source.dbc.generated_at"] = h.GeneratedAt().Format(time.RFC3339)
	meta["source.dbc.tables"] = strconv.Itoa(len(classes))
	meta["source.dbc.crc"] = "ok: header, string pool and all " + strconv.Itoa(len(classes)) + " tables match their stored crc32"
	meta["source.usmap.path"] = mapInfo.Path
	meta["source.usmap.sha256"] = mapInfo.SHA256
	meta["source.usmap.size"] = strconv.FormatInt(mapInfo.Size, 10)
	meta["decode.tables"] = strconv.Itoa(sum.TablesDecoded)
	meta["decode.rows"] = strconv.Itoa(sum.Rows)
	meta["decode.skipped_tables"] = strconv.Itoa(len(classes) - sum.TablesDecoded)
	meta["decode.nonfinite_floats"] = strconv.Itoa(sum.NonFinite)
	counts, _ := json.Marshal(tableRows) // map[string]int: sorted keys
	meta["decode.table_rows"] = string(counts)
	applied := []string{} // "[]" in meta, not "null", when none applied (the generated usmap)
	status := map[string]string{}
	for _, r := range dec.PatchResults() {
		status[r.Patch.String()] = r.Status.String()
		if r.Status == designdb.PatchApplied {
			applied = append(applied, r.Patch.String())
		}
	}
	sort.Strings(applied)
	pj, _ := json.Marshal(applied)
	meta["decode.patches"] = string(pj)
	sj, _ := json.Marshal(status) // map[string]string: sorted keys
	meta["decode.patch_status"] = string(sj)
	ej, _ := json.Marshal(append([]string{}, earlier...))
	meta["source.dbc.earlier_siblings"] = string(ej)
	meta["note.0_AoCEvolve"] = "Until 2026-10-01 the client's Client directory also held 0_AoCEvolve.dbc, a third-party patch of " +
		"CacheDB.dbc dated 2026-09-26 that sorted first, so the client loaded it; it has been removed. This file is built from CacheDB.dbc. " +
		"The two differed in 6 tables, none of them a table of this file's typed layer: GameStatProfile (ExperienceDebt \"0.0\"), " +
		"a dialogue table (+1 row), VendorDef, VendorInventoryDef and VendorOffer (+1 row each), and a second dialogue table whose rows only moved."
	meta["built_at"] = opt.now().UTC().Format(time.RFC3339)
	sum.Elapsed = opt.now().Sub(t0)
	meta["build_seconds"] = strconv.FormatFloat(sum.Elapsed.Seconds(), 'f', 1, 64)
	keys := make([]string, 0, len(meta))
	for k := range meta {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if err := w.exec(`INSERT INTO meta (key, value) VALUES (?, ?)`, k, meta[k]); err != nil {
			return nil, err
		}
	}

	if err := w.close(); err != nil {
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	// The stat program is compiled from the rows just committed (the compiler reads the file as the services do), so it comes last.
	if err := addStatProgram(ctx, sdb, tmp, &opt); err != nil {
		return nil, err
	}
	if err := sdb.Close(); err != nil {
		return nil, err
	}
	if err := os.Rename(tmp, opt.Out); err != nil {
		return nil, err
	}
	finished = true
	st, err := os.Stat(opt.Out)
	if err != nil {
		return nil, err
	}
	sum.Size = st.Size()
	opt.logf("wrote %s (%d MB)", opt.Out, sum.Size>>20)
	return sum, nil
}

// C-O-A + Cyb

// writer runs statements in the build's transaction, preparing each query once.
type writer struct {
	tx    *sql.Tx
	stmts map[string]*sql.Stmt
}

func (w *writer) exec(query string, args ...any) error {
	st, ok := w.stmts[query]
	if !ok {
		var err error
		if st, err = w.tx.Prepare(query); err != nil {
			return fmt.Errorf("prepare %q: %w", query, err)
		}
		w.stmts[query] = st
	}
	if _, err := st.Exec(args...); err != nil {
		return fmt.Errorf("exec %q: %w", query, err)
	}
	return nil
}

func (w *writer) close() error {
	var errs []error
	for _, st := range w.stmts {
		errs = append(errs, st.Close())
	}
	return errors.Join(errs...)
}

// rowJSON encodes a row's fields as JSON into buf and returns the bytes (valid
// until the next call). JSON has no NaN or infinity: such a float becomes the
// string "NaN", "+Inf" or "-Inf" (in the row's own Fields too, so a typed table
// that reads that field fails loudly instead of getting a number), and the
// count of them comes back.
func rowJSON(enc *json.Encoder, buf *bytes.Buffer, fields designdb.Struct) (js []byte, nonFinite int, err error) {
	buf.Reset()
	err = enc.Encode(fields)
	var unsupported *json.UnsupportedValueError
	if errors.As(err, &unsupported) {
		if nonFinite = stringifyNonFinite(fields); nonFinite > 0 {
			buf.Reset()
			err = enc.Encode(fields)
		}
	}
	if err != nil {
		return nil, 0, err
	}
	return bytes.TrimRight(buf.Bytes(), "\n"), nonFinite, nil
}

// nonFiniteString is the string a NaN or infinite float is stored as.
func nonFiniteString(v designdb.Value) (string, bool) {
	var f float64
	switch x := v.(type) {
	case float32:
		f = float64(x)
	case float64:
		f = x
	default:
		return "", false
	}
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return strconv.FormatFloat(f, 'g', -1, 64), true // NaN, +Inf, -Inf
	}
	return "", false
}

// [CoA ~ C-Y-B]

// stringifyNonFinite replaces every NaN and infinite float under v (in place)
// by its string and returns how many it replaced.
func stringifyNonFinite(v designdb.Value) int {
	n := 0
	fix := func(e *designdb.Value) {
		if s, ok := nonFiniteString(*e); ok {
			*e = s
			n++
		} else {
			n += stringifyNonFinite(*e)
		}
	}
	switch x := v.(type) {
	case designdb.Struct:
		for k, e := range x {
			fix(&e)
			x[k] = e
		}
	case []designdb.Value:
		for i := range x {
			fix(&x[i])
		}
	case designdb.Pairs:
		for i := range x {
			fix(&x[i].Key)
			fix(&x[i].Value)
		}
	}
	return n
}

// earlierDBCs lists the other .dbc files in the directory of the design database
// at path whose names sort before its own (compared without regard to case, as
// NTFS lists them). The client loads the first .dbc of its Client directory, so
// for such a file the client does not load the one at path (see package
// designdb).
func earlierDBCs(path string) ([]string, error) {
	entries, err := os.ReadDir(filepath.Dir(path))
	if err != nil {
		return nil, fmt.Errorf("design database directory: %w", err)
	}
	own := strings.ToLower(filepath.Base(path))
	var out []string
	for _, e := range entries {
		name := strings.ToLower(e.Name())
		if !e.IsDir() && strings.HasSuffix(name, ".dbc") && name < own {
			out = append(out, e.Name())
		}
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i]) < strings.ToLower(out[j]) })
	return out, nil
}
