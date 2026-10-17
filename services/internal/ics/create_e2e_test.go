// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

// Plan B1 end to end: the client's create payload goes through the gateway's
// ICS handler to the real character service (rules read from the real
// content.db, characters in aoc_test) and back out in the character list. The
// test needs both AOC_TEST_DATABASE_URL and AOC_CONTENT_DB (scripts\services.ps1
// test sets them) and is skipped without either.

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"

	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/character"
	"aocrop/services/internal/charactergrpc"
	"aocrop/services/internal/content"
	"aocrop/services/internal/customization"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// viaService is the character service's handlers as the gateway's Characters.
type viaService struct{ s *charactergrpc.Server }

func (v viaService) List(ctx context.Context, accountID int64) ([]*characterv1.Character, error) {
	r, err := v.s.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: accountID})
	return r.GetCharacters(), err
}

func (v viaService) Get(ctx context.Context, id string) (*characterv1.Character, error) {
	r, err := v.s.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id})
	return r.GetCharacter(), err
}

func (v viaService) Create(ctx context.Context, req *characterv1.CreateCharacterRequest) (*characterv1.Character, error) {
	r, err := v.s.CreateCharacter(ctx, req)
	return r.GetCharacter(), err
}

func (v viaService) Delete(ctx context.Context, accountID int64, id string) error {
	_, err := v.s.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{Id: id, AccountId: accountID})
	return err
}

// -- C Y B . C-O-A --

// realService is a character service on aoc_test with the real content.db's rules.
func realService(t *testing.T, rules charactergrpc.Checker) (*charactergrpc.Server, *character.Store) {
	t.Helper()
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	// the harness's account has id 7
	if _, err := pool.Exec(ctx, `INSERT INTO accounts (id, username, password_hash) VALUES ($1, $2, 'x')`, me.ID, me.Username); err != nil {
		t.Fatal(err)
	}
	store := character.NewStore(pool)
	return charactergrpc.New(store, rules), store
}

func realRules(t *testing.T) *customization.Rules {
	t.Helper()
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB not set (the content.db contentc builds)")
	}
	cdb, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cdb.Close() })
	rules, err := customization.Load(cdb)
	if err != nil {
		t.Fatal(err)
	}
	return rules
}

type acceptCreationRules struct{}

func (acceptCreationRules) Check(customization.New) error { return nil }

func TestConcurrentSessionsCannotExceedCharacterLimit(t *testing.T) {
	svc, store := realService(t, acceptCreationRules{})
	ctx := context.Background()
	for i := range 15 {
		if _, err := store.Create(ctx, character.New{AccountID: me.ID, Name: fmt.Sprintf("Char%02d", i)}); err != nil {
			t.Fatal(err)
		}
	}
	clients := []*harness{newHarness(t, &fakeCharacters{via: viaService{svc}}), newHarness(t, &fakeCharacters{via: viaService{svc}})}
	for _, h := range clients {
		h.open(goodToken, success)
	}
	for i, h := range clients {
		h.send("ics_xclient.CreateCharacterRequest", &icsxclient.CreateCharacterRequest{
			CharacterName: fmt.Sprintf("LastSlot%d", i), WorldId: worldID, CreateCharacterInfoRaw: createInfo(`{}`, 2),
		}, "")
	}
	counts := map[icscommon.IcsStatusCode]int{}
	for _, h := range clients {
		m, err := h.stream.Recv()
		if err != nil {
			t.Fatal(err)
		}
		if m.GetMessageTypeName() != "ics_xclient.CreateCharacterReply" {
			t.Fatalf("reply type = %s", m.GetMessageTypeName())
		}
		counts[m.GetStatusCode()]++
	}
	if counts[success] != 1 || counts[codeCharacterCount] != 1 {
		t.Fatalf("concurrent create statuses = %v, want one success and one character-count error", counts)
	}
	chars, err := store.List(ctx, me.ID)
	if err != nil || len(chars) != 16 {
		t.Fatalf("stored characters = %d, err %v; want 16", len(chars), err)
	}
	// Offline imports still preserve all existing characters, even beyond the
	// runtime cap. Their internal Create call does not ask for a limit.
	if _, err := store.Create(ctx, character.New{AccountID: me.ID, Name: "Imported"}); err != nil {
		t.Fatalf("offline creation above the runtime cap: %v", err)
	}
}

