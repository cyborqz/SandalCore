// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Gathering stage 2, "Gathering data": the professions over gRPC. SaveProgress reaches the store field by field and answers with the 22 professions, GetCharacter
// carries them with the items, and the store's refusals are the caller's codes.

import (
	"context"
	"errors"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
)

func someProfessions() ([]character.Profession, []*characterv1.ProfessionState) {
	in := []character.Profession{{ProfessionGuid: 108752769996425, CertificationTier: 2, Level: 25, XP: 250000}, {ProfessionGuid: 108752769996426, CertificationTier: 0, Level: 9, XP: 32400}}
	out := []*characterv1.ProfessionState{{ProfessionGuid: 108752769996425, CertificationTier: 2, Level: 25, Xp: 250000}, {ProfessionGuid: 108752769996426, Level: 9, Xp: 32400}}
	return in, out
}

func TestSaveProgressPassesTheProfessionsToTheStoreAndAnswersWithThem(t *testing.T) {
	profs, protoProfs := someProfessions()
	st := &recStore{ret: fullCharacter(), profs: profs}
	resp, err := dialRec(t, st).SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: "0123456789abcdef0123456789abcdef", Level: 10, Professions: protoProfs})
	if err != nil {
		t.Fatalf("SaveProgress = %v", err)
	}
	if len(st.gotProgress.Professions) != 2 || st.gotProgress.Professions[0] != profs[0] || st.gotProgress.Professions[1] != profs[1] {
		t.Errorf("the store was given %+v, want %+v", st.gotProgress.Professions, profs)
	}
	if len(resp.GetCharacter().GetProfessions()) != 2 {
		t.Fatalf("the reply has %d professions", len(resp.GetCharacter().GetProfessions()))
	}
	for i, p := range resp.GetCharacter().GetProfessions() {
		if !proto.Equal(p, protoProfs[i]) {
			t.Errorf("reply profession %d = %v, want %v", i, p, protoProfs[i])
		}
	}
	// a save without professions gives the store none
	st = &recStore{ret: fullCharacter()}
	if _, err := dialRec(t, st).SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: "0123456789abcdef0123456789abcdef", Level: 3}); err != nil {
		t.Fatal(err)
	}
	if len(st.gotProgress.Professions) != 0 {
		t.Errorf("the store was given %+v for a save with none", st.gotProgress.Professions)
	}
	// the store's refusals are the caller's codes
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"invalid":   {character.ErrInvalid, codes.InvalidArgument},
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		st := &recStore{err: tc.err}
		if _, err := dialRec(t, st).SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: "x", Professions: protoProfs}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

// CoA with C Y B

func TestGetCharacterCarriesTheProfessionsWithTheItems(t *testing.T) {
	profs, protoProfs := someProfessions()
	items, _ := someItems()
	st := &recStore{ret: fullCharacter(), items: items, profs: profs}
	c := dialRec(t, st)
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef"})
	if err != nil || len(got.GetCharacter().GetProfessions()) != 0 {
		t.Fatalf("GetCharacter without include_items = %v, %v; want no professions", got, err)
	}
	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil || len(got.GetCharacter().GetProfessions()) != 2 {
		t.Fatalf("GetCharacter with include_items = %v, %v", got, err)
	}
	for i, p := range got.GetCharacter().GetProfessions() {
		if !proto.Equal(p, protoProfs[i]) {
			t.Errorf("profession %d = %v, want %v", i, p, protoProfs[i])
		}
	}
}
