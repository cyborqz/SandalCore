// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

import (
	"reflect"
	"testing"
	"time"

	"aocrop/services/internal/character"
)

// The artisan ratings a processing job was queued with (migration 0019) go to the wire and come back: every field of the job is the same after the round trip, and a job with none keeps none.
func TestProcessingJobRatingsRoundTripThroughTheWire(t *testing.T) {
	at := time.UnixMilli(1_790_000_000_000).UTC()
	rated := character.ProcessingJob{JobGUID: "0123456789abcdef0123456789abcdef", StationKey: "0123456789abcdef", RecipeGUID: 6064632000000000001, SubRecipeGUID: 0, Quantity: 250,
		QueuedAt: at, StartedAt: at.Add(5 * time.Second), DurationMs: 300_000, ClientTxn: "fedcba9876543210fedcba9876543210",
		Resources:      []character.JobResource{{RecordID: 109162192590532, Rarities: []character.JobRarity{{Rarity: 1, Count: 250}}}},
		RatingXP:       285.25,
		RatingQuantity: 337,
		RatingRarity:   349.5}
	plain := rated
	plain.JobGUID = "00000000000000000000000000000001"
	plain.RatingXP, plain.RatingQuantity, plain.RatingRarity = 0, 0, 0
	jobs := []character.ProcessingJob{rated, plain}
	wire := toProtoJobs(jobs)
	if len(wire) != 2 || wire[0].GetRatingXp() != 285.25 || wire[0].GetRatingQuantity() != 337 || wire[0].GetRatingRarity() != 349.5 || wire[1].GetRatingXp() != 0 {
		t.Fatalf("the ratings on the wire: %v", wire)
	}
	if back := fromProtoJobs(wire); !reflect.DeepEqual(back, jobs) {
		t.Errorf("after the round trip:\n%+v\nwant\n%+v", back, jobs)
	}
}