// listed is CharacterInformation (field 100) of the character list's record i, as the client reads it.
func listed(t *testing.T, h *harness, i int) map[protowire.Number]uint64 {
	t.Helper()
	tr := h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	var rep icsxclient.GetCharactersReply
	h.expect("ics_xclient.GetCharactersReply", success, tr, &rep)
	if i >= len(rep.GetCharactersRaw()) {
		t.Fatalf("%d characters listed, want more than %d", len(rep.GetCharactersRaw()), i)
	}
	var info []byte
	for b := rep.GetCharactersRaw()[i]; len(b) > 0; {
		num, typ, n := protowire.ConsumeTag(b)
		b = b[n:]
		if num == 100 {
			info, _ = protowire.ConsumeBytes(b)
		}
		b = b[protowire.ConsumeFieldValue(num, typ, b):]
	}
	out := map[protowire.Number]uint64{}
	for b := info; len(b) > 0; {
		num, typ, n := protowire.ConsumeTag(b)
		b = b[n:]
		if typ == protowire.VarintType {
			v, n := protowire.ConsumeVarint(b)
			out[num] = v
			b = b[n:]
			continue
		}
		b = b[protowire.ConsumeFieldValue(num, typ, b):]
	}
	return out
}

// retailCustomization is the retail capture's customization of a race and gender that has a hair option (the first in
// the file), with the class name the client writes; "" when the captures have none (they have no female Vek or Renkai).
func retailCustomization(t *testing.T, race, gender, class string) string {
	t.Helper()
	raw, err := os.ReadFile("../customization/testdata/retail_customizations.json")
	if err != nil {
		t.Fatal(err)
	}
	var list []map[string]any
	if err := json.Unmarshal(raw, &list); err != nil {
		t.Fatal(err)
	}
	for _, c := range list {
		if c["race"] == race && c["gender"] == gender && c["headHair"] != nil && c["headHair"] != "0" {
			c["class"] = class
			out, err := json.Marshal(c)
			if err != nil {
				t.Fatal(err)
			}
			return string(out)
		}
	}
	return ""
}

// [CoA ~ C:Y:B]

