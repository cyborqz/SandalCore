// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"encoding/json"
	"fmt"
	"regexp"
	"time"

	"github.com/jackc/pgx/v5"
)

// Processing jobs (the crafting milestone, step 1, migration 0013): the batches a character started at the crafting stations, one row each in character_processing_jobs, in the order they
// were queued. worldd keeps them while the character plays and saves them with the items and the wallet. A job runs on the wall clock: it began at StartedAt (QueuedAt, or the end of the job
// before it when it had to wait) and is done DurationMs later, whether the player is logged in or not.

const (
	// MaxJobQuantity is the most units one job makes (the station gives 1 to 250).
	MaxJobQuantity = 250
	// MaxJobDurationMs bounds a job's length (the table's constraint): about 115 days.
	MaxJobDurationMs = 10_000_000_000
	// maxJobsPerCharacter bounds one save: a station queues at most 5 jobs a player, and a character has far fewer stations than that.
	maxJobsPerCharacter = 500
	maxJobResources     = 64
	maxJobRarities      = 8
	maxJobCount         = 1_000_000
)

var (
	jobGuidPattern    = regexp.MustCompile(`^[0-9a-f]{32}$`)
	stationKeyPattern = regexp.MustCompile(`^[0-9a-f]{16}$`)
)

// JobRarity is how many items of one rarity band (0 Common ... 6 Artifact) a job was paid with.
type JobRarity struct {
	Rarity        int32 `json:"rarity"`
	Count         int32 `json:"count"`
	FromWarehouse bool  `json:"warehouse"`
}

// JobResource is one resource (an item record) of a job with the rarities and counts that were paid.
type JobResource struct {
	RecordID int64       `json:"record"`
	Rarities []JobRarity `json:"rarities"`
}

// ProcessingJob is one row of character_processing_jobs.
type ProcessingJob struct {
	JobGUID       string // 32 lowercase hex digits, not all zero
	StationKey    string // 16 lowercase hex digits
	RecipeGUID    int64
	SubRecipeGUID int64
	Quantity      int32
	QueuedAt      time.Time
	StartedAt     time.Time
	DurationMs    int64
	ClientTxn     string // 32 lowercase hex digits
	Resources     []JobResource
}

