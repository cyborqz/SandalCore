// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"testing"

	"google.golang.org/grpc/metadata"

	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
)

// unreadable is message data that does not decode: field 1 says 5 bytes
// follow, and one does.
var unreadable = []byte{0x0a, 0x05, 'a'}

// The numbers of the statuses the gateway sends are pinned here: they are what goes on the wire, and the
// game executable's own (testdata/ics_status_codes_exe.txt).
func TestStatusCodeNumbers(t *testing.T) {
	for name, tc := range map[string]struct {
		code icscommon.IcsStatusCode
		want int32
	}{
		"success": {success, 0}, "acknowledged": {acknowledged, 1},
		"codeInternal": {codeInternal, 22}, "codeInvalidRequest": {codeInvalidRequest, 24},
		"codeAuthFailed": {codeAuthFailed, 40}, "codeSessionCheckFailed": {codeSessionCheckFailed, 43},
		"codeNameTaken": {codeNameTaken, 47}, "codeCharacterCount": {codeCharacterCount, 48},
		"codeNoServer": {codeNoServer, 51}, "codeCharacterNotFound": {codeCharacterNotFound, 59},
	} {
		if int32(tc.code) != tc.want {
			t.Errorf("%s = %d, want %d", name, tc.code, tc.want)
		}
	}
}

// [CoA + C:Y:B]

// The client reads 0-3 as a result and 10-127 as an error; a number between them or past 127 is neither, and
// a call that got one stays pending until it expires (the 10 s a refused create used to hang for).
func TestEveryErrorStatusIsInTheErrorRange(t *testing.T) {
	for name, code := range map[string]icscommon.IcsStatusCode{
		"codeInternal": codeInternal, "codeInvalidRequest": codeInvalidRequest, "codeAuthFailed": codeAuthFailed,
		"codeSessionCheckFailed": codeSessionCheckFailed, "codeNameTaken": codeNameTaken,
		"codeCharacterNotFound": codeCharacterNotFound, "codeNoServer": codeNoServer, "codeCharacterCount": codeCharacterCount,
	} {
		if !validStatus(code) || int32(code) < 10 {
			t.Errorf("%s = %d is not an error number (10-127)", name, code)
		}
	}
	for name, code := range map[string]icscommon.IcsStatusCode{"success": success, "acknowledged": acknowledged} {
		if code < 0 || code > 3 {
			t.Errorf("%s = %d is not a result number (0-3)", name, code)
		}
	}
}

// validStatus is whether the client knows the number: a result (0-3) or an error (10-127).
func validStatus(code icscommon.IcsStatusCode) bool {
	return code >= 0 && code <= 3 || code >= 10 && code <= 127
}

// proto/ics_common.proto has exactly the executable's names and numbers: nothing is renumbered, dropped or
// invented. The names are compared without case and underscores (IcsStatusCodeError_InvalidRequest and
// ICS_STATUS_CODE_ERROR_INVALID_REQUEST).
func TestStatusCodesAreTheExes(t *testing.T) {
	raw, err := os.ReadFile("testdata/ics_status_codes_exe.txt")
	if err != nil {
		t.Fatal(err)
	}
	flat := func(name string) string { return strings.ToLower(strings.ReplaceAll(name, "_", "")) }
	protoNumber := map[string]int32{}
	for name, n := range icscommon.IcsStatusCode_value {
		if _, dup := protoNumber[flat(name)]; dup {
			t.Fatalf("two names flatten to %s", flat(name))
		}
		protoNumber[flat(name)] = n
	}
	exe := map[string]int32{}
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		var n int32
		var name string
		if _, err := fmt.Sscanf(line, "%d %s", &n, &name); err != nil {
			t.Fatalf("line %q: %v", line, err)
		}
		exe[flat(name)] = n
		got, ok := protoNumber[flat(name)]
		switch {
		case !ok:
			t.Errorf("the executable's %s = %d is missing from proto/ics_common.proto", name, n)
		case got != n:
			t.Errorf("%s is %d in proto/ics_common.proto, the executable's number is %d", name, got, n)
		}
	}
	if len(exe) < 70 {
		t.Fatalf("only %d names in the executable's list", len(exe))
	}
	for name := range icscommon.IcsStatusCode_value {
		if _, ok := exe[flat(name)]; !ok {
			t.Errorf("proto/ics_common.proto has %s, which the executable's enum does not", name)
		}
	}
	if len(icscommon.IcsStatusCode_value) != len(exe) {
		t.Errorf("%d names in the proto, %d in the executable", len(icscommon.IcsStatusCode_value), len(exe))
	}
}

