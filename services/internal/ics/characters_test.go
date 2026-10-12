// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/aocchar"
	"aocrop/services/internal/createinfo"
)

// createInfo is the payload of a Kaelar male of the class (see createInfoFor).
func createInfo(json string, class uint64) []byte { return createInfoFor(json, 0, 0, class) }

// createInfoFor builds the client's CreateCharacterInfo payload: field 1 wraps
// the customization map entry, fields 2, 3 and 4 are the gender, race and class
// (a zero is left out, as proto3 does) and field 5 the starting location.
func createInfoFor(json string, gender, race, class uint64) []byte {
	var entry []byte
	entry = protowire.AppendTag(entry, 1, protowire.BytesType)
	entry = protowire.AppendString(entry, "CustomizationDataKey")
	entry = protowire.AppendTag(entry, 2, protowire.BytesType)
	entry = protowire.AppendString(entry, json)
	var wrapper []byte
	wrapper = protowire.AppendTag(wrapper, 2, protowire.BytesType)
	wrapper = protowire.AppendBytes(wrapper, entry)
	var b []byte
	b = protowire.AppendTag(b, 1, protowire.BytesType)
	b = protowire.AppendBytes(b, wrapper)
	for i, v := range []uint64{gender, race, class} {
		if v != 0 {
			b = protowire.AppendTag(b, protowire.Number(2+i), protowire.VarintType)
			b = protowire.AppendVarint(b, v)
		}
	}
	b = protowire.AppendTag(b, 5, protowire.BytesType)
	return protowire.AppendString(b, "Riverlands")
}

// createInfoOfSize is a well-formed payload of exactly size bytes, padded in
// its customization JSON.
func createInfoOfSize(t *testing.T, size int) []byte {
	t.Helper()
	for pad := size - len(createInfo(`{"class":"Mage","pad":""}`, 2)); pad > 0; pad-- {
		if b := createInfo(`{"class":"Mage","pad":"`+strings.Repeat("x", pad)+`"}`, 2); len(b) == size {
			return b
		}
	}
	t.Fatalf("no payload of %d bytes", size)
	return nil
}

// worldCount asks for the world list and returns its character count.
func (h *harness) worldCount() int32 {
	h.t.Helper()
	tr := h.send("ics_xclient.GetWorldsRequest", &icsxclient.GetWorldsRequest{}, "")
	var worlds icsxclient.GetWorldsReply
	h.expect("ics_xclient.GetWorldsReply", success, tr, &worlds)
	return worlds.GetWorlds()[0].GetCharacterCount()
}

// create sends a CreateCharacterRequest and checks the status of its reply.
func (h *harness) create(name string, info []byte, code icscommon.IcsStatusCode) {
	h.t.Helper()
	tr := h.send("ics_xclient.CreateCharacterRequest",
		&icsxclient.CreateCharacterRequest{CharacterName: name, WorldId: worldID, CreateCharacterInfoRaw: info}, "")
	if code == success {
		h.expect("ics_xclient.CreateCharacterReply", success, tr, nil)
	} else {
		h.expectEmpty("ics_xclient.CreateCharacterReply", code, tr)
	}
}

// (C | Y | B | C O A)

func TestGetCharactersEncodesOnlyMine(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	tr := h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	var rep icsxclient.GetCharactersReply
	m := h.expect("ics_xclient.GetCharactersReply", success, tr, &rep)
	mine, _ := chars.List(context.Background(), me.ID)
	if len(rep.GetCharactersRaw()) != len(mine) {
		t.Fatalf("%d characters, want %d", len(rep.GetCharactersRaw()), len(mine))
	}
	for i, c := range mine {
		if !bytes.Equal(rep.GetCharactersRaw()[i], aocchar.Record(c, worldID)) {
			t.Errorf("character %d is not aocchar.Record of %s", i, c.GetName())
		}
	}
	// No ftag of its own: the client's comes back.
	if m.GetSystemData().GetTags()["ftag"] != clientFtag {
		t.Errorf("ftag = %q", m.GetSystemData().GetTags()["ftag"])
	}
}

