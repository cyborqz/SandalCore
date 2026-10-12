// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/tickets"
)

// play sends a PlayRequest for character.
func (h *harness) play(character string) string {
	h.t.Helper()
	return h.send("ics_xclient.PlayRequest", &icsxclient.PlayRequest{CharacterId: character, WorldId: worldID}, "")
}

// The legacy server's three writes: PlayReply ACKNOWLEDGED with no body, a
// pushed PlayRequestStatusReport, then PlayReply SUCCESS with the ticket and
// the address of the world server the placer chose.
func TestPlayAnswersInThreeWrites(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	tr := h.play("0000000000000000000000000000000a")

	first := h.expectEmpty("ics_xclient.PlayReply", acknowledged, tr)
	var report icsxclient.PlayRequestStatusReport
	second := h.expect("ics_xclient.PlayRequestStatusReport", success, "", &report)
	if report.GetQueuePosition() != 0 {
		t.Errorf("queue position %d", report.GetQueuePosition())
	}
	var rep icsxclient.PlayReply
	third := h.expect("ics_xclient.PlayReply", success, tr, &rep)
	if rep.GetGameConnectionToken() != "7bc658710f9d3f333441c089d974dece" || rep.GetGameConnectionString() != "192.0.2.5:7778" {
		t.Errorf("PlayReply = %v, want the ticket and server 5's address", &rep)
	}
	for i, m := range []*icscommon.MessageWrapper{first, second, third} {
		if f := m.GetSystemData().GetTags()["ftag"]; f != "ics_play_character" {
			t.Errorf("write %d: ftag %q", i+1, f)
		}
	}
	if got := h.placer.placed(); !reflect.DeepEqual(got, []string{"0000000000000000000000000000000a"}) {
		t.Errorf("placed %v", got)
	}
	// A ticket for server 0 would be refused by the world server the client
	// goes to.
	want := []issued{{accountID: me.ID, characterID: "0000000000000000000000000000000a", serverID: 5}}
	if got := h.tickets.issued(); !reflect.DeepEqual(got, want) {
		t.Errorf("tickets issued %v, want %v", got, want)
	}
	h.nothingElse()
}

// [C.O.A - Cyb]

// The client may send its ids in upper case (as an Unreal FGuid prints); the
// services get them in lower case.
func TestPlayWithAnUppercaseID(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	tr := h.play("0000000000000000000000000000000A")
	h.expect("ics_xclient.PlayReply", acknowledged, tr, nil)
	h.expect("ics_xclient.PlayRequestStatusReport", success, "", nil)
	h.expect("ics_xclient.PlayReply", success, tr, nil)
	if got := h.tickets.issued(); len(got) != 1 || got[0].characterID != "0000000000000000000000000000000a" {
		t.Errorf("tickets issued %v", got)
	}
}

// After the acknowledgement a failure is one more PlayReply with the error,
// no body and nothing after it.
func TestPlayFailures(t *testing.T) {
	for _, tc := range []struct {
		name      string
		character string
		charsErr  error
		placerErr error
		ticketErr error
		want      icscommon.IcsStatusCode
	}{
		{"another account's character", "0000000000000000000000000000000c", nil, nil, nil, codeCharacterNotFound},
		{"no such character", "ffffffffffffffffffffffffffffffff", nil, nil, nil, codeCharacterNotFound},
		{"character service down", "0000000000000000000000000000000a", errors.New("connection refused"), nil, nil, codeInternal},
		{"no world server", "0000000000000000000000000000000a", nil, status.Error(codes.Unavailable, "no live world server"), nil, codeNoServer},
		{"placement fails", "0000000000000000000000000000000a", nil, status.Error(codes.Internal, "mesh unreachable"), nil, codeInternal},
		{"tickets fail", "0000000000000000000000000000000a", nil, nil, errors.New("database down"), codeInternal},
		// The character was deleted (by another client of the account) after
		// it was looked up.
		{"character deleted meanwhile", "0000000000000000000000000000000a", nil, nil, tickets.ErrInvalid, codeCharacterNotFound},
	} {
		t.Run(tc.name, func(t *testing.T) {
			chars := twoOfMine()
			h := newHarness(t, chars)
			h.open(goodToken, success)
			chars.fail(tc.charsErr)
			h.placer.err, h.tickets.err = tc.placerErr, tc.ticketErr
			tr := h.play(tc.character)
			h.expectEmpty("ics_xclient.PlayReply", acknowledged, tr)
			if m := h.expectEmpty("ics_xclient.PlayReply", tc.want, tr); m.GetSystemData().GetTags()["ftag"] != "ics_play_character" {
				t.Errorf("ftag %q", m.GetSystemData().GetTags()["ftag"])
			}
			h.nothingElse()
			if tc.ticketErr == nil && len(h.tickets.issued()) != 0 {
				t.Errorf("a ticket was issued: %v", h.tickets.issued())
			}
		})
	}
}

