// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// DB-free tests for the parts of the service the in-memory fakeStore in
// charactergrpc_test.go cannot see: which request field reaches which store
// argument, which store field reaches which response field, and how each
// store error and each ended context is reported by every RPC.
//
// recStore records exactly what the handler handed the store and returns
// values that are distinct from the request's, so a swapped, dropped or
// echoed field changes what the test sees.

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/encoding/prototext"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
)

// recStore is a Store that records its arguments. When err is set every
// method fails with it; otherwise Create, Get and SaveState return ret,
// List returns list and Delete returns only err (nil here).
type recStore struct {
	mu   sync.Mutex
	err  error
	ret  character.Character
	list []character.Character

	gotNew     character.New
	gotID      string
	gotAccount int64
	gotState   character.State
	gotCtx     any // ctx.Value(ctxKey{}) as the last call saw it

	items    []character.Item // what Inventory returns
	gotItems []character.Item // what SaveInventory was given

	xp          []character.Experience // what Experience returns
	gotProgress character.Progress     // what SaveProgress was given

	ranks []character.TreeRanks     // what SkillRanks returns (plan D1, stage 2)
	bar   []character.ActionbarSlot // what ActionBar returns

	profs []character.Profession // what Professions returns (Gathering stage 2)
}

type ctxKey struct{}

var _ Store = (*recStore)(nil)

// ~ Cyb & C-O-A ~

func (r *recStore) Create(ctx context.Context, n character.New) (character.Character, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotNew = n
	return r.ret, r.err
}

func (r *recStore) List(ctx context.Context, accountID int64) ([]character.Character, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotAccount = accountID
	return r.list, r.err
}

func (r *recStore) Get(ctx context.Context, id string) (character.Character, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID = id
	return r.ret, r.err
}

func (r *recStore) SaveState(ctx context.Context, id string, st character.State) (character.Character, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID, r.gotState = id, st
	return r.ret, r.err
}

func (r *recStore) Delete(ctx context.Context, accountID int64, id string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotAccount, r.gotID = accountID, id
	return r.err
}

func (r *recStore) Inventory(ctx context.Context, id string) ([]character.Item, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID = id
	return r.items, r.err
}

func (r *recStore) SaveInventory(ctx context.Context, id string, items []character.Item) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID, r.gotItems = id, items
	return r.err
}

func (r *recStore) Experience(ctx context.Context, id string) ([]character.Experience, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID = id
	return r.xp, r.err
}

func (r *recStore) SaveProgress(ctx context.Context, id string, p character.Progress) (character.Character, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID, r.gotProgress = id, p
	return r.ret, r.err
}

func (r *recStore) SkillRanks(ctx context.Context, id string) ([]character.TreeRanks, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID = id
	return r.ranks, r.err
}

func (r *recStore) Professions(ctx context.Context, id string) ([]character.Profession, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID = id
	return r.profs, r.err
}

func (r *recStore) ActionBar(ctx context.Context, id string) ([]character.ActionbarSlot, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gotCtx = ctx.Value(ctxKey{})
	r.gotID = id
	return r.bar, r.err
}

// C Y B with CoA

func dialRec(t *testing.T, store Store) characterv1.CharacterServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	characterv1.RegisterCharacterServiceServer(srv, New(store, acceptAll{}))
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return characterv1.NewCharacterServiceClient(conn)
}

// Every field has its own value (race 3, gender 5, ...), including the
// position's x/y/z/yaw, so no two fields can be swapped unnoticed. All
// numbers are exact in float32/float64.
func fullCharacter() character.Character {
	return character.Character{
		ID: "0123456789abcdef0123456789abcdef", AccountID: 41, Name: "Zed",
		RaceID: 3, GenderID: 5, ClassID: 6, ArchetypeID: 17747, ClassName: "Bard",
		AppearanceJSON: `{"a": [3], "b": 1}`, CreateInfo: []byte{9, 8, 7}, Level: 13,
		Position:   &character.Position{X: -777664.5, Y: 616578.75, Z: 15952.25, Yaw: 90.5},
		LastRegion: "Riverlands",
		CreatedAt:  time.UnixMilli(1760000000123),
	}
}