func TestGetCharactersFailure(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	chars.fail(errors.New("character service down"))
	tr := h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	h.expectEmpty("ics_xclient.GetCharactersReply", codeInternal, tr)
}

func TestCreateCharacterFromTheClientsPayload(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	const js = `{"class":"Mage","gender":"Female","race":"Dunir"}`
	info := createInfoFor(js, 1, 6, 8) // a Dunir (6) female (1) Mage (8)
	tr := h.send("ics_xclient.CreateCharacterRequest",
		&icsxclient.CreateCharacterRequest{CharacterName: "Newbie", WorldId: worldID, CreateCharacterInfoRaw: info}, "")
	var rep icsxclient.CreateCharacterReply
	h.expect("ics_xclient.CreateCharacterReply", success, tr, &rep)

	want := &characterv1.CreateCharacterRequest{
		AccountId: me.ID, Name: "Newbie", RaceId: 6, GenderId: 1, ClassId: 8, ArchetypeId: 17750, ClassName: "Mage",
		AppearanceJson: js, CreateInfo: info, Level: 1,
	}
	if !proto.Equal(chars.created, want) {
		t.Errorf("character service got %v, want %v", chars.created, want)
	}
	stored, err := chars.Get(context.Background(), rep.GetCharacterId())
	if err != nil {
		t.Fatal(err)
	}
	if rep.GetCharacterName() != "Newbie" || rep.GetWorldId() != worldID ||
		!bytes.Equal(rep.GetCharacterInfoRaw(), aocchar.CreateReplyInfo(stored)) {
		t.Errorf("CreateCharacterReply = %v", &rep)
	}
	if n := h.worldCount(); n != 3 {
		t.Errorf("world character count = %d after create, want 3", n)
	}
}

// The real client's payload of a Kaelar male Bard has only the class: Kaelar
// and Male are 0, which proto3 leaves out. Until plan B1 the 2 was read as the
// race.
func TestCreateCharacterFromTheRealClientsPayload(t *testing.T) {
	raw, err := os.ReadFile("../createinfo/testdata/client_create_info.hex")
	if err != nil {
		t.Fatal(err)
	}
	info, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	h.create("Newbie", info, success)
	want := &characterv1.CreateCharacterRequest{
		AccountId: me.ID, Name: "Newbie", RaceId: 0, GenderId: 0, ClassId: 2, ArchetypeId: 17747, ClassName: "Bard",
		AppearanceJson: createinfo.Parse(info).Appearance, CreateInfo: info, Level: 1,
	}
	if !proto.Equal(chars.created, want) || want.GetAppearanceJson() == "" {
		t.Errorf("character service got %v, want %v", chars.created, want)
	}
}

// The gateway reads the three fields and does not judge them: a payload without
// them goes on as a Kaelar male with no class, and one whose varint does not
// fit goes on as -1, for the character service to refuse.
func TestCreateCharacterPassesWhatItReads(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	h.create("Plain", createInfo(`{}`, 0), success)
	if c := chars.created; c.GetRaceId() != 0 || c.GetGenderId() != 0 || c.GetClassId() != 0 || c.GetArchetypeId() != 0 || c.GetClassName() != "" {
		t.Errorf("character service got %v", c)
	}
	oversized := protowire.AppendVarint(protowire.AppendTag(nil, 3, protowire.VarintType), 1<<40)
	h.create("Other", append(oversized, createInfo(`{}`, 2)...), success)
	if c := chars.created; c.GetRaceId() != createinfo.Invalid || c.GetClassId() != 2 || c.GetClassName() != "Bard" {
		t.Errorf("character service got %v", c)
	}
}

// (C_Y_B + C.O.A)

