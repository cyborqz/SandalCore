// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/binary"
	"fmt"
	"strconv"

	"aocrop/services/internal/statprog"
)

// addStatProgram compiles the stat formula graph of the content.db at path (plan B3), encodes it, decodes the bytes again as
// a check and stores it in stat_program, with what it holds in meta. It reads the file as every service does
// (statprog.LoadContentDB opens it read only), so the graph is the one the committed rows say, and it runs after them.
func addStatProgram(ctx context.Context, sdb *sql.DB, path string, opt *Options) error {
	g, err := statprog.LoadContentDB(path)
	if err != nil {
		return fmt.Errorf("stat program: %w", err)
	}
	exprs, exprIDs, err := statprog.LoadExprs(path)
	if err != nil {
		return fmt.Errorf("stat program: %w", err)
	}
	b, results, err := statprog.CompileExprs(g, statprog.Options{}, exprs)
	if err != nil {
		return fmt.Errorf("stat program: %w", err)
	}
	data := b.Encode()
	back, err := statprog.Decode(data)
	if err != nil {
		return fmt.Errorf("stat program: the encoded bundle does not verify: %w", err)
	}
	if again := back.Encode(); !bytes.Equal(again, data) {
		return fmt.Errorf("stat program: the bundle does not encode back to the same bytes")
	}
	version := int(binary.LittleEndian.Uint16(data[4:6]))
	crc := binary.LittleEndian.Uint32(data[12:16])
	compiled, usesTarget := 0, 0
	for _, r := range results {
		if r.Program >= 0 {
			compiled++
			if r.UsesTarget {
				usesTarget++
			}
		}
	}
	opt.logf("stat program: %d bytes, %d stats, %d classes, mode %s; %d of %d ability expressions compiled to %d programs", len(data), len(b.Stats), len(b.Classes), b.Mode, compiled, len(exprs), len(b.Exprs))

	tx, err := sdb.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`INSERT INTO stat_program (id, version, mode, stats, classes, crc32, data) VALUES (1, ?, ?, ?, ?, ?, ?)`,
		version, b.Mode.String(), len(b.Stats), len(b.Classes), int64(crc), data); err != nil {
		return err
	}
	for i, r := range results {
		if _, err := tx.Exec(`UPDATE ab_expr SET program = ?, uses_target = ?, error = ? WHERE id = ?`, r.Program, b2i(r.UsesTarget), r.Err, exprIDs[i]); err != nil {
			return err
		}
	}
	for k, v := range map[string]string{
		"statprog.exprs":          strconv.Itoa(len(b.Exprs)),
		"statprog.exprs_compiled": strconv.Itoa(compiled),
		"statprog.exprs_failed":   strconv.Itoa(len(exprs) - compiled),
		"statprog.exprs_target":   strconv.Itoa(usesTarget),
		"statprog.version":        strconv.Itoa(version),
		"statprog.mode":           b.Mode.String(),
		"statprog.bytes":          strconv.Itoa(len(data)),
		"statprog.stats":          strconv.Itoa(len(b.Stats)),
		"statprog.classes":        strconv.Itoa(len(b.Classes)),
	} {
		if _, err := tx.Exec(`INSERT INTO meta (key, value) VALUES (?, ?)`, k, v); err != nil {
			return err
		}
	}
	return tx.Commit()
}