func TestPlayEndAndShopReplies(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	tr := h.send("ics_xclient.PlayEndRequest", &icsxclient.PlayEndRequest{}, "")
	if m := h.expectEmpty("ics_xclient.PlayEndReply", success, tr); m.GetSystemData().GetTags()["ftag"] != "ics_play_character" {
		t.Errorf("PlayEndReply ftag %q", m.GetSystemData().GetTags()["ftag"])
	}
	tr = h.send("ics_xclient.GetShopHandoffCodeRequest", &icsxclient.GetShopHandoffCodeRequest{}, "")
	h.expectEmpty("ics_xclient.GetShopHandoffCodeReply", success, tr)
}

// ~ C-O-A & Cyb ~

// Requests that do not decode are invalid, and reach no service.
func TestUnreadableRequestsAreInvalid(t *testing.T) {
	chars := twoOfMine()
	h := newHarness(t, chars)
	tr := h.sendRaw("ics_common.SessionRequest", unreadable)
	var rep icscommon.SessionReply
	if h.expect("ics_common.SessionReply", codeInvalidRequest, tr, &rep); rep.GetResultCode() != codeInvalidRequest {
		t.Errorf("SessionReply = %v, want result %v", &rep, codeInvalidRequest)
	}
	if n := h.accounts.callCount(); n != 0 {
		t.Errorf("an unreadable OPEN reached the auth service")
	}
	h.open(goodToken, success)
	calls := chars.callCount()
	for _, c := range []struct{ req, rep string }{
		{"ics_xclient.CreateCharacterRequest", "ics_xclient.CreateCharacterReply"},
		{"ics_xclient.DeleteCharacterRequest", "ics_xclient.DeleteCharacterReply"},
		{"ics_xclient.PlayRequest", "ics_xclient.PlayReply"},
	} {
		tr := h.sendRaw(c.req, unreadable)
		h.expectEmpty(c.rep, codeInvalidRequest, tr)
	}
	if n := chars.callCount(); n != calls {
		t.Errorf("an unreadable request reached the character service (%d calls)", n-calls)
	}
}

// No message type name is longer than 128 bytes: a longer one is refused
// without being echoed.
func TestLongMessageTypeNames(t *testing.T) {
	h := newHarness(t, twoOfMine())
	name128 := "ics_xclient." + strings.Repeat("X", 128-len("ics_xclient."))
	tr := h.sendRaw(name128, nil)
	h.expectEmpty(name128, codeSessionCheckFailed, tr)

	for _, withSession := range []bool{false, true} {
		if withSession {
			h.open(goodToken, success)
		}
		tr := h.sendRaw(name128+"X", nil)
		h.expectEmpty("", codeInvalidRequest, tr)
	}
	tr = h.sendRaw(name128, nil)
	h.expectEmpty(name128, success, tr) // an unhandled type
}

func TestClip(t *testing.T) {
	s64 := strings.Repeat("a", 64)
	for _, tc := range []struct{ in, want string }{
		{"", ""},
		{"short", "short"},
		{s64, s64},
		{s64 + "b", s64 + "...(65 bytes)"},
		{s64 + strings.Repeat("b", 1000), s64 + "...(1064 bytes)"},
		// Cut before a character that would not fit whole.
		{strings.Repeat("a", 63) + "\u00e9" + "z", strings.Repeat("a", 63) + "...(66 bytes)"},
	} {
		if got := clip(tc.in); got != tc.want {
			t.Errorf("clip(%d bytes) = %q, want %q", len(tc.in), got, tc.want)
		}
	}
}

// What the client sends goes into the log cut to 64 bytes. (Character names
// are refused past 64 bytes before they are logged.)
func TestClientStringsAreClippedInTheLog(t *testing.T) {
	logs := captureLog(t)
	h := newHarness(t, twoOfMine())
	long := "ics_xclient." + strings.Repeat("Q", 100)
	tr := h.sendRaw(long, nil)
	h.expect(long, codeSessionCheckFailed, tr, nil)
	h.open(goodToken, success)
	tr = h.sendRaw(long, nil)
	h.expect(long, success, tr, nil)

	if strings.Contains(logs.String(), long) {
		t.Errorf("a client string was logged whole:\n%s", logs.String())
	}
	if n := strings.Count(logs.String(), clip(long)); n != 2 {
		t.Errorf("the clipped type name is logged %d times, want 2:\n%s", n, logs.String())
	}
}

