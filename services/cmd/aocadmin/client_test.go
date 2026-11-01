// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/svcauth"
)

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// otherToken is a token no server of the stack wants.
const otherToken = svcauth.Token("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210")

// A missing token file is an error: without the token the server refuses every
// call, so the dashboard says so instead of failing later.
func TestClientNeedsAToken(t *testing.T) {
	c := NewClient(Options{TokenFile: "no-such-token.txt"})
	if err := c.Dial(context.Background()); err == nil {
		t.Fatal("a missing token file must be an error")
	}
}

// The addresses have defaults, so the dashboard works with the stock stack.
func TestClientHasTheDefaultAddresses(t *testing.T) {
	c := NewClient(Options{})
	if c.AdminAddr() == "" {
		t.Fatal("the admin address must have a default")
	}
	if c.CharacterAddr() == "" {
		t.Fatal("the character address must have a default")
	}
	if c.WorldStreamAddr() == "" {
		t.Fatal("the world stream address must have a default")
	}
	if c.CityNodeAddr() == "" {
		t.Fatal("the city node address must have a default")
	}
}

// fakeAdmin is the admin API of a worldd that wants the service token.
type fakeAdmin struct {
	worldadminv1.UnimplementedWorldAdminServiceServer
	mu    sync.Mutex
	calls int
}

func (f *fakeAdmin) ListPlayers(context.Context, *worldadminv1.ListPlayersRequest) (*worldadminv1.ListPlayersResponse, error) {
	f.mu.Lock()
	f.calls++
	f.mu.Unlock()
	return &worldadminv1.ListPlayersResponse{}, nil
}

func (f *fakeAdmin) called() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

// fakeStream is the world stream of a worldd that wants the service token.
type fakeStream struct {
	worldadminv1.UnimplementedWorldStreamServiceServer
	mu    sync.Mutex
	calls int
}

func (f *fakeStream) GetStatus(context.Context, *worldadminv1.GetStatusRequest) (*worldadminv1.GetStatusResponse, error) {
	f.mu.Lock()
	f.calls++
	f.mu.Unlock()
	return &worldadminv1.GetStatusResponse{}, nil
}

func (f *fakeStream) called() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

// serveAdmin serves the admin API on a free port and returns its address.
func serveAdmin(t *testing.T, world *fakeAdmin) string {
	t.Helper()
	srv := grpc.NewServer(testToken.ServerOptions()...)
	worldadminv1.RegisterWorldAdminServiceServer(srv, world)
	return serve(t, srv)
}

// serveStream serves the world stream on a free port and returns its address.
func serveStream(t *testing.T, stream *fakeStream) string {
	t.Helper()
	srv := grpc.NewServer(testToken.ServerOptions()...)
	worldadminv1.RegisterWorldStreamServiceServer(srv, stream)
	return serve(t, srv)
}

func serve(t *testing.T, srv *grpc.Server) string {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return lis.Addr().String()
}

// writeToken writes a token file, the way scripts\services.ps1 does.
func writeToken(t *testing.T, tok svcauth.Token) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "service.token")
	if err := os.WriteFile(p, []byte(tok), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

// The calls carry the service token the way the other services authenticate to
// each other: in the "authorization" metadata, as "Bearer <token>". A server
// that wants the token lets the call in; one that does not get it refuses it.
func TestTheCallsCarryTheServiceToken(t *testing.T) {
	world := &fakeAdmin{}
	addr := serveAdmin(t, world)
	c := NewClient(Options{AdminAddr: addr, TokenFile: writeToken(t, testToken)})
	if err := c.Dial(context.Background()); err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if _, err := c.admin.ListPlayers(c.WithToken(ctx), &worldadminv1.ListPlayersRequest{}); err != nil {
		t.Fatalf("the call was refused: %v", err)
	}
	if got := world.called(); got != 1 {
		t.Errorf("the server saw %d calls, want 1", got)
	}
}

// A token no server wants is refused: the call says so, it does not pass.
func TestAWrongTokenIsRefused(t *testing.T) {
	world := &fakeAdmin{}
	addr := serveAdmin(t, world)
	c := NewClient(Options{AdminAddr: addr, TokenFile: writeToken(t, otherToken)})
	if err := c.Dial(context.Background()); err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	_, err := c.admin.ListPlayers(c.WithToken(ctx), &worldadminv1.ListPlayersRequest{})
	if err == nil {
		t.Fatal("a wrong token must be refused")
	}
	if s, ok := status.FromError(err); !ok || s.Code() != codes.Unauthenticated {
		t.Errorf("the refusal must be Unauthenticated: %v", err)
	}
	if got := world.called(); got != 0 {
		t.Errorf("the call reached the server %d time(s), want 0", got)
	}
}

// The world stream listens on its own address, not on the admin API's.
func TestTheStreamHasItsOwnConnection(t *testing.T) {
	world := &fakeAdmin{}
	stream := &fakeStream{}
	adminAddr := serveAdmin(t, world)
	streamAddr := serveStream(t, stream)
	c := NewClient(Options{AdminAddr: adminAddr, StreamAddr: streamAddr, TokenFile: writeToken(t, testToken)})
	if err := c.Dial(context.Background()); err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if _, err := c.stream.GetStatus(c.WithToken(ctx), &worldadminv1.GetStatusRequest{}); err != nil {
		t.Fatalf("the stream call was refused: %v", err)
	}
	if got := stream.called(); got != 1 {
		t.Errorf("the stream saw %d calls, want 1", got)
	}
	if got := world.called(); got != 0 {
		t.Errorf("the stream call went to the admin API (%d calls there)", got)
	}
}
