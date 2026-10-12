// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// CreateCharacter and the rules (plan B1): what the Checker is asked, what its
// answers become, and what the store is given.

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/customization"
)

// recChecker records what it was asked and answers err.
type recChecker struct {
	got   customization.New
	calls int
	err   error
}

func (r *recChecker) Check(n customization.New) error {
	r.got = n
	r.calls++
	return r.err
}

func createRequest() *characterv1.CreateCharacterRequest {
	return &characterv1.CreateCharacterRequest{
		AccountId: 5, Name: "Zed", RaceId: 6, GenderId: 1, ClassId: 7, ClassName: "Tank", ArchetypeId: 17754,
		AppearanceJson: `{"race":"Dunir"}`, CreateInfo: []byte{1},
	}
}

func TestTheCheckerIsAskedForTheRequestsRaceGenderClassAndAppearance(t *testing.T) {
	chk, st := &recChecker{}, &recStore{}
	s := New(st, chk)
	if _, err := s.CreateCharacter(context.Background(), createRequest()); err != nil {
		t.Fatal(err)
	}
	want := customization.New{Race: 6, Gender: 1, Class: 7, ClassName: "Tank", AppearanceJSON: `{"race":"Dunir"}`}
	if chk.got != want || chk.calls != 1 {
		t.Errorf("Check(%+v) called %d times, want %+v once", chk.got, chk.calls, want)
	}
}

// ~ C-O-A :: C-Y-B ~

// The stored class name and archetype are the class's own, whatever the caller
// said about them (an empty name and no archetype are filled in).
func TestTheStoredClassNameAndArchetypeAreTheClasses(t *testing.T) {
	for _, tc := range []struct {
		class     int32
		name      string
		archetype int32
	}{{1, "Ranger", 17751}, {2, "Bard", 17747}, {3, "Cleric", 17748}, {4, "Fighter", 17749},
		{5, "Rogue", 17752}, {6, "Summoner", 17753}, {7, "Tank", 17754}, {8, "Mage", 17750}} {
		for _, said := range []*characterv1.CreateCharacterRequest{
			{AccountId: 5, Name: "Zed", ClassId: tc.class},
			{AccountId: 5, Name: "Zed", ClassId: tc.class, ClassName: tc.name, ArchetypeId: tc.archetype},
		} {
			st := &recStore{}
			if _, err := New(st, &recChecker{}).CreateCharacter(context.Background(), said); err != nil {
				t.Fatal(err)
			}
			if st.gotNew.ClassID != tc.class || st.gotNew.ClassName != tc.name || st.gotNew.ArchetypeID != tc.archetype {
				t.Errorf("class %d: stored %d %q %d, want %d %q %d", tc.class,
					st.gotNew.ClassID, st.gotNew.ClassName, st.gotNew.ArchetypeID, tc.class, tc.name, tc.archetype)
			}
		}
	}
}

func TestAnArchetypeThatIsNotTheClassesIsRefused(t *testing.T) {
	st := &recStore{}
	req := createRequest()
	req.ArchetypeId = 17747 // a Bard's, for a Tank
	_, err := New(st, &recChecker{}).CreateCharacter(context.Background(), req)
	if status.Code(err) != codes.InvalidArgument || !strings.Contains(err.Error(), "archetype 17747 is not the archetype of class 7 (Tank), 17754") {
		t.Errorf("CreateCharacter = %v", err)
	}
	if st.gotNew.Name != "" {
		t.Error("the store was called")
	}
}

// A refusal is InvalidArgument with the rule's own text, and nothing is stored.
func TestARefusalIsInvalidArgumentAndStoresNothing(t *testing.T) {
	st := &recStore{}
	refusal := errors.New("customization for Kae_M: headHair 12345 is not one of the 2 options")
	_, err := New(st, &recChecker{err: refusal}).CreateCharacter(context.Background(), createRequest())
	if status.Code(err) != codes.InvalidArgument || status.Convert(err).Message() != refusal.Error() {
		t.Errorf("CreateCharacter = %v, want InvalidArgument with %q", err, refusal)
	}
	if st.gotNew.Name != "" {
		t.Errorf("the store was called with %+v", st.gotNew)
	}
}