func fullProto() *characterv1.Character {
	return &characterv1.Character{
		Id: "0123456789abcdef0123456789abcdef", AccountId: 41, Name: "Zed",
		RaceId: 3, GenderId: 5, ClassId: 6, ArchetypeId: 17747, ClassName: "Bard",
		AppearanceJson: `{"a": [3], "b": 1}`, CreateInfo: []byte{9, 8, 7}, Level: 13,
		Position:    &characterv1.Position{X: -777664.5, Y: 616578.75, Z: 15952.25, Yaw: 90.5},
		LastRegion:  "Riverlands",
		CreatedAtMs: 1760000000123,
	}
}

func wantProtoEqual(t *testing.T, what string, got, want *characterv1.Character) {
	t.Helper()
	if !proto.Equal(got, want) {
		t.Errorf("%s:\n got  %s\n want %s", what, prototext.Format(got), prototext.Format(want))
	}
}

// Each field of CreateCharacterRequest reaches the store's character.New in
// its own field.
func TestCreateCharacterPassesEveryFieldToTheStore(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	c := dialRec(t, st)
	_, err := c.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{
		Id: "fedcba9876543210fedcba9876543210", AccountId: 77, Name: "Ann",
		RaceId: 2, GenderId: 1, ClassId: 4, ArchetypeId: 17749, ClassName: "Fighter",
		AppearanceJson: `{"x":1}`, CreateInfo: []byte{1, 2, 3, 4}, Level: 21,
	})
	if err != nil {
		t.Fatal(err)
	}
	want := character.New{
		ID: "fedcba9876543210fedcba9876543210", AccountID: 77, Name: "Ann",
		RaceID: 2, GenderID: 1, ClassID: 4, ArchetypeID: 17749, ClassName: "Fighter",
		AppearanceJSON: `{"x":1}`, CreateInfo: []byte{1, 2, 3, 4}, Level: 21, MaxCharacters: character.DefaultMaxCharacters,
	}
	if !reflect.DeepEqual(st.gotNew, want) {
		t.Errorf("store got %+v\n want %+v", st.gotNew, want)
	}
}

// Every field of the store's Character reaches the response in its own
// field, for all four RPCs; the response comes from the store's answer, not
// from the request.
func TestEveryRPCReturnsTheStoresCharacterFieldForField(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	c := dialRec(t, st)
	ctx := context.Background()
	id := "fedcba9876543210fedcba9876543210"

	created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: 77, Name: "Ann"})
	if err != nil {
		t.Fatal(err)
	}
	wantProtoEqual(t, "CreateCharacter", created.GetCharacter(), fullProto())

	got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id})
	if err != nil {
		t.Fatal(err)
	}
	wantProtoEqual(t, "GetCharacter", got.GetCharacter(), fullProto())
	if st.gotID != id {
		t.Errorf("GetCharacter passed id %q to the store, want %q", st.gotID, id)
	}

	saved, err := c.SaveCharacterState(ctx, &characterv1.SaveCharacterStateRequest{Id: id})
	if err != nil {
		t.Fatal(err)
	}
	wantProtoEqual(t, "SaveCharacterState", saved.GetCharacter(), fullProto())
}

// ListCharacters passes the account id, maps every character in the store's
// order, and answers an account without characters with an empty list.
func TestListCharactersMapsAllInOrder(t *testing.T) {
	second := fullCharacter()
	second.ID, second.Name, second.Position, second.CreateInfo = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Amy", nil, nil
	third := fullCharacter()
	third.ID, third.Name = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "Bob"
	st := &recStore{list: []character.Character{fullCharacter(), second, third}}
	c := dialRec(t, st)

	got, err := c.ListCharacters(context.Background(), &characterv1.ListCharactersRequest{AccountId: 41})
	if err != nil {
		t.Fatal(err)
	}
	if st.gotAccount != 41 {
		t.Errorf("store got account %d, want 41", st.gotAccount)
	}
	wantSecond := fullProto()
	wantSecond.Id, wantSecond.Name, wantSecond.Position, wantSecond.CreateInfo = second.ID, "Amy", nil, nil
	wantThird := fullProto()
	wantThird.Id, wantThird.Name = third.ID, "Bob"
	want := []*characterv1.Character{fullProto(), wantSecond, wantThird}
	if len(got.Characters) != len(want) {
		t.Fatalf("got %d characters, want %d", len(got.Characters), len(want))
	}
	for i := range want {
		wantProtoEqual(t, fmt.Sprintf("characters[%d]", i), got.Characters[i], want[i])
	}
}