func TestCreateThroughTheRealCharacterService(t *testing.T) {
	rules := realRules(t)
	svc, store := realService(t, rules)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)

	// The real client's payload: a Kaelar male Bard, which has only the class on the wire.
	raw, err := os.ReadFile("../createinfo/testdata/client_create_info.hex")
	if err != nil {
		t.Fatal(err)
	}
	cyb, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	h.create("Newbie", cyb, success)
	if got := listed(t, h, 0); got[4] != 0 || got[5] != 0 || got[6] != 2 || got[2] != 1 {
		t.Errorf("a Kaelar male Bard is listed with level %d gender %d race %d class %d, want 1 0 0 2", got[2], got[4], got[5], got[6])
	}

	// A Dunir female Tank, with a retail customization of that race and gender: gender 1, race 6, class 7 on the wire.
	const dunir = 6
	js := retailCustomization(t, "Dunir", "Female", "Tank")
	if js == "" {
		t.Fatal("no retail customization of a female Dunir")
	}
	h.create("Dunirtank", createInfoFor(js, 1, dunir, 7), success)

	// The other races of the retail captures, both genders: every one of them is created and listed as it was made.
	n := 2
	for race, rn := range []string{"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua"} {
		for gender, gn := range []string{"Male", "Female"} {
			if race == dunir && gender == 1 || race == 0 && gender == 0 {
				continue // made above
			}
			class := uint64(1 + (race+gender)%8)
			js := retailCustomization(t, rn, gn, classNames[class])
			if js == "" {
				t.Logf("no retail customization of a %s %s: not created", gn, rn)
				continue
			}
			h.create(fmt.Sprintf("Char%d", n), createInfoFor(js, uint64(gender), uint64(race), class), success)
			n++
		}
	}

	ctx := context.Background()
	list, err := store.List(ctx, me.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 14 { // the 16 race and gender pairs, but the retail captures have no female Vek or Renkai
		t.Fatalf("%d characters stored, want 14", len(list))
	}
	byName := map[string]character.Character{}
	for _, c := range list {
		byName[c.Name] = c
	}
	if c := byName["Newbie"]; c.RaceID != 0 || c.GenderID != 0 || c.ClassID != 2 || c.ClassName != "Bard" || c.ArchetypeID != 17747 {
		t.Errorf("the stored Kaelar male Bard: %+v", c)
	}
	if c := byName["Dunirtank"]; c.RaceID != 6 || c.GenderID != 1 || c.ClassID != 7 || c.ClassName != "Tank" || c.ArchetypeID != 17754 {
		t.Errorf("the stored Dunir female Tank: %+v", c)
	}
	// ... and the list the client reads has each at the fields of the client's descriptors.
	tr := h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	var rep icsxclient.GetCharactersReply
	h.expect("ics_xclient.GetCharactersReply", success, tr, &rep)
	if len(rep.GetCharactersRaw()) != len(list) {
		t.Fatalf("%d characters listed, %d stored", len(rep.GetCharactersRaw()), len(list))
	}
	for i := range list {
		got := listed(t, h, i)
		want := list[i]
		if got[4] != uint64(want.GenderID) || got[5] != uint64(want.RaceID) || got[6] != uint64(want.ClassID) {
			t.Errorf("%s: listed gender %d race %d class %d, stored %d %d %d", want.Name, got[4], got[5], got[6], want.GenderID, want.RaceID, want.ClassID)
		}
	}
}

var classNames = []string{"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"}

// What the character service refuses is INVALID_REQUEST to the client and is not stored.
func TestTheRealCharacterServiceRefuses(t *testing.T) {
	rules := realRules(t)
	svc, store := realService(t, rules)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)

	kaelar := retailCustomization(t, "Kaelar", "Male", "Bard")
	dunir := retailCustomization(t, "Dunir", "Female", "Tank")
	if kaelar == "" || dunir == "" {
		t.Fatal("the retail captures have no Kaelar male or Dunir female")
	}
	for name, tc := range map[string]struct {
		json                string
		gender, race, class uint64
	}{
		"a Dunir customization with no race field (the zero is Kaelar)": {dunir, 1, 0, 7},
		"a Kaelar customization on a Dunir":                             {kaelar, 0, 6, 2},
		"a Dunir female whose gender field is left Male":                {dunir, 0, 6, 7},
		"Tulnar":                           {strings.Replace(kaelar, `"Kaelar"`, `"Tulnar"`, 1), 0, 8, 2},
		"race 9":                           {kaelar, 0, 9, 2},
		"gender 2":                         {kaelar, 2, 0, 2},
		"no class":                         {kaelar, 0, 0, 0},
		"class 9":                          {kaelar, 0, 0, 9},
		"a customization that is not JSON": {"nope", 0, 0, 2},
		"no customization":                 {"", 0, 0, 2},
		"hair that no race has":            {strings.Replace(kaelar, `"headHair":"`, `"headHair":"1`, 1), 0, 0, 2},
	} {
		info := createInfoFor(tc.json, tc.gender, tc.race, tc.class)
		if tc.json == "" {
			info = protowire.AppendVarint(protowire.AppendTag(nil, 4, protowire.VarintType), tc.class)
		}
		h.create(fmt.Sprintf("Refused%d", len(name)), info, codeInvalidRequest)
	}
	if list, err := store.List(context.Background(), me.ID); err != nil || len(list) != 0 {
		t.Errorf("%d characters stored after the refusals, %v", len(list), err)
	}
	// the same customizations, made right, are accepted
	h.create("Fine", createInfoFor(dunir, 1, 6, 7), success)
	h.create("AlsoFine", createInfoFor(kaelar, 0, 0, 2), success)
	if list, err := store.List(context.Background(), me.ID); err != nil || len(list) != 2 {
		t.Errorf("%d characters stored after the two good ones, %v", len(list), err)
	}
}

