// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"errors"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
)

func miniRecords(t *testing.T, state []byte) []string {
	t.Helper()
	var s icsxclient.PlayerSessionState
	if err := proto.Unmarshal(state, &s); err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, r := range s.GetCharacterMiniRecords() {
		out = append(out, r.GetCharacterId()+"/"+r.GetCharacterName()+"/"+r.GetWorldId())
	}
	return out
}

// built with C Y B and CoA

func TestSessionOpensWithTheLauncherToken(t *testing.T) {
	h := newHarness(t, twoOfMine())
	m := h.open(goodToken, success)

	tags := m.GetSystemData().GetTags()
	if tags["ftag"] != "ics_session" || tags["server_id"] != "aoc-gateway" || tags["client_id"] != "ue_app_test" ||
		len(tags["msg_uuid"]) != 32 || tags["server_session_id"] != "10001" { // the legacy server's first id too
		t.Errorf("reply tags = %v", tags)
	}
	if ns, err := strconv.ParseInt(tags["server_epoch_nanosec"], 10, 64); err != nil || time.Since(time.Unix(0, ns)).Abs() > time.Minute {
		t.Errorf("server_epoch_nanosec %q is not now", tags["server_epoch_nanosec"])
	}
	var rep icscommon.SessionReply
	if err := proto.Unmarshal(m.GetMessageData(), &rep); err != nil {
		t.Fatal(err)
	}
	if rep.GetResultCode() != success || rep.GetRequestType() != icscommon.SessionRequestType_SESSION_REQUEST_TYPE_OPEN ||
		!strings.Contains(rep.GetDataMap()["ics_ping_client_config.json"], `"ping_hosts"`) {
		t.Errorf("SessionReply = %v", &rep)
	}
	want := []string{
		"0000000000000000000000000000000a/cyb/" + worldID,
		"0000000000000000000000000000000b/Zed/" + worldID,
	}
	if got := miniRecords(t, []byte(rep.GetDataMap()["player_session_state"])); !reflect.DeepEqual(got, want) {
		t.Errorf("mini records = %v, want %v", got, want)
	}
}

func TestSessionRefusesAnInvalidToken(t *testing.T) {
	h := newHarness(t, twoOfMine())
	m := h.open("emulator-ics-token-deadbeef", codeAuthFailed)
	var rep icscommon.SessionReply
	if err := proto.Unmarshal(m.GetMessageData(), &rep); err != nil || rep.GetResultCode() != codeAuthFailed {
		t.Errorf("SessionReply = %v %v, want result %v", &rep, err, codeAuthFailed)
	}
	tr := h.send("ics_xclient.GetWorldsRequest", &icsxclient.GetWorldsRequest{}, "")
	h.expect("ics_xclient.GetWorldsRequest", codeSessionCheckFailed, tr, nil)
}

// The auth service being down is not the player's bad token.
func TestSessionWithTheAuthServiceDownIsInternal(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.accounts.fail(status.Error(codes.Unavailable, "connection refused"))
	m := h.open(goodToken, codeInternal)
	var rep icscommon.SessionReply
	if err := proto.Unmarshal(m.GetMessageData(), &rep); err != nil || rep.GetResultCode() != codeInternal {
		t.Errorf("SessionReply = %v %v, want result %v", &rep, err, codeInternal)
	}
}

// An OPEN on a stream that has a session replaces it: when the new one is
// refused, none is left.
func TestRefusedReopenEndsTheSession(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	h.open("someone-elses-expired-token", codeAuthFailed)
	tr := h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	h.expectEmpty("ics_xclient.GetCharactersRequest", codeSessionCheckFailed, tr)
}

