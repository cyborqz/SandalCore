// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/svcauth"
)

const (
	testToken  = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
	otherToken = svcauth.Token("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210")
)

// fakeWorld is a worldd that keeps what it was asked.
type fakeWorld struct {
	worldadminv1.UnimplementedWorldAdminServiceServer
	mu        sync.Mutex
	teleports []*worldadminv1.TeleportRequest
	speeds    []*worldadminv1.SetMoveSpeedRequest
}

func (f *fakeWorld) ListPlayers(context.Context, *worldadminv1.ListPlayersRequest) (*worldadminv1.ListPlayersResponse, error) {
	return &worldadminv1.ListPlayersResponse{Players: []*worldadminv1.Player{
		{Name: "cyb", CharacterId: "bbec5c86eb101fe5f38c87f0fa22e7d0", Position: &worldadminv1.Position{X: -777664, Y: 616578, Z: 15952}, Mounted: true, MoveSpeedMultiplier: 1},
		{Name: "dead", CharacterId: "00000000000000000000000000000001", Position: &worldadminv1.Position{}, Dead: true, MoveSpeedMultiplier: 5},
	}}, nil
}

func (f *fakeWorld) ListPlaces(context.Context, *worldadminv1.ListPlacesRequest) (*worldadminv1.ListPlacesResponse, error) {
	return &worldadminv1.ListPlacesResponse{Places: []*worldadminv1.Place{
		{Name: "Verra_RVR_Joeva", DisplayName: "Joeva", Position: &worldadminv1.Position{X: 1, Y: 2, Z: 3}, Kind: "node"},
		{Name: "login", DisplayName: "login", Position: &worldadminv1.Position{X: 4, Y: 5, Z: 6}, Kind: "login"},
	}}, nil
}

// CoA : C_Y_B

func (f *fakeWorld) Teleport(_ context.Context, r *worldadminv1.TeleportRequest) (*worldadminv1.TeleportResponse, error) {
	f.mu.Lock()
	f.teleports = append(f.teleports, r)
	f.mu.Unlock()
	if r.GetCharacter() == "nobody" {
		return nil, status.Error(codes.NotFound, `no player named "nobody" is in the world`)
	}
	if p := r.GetPoint(); p != nil && !p.GetHasZ() {
		return nil, status.Error(codes.InvalidArgument, "the server has no ground at that point: give z")
	}
	resp := &worldadminv1.TeleportResponse{Character: r.GetCharacter(), From: &worldadminv1.Position{X: 10, Y: 20, Z: 30}, To: &worldadminv1.Position{X: 1, Y: 2, Z: 3}, Dismounted: true}
	if r.GetPlace() != "" {
		resp.Place = "Verra_RVR_Joeva"
	}
	return resp, nil
}

func (f *fakeWorld) SetMoveSpeed(_ context.Context, r *worldadminv1.SetMoveSpeedRequest) (*worldadminv1.SetMoveSpeedResponse, error) {
	f.mu.Lock()
	f.speeds = append(f.speeds, r)
	f.mu.Unlock()
	return &worldadminv1.SetMoveSpeedResponse{Character: r.GetCharacter(), Previous: 1, Multiplier: r.GetMultiplier()}, nil
}

type ctl struct {
	t         *testing.T
	addr      string
	tokenFile string
	world     *fakeWorld
}

// newCtl serves a fake worldd that wants testToken.
func newCtl(t *testing.T) *ctl {
	t.Helper()
	f := &fakeWorld{}
	srv := grpc.NewServer(testToken.ServerOptions()...)
	worldadminv1.RegisterWorldAdminServiceServer(srv, f)
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return &ctl{t: t, addr: lis.Addr().String(), tokenFile: writeToken(t, testToken), world: f}
}