// ValidateJobs checks the jobs of a save: the table's constraints, said where the message can name the job, each guid once, and not more than a character can have.
func ValidateJobs(jobs []ProcessingJob) error {
	if len(jobs) > maxJobsPerCharacter {
		return fmt.Errorf("%w: %d processing jobs (at most %d)", ErrInvalid, len(jobs), maxJobsPerCharacter)
	}
	seen := make(map[string]bool, len(jobs))
	for i, j := range jobs {
		switch {
		case !jobGuidPattern.MatchString(j.JobGUID) || j.JobGUID == "00000000000000000000000000000000":
			return fmt.Errorf("%w: processing job %d: guid %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, i, j.JobGUID)
		case seen[j.JobGUID]:
			return fmt.Errorf("%w: processing job %d: guid %s again", ErrInvalid, i, j.JobGUID)
		case !stationKeyPattern.MatchString(j.StationKey):
			return fmt.Errorf("%w: processing job %d: station key %q is not 16 lowercase hex digits", ErrInvalid, i, j.StationKey)
		case j.RecipeGUID <= 0:
			return fmt.Errorf("%w: processing job %d: recipe %d is not a record", ErrInvalid, i, j.RecipeGUID)
		case j.SubRecipeGUID < 0:
			return fmt.Errorf("%w: processing job %d: sub recipe %d", ErrInvalid, i, j.SubRecipeGUID)
		case j.Quantity < 1 || j.Quantity > MaxJobQuantity:
			return fmt.Errorf("%w: processing job %d: quantity %d is not 1 to %d", ErrInvalid, i, j.Quantity, MaxJobQuantity)
		case j.DurationMs < 1 || j.DurationMs > MaxJobDurationMs:
			return fmt.Errorf("%w: processing job %d: duration %d ms is not 1 to %d", ErrInvalid, i, j.DurationMs, int64(MaxJobDurationMs))
		case j.QueuedAt.IsZero() || j.QueuedAt.UnixMilli() < minCreated || j.StartedAt.IsZero() || j.StartedAt.UnixMilli() < minCreated:
			return fmt.Errorf("%w: processing job %d: it has no queue or start time", ErrInvalid, i)
		case j.StartedAt.Before(j.QueuedAt):
			return fmt.Errorf("%w: processing job %d: it started before it was queued", ErrInvalid, i)
		case !jobGuidPattern.MatchString(j.ClientTxn):
			return fmt.Errorf("%w: processing job %d: client id %q is not 32 lowercase hex digits", ErrInvalid, i, j.ClientTxn)
		case len(j.Resources) > maxJobResources:
			return fmt.Errorf("%w: processing job %d: %d resources (at most %d)", ErrInvalid, i, len(j.Resources), maxJobResources)
		}
		seen[j.JobGUID] = true
		for _, r := range j.Resources {
			if r.RecordID <= 0 || len(r.Rarities) > maxJobRarities {
				return fmt.Errorf("%w: processing job %d: resource %d with %d rarities", ErrInvalid, i, r.RecordID, len(r.Rarities))
			}
			for _, ra := range r.Rarities {
				if ra.Rarity < 0 || ra.Rarity > 6 || ra.Count < 1 || ra.Count > maxJobCount {
					return fmt.Errorf("%w: processing job %d: resource %d: rarity %d count %d", ErrInvalid, i, r.RecordID, ra.Rarity, ra.Count)
				}
			}
		}
	}
	return nil
}

// ProcessingJobs returns the jobs of a character in the order they were queued. A character with none has an empty list; an unknown character is ErrNotFound.
func (s *Store) ProcessingJobs(ctx context.Context, characterID string) ([]ProcessingJob, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT job_guid, station_key, recipe_guid, sub_recipe_guid, quantity, queued_at, started_at, duration_ms, client_txn, resources
		FROM character_processing_jobs WHERE character_id = $1 ORDER BY ordinal`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ProcessingJob
	for rows.Next() {
		var j ProcessingJob
		var resources []byte
		if err := rows.Scan(&j.JobGUID, &j.StationKey, &j.RecipeGUID, &j.SubRecipeGUID, &j.Quantity, &j.QueuedAt, &j.StartedAt, &j.DurationMs, &j.ClientTxn, &resources); err != nil {
			return nil, err
		}
		if err := json.Unmarshal(resources, &j.Resources); err != nil {
			return nil, fmt.Errorf("processing job %s of %s: its resources do not read: %w", j.JobGUID, characterID, err)
		}
		out = append(out, j)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return nil, err
		}
		if !exists {
			return nil, ErrNotFound
		}
	}
	return out, nil
}

// SaveInventoryAll is SaveInventory plus, in the same transaction, the wallet (when replaceWallet: each currency named is set to its amount, the others are left), the processing jobs
// (when replaceJobs: the character's jobs become exactly jobs, an empty list removes them all) and the recipes the character was taught (when replaceKnown: they become exactly known).
// The wallet, the jobs and the recipes are checked before anything is written.
func (s *Store) SaveInventoryAll(ctx context.Context, characterID string, items []Item, wallet []WalletEntry, replaceWallet bool, jobs []ProcessingJob, replaceJobs bool, known []int64, replaceKnown bool) error {
	if !idPattern.MatchString(characterID) {
		return ErrNotFound
	}
	if replaceWallet {
		if err := ValidateWallet(wallet); err != nil {
			return err
		}
	} else {
		wallet = nil
	}
	if replaceJobs {
		if err := ValidateJobs(jobs); err != nil {
			return err
		}
	}
	if replaceKnown {
		if err := ValidateKnownRecipes(known); err != nil {
			return err
		}
	}
	return s.saveInventory(ctx, characterID, items, wallet, jobs, replaceJobs, known, replaceKnown)
}

// replaceProcessingJobs makes the character's jobs exactly jobs in tx, in the order given.
func replaceProcessingJobs(ctx context.Context, tx pgx.Tx, characterID string, jobs []ProcessingJob) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_processing_jobs WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	for i, j := range jobs {
		resources := j.Resources
		if resources == nil {
			resources = []JobResource{}
		}
		body, err := json.Marshal(resources)
		if err != nil {
			return fmt.Errorf("%w: processing job %d: %v", ErrInvalid, i, err)
		}
		if _, err := tx.Exec(ctx, `INSERT INTO character_processing_jobs (character_id, job_guid, ordinal, station_key, recipe_guid, sub_recipe_guid, quantity, queued_at, started_at, duration_ms, client_txn, resources)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12::jsonb)`,
			characterID, j.JobGUID, i, j.StationKey, j.RecipeGUID, j.SubRecipeGUID, j.Quantity, j.QueuedAt, j.StartedAt, j.DurationMs, j.ClientTxn, string(body)); err != nil {
			return itemsError(err)
		}
	}
	return nil
}