// Only OPEN and CLOSE are session requests; anything else (INVALID, the
// default, or an unknown value) is refused and leaves the session as it is.
func TestSessionRequestTypeMustBeOpenOrClose(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	for _, typ := range []icscommon.SessionRequestType{icscommon.SessionRequestType_SESSION_REQUEST_TYPE_INVALID, 3} {
		tr := h.send("ics_common.SessionRequest", &icscommon.SessionRequest{RequestType: typ}, "")
		var rep icscommon.SessionReply
		m := h.expect("ics_common.SessionReply", codeInvalidRequest, tr, &rep)
		if rep.GetResultCode() != codeInvalidRequest || rep.GetRequestType() != typ || len(rep.GetDataMap()) != 0 ||
			m.GetSystemData().GetTags()["ftag"] != "ics_session" {
			t.Errorf("type %d: SessionReply = %v, ftag %q", typ, &rep, m.GetSystemData().GetTags()["ftag"])
		}
	}
	if n := h.worldCount(); n != 2 {
		t.Errorf("world character count = %d, want the session's 2", n)
	}
}

func TestKeepAliveNeedsNoSession(t *testing.T) {
	h := newHarness(t, twoOfMine())
	tr := h.send("ics_common.KeepAliveMessage", &icscommon.KeepAliveMessage{TimestampMsec: 1}, "")
	var ka icscommon.KeepAliveMessage
	m := h.expect("ics_common.KeepAliveMessage", success, tr, &ka)
	if d := time.Since(time.UnixMilli(ka.GetTimestampMsec())); d < 0 || d > time.Minute {
		t.Errorf("timestamp %d is not now", ka.GetTimestampMsec())
	}
	if m.GetSystemData().GetTags()["ftag"] != "ics_keep_alive" {
		t.Errorf("ftag = %q", m.GetSystemData().GetTags()["ftag"])
	}
}

// CoA.C | Y | B

func TestLobby(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	ftag := func(m *icscommon.MessageWrapper) string { return m.GetSystemData().GetTags()["ftag"] }

	tr := h.send("ics_xclient.CheckGameVersionRequest", &icsxclient.CheckGameVersionRequest{GameVersion: "20260119002-438018"}, "")
	var ver icsxclient.CheckGameVersionReply
	m := h.expect("ics_xclient.CheckGameVersionReply", success, tr, &ver)
	if !ver.GetSupported() || ver.GetMessage() != "" || ftag(m) != "ics_lobby" {
		t.Errorf("CheckGameVersionReply = %v, ftag %q", &ver, ftag(m))
	}

	wantWorld := &icsxclient.WorldInfo{WorldId: worldID, WorldDisplayName: "[EMU] Local Server", Available: true, CharacterCount: 2}
	tr = h.send("ics_xclient.GetWorldsRequest", &icsxclient.GetWorldsRequest{}, "")
	var worlds icsxclient.GetWorldsReply
	m = h.expect("ics_xclient.GetWorldsReply", success, tr, &worlds)
	if len(worlds.GetWorlds()) != 1 || !proto.Equal(worlds.GetWorlds()[0], wantWorld) || ftag(m) != "ics_lobby" {
		t.Errorf("GetWorldsReply = %v, ftag %q", &worlds, ftag(m))
	}

	// Starting interest is answered with a pushed world status; stopping is not answered.
	h.send("ics_xclient.GameClientInterestEvent", &icsxclient.GameClientInterestEvent{StartInterestBitmask: 1}, "")
	var ev icsxclient.WorldStatusEvent
	m = h.expect("ics_xclient.WorldStatusEvent", success, "", &ev)
	if len(ev.GetWorlds()) != 1 || !proto.Equal(ev.GetWorlds()[0], wantWorld) || ftag(m) != "ics_lobby" {
		t.Errorf("WorldStatusEvent = %v, ftag %q", &ev, ftag(m))
	}
	h.send("ics_xclient.GameClientInterestEvent", &icsxclient.GameClientInterestEvent{StopInterestBitmask: 1}, "")
	h.nothingElse()
	// Nor is an interest event that does not decode.
	h.sendRaw("ics_xclient.GameClientInterestEvent", unreadable)
	h.nothingElse()
}