// With nothing to judge by the character is refused, not accepted unchecked:
// FailedPrecondition, with the reason, and nothing is stored.
func TestWithoutRulesCreationIsRefused(t *testing.T) {
	st := &recStore{}
	for name, srv := range map[string]*Server{
		"no checker":             New(st, nil),
		"an unavailable checker": New(st, &recChecker{err: fmt.Errorf("%w: content.db is missing", ErrUnavailable)}),
	} {
		_, err := srv.CreateCharacter(context.Background(), createRequest())
		if status.Code(err) != codes.FailedPrecondition || !strings.Contains(err.Error(), "character creation is unavailable") {
			t.Errorf("%s: CreateCharacter = %v, want FailedPrecondition", name, err)
		}
		if st.gotNew.Name != "" {
			t.Errorf("%s: the store was called with %+v", name, st.gotNew)
		}
	}
	// ... but everything else is served: the rules are for creation only.
	st.ret = fullCharacter()
	srv := New(st, nil)
	if _, err := srv.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: st.ret.ID}); err != nil {
		t.Errorf("GetCharacter without rules: %v", err)
	}
	if _, err := srv.ListCharacters(context.Background(), &characterv1.ListCharactersRequest{AccountId: 1}); err != nil {
		t.Errorf("ListCharacters without rules: %v", err)
	}
}

// The class id reaches the store and comes back in the character.
func TestTheClassIDRoundTrips(t *testing.T) {
	st := &recStore{ret: character.Character{ID: "0123456789abcdef0123456789abcdef", Name: "Zed", RaceID: 6, GenderID: 1, ClassID: 7}}
	resp, err := New(st, &recChecker{}).CreateCharacter(context.Background(), createRequest())
	if err != nil {
		t.Fatal(err)
	}
	c := resp.GetCharacter()
	if c.GetRaceId() != 6 || c.GetGenderId() != 1 || c.GetClassId() != 7 {
		t.Errorf("response race %d gender %d class %d, want 6 1 7", c.GetRaceId(), c.GetGenderId(), c.GetClassId())
	}
	if st.gotNew.RaceID != 6 || st.gotNew.GenderID != 1 || st.gotNew.ClassID != 7 {
		t.Errorf("store got race %d gender %d class %d", st.gotNew.RaceID, st.gotNew.GenderID, st.gotNew.ClassID)
	}
}

// C.O.A.C Y B

// A real preset passes the rules and is stored as preset 0 (worldd takes no real preset, and the creator's JSON
// carries every member the preset resolved to); the rest of the customization and create_info are as sent.
func TestARealPresetIsStoredAsPresetZero(t *testing.T) {
	const sent = `{"race":"Dunir","presetGuid":"6064631211128848384","randomSeed":7,"skinSet":1}`
	chk, st := &recChecker{}, &recStore{}
	req := createRequest()
	req.AppearanceJson = sent
	if _, err := New(st, chk).CreateCharacter(context.Background(), req); err != nil {
		t.Fatal(err)
	}
	if chk.got.AppearanceJSON != sent {
		t.Errorf("the rules were asked about %s, want what was sent", chk.got.AppearanceJSON)
	}
	if want := `{"presetGuid":"0","race":"Dunir","randomSeed":7,"skinSet":1}`; st.gotNew.AppearanceJSON != want {
		t.Errorf("stored %s, want %s", st.gotNew.AppearanceJSON, want)
	}
	if string(st.gotNew.CreateInfo) != string(req.CreateInfo) {
		t.Errorf("create_info %v, want what was sent %v", st.gotNew.CreateInfo, req.CreateInfo)
	}
	// preset 0, and a customization with no preset, are stored as sent
	for _, same := range []string{`{"race":"Dunir","presetGuid":"0"}`, `{"race":"Dunir"}`} {
		st := &recStore{}
		req := createRequest()
		req.AppearanceJson = same
		if _, err := New(st, &recChecker{}).CreateCharacter(context.Background(), req); err != nil {
			t.Fatal(err)
		}
		if st.gotNew.AppearanceJSON != same {
			t.Errorf("stored %s, want %s as sent", st.gotNew.AppearanceJSON, same)
		}
	}
}