// A request that cannot be played (it does not decode, or its id is not a
// character id) gets one reply and no acknowledgement, and reaches no
// service.
func TestPlayRefusedBeforeTheAck(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	tr := h.play("0000000000000000000000000000000a")
	h.expectEmpty("ics_xclient.PlayRequest", codeSessionCheckFailed, tr) // no session yet
	h.open(goodToken, success)
	calls := chars.callCount()

	tr = h.sendRaw("ics_xclient.PlayRequest", unreadable)
	h.expectEmpty("ics_xclient.PlayReply", codeInvalidRequest, tr)
	for _, id := range malformedIDs {
		tr := h.play(id)
		if m := h.expectEmpty("ics_xclient.PlayReply", codeCharacterNotFound, tr); m.GetSystemData().GetTags()["ftag"] != "ics_play_character" {
			t.Errorf("ftag %q", m.GetSystemData().GetTags()["ftag"])
		}
	}
	h.nothingElse()
	if n := chars.callCount(); n != calls || len(h.placer.placed()) != 0 || len(h.tickets.issued()) != 0 {
		t.Errorf("a refused Play reached a service: %d character calls, placed %v, issued %v",
			n-calls, h.placer.placed(), h.tickets.issued())
	}
}

// -- C.O.A & C.Y.B --

// The legacy and retail servers acknowledge within about 60 ms, before any
// lookup: the acknowledgement does not wait for the character service.
func TestPlayAcknowledgesBeforeLookingUp(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	h.open(goodToken, success)
	waiting := chars.start()
	tr := h.play("0000000000000000000000000000000a")
	h.expectEmpty("ics_xclient.PlayReply", acknowledged, tr)
	h.hanging(waiting) // the lookup is still hanging
	h.cancel()
	if err := h.handlerReturns(); !errors.Is(err, context.Canceled) {
		t.Errorf("handler returned %v, want the stream's context.Canceled", err)
	}
}

func TestPlayIsPaced(t *testing.T) {
	if DefaultConfig().PlayPause != 400*time.Millisecond {
		t.Errorf("PlayPause = %v, want the legacy 400ms", DefaultConfig().PlayPause)
	}
	const pause = 150 * time.Millisecond
	h := newHarnessWith(t, twoOfMine(), func(c *Config) { c.PlayPause = pause })
	h.open(goodToken, success)
	tr := h.play("0000000000000000000000000000000a")
	h.expect("ics_xclient.PlayReply", acknowledged, tr, nil)
	t1 := time.Now()
	h.expect("ics_xclient.PlayRequestStatusReport", success, "", nil)
	t2 := time.Now()
	h.expect("ics_xclient.PlayReply", success, tr, nil)
	t3 := time.Now()
	if d := t2.Sub(t1); d < pause-20*time.Millisecond {
		t.Errorf("report came %v after the ack, want about %v", d, pause)
	}
	if d := t3.Sub(t2); d < pause-20*time.Millisecond {
		t.Errorf("reply came %v after the report, want about %v", d, pause)
	}
}

// A client that leaves in the middle of Play: the handler returns at once,
// without the remaining writes.
func TestPlayStopsWhenTheClientLeaves(t *testing.T) {
	h := newHarnessWith(t, twoOfMine(), func(c *Config) { c.PlayPause = 5 * time.Second })
	h.open(goodToken, success)
	tr := h.play("0000000000000000000000000000000a")
	h.expect("ics_xclient.PlayReply", acknowledged, tr, nil)
	h.cancel()
	if err := h.handlerReturns(); !errors.Is(err, context.Canceled) {
		t.Errorf("handler returned %v, want the stream's context.Canceled", err)
	}
}

// What the operator reads: why there was no world server, and a character
// deleted meanwhile as information, not an error.
func TestPlayFailureLogs(t *testing.T) {
	logs := captureLog(t)
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	h.placer.err = status.Error(codes.Unavailable, "no live world server")
	tr := h.play("0000000000000000000000000000000a")
	h.expect("ics_xclient.PlayReply", acknowledged, tr, nil)
	h.expect("ics_xclient.PlayReply", codeNoServer, tr, nil)
	if !strings.Contains(logs.String(), `level=WARN msg="ics: no world server to play on"`) ||
		!strings.Contains(logs.String(), `desc = no live world server`) {
		t.Errorf("log after no world server:\n%s", logs.String())
	}

	h.placer.err, h.tickets.err = nil, tickets.ErrInvalid
	tr = h.play("0000000000000000000000000000000a")
	h.expect("ics_xclient.PlayReply", acknowledged, tr, nil)
	h.expect("ics_xclient.PlayReply", codeCharacterNotFound, tr, nil)
	if strings.Contains(logs.String(), "level=ERROR") || !strings.Contains(logs.String(), "level=INFO msg=\"ics: character deleted while joining\"") {
		t.Errorf("log after a character deleted meanwhile:\n%s", logs.String())
	}
}
