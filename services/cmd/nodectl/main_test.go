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

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/internal/citynodegrpc"
	"aocrop/services/internal/citynodetest"
	"aocrop/services/internal/svcauth"
)

const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// ctl runs nodectl against a service on a loopback port.
type ctl struct {
	t         *testing.T
	addr      string
	tokenFile string
	env       *citynodetest.Env
}

func newCtl(t *testing.T) *ctl {
	t.Helper()
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	srv := grpc.NewServer(testToken.ServerOptions()...)
	citynodev1.RegisterCityNodeServiceServer(srv, citynodegrpc.New(e.Svc))
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(func() { e.Svc.Shutdown(); srv.Stop() })
	tf := filepath.Join(t.TempDir(), "service.token")
	if err := os.WriteFile(tf, []byte(testToken), 0o600); err != nil {
		t.Fatal(err)
	}
	return &ctl{t: t, addr: lis.Addr().String(), tokenFile: tf, env: e}
}

// run is nodectl <command> with --addr and --token-file after the command; it returns the exit status, stdout and stderr.
func (c *ctl) run(command string, args ...string) (int, string, string) {
	c.t.Helper()
	var out, errw bytes.Buffer
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	full := append([]string{command, "--addr", c.addr, "--token-file", c.tokenFile}, args...)
	rc := run(ctx, full, &out, &errw)
	return rc, out.String(), errw.String()
}

// ~ C O A - Cyb ~

func TestListShowsThe46Nodes(t *testing.T) {
	c := newCtl(t)
	rc, out, errw := c.run("list")
	if rc != 0 || errw != "" {
		t.Fatalf("list = %d, %q", rc, errw)
	}
	lines := strings.Split(strings.TrimSpace(out), "\n")
	// a header, 46 nodes, the count line, the clock
	if len(lines) != 1+46+2 {
		t.Fatalf("%d lines:\n%s", len(lines), out)
	}
	find := func(name string) string {
		for _, l := range lines {
			if strings.HasPrefix(l, name+" ") {
				return l
			}
		}
		t.Fatalf("no line for %s:\n%s", name, out)
		return ""
	}
	if l := find("Verra_RVR_Joeva"); !strings.Contains(l, "4 Town") {
		t.Errorf("Joeva: %q", l)
	}
	if l := find("Verra_RVR_Halcyon"); !strings.Contains(l, "3 Village") || !strings.HasSuffix(strings.TrimSpace(l), "Verra_RVR_Joeva") {
		t.Errorf("Halcyon: %q", l)
	}
	if l := find("Verra_RVR_Winstead"); !strings.Contains(l, "0 Wilderness") {
		t.Errorf("Winstead: %q", l)
	}
	if !strings.Contains(lines[len(lines)-2], "46 nodes (max level 4)") || !strings.Contains(lines[len(lines)-1], "node clock:") {
		t.Errorf("last lines: %q / %q", lines[len(lines)-2], lines[len(lines)-1])
	}
}

func TestSetLevelAndGetFollow(t *testing.T) {
	c := newCtl(t)
	if rc, out, errw := c.run("set-level", "Winstead", "2"); rc != 0 || !strings.Contains(out, "Verra_RVR_Winstead is now level 2 (Encampment)") {
		t.Fatalf("set-level = %d, %q, %q", rc, out, errw)
	}
	if rc, out, _ := c.run("get", "Winstead"); rc != 0 || !strings.Contains(out, "level 2 (Encampment)") {
		t.Errorf("get after set-level = %d, %q", rc, out)
	}
	if rc, out, _ := c.run("set-level", "--disable-vassalship", "Winstead", "2"); rc != 0 || !strings.Contains(out, "nothing changed") {
		t.Errorf("the same level again = %d, %q", rc, out)
	}
	if rc, _, errw := c.run("set-level", "Winstead", "5"); rc != 1 || !strings.Contains(errw, "refused: level 5 is outside 0..4") {
		t.Errorf("level 5 = %d, %q; want a refusal", rc, errw)
	}
	if rc, _, errw := c.run("set-level", "Atlantis", "1"); rc != 1 || !strings.Contains(errw, "not found") {
		t.Errorf("unknown node = %d, %q", rc, errw)
	}
	// by guid
	if rc, out, _ := c.run("get", "108646121231993"); rc != 0 || !strings.Contains(out, "Verra_RVR_Joeva (Joeva)") || !strings.Contains(out, "level 4 (Town)") {
		t.Errorf("get by guid = %d, %q", rc, out)
	}
}

