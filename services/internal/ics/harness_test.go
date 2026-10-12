// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"bytes"
	"context"
	"fmt"
	"log"
	"log/slog"
	"net"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
)

const (
	goodToken = "aoc_good"
	worldID   = "aoc-release-useast2-local"
	// clientFtag is the ftag the harness sends: a reply that carries it has no
	// ftag of its own, and one with a server ftag shows it.
	clientFtag = "client_ftag"
)

// me is the account behind goodToken.
var me = Account{ID: 7, Username: "cyb_account"}

// hang makes a fake's calls wait for the caller's context to end, as a service
// that stopped answering does, and tells the test each time a call starts
// waiting.
type hang struct {
	mu      sync.Mutex
	waiting chan struct{} // nil: calls do not wait
}

// start makes the following calls wait, and returns the channel that gets a
// value as each one starts waiting.
func (h *hang) start() <-chan struct{} {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.waiting = make(chan struct{}, 16)
	return h.waiting
}

// wait returns nil at once, or when the fake is hanging, the gRPC error of
// ctx once it ends.
func (h *hang) wait(ctx context.Context) error {
	h.mu.Lock()
	waiting := h.waiting
	h.mu.Unlock()
	if waiting == nil {
		return nil
	}
	waiting <- struct{}{}
	<-ctx.Done()
	return status.FromContextError(ctx.Err()).Err()
}

// -- C:Y:B x C-O-A --

// fakeAccounts knows the tokens in tokens; err, when set, is every lookup's
// answer.
type fakeAccounts struct {
	hang
	mu     sync.Mutex
	tokens map[string]Account
	err    error
	calls  int
}

func newAccounts() *fakeAccounts {
	return &fakeAccounts{tokens: map[string]Account{goodToken: me}}
}