func TestAuthRefreshGivesBackTheSessionToken(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	tr := h.send("ics_xclient.AuthRefreshRequest", &icsxclient.AuthRefreshRequest{AuthToken: "x", RefreshToken: "y"}, "")
	var rep icsxclient.AuthRefreshReply
	m := h.expect("ics_xclient.AuthRefreshReply", success, tr, &rep)
	if rep.GetAuthToken() != goodToken || rep.GetRefreshToken() != goodToken ||
		m.GetSystemData().GetTags()["ftag"] != "ics_xclient_auth" {
		t.Errorf("AuthRefreshReply = %v, ftag %q", &rep, m.GetSystemData().GetTags()["ftag"])
	}
}

// The token is checked again at each refresh (every 6 minutes): once it has
// expired the session ends.
func TestAuthRefreshWithAnExpiredTokenEndsTheSession(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	h.accounts.forget(goodToken)
	tr := h.send("ics_xclient.AuthRefreshRequest", &icsxclient.AuthRefreshRequest{}, "")
	m := h.expectEmpty("ics_xclient.AuthRefreshReply", codeAuthFailed, tr)
	if f := m.GetSystemData().GetTags()["ftag"]; f != "ics_xclient_auth" {
		t.Errorf("ftag %q", f)
	}
	tr = h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	h.expectEmpty("ics_xclient.GetCharactersRequest", codeSessionCheckFailed, tr)
}

// With the auth service unreachable nothing says the token has expired: the
// session carries on.
func TestAuthRefreshWithTheAuthServiceDownKeepsTheSession(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	h.accounts.fail(status.Error(codes.Unavailable, "connection refused"))
	tr := h.send("ics_xclient.AuthRefreshRequest", &icsxclient.AuthRefreshRequest{}, "")
	h.expectEmpty("ics_xclient.AuthRefreshReply", codeInternal, tr)
	tr = h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{WorldId: worldID}, "")
	h.expect("ics_xclient.GetCharactersReply", success, tr, nil)
	if n := h.accounts.callCount(); n != 2 {
		t.Errorf("%d token checks, want 2 (OPEN and the refresh)", n)
	}
}

func TestUnhandledMessagesGetAnEmptyReplyOfTheSameType(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	tr := h.send("ics_xclient.SelectCharacterRequest", &icsxclient.SelectCharacterRequest{CharacterId: "x"}, "")
	h.expectEmpty("ics_xclient.SelectCharacterRequest", success, tr)
}

// ~ C O A & C | Y | B ~

func TestSessionCloseEndsTheSession(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	tr := h.send("ics_common.SessionRequest",
		&icscommon.SessionRequest{RequestType: icscommon.SessionRequestType_SESSION_REQUEST_TYPE_CLOSE}, "")
	var rep icscommon.SessionReply
	m := h.expect("ics_common.SessionReply", success, tr, &rep)
	if rep.GetRequestType() != icscommon.SessionRequestType_SESSION_REQUEST_TYPE_CLOSE || len(rep.GetDataMap()) != 0 ||
		rep.GetResultCode() != success || m.GetSystemData().GetTags()["ftag"] != "ics_session" {
		t.Errorf("SessionReply = %v", &rep)
	}
	tr = h.send("ics_xclient.GetWorldsRequest", &icsxclient.GetWorldsRequest{}, "")
	h.expect("ics_xclient.GetWorldsRequest", codeSessionCheckFailed, tr, nil)
}

func TestSessionFailsWhenCharactersCannotBeListed(t *testing.T) {
	chars := twoOfMine()
	chars.err = errors.New("character service down")
	h := newHarness(t, chars)
	var rep icscommon.SessionReply
	if err := proto.Unmarshal(h.open(goodToken, codeInternal).GetMessageData(), &rep); err != nil || rep.GetResultCode() != codeInternal {
		t.Errorf("SessionReply = %v %v, want result %v", &rep, err, codeInternal)
	}
	tr := h.send("ics_xclient.GetWorldsRequest", &icsxclient.GetWorldsRequest{}, "")
	h.expect("ics_xclient.GetWorldsRequest", codeSessionCheckFailed, tr, nil)
}