// With no usable content.db the character service refuses every creation, and the client is told
// an internal error, not that its customization was wrong.
func TestWithoutRulesTheClientGetsAnInternalError(t *testing.T) {
	svc, store := realService(t, nil)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)
	h.create("Newbie", createInfoFor(`{"race":"Kaelar","gender":"Male","class":"Bard"}`, 0, 0, 2), codeInternal)
	if list, err := store.List(context.Background(), me.ID); err != nil || len(list) != 0 {
		t.Errorf("%d characters stored, %v", len(list), err)
	}
}

// createReply sends a create and returns its reply, which must be a CreateCharacterReply that echoes the request's
// tracking tag.
func createReply(t *testing.T, h *harness, name string, info []byte, code icscommon.IcsStatusCode) *icscommon.MessageWrapper {
	t.Helper()
	tr := h.send("ics_xclient.CreateCharacterRequest",
		&icsxclient.CreateCharacterRequest{CharacterName: name, WorldId: worldID, CreateCharacterInfoRaw: info}, "")
	if code == success {
		return h.expect("ics_xclient.CreateCharacterReply", success, tr, nil)
	}
	return h.expectEmpty("ics_xclient.CreateCharacterReply", code, tr)
}

// What the client sees of a refused create (the review's I1): a CreateCharacterReply with an empty body, the
// request's tracking tag, and the executable's number for InvalidRequest, 24: any number outside 10-127 left the
// client's call pending for 10 s (CallExpired), and its next create said "Cannot set new client state". A second
// create on the same stream goes through.
func TestARefusedCreateIsStatus24AndTheNextCreateOnTheStreamWorks(t *testing.T) {
	rules := realRules(t)
	svc, store := realService(t, rules)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)
	kaelar := retailCustomization(t, "Kaelar", "Male", "Bard")
	if kaelar == "" {
		t.Fatal("the retail captures have no Kaelar male")
	}

	// refused by the rules: a Kaelar customization on a Dunir
	m := createReply(t, h, "Refused", createInfoFor(kaelar, 0, 6, 2), codeInvalidRequest)
	if int32(m.GetStatusCode()) != 24 || m.GetMessageTypeName() != "ics_xclient.CreateCharacterReply" || len(m.GetMessageData()) != 0 {
		t.Errorf("a refused create: %q status %d with %d bytes, want ics_xclient.CreateCharacterReply, 24, none", m.GetMessageTypeName(), int32(m.GetStatusCode()), len(m.GetMessageData()))
	}
	var rep icsxclient.CreateCharacterReply
	if err := proto.Unmarshal(m.GetMessageData(), &rep); err != nil || proto.Size(&rep) != 0 {
		t.Errorf("the refused reply decodes to %v, %v; want an empty CreateCharacterReply", &rep, err)
	}
	// refused before the service: a payload over the cap, and a name over the cap
	m = createReply(t, h, "TooBig", createInfoOfSize(t, 64<<10+1), codeInvalidRequest)
	if int32(m.GetStatusCode()) != 24 {
		t.Errorf("an oversized create: status %d, want 24", int32(m.GetStatusCode()))
	}
	// the same stream creates the next one, and the one after
	m = createReply(t, h, "Fine", createInfoFor(kaelar, 0, 0, 2), success)
	if int32(m.GetStatusCode()) != 0 {
		t.Errorf("the create after a refusal: status %d, want 0", int32(m.GetStatusCode()))
	}
	createReply(t, h, "AlsoFine", createInfoFor(kaelar, 0, 0, 2), success)
	if list, err := store.List(context.Background(), me.ID); err != nil || len(list) != 2 {
		t.Errorf("%d characters stored, want the two good ones, %v", len(list), err)
	}
}