func (f *fakeAccounts) ValidateToken(ctx context.Context, token string) (Account, error) {
	if err := f.wait(ctx); err != nil {
		return Account{}, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.err != nil {
		return Account{}, f.err
	}
	if a, ok := f.tokens[token]; ok {
		return a, nil
	}
	return Account{}, status.Error(codes.Unauthenticated, "invalid or expired token")
}

// fail makes every lookup answer err (nil: back to the tokens).
func (f *fakeAccounts) fail(err error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.err = err
}

// forget makes token unknown, as when it expires.
func (f *fakeAccounts) forget(token string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	delete(f.tokens, token)
}

func (f *fakeAccounts) callCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

// fakeCharacters keeps characters in memory and lists them in id order.
type fakeCharacters struct {
	hang
	mu      sync.Mutex
	byID    map[string]*characterv1.Character
	created *characterv1.CreateCharacterRequest // the last Create request
	err     error                               // when set, every method fails with it
	calls   int                                 // calls of any method
	made    int                                 // characters created, for their ids
	via     Characters                          // when set, List, Get and Create go to it (an end-to-end test's real service)
}

func newCharacters(cs ...*characterv1.Character) *fakeCharacters {
	f := &fakeCharacters{byID: map[string]*characterv1.Character{}}
	for _, c := range cs {
		f.byID[c.GetId()] = c
	}
	return f
}

func (f *fakeCharacters) List(ctx context.Context, accountID int64) ([]*characterv1.Character, error) {
	if err := f.wait(ctx); err != nil {
		return nil, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.via != nil {
		return f.via.List(ctx, accountID)
	}
	if f.err != nil {
		return nil, f.err
	}
	var out []*characterv1.Character
	for _, c := range f.byID {
		if c.GetAccountId() == accountID {
			out = append(out, c)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].GetId() < out[j].GetId() })
	return out, nil
}

func (f *fakeCharacters) Get(ctx context.Context, id string) (*characterv1.Character, error) {
	if err := f.wait(ctx); err != nil {
		return nil, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.via != nil {
		return f.via.Get(ctx, id)
	}
	if f.err != nil {
		return nil, f.err
	}
	if c, ok := f.byID[id]; ok {
		return c, nil
	}
	return nil, status.Error(codes.NotFound, "character not found")
}

func (f *fakeCharacters) Create(ctx context.Context, req *characterv1.CreateCharacterRequest) (*characterv1.Character, error) {
	if err := f.wait(ctx); err != nil {
		return nil, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	f.created = req
	if f.via != nil {
		return f.via.Create(ctx, req)
	}
	if f.err != nil {
		return nil, f.err
	}
	for _, c := range f.byID {
		if strings.EqualFold(c.GetName(), req.GetName()) {
			return nil, status.Error(codes.AlreadyExists, "character name taken")
		}
	}
	f.made++
	c := &characterv1.Character{
		Id: fmt.Sprintf("%032x", 0x1000+f.made), AccountId: req.GetAccountId(), Name: req.GetName(),
		RaceId: req.GetRaceId(), GenderId: req.GetGenderId(), ClassId: req.GetClassId(), ArchetypeId: req.GetArchetypeId(),
		ClassName: req.GetClassName(), AppearanceJson: req.GetAppearanceJson(), CreateInfo: req.GetCreateInfo(),
		Level: req.GetLevel(), CreatedAtMs: 1760000000123,
	}
	f.byID[c.GetId()] = c
	return c, nil
}

func (f *fakeCharacters) Delete(ctx context.Context, accountID int64, id string) error {
	if err := f.wait(ctx); err != nil {
		return err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.err != nil {
		return f.err
	}
	c, ok := f.byID[id]
	if !ok || c.GetAccountId() != accountID {
		return status.Error(codes.NotFound, "character not found")
	}
	delete(f.byID, id)
	return nil
}

func (f *fakeCharacters) has(id string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	_, ok := f.byID[id]
	return ok
}

// ~ Cyb & C O A ~

// fail makes every method fail with err (nil: back to normal).
func (f *fakeCharacters) fail(err error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.err = err
}

func (f *fakeCharacters) callCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

type fakePlacer struct {
	hang
	mu       sync.Mutex
	serverID uint32
	address  string
	err      error
	got      []string // the characters placed
}

func (f *fakePlacer) Place(ctx context.Context, characterID string) (uint32, string, error) {
	if err := f.wait(ctx); err != nil {
		return 0, "", err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.got = append(f.got, characterID)
	return f.serverID, f.address, f.err
}

func (f *fakePlacer) placed() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.got...)
}

type issued struct {
	accountID   int64
	characterID string
	serverID    uint32
}

type fakeTickets struct {
	hang
	mu     sync.Mutex
	ticket string
	err    error
	got    []issued
}

func (f *fakeTickets) Issue(ctx context.Context, accountID int64, characterID string, serverID uint32) (string, error) {
	if err := f.wait(ctx); err != nil {
		return "", err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.got = append(f.got, issued{accountID, characterID, serverID})
	return f.ticket, f.err
}

func (f *fakeTickets) issued() []issued {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]issued(nil), f.got...)
}

// reporting is the Server with a channel that gets what its handler returns.
type reporting struct {
	*Server
	returned chan error
}

func (r reporting) ProcessXClientMessage(s icsxclient.XClientService_ProcessXClientMessageServer) error {
	err := r.Server.ProcessXClientMessage(s)
	r.returned <- err
	return err
}

// harness is one client stream to a Server backed by the fakes.
type harness struct {
	t        *testing.T
	stream   grpc.BidiStreamingClient[icscommon.MessageWrapper, icscommon.MessageWrapper]
	accounts *fakeAccounts
	chars    *fakeCharacters
	placer   *fakePlacer
	tickets  *fakeTickets
	cancel   context.CancelFunc // ends the stream, as a client that goes away does
	returned chan error         // what the server's handler returned
	seq      int
}

// Cyb and C.O.A

// newHarness serves a client stream with DefaultConfig, except that Play is
// answered without pauses. The placer puts every character on server 5.
func newHarness(t *testing.T, chars *fakeCharacters) *harness {
	t.Helper()
	return newHarnessWith(t, chars, nil)
}

// newHarnessWith is newHarness with the config changed by adjust.
func newHarnessWith(t *testing.T, chars *fakeCharacters, adjust func(*Config)) *harness {
	t.Helper()
	h := &harness{
		t:        t,
		accounts: newAccounts(),
		chars:    chars,
		placer:   &fakePlacer{serverID: 5, address: "192.0.2.5:7778"},
		tickets:  &fakeTickets{ticket: "7bc658710f9d3f333441c089d974dece"},
		returned: make(chan error, 1),
	}
	cfg := DefaultConfig()
	cfg.PlayPause = 0
	if adjust != nil {
		adjust(&cfg)
	}
	srv := New(Deps{Accounts: h.accounts, Characters: chars, Placer: h.placer, Tickets: h.tickets}, cfg)

	lis := bufconn.Listen(1 << 20)
	gs := grpc.NewServer()
	icsxclient.RegisterXClientServiceServer(gs, reporting{srv, h.returned})
	go gs.Serve(lis)
	t.Cleanup(gs.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second) // bounds every Recv
	t.Cleanup(cancel)
	h.cancel = cancel
	h.stream, err = icsxclient.NewXClientServiceClient(conn).ProcessXClientMessage(ctx)
	if err != nil {
		t.Fatal(err)
	}
	return h
}

// send writes a request as the client does, tagged with a tracking id the
// reply must echo (returned), the harness's ftag and, when token is not
// empty, the auth token.
func (h *harness) send(typeName string, body proto.Message, token string) string {
	h.t.Helper()
	data, err := proto.Marshal(body)
	if err != nil {
		h.t.Fatal(err)
	}
	return h.sendWith(typeName, data, token)
}

// sendRaw sends message_data as given (e.g. bytes that do not decode).
func (h *harness) sendRaw(typeName string, data []byte) string {
	h.t.Helper()
	return h.sendWith(typeName, data, "")
}

func (h *harness) sendWith(typeName string, data []byte, token string) string {
	h.t.Helper()
	h.seq++
	tracking := fmt.Sprintf("t%d", h.seq)
	tags := map[string]string{"client_tracking_id": tracking, "client_id": "ue_app_test", "ftag": clientFtag}
	if token != "" {
		tags["auth_token"] = token
	}
	if err := h.stream.Send(&icscommon.MessageWrapper{
		SystemData: &icscommon.SystemData{Tags: tags}, MessageTypeName: typeName, MessageData: data,
	}); err != nil {
		h.t.Fatalf("send %s: %v", typeName, err)
	}
	return tracking
}

// expect reads the next message and checks its type, status and echoed
// tracking id ("" for a push, which echoes nothing). When body is not nil the
// message data is decoded into it.
func (h *harness) expect(typeName string, code icscommon.IcsStatusCode, tracking string, body proto.Message) *icscommon.MessageWrapper {
	h.t.Helper()
	m, err := h.stream.Recv()
	if err != nil {
		h.t.Fatalf("waiting for %s: %v", typeName, err)
	}
	if m.GetMessageTypeName() != typeName || m.GetStatusCode() != code {
		h.t.Fatalf("got %q status %v, want %q status %v", m.GetMessageTypeName(), m.GetStatusCode(), typeName, code)
	}
	// Every status any test reads is one the client knows (0-3, or an error of 10-127).
	if !validStatus(m.GetStatusCode()) {
		h.t.Errorf("%s: status %d is outside 0-3 and 10-127", typeName, int32(m.GetStatusCode()))
	}
	if got := m.GetSystemData().GetTags()["client_tracking_id"]; got != tracking {
		h.t.Errorf("%s: client_tracking_id %q, want %q", typeName, got, tracking)
	}
	if body != nil {
		if err := proto.Unmarshal(m.GetMessageData(), body); err != nil {
			h.t.Fatalf("%s: %v", typeName, err)
		}
	}
	return m
}

// expectEmpty is expect for a reply that must carry no body.
func (h *harness) expectEmpty(typeName string, code icscommon.IcsStatusCode, tracking string) *icscommon.MessageWrapper {
	h.t.Helper()
	m := h.expect(typeName, code, tracking, nil)
	if len(m.GetMessageData()) != 0 {
		h.t.Errorf("%s status %v carries %d bytes, want none", typeName, code, len(m.GetMessageData()))
	}
	return m
}

// nothingElse checks that nothing more was written for the last request: the
// next message is the answer to a keep-alive sent now.
func (h *harness) nothingElse() {
	h.t.Helper()
	tr := h.send("ics_common.KeepAliveMessage", &icscommon.KeepAliveMessage{}, "")
	h.expect("ics_common.KeepAliveMessage", success, tr, nil)
}

// open sends SessionRequest(OPEN) with token and returns the reply wrapper,
// not decoded: callers decode what they check.
func (h *harness) open(token string, code icscommon.IcsStatusCode) *icscommon.MessageWrapper {
	h.t.Helper()
	tr := h.send("ics_common.SessionRequest",
		&icscommon.SessionRequest{RequestType: icscommon.SessionRequestType_SESSION_REQUEST_TYPE_OPEN}, token)
	return h.expect("ics_common.SessionReply", code, tr, nil)
}

// (C_Y_B ~ CoA)

// hanging waits up to 2 s for a call of a fake to start hanging (waiting is
// what the fake's start returned).
func (h *harness) hanging(waiting <-chan struct{}) {
	h.t.Helper()
	select {
	case <-waiting:
	case <-time.After(2 * time.Second):
		h.t.Fatal("no service call is hanging 2 s after the request")
	}
}

// handlerReturns waits up to 2 s for the server's handler to return, and
// returns what it returned.
func (h *harness) handlerReturns() error {
	h.t.Helper()
	select {
	case err := <-h.returned:
		return err
	case <-time.After(2 * time.Second):
		h.t.Fatal("the handler is still running 2 s after the client left")
		return nil
	}
}

// twoOfMine is two characters of me and one of another account.
func twoOfMine() *fakeCharacters {
	return newCharacters(
		&characterv1.Character{Id: "0000000000000000000000000000000a", AccountId: 7, Name: "cyb", Level: 1},
		&characterv1.Character{Id: "0000000000000000000000000000000b", AccountId: 7, Name: "Zed", Level: 3},
		&characterv1.Character{Id: "0000000000000000000000000000000c", AccountId: 8, Name: "NotMine", Level: 1},
	)
}

// syncBuffer is a bytes.Buffer the handler goroutine and the test share.
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

// captureLog sends slog's output to the returned buffer until the test ends,
// then restores the default logger and the log package's output and flags,
// which slog.SetDefault also changes.
func captureLog(t *testing.T) *syncBuffer {
	t.Helper()
	buf := &syncBuffer{}
	prev, out, flags := slog.Default(), log.Writer(), log.Flags()
	slog.SetDefault(slog.New(slog.NewTextHandler(buf, nil)))
	t.Cleanup(func() {
		slog.SetDefault(prev)
		log.SetOutput(out)
		log.SetFlags(flags)
	})
	return buf
}
