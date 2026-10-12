// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package designdb reads the client's cached design database: Intrepid's
// CacheDB.dbc, a 144 MB binary copy of the "design_data" database (items,
// abilities, effects, stats, NPCs, ...). Only the stat, ability and class
// tables matter to the first content slice (plan C1), but the reader decodes
// every table whose record type the mappings file (package usmap) describes,
// and checks each one: its rows must end exactly on the table's last byte.
//
// # Which file the client loads
//
// The game directory Game\AOC\Plugins\DesignDataPlugin\Generated\Client holds
// one file, CacheDB.dbc (144,085,052 bytes, written 2026-01-29 with the client
// build). The client names no file. Its CacheDesignDataProvider
// (AOCClient-Win64-Shipping.exe, the loader at 0x145811490) lists the Client
// directory with IFileManager::FindFiles(dir, ".dbc") and loads the first name
// returned, with no sorting or filtering; the directory is built at 0x14104adf0
// and 0x14104af30 (the plugins directory, DesignDataPlugin, Generated, Client).
// The ini key [CacheDesignDataProvider] Path (DesignDataSystem.ini holds a
// build machine's Perforce path to CacheDB.dbc) and the default DesignDataCache.dbc
// (0x14104acd0) are read into the provider (0x145810058) but the loader does
// not use them to pick the file. The parser it calls (0x145810c20) checks the
// magic and the crc32 at 0x04 over the header and directory. So any other .dbc
// in that directory whose name the listing returns first, as NTFS lists names
// in collation order for one that sorts before "CacheDB.dbc", is loaded instead
// of CacheDB.dbc. (This is read from the executable's code; no run of the
// client was traced.)
//
// There was such a file until 2026-10-01: 0_AoCEvolve.dbc (144,103,684 bytes,
// written 2026-09-26 by someone else's AoCEvolveCompanion tool), whose "0_"
// prefix is what put it first, so it was the file the client loaded. The user
// removed it on 2026-10-01 and the folder holds only CacheDB.dbc again; it is
// kept here as the evidence of the loader's first-file rule, and for what it
// said about the data. It was CacheDB.dbc plus a small server-side patch: the
// same generation time (2026-01-20 01:53:09 UTC), the same 369-table directory
// and TypeIds, and byte-identical rows in 363 tables, among them every table
// of the first slice (StatTypeDef, StatInitializerList, StatCurve, Effect,
// Ability, AbilityHit, CharacterClasses). Its string pool was the other pool
// plus four strings, AoCEvolve_DN_EmberspringMenu, AoCEvolve_Offer_1Copper,
// AoCEvolve_AvensEnd_Mounts and AoCEvolve_AvensEnd_GearAndMounts. The six
// tables that differed:
//
//	134 GameStatProfile: its one row has ExperienceDebt = "0.0" (the original is a
//	    stat-driven expression, so no experience debt)
//	201 (a dialogue table): one row more, "The spring waits. Where will the tether
//	    carry you?" with the choices "Attune here." and "Travel to 2nd Sword Outpost."
//	202: the same rows (its start moved by 8,223 bytes, so only the padding differs)
//	338 VendorDef, 339 VendorInventoryDef, 340 VendorOffer: one row more each, the
//	    vendor AoCEvolve_AvensEnd_GearAndMounts, its inventory AoCEvolve_AvensEnd_Mounts
//	    and an offer priced at 1 copper (Guids 6064635900000000100-103)
//
// So the choice changed no value the slice reads, and contentc records which
// file it decoded (name, size, header crc) in content.db, and warns when the
// folder holds another .dbc that sorts first. The third copy, from a downloads folder, is
// another build: generated 2026-01-20 19:37:03 UTC, 2131 Ability rows and many
// of them changed, and not what the game directory holds.
//
// # File layout
//
// All integers are little-endian. Verified on CacheDB.dbc and on the
// 0_AoCEvolve.dbc that was beside it: the header and directory parse as below,
// and every crc matches (Verify).
//
//	0x00 "IDB\x01"
//	0x04 u32 crc32 (IEEE, as zlib) of bytes [8, DataStart)
//	0x08 u64 generation time (Unix seconds)
//	0x10 u32 0, compared by the client with a version it computes
//	0x14 u32 table count (369)
//	0x28 u64 string pool offset
//	0x30 u32 string pool length
//	0x34 u32 crc32 of the pool
//	0x38 u32 DataStart: where the first table's bytes start (0x3C + 32*count)
//	0x3C directory: per table 32 bytes: u64 TypeId, u64 offset, u64 size,
//	     u32 row count, u32 crc32 of the table's bytes
//
// The tables follow one another without gaps, then a few bytes of padding and
// the pool (CacheDB.dbc's last table ends at 0x7B0837D and the pool starts at
// 0x7B08380; the third copy has 11 bytes between). The survey's first reading,
// 32-byte entries at 0x38 holding "u32 ?, TypeId, offset, size, rows", is the
// same bytes shifted by four: the "?" is the previous table's crc.
//
// The string pool is a run of strings, each a u32 length (counting the NUL)
// and the bytes with the NUL. Every name, string, enum value ("EType::Value")
// and expression text in a row is a u32 offset into the pool; 0xFFFFFFFF is
// the empty reference. The rows are described on Decoder.
//
// # What the walk needs besides the mappings
//
// A mappings file (.usmap) carries struct layouts, but not the property flags,
// the struct alignment, the struct serializers or the record types' TypeIds,
// and the database's rows depend on all four. tools/ghidra/usmapgen.py reads
// them from the client executable's reflection data into exe_gen.go (Facts,
// KnownTypes): the 398 record types the client registers by TypeId (the
// directory's 369 among them), the properties the persistent archive does not
// store (Transient, Deprecated, SkipSerialization), the four record structs
// whose rows are 16-byte aligned, and the structs whose C++ type serializes
// itself. Patches covers the difference between the build the mappings and
// the facts describe and the build that generated the database: a property
// to insert, to remove, or to store although it is Transient now.
//
// # What decodes
//
// All 369 tables, 205,882 rows, with either mappings file (the old
// 5.6.0-434126 AOC_FIXED usmap or the generated one): every table self-checks
// with its record struct, 268 with that struct alone and 101 with others too,
// and the TypeIds settle them all (Classify), in about 1 s on 24 threads (a
// Ryzen 9 7900X3D) and about 1.7 s to decode every one. The earlier walker,
// with the old usmap and the record structs' names alone, decoded 301 tables
// (163,258 rows): what it lacked was the leaf record struct's own properties
// (Toast, CertificationLevel), the properties not stored (ContributionThresholds,
// InteractableComponentDef), the 16-byte row alignment (CraftingStationVisualDef,
// PathingRoute, StoryArcPhase, SurveyPylonVisualDef), the inherited row's tail
// (Item, ContentSocket), InstancedStruct (AIUseAbilityCondition), Optional
// (GlobalKillVolumeList, NarrativeObjectiveV2), the 30 record structs not named
// XxxRecordBase (DialogueNode, NarrativeQuestV2, WarRecordV2, ...), the TypeIds
// of the tables several structs fit, and six patches.
package designdb

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"time"
)