// ~ C_Y_B - CoA ~

// The gRPC metadata keys the client sends are logged, not their values, and
// each is clipped.
func TestMetadataKeys(t *testing.T) {
	long := strings.Repeat("k", 100)
	got := metadataKeys(metadata.Pairs("user-agent", "grpc-c++", long, "v", "authorization", "secret"))
	want := []string{"authorization", clip(long), "user-agent"}
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Errorf("metadataKeys = %q, want %q", got, want)
	}
}

// open sends SessionRequest(OPEN) without waiting for the answer.
func open(h *harness) {
	h.send("ics_common.SessionRequest", &icscommon.SessionRequest{RequestType: icscommon.SessionRequestType_SESSION_REQUEST_TYPE_OPEN}, goodToken)
}

// A service call that fails because the client left is not an error: the
// handler returns the stream's own error at once, and logs no error.
func TestClientLeavingIsNotAnError(t *testing.T) {
	for _, tc := range []struct {
		name string
		hang func(h *harness) <-chan struct{}
		send func(h *harness)
	}{
		{"OPEN", func(h *harness) <-chan struct{} { return h.accounts.start() }, open},
		{"OPEN: List", func(h *harness) <-chan struct{} { return h.chars.start() }, open},
		{"AuthRefresh", func(h *harness) <-chan struct{} { return h.accounts.start() }, func(h *harness) {
			h.send("ics_xclient.AuthRefreshRequest", &icsxclient.AuthRefreshRequest{}, "")
		}},
		{"GetCharacters", func(h *harness) <-chan struct{} { return h.chars.start() }, func(h *harness) {
			h.send("ics_xclient.GetCharactersRequest", &icsxclient.GetCharactersRequest{}, "")
		}},
		{"Create", func(h *harness) <-chan struct{} { return h.chars.start() }, func(h *harness) {
			h.send("ics_xclient.CreateCharacterRequest", &icsxclient.CreateCharacterRequest{CharacterName: "N", CreateCharacterInfoRaw: createInfo(`{}`, 2)}, "")
		}},
		{"Delete", func(h *harness) <-chan struct{} { return h.chars.start() }, func(h *harness) {
			h.send("ics_xclient.DeleteCharacterRequest", &icsxclient.DeleteCharacterRequest{CharacterId: "0000000000000000000000000000000a"}, "")
		}},
		{"Play: Get", func(h *harness) <-chan struct{} { return h.chars.start() }, func(h *harness) { h.play("0000000000000000000000000000000a") }},
		{"Play: Place", func(h *harness) <-chan struct{} { return h.placer.start() }, func(h *harness) { h.play("0000000000000000000000000000000a") }},
		{"Play: Issue", func(h *harness) <-chan struct{} { return h.tickets.start() }, func(h *harness) { h.play("0000000000000000000000000000000a") }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			logs := captureLog(t)
			h := newHarness(t, twoOfMine())
			if !strings.HasPrefix(tc.name, "OPEN") {
				h.open(goodToken, success)
			}
			waiting := tc.hang(h)
			tc.send(h)
			h.hanging(waiting)
			h.cancel()
			if err := h.handlerReturns(); !errors.Is(err, context.Canceled) {
				t.Errorf("handler returned %v, want the stream's context.Canceled", err)
			}
			if strings.Contains(logs.String(), "level=ERROR") {
				t.Errorf("a client that left was logged as an error:\n%s", logs.String())
			}
		})
	}
}

// The client ending its side ends the stream cleanly.
func TestStreamEndsWhenTheClientCloses(t *testing.T) {
	h := newHarness(t, twoOfMine())
	h.open(goodToken, success)
	if err := h.stream.CloseSend(); err != nil {
		t.Fatal(err)
	}
	if _, err := h.stream.Recv(); err != io.EOF {
		t.Errorf("after CloseSend: %v, want a clean end", err)
	}
	if err := h.handlerReturns(); err != nil {
		t.Errorf("handler returned %v, want nil", err)
	}
}
