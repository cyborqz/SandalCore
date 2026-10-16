// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"

	"github.com/jackc/pgx/v5"
)

// Gathering stage 2, "Gathering data": the professions of a character (character_professions, migration 0010).
//
// Every character holds Novice certification in all 22 CharacterProfessions (the user's rule of 2026-10-09; the data's Novice tier has no MaximumAllowable): a new character
// gets the 22 rows in the transaction that creates it, the migration backfilled the characters that existed, and a profession with no row reads as Novice, level 1, XP 0.
// A profession's XP lives only here (nothing of it is in character_experience).

// ProfessionGuids are the 22 CharacterProfessions of the design database (content.db profession.guid), in guid order: the rows of every character. Migration 0010's SQL lists
// the same 22, and tests hold the three together (db's TestMigration0010ProfessionGuidsAreContentDBs and TestProfessionGuidsAreContentDBs).
var ProfessionGuids = [22]int64{
	108752746862200, // Weaponsmithing
	108752746927737, // Carpentry
	108752746927738, // ArcaneEngineering
	108752746927739, // Armorsmithing
	108752746927740, // Leatherworking
	108752746993277, // Tailoring
	108752746993278, // Jeweler
	108752746993279, // Scribe
	108752756889216, // Metalworking
	108752756889217, // Stonemasonry
	108752756889218, // Weaving
	108752756954755, // Tanning
	108752756954756, // LumberMilling
	108752756954757, // Farming
	108752756954758, // AnimalHusbandry
	108752757020295, // Alchemy
	108752757020296, // Cooking
	108752769996425, // Mining
	108752769996426, // Lumberjacking
	108752770061963, // Herbalism
	108752770061964, // Fishing
	108752770061965, // Hunting
}

// The bounds of a profession's state: the certification tiers are the data's (CertificationLevel.CertificationTier: Novice 0 ... GrandMaster 4) and the levels the
// profession curves' (1 to 50).
const (
	MaxCertificationTier  = 4
	MaxProfessionLevel    = 50
	professionRowsPerChar = len(ProfessionGuids)
)

// C_Y_B : CoA

// Profession is one profession of a character (a row of character_professions).
type Profession struct {
	ProfessionGuid    int64 // the design database's CharacterProfessions Guid (one of ProfessionGuids)
	CertificationTier int32 // 0 Novice, 1 Apprentice, 2 Journeyman, 3 Master, 4 GrandMaster
	Level             int32 // 1 to 50
	XP                int64 // the total on the profession's level-up curve
}

// IsProfession says whether guid is one of the 22 professions.
func IsProfession(guid int64) bool {
	for _, g := range ProfessionGuids {
		if g == guid {
			return true
		}
	}
	return false
}

// ValidateProfessions says what is wrong with a list of professions to save: a profession that is none of the 22, given twice, a tier outside 0-4, a level outside 1-50
// or a negative XP. An empty list is fine (it saves nothing).
func ValidateProfessions(ps []Profession) error {
	seen := map[int64]bool{}
	for _, p := range ps {
		switch {
		case !IsProfession(p.ProfessionGuid):
			return fmt.Errorf("%w: profession %d is none of the 22 CharacterProfessions", ErrInvalid, p.ProfessionGuid)
		case p.CertificationTier < 0 || p.CertificationTier > MaxCertificationTier:
			return fmt.Errorf("%w: certification tier %d of profession %d is not 0 to %d", ErrInvalid, p.CertificationTier, p.ProfessionGuid, MaxCertificationTier)
		case p.Level < 1 || p.Level > MaxProfessionLevel:
			return fmt.Errorf("%w: level %d of profession %d is not 1 to %d", ErrInvalid, p.Level, p.ProfessionGuid, MaxProfessionLevel)
		case p.XP < 0:
			return fmt.Errorf("%w: xp %d of profession %d is negative", ErrInvalid, p.XP, p.ProfessionGuid)
		case seen[p.ProfessionGuid]:
			return fmt.Errorf("%w: profession %d is given twice", ErrInvalid, p.ProfessionGuid)
		}
		seen[p.ProfessionGuid] = true
	}
	return nil
}

// insertNovices gives a character the 22 professions at Novice, level 1, XP 0 (Create's transaction).
func insertNovices(ctx context.Context, tx pgx.Tx, characterID string) error {
	_, err := tx.Exec(ctx, `INSERT INTO character_professions (character_id, profession_guid) SELECT $1, unnest($2::bigint[])`, characterID, ProfessionGuids[:])
	return err
}

// Professions returns the character's 22 professions in guid order: a profession with no row is Novice, level 1, XP 0. An unknown character is ErrNotFound.
func (s *Store) Professions(ctx context.Context, characterID string) ([]Profession, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	var exists bool
	if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return nil, err
	}
	if !exists {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT profession_guid, certification_tier, level, xp FROM character_professions WHERE character_id = $1`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	have := map[int64]Profession{}
	for rows.Next() {
		var p Profession
		if err := rows.Scan(&p.ProfessionGuid, &p.CertificationTier, &p.Level, &p.XP); err != nil {
			return nil, err
		}
		have[p.ProfessionGuid] = p
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	out := make([]Profession, 0, professionRowsPerChar)
	for _, g := range ProfessionGuids {
		p, ok := have[g]
		if !ok {
			p = Profession{ProfessionGuid: g, CertificationTier: 0, Level: 1, XP: 0}
		}
		out = append(out, p)
	}
	return out, nil
}

// saveProfessions writes the professions of a save: each replaces the character's row of its profession (SaveProgress's transaction).
func saveProfessions(ctx context.Context, tx pgx.Tx, characterID string, ps []Profession) error {
	for _, p := range ps {
		if _, err := tx.Exec(ctx, `INSERT INTO character_professions (character_id, profession_guid, certification_tier, level, xp) VALUES ($1, $2, $3, $4, $5)
			ON CONFLICT (character_id, profession_guid) DO UPDATE SET certification_tier = EXCLUDED.certification_tier, level = EXCLUDED.level, xp = EXCLUDED.xp, updated_at = now()`,
			characterID, p.ProfessionGuid, p.CertificationTier, p.Level, p.XP); err != nil {
			return err
		}
	}
	return nil
}