// C | O | A : Cyb

const (
	magic        = "IDB\x01"
	headerSize   = 0x3c
	dirEntrySize = 32
	// minRowSize is the fewest bytes a row takes: the DesignDataRecordBase
	// properties every row ends with, Guid i64, ParentGuid i64, Name u32,
	// bDevOnly u8.
	minRowSize = 21
	// maxPoolString bounds a pool string (the longest expression is a few
	// hundred bytes); a larger length is a corrupt pool.
	maxPoolString = 1 << 20
)

// Header is the file header.
type Header struct {
	CRC        uint32 // crc32 of the header and directory, bytes [8, DataStart)
	Generated  uint64 // Unix seconds the database was generated
	Tables     uint32
	PoolOffset uint64
	PoolSize   uint32
	PoolCRC    uint32
	DataStart  uint32
}

// GeneratedAt is Generated as a time.
func (h Header) GeneratedAt() time.Time { return time.Unix(int64(h.Generated), 0).UTC() }

// TableInfo is a directory entry.
type TableInfo struct {
	Index  int    // position in the directory
	TypeID uint64 // the record type's id, as the Id structs in rows carry it
	Offset uint64 // file offset of the table's first byte (rows start 8-aligned after it)
	Size   uint64
	Rows   uint32
	CRC    uint32 // crc32 of the table's bytes
}