// C_Y_B / C-O-A

func TestListCharactersForAnAccountWithNoneIsEmptyNotAnError(t *testing.T) {
	c := dialRec(t, &recStore{})
	got, err := c.ListCharacters(context.Background(), &characterv1.ListCharactersRequest{AccountId: 9})
	if err != nil || len(got.GetCharacters()) != 0 {
		t.Errorf("ListCharacters = %v, %v; want an empty list and no error", got, err)
	}
}

// SaveCharacterStateRequest's id, position (x/y/z/yaw), region and level
// reach the store, and an unset position stays nil: it is what tells the
// store to keep the stored one. A position at the origin is a position.
func TestSaveCharacterStateMapsTheRequest(t *testing.T) {
	const id = "0123456789abcdef0123456789abcdef"
	for _, tc := range []struct {
		name string
		req  *characterv1.SaveCharacterStateRequest
		want character.State
	}{
		{"everything",
			&characterv1.SaveCharacterStateRequest{Id: id, LastRegion: "Riverlands", Level: 9,
				Position: &characterv1.Position{X: 1.5, Y: -2.25, Z: 3.125, Yaw: 4.5}},
			character.State{LastRegion: "Riverlands", Level: 9,
				Position: &character.Position{X: 1.5, Y: -2.25, Z: 3.125, Yaw: 4.5}}},
		{"no position stays nil",
			&characterv1.SaveCharacterStateRequest{Id: id, LastRegion: "B", Level: 2},
			character.State{LastRegion: "B", Level: 2}},
		{"position at the origin is not unset",
			&characterv1.SaveCharacterStateRequest{Id: id, Position: &characterv1.Position{}},
			character.State{Position: &character.Position{}}},
		{"only x set",
			&characterv1.SaveCharacterStateRequest{Id: id, Position: &characterv1.Position{X: 7}},
			character.State{Position: &character.Position{X: 7}}},
		{"only yaw set",
			&characterv1.SaveCharacterStateRequest{Id: id, Position: &characterv1.Position{Yaw: 7.5}},
			character.State{Position: &character.Position{Yaw: 7.5}}},
		{"only the id", &characterv1.SaveCharacterStateRequest{Id: id}, character.State{}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			st := &recStore{ret: fullCharacter()}
			c := dialRec(t, st)
			if _, err := c.SaveCharacterState(context.Background(), tc.req); err != nil {
				t.Fatal(err)
			}
			if st.gotID != id {
				t.Errorf("store got id %q, want %q", st.gotID, id)
			}
			if !reflect.DeepEqual(st.gotState, tc.want) {
				t.Errorf("store got %+v (position %+v), want %+v (position %+v)",
					st.gotState, st.gotState.Position, tc.want, tc.want.Position)
			}
		})
	}
}

// A character with no saved position has no position in the response, and
// one saved at the origin has one: the two must stay apart on the wire.
func TestResponsePositionPresence(t *testing.T) {
	for _, tc := range []struct {
		name    string
		pos     *character.Position
		wantSet bool
	}{
		{"never saved", nil, false},
		{"saved at the origin", &character.Position{}, true},
		{"saved elsewhere", &character.Position{X: 1}, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ch := fullCharacter()
			ch.Position = tc.pos
			c := dialRec(t, &recStore{ret: ch})
			got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: ch.ID})
			if err != nil {
				t.Fatal(err)
			}
			if set := got.GetCharacter().GetPosition() != nil; set != tc.wantSet {
				t.Errorf("position set = %v, want %v (%s)", set, tc.wantSet, prototext.Format(got.GetCharacter()))
			}
		})
	}
}

// ~ C.O.A - Cyb ~