func writeToken(t *testing.T, tok svcauth.Token) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "service.token")
	if err := os.WriteFile(p, []byte(tok), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func (c *ctl) run(command string, args ...string) (int, string, string) {
	c.t.Helper()
	var out, errw bytes.Buffer
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	full := append([]string{command, "--addr", c.addr, "--token-file", c.tokenFile}, args...)
	rc := run(ctx, full, &out, &errw)
	return rc, out.String(), errw.String()
}

func TestPlayersAndPlaces(t *testing.T) {
	c := newCtl(t)
	rc, out, errw := c.run("players")
	if rc != 0 {
		t.Fatalf("players: rc %d, %s", rc, errw)
	}
	for _, want := range []string{"cyb", "bbec5c86eb101fe5f38c87f0fa22e7d0", "-777664", "616578", "15952", "yes", "x5 (dead)"} {
		if !strings.Contains(out, want) {
			t.Errorf("players output lacks %q:\n%s", want, out)
		}
	}
	rc, out, errw = c.run("places")
	if rc != 0 || !strings.Contains(out, "Joeva") || !strings.Contains(out, "Verra_RVR_Joeva") || !strings.Contains(out, "2 places") {
		t.Errorf("places: rc %d\n%s%s", rc, out, errw)
	}
}

func TestTeleportToAPlaceAndAPoint(t *testing.T) {
	c := newCtl(t)
	rc, out, errw := c.run("tp", "cyb", "joeva")
	if rc != 0 {
		t.Fatalf("tp place: rc %d, %s", rc, errw)
	}
	if !strings.Contains(out, "Verra_RVR_Joeva") || !strings.Contains(out, "dismounted first") {
		t.Errorf("tp place output: %s", out)
	}
	if got := c.world.teleports[0]; got.GetCharacter() != "cyb" || got.GetPlace() != "joeva" {
		t.Errorf("request = %v", got)
	}
	// a point with z, with negative numbers (they must not be taken for flags)
	rc, _, errw = c.run("tp", "cyb", "-777664.5", "616578.7", "-15952")
	if rc != 0 {
		t.Fatalf("tp point: rc %d, %s", rc, errw)
	}
	p := c.world.teleports[1].GetPoint()
	if p == nil || p.GetX() != -777664.5 || p.GetY() != 616578.7 || p.GetZ() != -15952 || !p.GetHasZ() {
		t.Errorf("point = %v", p)
	}
	// without z the server may ask for it
	rc, _, errw = c.run("tp", "cyb", "1", "2")
	if rc != 1 || !strings.Contains(errw, "give z") {
		t.Errorf("tp without z: rc %d, %s", rc, errw)
	}
	if p := c.world.teleports[2].GetPoint(); p == nil || p.GetHasZ() {
		t.Errorf("a point without z must not claim one: %v", p)
	}
	// unknown character
	rc, _, errw = c.run("tp", "nobody", "joeva")
	if rc != 1 || !strings.Contains(errw, "not found") {
		t.Errorf("unknown character: rc %d, %s", rc, errw)
	}
}

func TestTeleportUsageErrors(t *testing.T) {
	c := newCtl(t)
	for _, args := range [][]string{
		{"cyb"},                     // no target
		{"cyb", "100"},              // a lone number is no place
		{"cyb", "1", "x"},           // not a number
		{"cyb", "1", "2", "3", "4"}, // too many
		{"cyb", "NaN", "2"},         // not finite
		{"cyb", "Inf", "2", "3"},    // not finite
	} {
		rc, _, errw := c.run("tp", args...)
		if rc != 2 {
			t.Errorf("tp %v: rc %d, %s (want 2)", args, rc, errw)
		}
	}
	if len(c.world.teleports) != 0 {
		t.Errorf("a bad command line reached the server: %v", c.world.teleports)
	}
}

// [C O A - C Y B]

func TestSpeed(t *testing.T) {
	c := newCtl(t)
	rc, out, errw := c.run("speed", "cyb", "5")
	if rc != 0 || !strings.Contains(out, "x5") || !strings.Contains(out, "relog") {
		t.Errorf("speed 5: rc %d\n%s%s", rc, out, errw)
	}
	rc, out, errw = c.run("speed", "cyb", "1")
	if rc != 0 || !strings.Contains(out, "back to normal") {
		t.Errorf("speed 1: rc %d\n%s%s", rc, out, errw)
	}
	if len(c.world.speeds) != 2 || c.world.speeds[0].GetMultiplier() != 5 || c.world.speeds[1].GetMultiplier() != 1 {
		t.Errorf("requests = %v", c.world.speeds)
	}
	for _, m := range []string{"0.4", "10.5", "abc", "0", "-1", "NaN"} {
		if rc, _, errw := c.run("speed", "cyb", m); rc != 2 {
			t.Errorf("speed %s: rc %d, %s (want 2)", m, rc, errw)
		}
	}
	if rc, _, _ := c.run("speed", "cyb"); rc != 2 {
		t.Errorf("speed without a multiplier: rc %d", rc)
	}
}

func TestWrongTokenAndNoServer(t *testing.T) {
	c := newCtl(t)
	c.tokenFile = writeToken(t, otherToken)
	rc, _, errw := c.run("players")
	if rc != 1 || !strings.Contains(errw, "token") {
		t.Errorf("wrong token: rc %d, %s", rc, errw)
	}
	if strings.Contains(errw, string(otherToken)) || strings.Contains(errw, string(testToken)) {
		t.Errorf("a token was printed: %s", errw)
	}
	// nobody listens
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := lis.Addr().String()
	lis.Close()
	c.addr = addr
	c.tokenFile = writeToken(t, testToken)
	rc, _, errw = c.run("players")
	if rc != 1 || !strings.Contains(errw, "not reachable") {
		t.Errorf("no server: rc %d, %s", rc, errw)
	}
}

func TestUsage(t *testing.T) {
	var out, errw bytes.Buffer
	if rc := run(context.Background(), nil, &out, &errw); rc != 2 {
		t.Errorf("no command: rc %d", rc)
	}
	if rc := run(context.Background(), []string{"fly"}, &out, &errw); rc != 2 {
		t.Errorf("unknown command: rc %d", rc)
	}
	if rc := run(context.Background(), []string{"help"}, &out, &errw); rc != 0 || !strings.Contains(out.String(), "tp <character>") {
		t.Errorf("help: rc %d\n%s", rc, out.String())
	}
}