// End is the file offset just past the table.
func (t TableInfo) End() uint64 { return t.Offset + t.Size }

// DB is an opened design database. It is read-only and safe for concurrent
// use.
type DB struct {
	data   []byte
	hdr    Header
	tables []TableInfo

	// poolStart has a bit per pool byte, set where a string starts, so a pool
	// reference is checked to point at a string and not into the middle of one.
	poolStart []uint64
}

// Open reads the .dbc at path (all of it, ~144 MB) and checks its structure:
// header, directory, and that the string pool is a clean run of strings. It
// does not compute the checksums; see Verify.
func Open(path string) (*DB, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	db, err := Parse(data)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return db, nil
}

// Parse reads a database from memory; the DB keeps data.
func Parse(data []byte) (*DB, error) {
	if len(data) < headerSize || string(data[:4]) != magic {
		return nil, errors.New("designdb: not a design database (bad magic)")
	}
	le := binary.LittleEndian
	h := Header{
		CRC:        le.Uint32(data[0x04:]),
		Generated:  le.Uint64(data[0x08:]),
		Tables:     le.Uint32(data[0x14:]),
		PoolOffset: le.Uint64(data[0x28:]),
		PoolSize:   le.Uint32(data[0x30:]),
		PoolCRC:    le.Uint32(data[0x34:]),
		DataStart:  le.Uint32(data[0x38:]),
	}
	dirEnd := uint64(headerSize) + uint64(h.Tables)*dirEntrySize
	if dirEnd > uint64(len(data)) {
		return nil, fmt.Errorf("designdb: %d tables do not fit the file", h.Tables)
	}
	if uint64(h.DataStart) != dirEnd {
		return nil, fmt.Errorf("designdb: data starts at %#x, the directory ends at %#x", h.DataStart, dirEnd)
	}
	// The pool ends the file. (PoolOffset+PoolSize is not computed: it can wrap.)
	if h.PoolOffset < dirEnd || h.PoolOffset > uint64(len(data)) || uint64(h.PoolSize) != uint64(len(data))-h.PoolOffset {
		return nil, fmt.Errorf("designdb: pool [%#x, +%#x) does not end the %#x-byte file", h.PoolOffset, h.PoolSize, len(data))
	}
	db := &DB{data: data, hdr: h, tables: make([]TableInfo, h.Tables)}
	next := uint64(h.DataStart)
	for i := range db.tables {
		e := data[headerSize+i*dirEntrySize:]
		t := TableInfo{
			Index:  i,
			TypeID: le.Uint64(e[0:]),
			Offset: le.Uint64(e[8:]),
			Size:   le.Uint64(e[16:]),
			Rows:   le.Uint32(e[24:]),
			CRC:    le.Uint32(e[28:]),
		}
		if t.Offset != next || t.End() < t.Offset || t.End() > h.PoolOffset {
			return nil, fmt.Errorf("designdb: table %d [%#x, +%#x) is not the next run of bytes before the pool (expected offset %#x)", i, t.Offset, t.Size, next)
		}
		// Every row ends with Guid, ParentGuid, Name and bDevOnly (21 bytes), so a
		// count above Size/21 is corrupt; a decoder sizes its rows by it.
		if uint64(t.Rows) > t.Size/minRowSize {
			return nil, fmt.Errorf("designdb: table %d claims %d rows in %d bytes (a row takes at least %d)", i, t.Rows, t.Size, minRowSize)
		}
		next = t.End()
		db.tables[i] = t
	}
	if h.PoolOffset < next || h.PoolOffset-next >= 16 {
		return nil, fmt.Errorf("designdb: tables end at %#x, the pool starts at %#x (more than 15 bytes of padding)", next, h.PoolOffset)
	}
	if err := db.scanPool(); err != nil {
		return nil, err
	}
	return db, nil
}

// (C_Y_B | C.O.A)

// scanPool walks the pool's strings, marking where each starts.
func (db *DB) scanPool() error {
	pool := db.pool()
	db.poolStart = make([]uint64, (len(pool)+63)/64)
	for o := 0; o < len(pool); {
		if len(pool)-o < 4 {
			return fmt.Errorf("designdb: pool: truncated length at %#x", o)
		}
		n := int(binary.LittleEndian.Uint32(pool[o:]))
		if n > maxPoolString || o+4+n > len(pool) {
			return fmt.Errorf("designdb: pool: string at %#x claims %d bytes", o, n)
		}
		if n > 0 && pool[o+3+n] != 0 {
			return fmt.Errorf("designdb: pool: string at %#x is not NUL-terminated", o)
		}
		db.poolStart[o/64] |= 1 << (o % 64)
		o += 4 + n
	}
	return nil
}