// Every RPC, called on the handler directly so the test controls ctx.
var rpcs = []struct {
	name string
	call func(s *Server, ctx context.Context) error
}{
	{"CreateCharacter", func(s *Server, ctx context.Context) error {
		_, err := s.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{})
		return err
	}},
	{"ListCharacters", func(s *Server, ctx context.Context) error {
		_, err := s.ListCharacters(ctx, &characterv1.ListCharactersRequest{})
		return err
	}},
	{"GetCharacter", func(s *Server, ctx context.Context) error {
		_, err := s.GetCharacter(ctx, &characterv1.GetCharacterRequest{})
		return err
	}},
	{"SaveCharacterState", func(s *Server, ctx context.Context) error {
		_, err := s.SaveCharacterState(ctx, &characterv1.SaveCharacterStateRequest{})
		return err
	}},
	{"DeleteCharacter", func(s *Server, ctx context.Context) error {
		_, err := s.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{})
		return err
	}},
	{"SaveInventory", func(s *Server, ctx context.Context) error {
		_, err := s.SaveInventory(ctx, &characterv1.SaveInventoryRequest{})
		return err
	}},
}

func init() {
	// GetCharacter with its items asks the store for them too: the store's error there is every RPC's
	// error, whichever of the two calls returns it.
	rpcs = append(rpcs, struct {
		name string
		call func(s *Server, ctx context.Context) error
	}{"GetCharacter with items", func(s *Server, ctx context.Context) error {
		_, err := s.GetCharacter(ctx, &characterv1.GetCharacterRequest{IncludeItems: true})
		return err
	}})
}

// The RPC's context is the one the store runs under, so a caller that gives
// up also stops the database call.
func TestEveryRPCPassesItsContextToTheStore(t *testing.T) {
	for _, rpc := range rpcs {
		st := &recStore{}
		ctx := context.WithValue(context.Background(), ctxKey{}, "from the caller")
		_ = rpc.call(New(st, acceptAll{}), ctx)
		if st.gotCtx != "from the caller" {
			t.Errorf("%s: the store ran under a different context (saw %v)", rpc.name, st.gotCtx)
		}
	}
}

// Every store error maps to the same code in every RPC, whether the store
// returns it bare or wrapped, and the character errors keep their text (the
// gateway shows it); anything else is an Internal without the store's text.
func TestEveryRPCMapsEveryStoreError(t *testing.T) {
	invalid := fmt.Errorf("%w: name must be 3-20 letters or digits, starting with a letter", character.ErrInvalid)
	for _, tc := range []struct {
		name     string
		err      error
		code     codes.Code
		wantText string // the message must contain it
	}{
		{"name taken", character.ErrNameTaken, codes.AlreadyExists, "name taken"},
		{"character limit", character.ErrCharacterLimit, codes.ResourceExhausted, "character count limit"},
		{"id taken", character.ErrIDTaken, codes.AlreadyExists, "id taken"},
		{"not found", character.ErrNotFound, codes.NotFound, "not found"},
		{"invalid", invalid, codes.InvalidArgument, "name must be 3-20"},
		{"wrapped name taken", fmt.Errorf("create: %w", character.ErrNameTaken), codes.AlreadyExists, "name taken"},
		{"wrapped id taken", fmt.Errorf("create: %w", character.ErrIDTaken), codes.AlreadyExists, "id taken"},
		{"wrapped not found", fmt.Errorf("get: %w", character.ErrNotFound), codes.NotFound, "not found"},
		{"wrapped invalid", fmt.Errorf("create: %w", invalid), codes.InvalidArgument, "name must be 3-20"},
		{"other", errors.New("pq: secret detail"), codes.Internal, ""},
		{"store timeout while the caller waits",
			fmt.Errorf("failed to connect to host=secret detail: %w", context.DeadlineExceeded), codes.Internal, ""},
	} {
		for _, rpc := range rpcs {
			t.Run(tc.name+"/"+rpc.name, func(t *testing.T) {
				err := rpc.call(New(&recStore{err: tc.err}, acceptAll{}), context.Background())
				got := status.Convert(err)
				if got.Code() != tc.code {
					t.Fatalf("code = %v (%v), want %v", got.Code(), err, tc.code)
				}
				if !strings.Contains(got.Message(), tc.wantText) {
					t.Errorf("message %q does not contain %q", got.Message(), tc.wantText)
				}
				if tc.code == codes.Internal && (got.Message() == "" || strings.Contains(got.Message(), "secret detail")) {
					t.Errorf("Internal message %q must be non-empty and free of the store's text", got.Message())
				}
			})
		}
	}
}

