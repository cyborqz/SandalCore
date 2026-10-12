// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package meshgrpc

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log"
	"log/slog"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"

	"google.golang.org/genproto/googleapis/rpc/errdetails"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/proto"

	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	"aocrop/services/internal/mesh"
)

// fakeStore records what it was asked; err, when set, is every method's error.
type fakeStore struct {
	err        error
	ttl        time.Duration // 15 s when zero
	gotAddress string
	gotRegions []string
	gotID      uint32
	gotChar    string
}

func (f *fakeStore) Register(_ context.Context, address string, regions []string) (uint32, error) {
	f.gotAddress, f.gotRegions = address, regions
	return 7, f.err
}

// == C.O.A | C-Y-B ==

func (f *fakeStore) Heartbeat(_ context.Context, id uint32) error {
	f.gotID = id
	return f.err
}

func (f *fakeStore) Place(_ context.Context, characterID string) (uint32, string, error) {
	f.gotChar = characterID
	return 7, "192.0.2.5:7001", f.err
}

func (f *fakeStore) HeartbeatTTL() time.Duration {
	if f.ttl != 0 {
		return f.ttl
	}
	return 15 * time.Second
}

func client(t *testing.T, store Store) meshv1.MeshServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	meshv1.RegisterMeshServiceServer(srv, New(store))
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return meshv1.NewMeshServiceClient(conn)
}

func TestRegisterHeartbeatPlace(t *testing.T) {
	st := &fakeStore{}
	c := client(t, st)
	ctx := context.Background()

	reg, err := c.Register(ctx, &meshv1.RegisterRequest{Address: "192.0.2.5:7001", Regions: []string{"verra"}})
	if err != nil || reg.GetServerId() != 7 || reg.GetHeartbeatIntervalMs() != 5000 {
		t.Fatalf("Register = %v %v, want server 7 and a 5000 ms interval (a third of the TTL)", reg, err)
	}
	if st.gotAddress != "192.0.2.5:7001" || !reflect.DeepEqual(st.gotRegions, []string{"verra"}) {
		t.Errorf("store got %q %q", st.gotAddress, st.gotRegions)
	}
	if _, err := c.Heartbeat(ctx, &meshv1.HeartbeatRequest{ServerId: 7}); err != nil || st.gotID != 7 {
		t.Errorf("Heartbeat: %v, store got %d", err, st.gotID)
	}
	pl, err := c.Place(ctx, &meshv1.PlaceRequest{CharacterId: "0123456789abcdef0123456789abcdef"})
	if err != nil || pl.GetServerId() != 7 || pl.GetAddress() != "192.0.2.5:7001" ||
		st.gotChar != "0123456789abcdef0123456789abcdef" {
		t.Errorf("Place = %v %v, store got %q", pl, err, st.gotChar)
	}
}

func TestStoreErrorsMapToCodes(t *testing.T) {
	ctx := context.Background()
	for _, tc := range []struct {
		err  error
		want codes.Code
	}{
		{mesh.ErrInvalid, codes.InvalidArgument},
		{mesh.ErrUnknownServer, codes.NotFound},
		{mesh.ErrNoServer, codes.Unavailable},
		{errors.New("pq: secret detail"), codes.Internal},
	} {
		c := client(t, &fakeStore{err: tc.err})
		_, errR := c.Register(ctx, &meshv1.RegisterRequest{Address: "x:1"})
		_, errH := c.Heartbeat(ctx, &meshv1.HeartbeatRequest{ServerId: 1})
		_, errP := c.Place(ctx, &meshv1.PlaceRequest{})
		for name, err := range map[string]error{"Register": errR, "Heartbeat": errH, "Place": errP} {
			if status.Code(err) != tc.want || strings.Contains(status.Convert(err).Message(), "secret detail") {
				t.Errorf("%s with store error %q: %v, want %v without the store's text", name, tc.err, err, tc.want)
			}
		}
	}
}

// The caller's own ended context keeps its code. Over a real connection the
// client gives up first, so this calls the handler directly.
func TestCallerCancellationKeepsItsCode(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := New(&fakeStore{err: context.Canceled}).Place(ctx, &meshv1.PlaceRequest{})
	if status.Code(err) != codes.Canceled {
		t.Errorf("Place = %v, want Canceled", err)
	}
}

// [C | O | A x C | Y | B]

// A database timeout wraps DeadlineExceeded, but the RPC's own context is
// alive: that is the server's failure (Internal, no database text), not the
// caller's deadline.
func TestStoreTimeoutWhileCallerIsLiveIsInternal(t *testing.T) {
	c := client(t, &fakeStore{err: fmt.Errorf("query world_servers: %w", context.DeadlineExceeded)})
	_, err := c.Place(context.Background(), &meshv1.PlaceRequest{})
	if status.Code(err) != codes.Internal || strings.Contains(status.Convert(err).Message(), "world_servers") {
		t.Errorf("Place = %v, want Internal without the database's text", err)
	}
}