func TestCreateCharacterFailures(t *testing.T) {
	for _, tc := range []struct {
		name string
		err  error // what the character service answers; nil: the fake's own behaviour
		want icscommon.IcsStatusCode
	}{
		{"cyb", nil, codeNameTaken}, // taken by twoOfMine
		{"Newbie", status.Error(codes.InvalidArgument, "name must be 3-20 letters"), codeInvalidRequest},
		{"Newbie", errors.New("connection refused"), codeInternal},
		// the character service has no content.db: not the client's doing
		{"Newbie", status.Error(codes.FailedPrecondition, "character creation is unavailable: no content.db"), codeInternal},
	} {
		chars := twoOfMine()
		h := newHarness(t, chars)
		h.open(goodToken, success)
		chars.fail(tc.err)
		h.create(tc.name, createInfo(`{}`, 2), tc.want)
	}
}

// The payload is stored twice (as create_info, and its JSON as the
// appearance) and every character comes back in each list: a few huge ones
// would push the account's list past gRPC's 4 MB and lock it out. The
// client's are about 6.5 KB.
func TestCreateCharacterCapsThePayloadAndTheName(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)

	h.create("Biggest", createInfoOfSize(t, 64<<10), success)
	h.create(strings.Repeat("n", 64), createInfo(`{}`, 2), success)

	calls := chars.callCount()
	h.create("TooBig", createInfoOfSize(t, 64<<10+1), codeInvalidRequest)
	h.create(strings.Repeat("n", 65), createInfo(`{}`, 2), codeInvalidRequest)
	if n := chars.callCount(); n != calls {
		t.Errorf("an oversized create reached the character service (%d calls)", n-calls)
	}
}

// An account has at most MaxCharacters (16 by default), counted from the
// list the session holds.
func TestCreateCharacterLimit(t *testing.T) {
	if n := DefaultConfig().MaxCharacters; n != 16 {
		t.Errorf("DefaultConfig().MaxCharacters = %d, want 16", n)
	}
	chars := newCharacters()
	for i := range 15 {
		chars.byID[fmt.Sprintf("%032x", i+1)] = &characterv1.Character{Id: fmt.Sprintf("%032x", i+1), AccountId: me.ID, Name: fmt.Sprintf("c%d", i)}
	}
	h := newHarness(t, chars)
	h.open(goodToken, success)
	h.create("Sixteenth", createInfo(`{}`, 2), success)

	calls := chars.callCount()
	h.create("Seventeenth", createInfo(`{}`, 2), codeCharacterCount)
	if n := chars.callCount(); n != calls {
		t.Errorf("a create over the limit reached the character service")
	}

	tr := h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: fmt.Sprintf("%032x", 1)}, "")
	h.expect("ics_xclient.DeleteCharacterReply", success, tr, nil)
	h.create("Seventeenth", createInfo(`{}`, 2), success)

	// A smaller limit, and the list refreshes the count: another client of
	// the account deleted two meanwhile.
	h2 := newHarnessWith(t, twoOfMine(), func(c *Config) { c.MaxCharacters = 3 })
	h2.open(goodToken, success)
	h2.create("Third", createInfo(`{}`, 2), success)
	h2.create("Fourth", createInfo(`{}`, 2), codeCharacterCount)
	for _, id := range []string{"0000000000000000000000000000000a", "0000000000000000000000000000000b"} {
		if err := h2.chars.Delete(context.Background(), me.ID, id); err != nil {
			t.Fatal(err)
		}
	}
	tr = h2.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	h2.expect("ics_xclient.GetCharactersReply", success, tr, nil)
	h2.create("Fourth", createInfo(`{}`, 2), success)
}