// A caller whose own context has ended gets that context's code from every
// RPC, whatever the store returned, and never the store's text.
func TestEveryRPCReportsAnEndedCallerContext(t *testing.T) {
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	expired, cancelExpired := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancelExpired()
	for _, tc := range []struct {
		name string
		ctx  context.Context
		code codes.Code
	}{
		{"canceled", canceled, codes.Canceled},
		{"deadline exceeded", expired, codes.DeadlineExceeded},
	} {
		for _, storeErr := range []error{
			errors.New("pq: secret detail"),
			fmt.Errorf("query secret detail: %w", context.Canceled),
			fmt.Errorf("query secret detail: %w", context.DeadlineExceeded),
		} {
			for _, rpc := range rpcs {
				err := rpc.call(New(&recStore{err: storeErr}, acceptAll{}), tc.ctx)
				got := status.Convert(err)
				if got.Code() != tc.code || strings.Contains(got.Message(), "secret detail") {
					t.Errorf("%s, %s, store error %q: %v; want %v without the store's text",
						tc.name, rpc.name, storeErr, err, tc.code)
				}
			}
		}
	}
}

// CoA + C Y B

// When the store answers with one of the character errors, that answer is
// about the caller's input and is kept even if the caller has since gone.
// (Pins the order of the checks in toStatus; drop this test if the order is
// changed on purpose.)
func TestCharacterErrorBeatsAnEndedContext(t *testing.T) {
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	for _, rpc := range rpcs {
		err := rpc.call(New(&recStore{err: character.ErrNotFound}, acceptAll{}), canceled)
		if status.Code(err) != codes.NotFound {
			t.Errorf("%s: %v, want NotFound", rpc.name, err)
		}
	}
}

// The detail that Internal hides from the caller is what the operator gets,
// with the RPC it came from. It goes through a client, because only a served
// call has a method in its context. It swaps the process-wide slog default, so
// no test in this package may run in parallel with it.
func TestInternalFailureIsLogged(t *testing.T) {
	var buf bytes.Buffer
	prev := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, nil)))
	t.Cleanup(func() { slog.SetDefault(prev) })

	c := dialRec(t, &recStore{err: errors.New("pq: secret detail")})
	ctx := context.Background()
	for _, tc := range []struct {
		method string
		call   func() error
	}{
		{"CreateCharacter", func() error {
			_, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{})
			return err
		}},
		{"ListCharacters", func() error {
			_, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{})
			return err
		}},
		{"GetCharacter", func() error {
			_, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{})
			return err
		}},
		{"SaveCharacterState", func() error {
			_, err := c.SaveCharacterState(ctx, &characterv1.SaveCharacterStateRequest{})
			return err
		}},
		{"DeleteCharacter", func() error {
			_, err := c.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{})
			return err
		}},
	} {
		buf.Reset()
		if err := tc.call(); status.Code(err) != codes.Internal {
			t.Fatalf("%s: got %v, want Internal", tc.method, err)
		}
		logged := buf.String()
		if !strings.Contains(logged, "secret detail") {
			t.Errorf("%s: the store's error was not logged; log output: %q", tc.method, logged)
		}
		if want := "/aoc.character.v1.CharacterService/" + tc.method; !strings.Contains(logged, want) {
			t.Errorf("%s: the log does not name the method %q; log output: %q", tc.method, want, logged)
		}
	}
}

func TestDeleteCharacterPassesTheOwnerAndID(t *testing.T) {
	st := &recStore{}
	if _, err := dialRec(t, st).DeleteCharacter(context.Background(),
		&characterv1.DeleteCharacterRequest{Id: "0123456789abcdef0123456789abcdef", AccountId: 41}); err != nil {
		t.Fatal(err)
	}
	if st.gotAccount != 41 || st.gotID != "0123456789abcdef0123456789abcdef" {
		t.Errorf("store got account %d id %q", st.gotAccount, st.gotID)
	}
}

// The zero time is "no creation time", not the year 1.
func TestZeroCreationTimeIsZeroMilliseconds(t *testing.T) {
	c := fullCharacter()
	c.CreatedAt = time.Time{}
	st := &recStore{ret: c}
	resp, err := dialRec(t, st).GetCharacter(context.Background(),
		&characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef"})
	if err != nil || resp.GetCharacter().GetCreatedAtMs() != 0 {
		t.Errorf("created_at_ms = %d (%v), want 0", resp.GetCharacter().GetCreatedAtMs(), err)
	}
}