// The caller's expired deadline keeps its own code (and is not Canceled).
func TestCallerDeadlineKeepsItsCode(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Nanosecond)
	defer cancel()
	<-ctx.Done()
	_, err := New(&fakeStore{err: context.DeadlineExceeded}).Heartbeat(ctx, &meshv1.HeartbeatRequest{ServerId: 1})
	if status.Code(err) != codes.DeadlineExceeded {
		t.Errorf("Heartbeat = %v, want DeadlineExceeded", err)
	}
}

// The interval is a third of the TTL but never 0, which would have worldd
// heartbeating in a busy loop.
func TestHeartbeatIntervalHasAFloor(t *testing.T) {
	c := client(t, &fakeStore{ttl: 2 * time.Millisecond})
	reg, err := c.Register(context.Background(), &meshv1.RegisterRequest{Address: "192.0.2.5:7001"})
	if err != nil || reg.GetHeartbeatIntervalMs() < 1 {
		t.Errorf("Register with a 2 ms TTL = %v %v, want an interval of at least 1 ms", reg, err)
	}
}

// The two answers an operator has to notice in the logs: a world server the
// registry has forgotten, and no world server to place on.
func TestOperatorWarnings(t *testing.T) {
	var buf bytes.Buffer
	// slog.SetDefault also points the log package at the new handler, and
	// setting the old default back does not undo that.
	prev, out, flags := slog.Default(), log.Writer(), log.Flags()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, nil)))
	t.Cleanup(func() {
		slog.SetDefault(prev)
		log.SetOutput(out)
		log.SetFlags(flags)
	})
	ctx := context.Background()

	_, err := New(&fakeStore{err: mesh.ErrUnknownServer}).Heartbeat(ctx, &meshv1.HeartbeatRequest{ServerId: 9})
	if status.Code(err) != codes.NotFound {
		t.Errorf("Heartbeat = %v, want NotFound", err)
	}
	for _, want := range []string{"level=WARN", "heartbeat from an unregistered server", "server_id=9"} {
		if !strings.Contains(buf.String(), want) {
			t.Errorf("log after an unknown heartbeat is %q, want it to contain %q", buf.String(), want)
		}
	}

	buf.Reset()
	_, err = New(&fakeStore{err: mesh.ErrNoServer}).Place(ctx, &meshv1.PlaceRequest{})
	if status.Code(err) != codes.Unavailable {
		t.Errorf("Place = %v, want Unavailable", err)
	}
	for _, want := range []string{"level=WARN", "no live world server to place on"} {
		if !strings.Contains(buf.String(), want) {
			t.Errorf("log after an unplaceable character is %q, want it to contain %q", buf.String(), want)
		}
	}
}

// (Cyb | C.O.A)

// grpc-go also answers Unavailable when mesh is not listening: Place's own
// "no world server" carries an ErrorInfo, which is how a client tells them
// apart.
func TestNoWorldServerCarriesItsReason(t *testing.T) {
	ctx := context.Background()
	_, err := client(t, &fakeStore{err: mesh.ErrNoServer}).Place(ctx, &meshv1.PlaceRequest{})
	st := status.Convert(err)
	want := &errdetails.ErrorInfo{Reason: "NO_WORLD_SERVER", Domain: "mesh.aoc"}
	if st.Code() != codes.Unavailable || len(st.Details()) != 1 || !proto.Equal(st.Details()[0].(proto.Message), want) {
		t.Errorf("Place with no world server = %v, details %v; want Unavailable with %v", err, st.Details(), want)
	}
	if !IsNoWorldServer(err) {
		t.Errorf("IsNoWorldServer(%v) = false", err)
	}

	for _, e := range []error{mesh.ErrUnknownServer, errors.New("database down")} {
		_, err := client(t, &fakeStore{err: e}).Place(ctx, &meshv1.PlaceRequest{})
		if IsNoWorldServer(err) || len(status.Convert(err).Details()) != 0 {
			t.Errorf("Place with store error %q = %v, details %v; want no ErrorInfo", e, err, status.Convert(err).Details())
		}
	}
	info := func(reason, domain string) error {
		st, err := status.New(codes.Unavailable, "x").WithDetails(&errdetails.ErrorInfo{Reason: reason, Domain: domain})
		if err != nil {
			t.Fatal(err)
		}
		return st.Err()
	}
	internal, err := status.New(codes.Internal, "x").WithDetails(want)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range []error{
		nil, errors.New("no live world server"), status.Error(codes.Unavailable, "no live world server"),
		info("NO_WORLD_SERVER", "other.domain"), info("OTHER", "mesh.aoc"), internal.Err(),
	} {
		if IsNoWorldServer(e) {
			t.Errorf("IsNoWorldServer(%v) = true", e)
		}
	}
}