func TestDeleteCharacter(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	tr := h.send("ics_xclient.DeleteCharacterRequest",
		&icsxclient.DeleteCharacterRequest{CharacterId: "0000000000000000000000000000000b", CharacterName: "Zed", WorldId: worldID}, "")
	var rep icsxclient.DeleteCharacterReply
	h.expect("ics_xclient.DeleteCharacterReply", success, tr, &rep)
	if rep.GetCharacterId() != "0000000000000000000000000000000b" || chars.has("0000000000000000000000000000000b") {
		t.Errorf("DeleteCharacterReply = %v; still stored: %v", &rep, chars.has("0000000000000000000000000000000b"))
	}
	if n := h.worldCount(); n != 1 {
		t.Errorf("world character count = %d after delete, want 1", n)
	}
	tr = h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: "0000000000000000000000000000000a"}, "")
	h.expect("ics_xclient.DeleteCharacterReply", success, tr, nil)
	if n := h.worldCount(); n != 0 {
		t.Errorf("world character count = %d after deleting both, want 0", n)
	}
}

// built with C-Y-B and CoA

// A delete of a character that is not there (a double click, or another
// account's) answers SUCCESS as the legacy server did: no error pops up, and
// the reply does not tell whether the id exists on another account.
func TestDeleteMissingCharacterSucceeds(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	for _, id := range []string{"0000000000000000000000000000000c", "ffffffffffffffffffffffffffffffff"} {
		tr := h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: id}, "")
		var rep icsxclient.DeleteCharacterReply
		h.expect("ics_xclient.DeleteCharacterReply", success, tr, &rep)
		if rep.GetCharacterId() != id {
			t.Errorf("DeleteCharacterReply = %v, want id %s", &rep, id)
		}
	}
	if !chars.has("0000000000000000000000000000000c") {
		t.Error("another account's character was deleted")
	}
	if n := h.worldCount(); n != 2 {
		t.Errorf("world character count = %d, want 2: nothing was deleted", n)
	}
	// A real failure is still an error.
	chars.fail(errors.New("database down"))
	tr := h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: "0000000000000000000000000000000a"}, "")
	h.expectEmpty("ics_xclient.DeleteCharacterReply", codeInternal, tr)
}

// Character ids reach the services as 32 lowercase hex digits: the client's
// are lowercased, and anything else is refused without a service call.
func TestDeleteCharacterIDs(t *testing.T) {
	chars := newCharacters(&characterv1.Character{Id: "0123456789abcdef0123456789abcdef", AccountId: me.ID, Name: "Hex"})
	h := newHarness(t, chars)
	h.open(goodToken, success)

	calls := chars.callCount()
	for _, id := range malformedIDs {
		tr := h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: id}, "")
		h.expectEmpty("ics_xclient.DeleteCharacterReply", codeCharacterNotFound, tr)
	}
	if n := chars.callCount(); n != calls {
		t.Errorf("a malformed id reached the character service (%d calls)", n-calls)
	}

	const upper = "0123456789ABCDEF0123456789ABCDEF"
	tr := h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: upper}, "")
	var rep icsxclient.DeleteCharacterReply
	h.expect("ics_xclient.DeleteCharacterReply", success, tr, &rep)
	if chars.has("0123456789abcdef0123456789abcdef") || rep.GetCharacterId() != upper {
		t.Errorf("DeleteCharacterReply = %v; still stored: %v", &rep, chars.has("0123456789abcdef0123456789abcdef"))
	}
}

// malformedIDs are not 32 hex digits, in either case.
var malformedIDs = []string{
	"",
	"0123456789abcdef0123456789abcde",   // 31
	"0123456789abcdef0123456789abcdef0", // 33
	"0123456789abcdef0123456789abcdeg",
	"0123456789abcdef0123456789abcdeG",
	"0123456789abcdef0123456789abcde/",
	"0123456789abcdef0123456789abcde:",
	"0123456789abcdef0123456789abcde@",
	"0123456789abcdef0123456789abcde`",
	"0123456789abcdef-0123-4567-89abcdef",
	"0123456789abcdef0123456789abcd\u00e9", // 32 bytes, not ASCII
	"0123456789abcdef0123456789abc\u212a",  // Kelvin sign: lowercases to k
}