// C-Y-B / CoA

// A name that is taken is 47 (ExistingNameFound), whatever its case; the stream goes on.
func TestADuplicateNameIsStatus47(t *testing.T) {
	rules := realRules(t)
	svc, _ := realService(t, rules)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)
	kaelar := retailCustomization(t, "Kaelar", "Male", "Bard")
	createReply(t, h, "Twice", createInfoFor(kaelar, 0, 0, 2), success)
	for _, name := range []string{"Twice", "twice", "TWICE"} {
		m := createReply(t, h, name, createInfoFor(kaelar, 0, 0, 2), codeNameTaken)
		if int32(m.GetStatusCode()) != 47 {
			t.Errorf("a create of the taken name %s: status %d, want 47", name, int32(m.GetStatusCode()))
		}
	}
	createReply(t, h, "Other", createInfoFor(kaelar, 0, 0, 2), success)
}

// The 17th character is 48 (CharacterCountCheckFailed); the 16th is created.
func TestTheSeventeenthCharacterIsStatus48(t *testing.T) {
	rules := realRules(t)
	svc, store := realService(t, rules)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)
	kaelar := retailCustomization(t, "Kaelar", "Male", "Bard")
	for i := 1; i <= 16; i++ {
		createReply(t, h, fmt.Sprintf("Char%d", i), createInfoFor(kaelar, 0, 0, 2), success)
	}
	m := createReply(t, h, "Seventeen", createInfoFor(kaelar, 0, 0, 2), codeCharacterCount)
	if int32(m.GetStatusCode()) != 48 {
		t.Errorf("the 17th create: status %d, want 48", int32(m.GetStatusCode()))
	}
	if list, err := store.List(context.Background(), me.ID); err != nil || len(list) != 16 {
		t.Errorf("%d characters stored, want 16, %v", len(list), err)
	}
}

// A creator preset (a non-zero presetGuid, a real preset of the race) is created and stored as preset 0, so that
// worldd, which takes no real preset, can build the pawn at join; create_info is kept as the client sent it.
func TestACreatorPresetIsCreatedAndStoredAsPresetZero(t *testing.T) {
	rules := realRules(t)
	svc, store := realService(t, rules)
	chars := newCharacters()
	chars.via = viaService{svc}
	h := newHarness(t, chars)
	h.open(goodToken, success)
	js := retailCustomization(t, "Kaelar", "Male", "Bard")
	if js == "" {
		t.Fatal("the retail captures have no Kaelar male")
	}
	var c map[string]any
	if err := json.Unmarshal([]byte(js), &c); err != nil {
		t.Fatal(err)
	}
	const preset = "6064631211128848384" // a Kaelar male preset of the real content.db
	c["presetGuid"] = preset
	withPreset, err := json.Marshal(c)
	if err != nil {
		t.Fatal(err)
	}
	info := createInfoFor(string(withPreset), 0, 0, 2)
	createReply(t, h, "Presetty", info, success)

	list, err := store.List(context.Background(), me.ID)
	if err != nil || len(list) != 1 {
		t.Fatalf("%d characters stored, %v", len(list), err)
	}
	var stored map[string]any
	if err := json.Unmarshal([]byte(list[0].AppearanceJSON), &stored); err != nil {
		t.Fatal(err)
	}
	if stored["presetGuid"] != "0" {
		t.Errorf("stored presetGuid %v, want \"0\" (the preset was %s)", stored["presetGuid"], preset)
	}
	delete(stored, "presetGuid")
	delete(c, "presetGuid")
	if want, got := text(t, c), text(t, stored); want != got {
		t.Errorf("the rest of the customization changed:\n got %s\nwant %s", got, want)
	}
	if string(list[0].CreateInfo) != string(info) {
		t.Errorf("create_info is not what the client sent (%d bytes, sent %d)", len(list[0].CreateInfo), len(info))
	}
}

func text(t *testing.T, m map[string]any) string {
	t.Helper()
	b, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}
