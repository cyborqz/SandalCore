// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"math"
	"testing"
	"time"
)

// aJob is a valid processing job; the tests change what they want.
func aJob(n int) ProcessingJob {
	at := time.UnixMilli(1_790_000_000_000 + int64(n)*1000).UTC()
	return ProcessingJob{JobGUID: guid(100 + n), StationKey: "0123456789abcdef", RecipeGUID: 6064632000000000000 + int64(n), Quantity: 250, QueuedAt: at, StartedAt: at,
		DurationMs: 300_000, ClientTxn: guid(200 + n), Resources: []JobResource{{RecordID: 109162192590532, Rarities: []JobRarity{{Rarity: 1, Count: 250}}}}}
}

// A job's rating sums are numbers from 0 to MaxJobRating; NaN, a negative number, one above the bound and the infinities are refused, and the refusal names the job.
func TestValidateJobsRatings(t *testing.T) {
	good := aJob(1)
	good.RatingXP, good.RatingQuantity, good.RatingRarity = 285.5, 337, MaxJobRating
	if err := ValidateJobs([]ProcessingJob{good, aJob(2)}); err != nil {
		t.Fatalf("good ratings: %v", err)
	}
	for _, tc := range []struct {
		name string
		mod  func(*ProcessingJob)
	}{
		{"a negative experience rating", func(j *ProcessingJob) { j.RatingXP = -0.5 }},
		{"a quantity rating that is NaN", func(j *ProcessingJob) { j.RatingQuantity = math.NaN() }},
		{"a rarity rating above the bound", func(j *ProcessingJob) { j.RatingRarity = MaxJobRating * 2 }},
		{"an infinite rating", func(j *ProcessingJob) { j.RatingXP = math.Inf(1) }},
		{"a negative infinite rating", func(j *ProcessingJob) { j.RatingRarity = math.Inf(-1) }},
	} {
		bad := aJob(3)
		tc.mod(&bad)
		err := ValidateJobs([]ProcessingJob{aJob(1), bad})
		if !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", tc.name, err)
		}
	}
}

// A job keeps the artisan ratings it was queued with (migration 0019): they are saved with it, read back exactly, replaced by a later save, and a job saved with none reads back with none.
func TestJobsKeepTheRatingsTheyWereQueuedWith(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "crafter", ClassID: 7, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	rated, plain := aJob(1), aJob(2)
	rated.RatingXP, rated.RatingQuantity, rated.RatingRarity = 285.25, 337, 349.5
	save := func(jobs []ProcessingJob) error {
		return s.SaveInventoryAll(ctx, c.ID, smallKit(), nil, false, jobs, true, nil, false)
	}
	if err := save([]ProcessingJob{rated, plain}); err != nil {
		t.Fatalf("save: %v", err)
	}
	got, err := s.ProcessingJobs(ctx, c.ID)
	if err != nil || len(got) != 2 {
		t.Fatalf("the jobs: %+v, %v", got, err)
	}
	if got[0].JobGUID != rated.JobGUID || got[0].RatingXP != 285.25 || got[0].RatingQuantity != 337 || got[0].RatingRarity != 349.5 {
		t.Errorf("the rated job reads back with %v, %v, %v, want 285.25, 337, 349.5", got[0].RatingXP, got[0].RatingQuantity, got[0].RatingRarity)
	}
	if got[1].JobGUID != plain.JobGUID || got[1].RatingXP != 0 || got[1].RatingQuantity != 0 || got[1].RatingRarity != 0 {
		t.Errorf("the job saved with none reads back with %v, %v, %v", got[1].RatingXP, got[1].RatingQuantity, got[1].RatingRarity)
	}
	// the rest of the job is what it was: its resources, times and ids
	if got[0].Quantity != 250 || got[0].DurationMs != 300_000 || got[0].ClientTxn != rated.ClientTxn || len(got[0].Resources) != 1 || got[0].Resources[0].Rarities[0].Count != 250 || !got[0].QueuedAt.Equal(rated.QueuedAt) {
		t.Errorf("the rated job changed besides its ratings: %+v", got[0])
	}
	// a later save replaces them (the job was saved again with what it carries)
	rated.RatingXP, rated.RatingQuantity, rated.RatingRarity = 0, 0, 12
	if err := save([]ProcessingJob{rated}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.ProcessingJobs(ctx, c.ID); len(got) != 1 || got[0].RatingXP != 0 || got[0].RatingQuantity != 0 || got[0].RatingRarity != 12 {
		t.Errorf("after the second save: %+v", got)
	}
	// a rating the table cannot hold is refused whole, with nothing changed
	bad := aJob(1)
	bad.RatingQuantity = math.NaN()
	if err := save([]ProcessingJob{bad}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a NaN rating: %v, want ErrInvalid", err)
	}
	if got, _ := s.ProcessingJobs(ctx, c.ID); len(got) != 1 || got[0].RatingRarity != 12 {
		t.Errorf("a refused save changed the jobs: %+v", got)
	}
}