func TestAddXPClockAndSeed(t *testing.T) {
	c := newCtl(t)
	if rc, out, errw := c.run("add-xp", "Winstead", "1000"); rc != 0 || !strings.Contains(out, "holds 1000 XP") || !strings.Contains(out, "the level is unchanged") {
		t.Fatalf("add-xp = %d, %q, %q", rc, out, errw)
	}
	if rc, _, errw := c.run("add-xp", "Winstead", "x"); rc != 2 || !strings.Contains(errw, "not a whole number") {
		t.Errorf("add-xp x = %d, %q", rc, errw)
	}
	if rc, out, _ := c.run("clock", "advance", "3600"); rc != 0 || !strings.Contains(out, "offset 3600s") {
		t.Errorf("clock advance = %d, %q", rc, out)
	}
	if rc, out, _ := c.run("clock", "speed", "60"); rc != 0 || !strings.Contains(out, "speed x60") {
		t.Errorf("clock speed = %d, %q", rc, out)
	}
	if rc, out, _ := c.run("clock"); rc != 0 || !strings.Contains(out, "speed x60") {
		t.Errorf("clock = %d, %q", rc, out)
	}
	if rc, _, errw := c.run("clock", "speed", "0"); rc != 1 || !strings.Contains(errw, "refused:") {
		t.Errorf("clock speed 0 = %d, %q", rc, errw)
	}

	// seed: without --force it says the seed was applied before; with --force it says so, loudly
	if rc, out, _ := c.run("seed"); rc != 0 || !strings.Contains(out, "applied before") || !strings.Contains(out, "--force") {
		t.Errorf("seed = %d, %q", rc, out)
	}
	if rc, out, _ := c.run("set-level", "Joeva", "1"); rc != 0 {
		t.Fatal(out)
	}
	rc, out, _ := c.run("seed", "--force")
	if rc != 0 || !strings.HasPrefix(out, "FORCED:") || !strings.Contains(out, "1 node(s) changed") {
		t.Errorf("seed --force = %d, %q", rc, out)
	}
	if rc, out, _ := c.run("get", "Joeva"); rc != 0 || !strings.Contains(out, "level 4 (Town)") {
		t.Errorf("Joeva after seed --force: %q", out)
	}
	// --file hands the service a seed text
	f := filepath.Join(t.TempDir(), "seed.json")
	if err := os.WriteFile(f, []byte(`{"nodes":{"Winstead":{"level":1}}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if rc, out, _ := c.run("seed", "--force", "--file", f); rc != 0 || !strings.Contains(out, "3 node(s) changed") {
		t.Errorf("seed --file = %d, %q", rc, out)
	}
	if rc, _, errw := c.run("seed", "--file", filepath.Join(t.TempDir(), "missing.json")); rc != 2 || errw == "" {
		t.Errorf("seed --file missing = %d, %q", rc, errw)
	}
}

// [C:Y:B :: CoA]

func TestWatchPrintsTheSnapshotThenChanges(t *testing.T) {
	c := newCtl(t)
	ctx, cancel := context.WithCancel(context.Background())
	var out, errw lockedBuffer
	done := make(chan int, 1)
	go func() {
		done <- run(ctx, []string{"watch", "--addr", c.addr, "--token-file", c.tokenFile}, &out, &errw)
	}()
	deadline := time.Now().Add(10 * time.Second)
	for !strings.Contains(out.String(), "snapshot: 46 nodes") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if _, _, err := c.env.Svc.SetNodeLevel(context.Background(), 0, "Winstead", 2, false); err != nil {
		t.Fatal(err)
	}
	for !strings.Contains(out.String(), "#1 SET_LEVEL") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	cancel()
	if rc := <-done; rc != 0 {
		t.Errorf("watch ended with %d: %s", rc, errw.String())
	}
	if !strings.Contains(out.String(), "#0 snapshot: 46 nodes") || !strings.Contains(out.String(), "#1 SET_LEVEL: Verra_RVR_Winstead level 0 -> 2") {
		t.Errorf("watch printed:\n%s", out.String())
	}
}

func TestUsageAndConnectionErrors(t *testing.T) {
	var out, errw bytes.Buffer
	ctx := context.Background()
	for _, args := range [][]string{{}, {"nonsense"}, {"get"}, {"set-level", "Joeva"}, {"set-level", "Joeva", "high"}, {"clock", "speed"}, {"list", "extra"}} {
		out.Reset()
		errw.Reset()
		if rc := run(ctx, args, &out, &errw); rc != 2 || errw.Len() == 0 {
			t.Errorf("nodectl %v = %d, %q; want exit 2 with a message", args, rc, errw.String())
		}
	}
	// a service that is not running
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	dead := lis.Addr().String()
	lis.Close()
	tf := filepath.Join(t.TempDir(), "service.token")
	if err := os.WriteFile(tf, []byte(testToken), 0o600); err != nil {
		t.Fatal(err)
	}
	errw.Reset()
	cctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	if rc := run(cctx, []string{"list", "--addr", dead, "--token-file", tf}, &out, &errw); rc != 1 || !strings.Contains(errw.String(), "not reachable") {
		t.Errorf("list with no service = %d, %q", rc, errw.String())
	}
	// no token file
	errw.Reset()
	if rc := run(ctx, []string{"list", "--addr", dead, "--token-file", filepath.Join(t.TempDir(), "none")}, &out, &errw); rc != 1 || !strings.Contains(errw.String(), "service token") {
		t.Errorf("list with no token = %d, %q", rc, errw.String())
	}
}

// lockedBuffer is a bytes.Buffer a test can read while nodectl writes to it.
type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *lockedBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func (l *lockedBuffer) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.String()
}