func (db *DB) pool() []byte {
	return db.data[db.hdr.PoolOffset : db.hdr.PoolOffset+uint64(db.hdr.PoolSize)]
}

// poolString returns the string at pool offset ref. ok is false when ref does
// not point at the start of a string. 0xFFFFFFFF reads as "".
func (db *DB) poolString(ref uint32) (s string, ok bool) {
	if ref == 0xFFFFFFFF {
		return "", true
	}
	if ref >= db.hdr.PoolSize || db.poolStart[ref/64]&(1<<(ref%64)) == 0 {
		return "", false
	}
	pool := db.pool()
	n := int(binary.LittleEndian.Uint32(pool[ref:]))
	if n == 0 {
		return "", true
	}
	return string(pool[ref+4 : int(ref)+3+n]), true
}

// validPoolRef reports whether ref is a pool reference a row may hold.
func (db *DB) validPoolRef(ref uint32) bool {
	if ref == 0xFFFFFFFF {
		return true
	}
	return ref < db.hdr.PoolSize && db.poolStart[ref/64]&(1<<(ref%64)) != 0
}

// Header returns the file header.
func (db *DB) Header() Header { return db.hdr }

// Tables returns the directory. The slice is the DB's own; do not modify it.
func (db *DB) Tables() []TableInfo { return db.tables }

// Table returns table i, or false when there is none.
func (db *DB) Table(i int) (TableInfo, bool) {
	if i < 0 || i >= len(db.tables) {
		return TableInfo{}, false
	}
	return db.tables[i], true
}

// TableByTypeID finds the table whose directory entry has TypeId id.
func (db *DB) TableByTypeID(id uint64) (TableInfo, bool) {
	for _, t := range db.tables {
		if t.TypeID == id {
			return t, true
		}
	}
	return TableInfo{}, false
}

// TableBytes returns the bytes of table i (shared with the DB; do not modify).
func (db *DB) TableBytes(i int) []byte {
	t := db.tables[i]
	return db.data[t.Offset:t.End()]
}

// Size is the file's length in bytes.
func (db *DB) Size() int { return len(db.data) }

// CoA.C.Y.B

// Verify checks the three kinds of checksum the file carries: the header and
// directory, the string pool, and every table. It reads the whole file
// (about 0.2 s).
func (db *DB) Verify() error {
	if got := crc32.ChecksumIEEE(db.data[8:db.hdr.DataStart]); got != db.hdr.CRC {
		return fmt.Errorf("designdb: header crc %#08x, stored %#08x", got, db.hdr.CRC)
	}
	if got := crc32.ChecksumIEEE(db.pool()); got != db.hdr.PoolCRC {
		return fmt.Errorf("designdb: pool crc %#08x, stored %#08x", got, db.hdr.PoolCRC)
	}
	for _, t := range db.tables {
		if got := crc32.ChecksumIEEE(db.TableBytes(t.Index)); got != t.CRC {
			return fmt.Errorf("designdb: table %d crc %#08x, stored %#08x", t.Index, got, t.CRC)
		}
	}
	return nil
}

// FindInt64 returns the file offsets inside table i where the little-endian
// bytes of v occur (any alignment). It finds a record's Guid, or the places
// that refer to it, in a table the row walker cannot decode.
func (db *DB) FindInt64(i int, v int64) []int64 {
	t := db.tables[i]
	var pat [8]byte
	binary.LittleEndian.PutUint64(pat[:], uint64(v))
	var hits []int64
	b := db.data[t.Offset:t.End()]
	for o := 0; o+8 <= len(b); {
		k := bytes.IndexByte(b[o:], pat[0])
		if k < 0 {
			break
		}
		o += k
		if o+8 <= len(b) && string(b[o:o+8]) == string(pat[:]) {
			hits = append(hits, int64(t.Offset)+int64(o))
		}
		o++
	}
	return hits
}
