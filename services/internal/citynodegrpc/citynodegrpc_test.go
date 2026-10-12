// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynodegrpc_test

import (
	"context"
	"net"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/internal/citynodegrpc"
	"aocrop/services/internal/citynodetest"
	"aocrop/services/internal/svcauth"
)

const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

const joeva = int64(108646121231993)

// serve starts the service behind a token-checking gRPC server on a loopback port and returns a client.
func serve(t *testing.T) (citynodev1.CityNodeServiceClient, *citynodetest.Env) {
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
	cc, err := grpc.NewClient(lis.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()), testToken.DialOption())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cc.Close() })
	return citynodev1.NewCityNodeServiceClient(cc), e
}

func code(err error) codes.Code { return status.Code(err) }

// (CoA | C-Y-B)

func TestReadsAndControls(t *testing.T) {
	c, _ := serve(t)
	ctx := context.Background()

	l, err := c.ListNodes(ctx, &citynodev1.ListNodesRequest{})
	if err != nil || len(l.GetNodes()) != 46 || l.GetMaxNodeLevel() != 4 || l.GetClock().GetSpeed() != 1 || l.GetSeq() != 0 {
		t.Fatalf("ListNodes = %v, %v", l, err)
	}
	var sawJoeva, sawHalcyon bool
	for _, n := range l.GetNodes() {
		switch n.GetName() {
		case "Verra_RVR_Joeva":
			sawJoeva = n.GetLevel() == 4 && n.GetLevelName() == "Town" && n.GetGuid() == joeva
		case "Verra_RVR_Halcyon":
			sawHalcyon = n.GetLevel() == 3 && n.GetLevelName() == "Village" && n.GetParentGuid() == joeva
		}
	}
	if !sawJoeva || !sawHalcyon {
		t.Errorf("Joeva Town %v, Halcyon Village under Joeva %v", sawJoeva, sawHalcyon)
	}

	g, err := c.GetNode(ctx, &citynodev1.GetNodeRequest{Node: &citynodev1.NodeRef{Name: "Halcyon"}})
	if err != nil || g.GetNode().GetName() != "Verra_RVR_Halcyon" {
		t.Fatalf("GetNode by name = %v, %v", g, err)
	}
	g, err = c.GetNode(ctx, &citynodev1.GetNodeRequest{Node: &citynodev1.NodeRef{Guid: joeva}})
	if err != nil || g.GetNode().GetDisplayName() != "Joeva" {
		t.Fatalf("GetNode by guid = %v, %v", g, err)
	}
	if _, err := c.GetNode(ctx, &citynodev1.GetNodeRequest{Node: &citynodev1.NodeRef{Name: "Atlantis"}}); code(err) != codes.NotFound {
		t.Errorf("unknown node = %v, want NotFound", err)
	}
	if _, err := c.GetNode(ctx, &citynodev1.GetNodeRequest{}); code(err) != codes.InvalidArgument {
		t.Errorf("no node = %v, want InvalidArgument", err)
	}

	s, err := c.SetNodeLevel(ctx, &citynodev1.SetNodeLevelRequest{Node: &citynodev1.NodeRef{Name: "Winstead"}, Level: 2})
	if err != nil || s.GetNode().GetLevel() != 2 || !s.GetChanged() {
		t.Fatalf("SetNodeLevel = %v, %v", s, err)
	}
	if s, err = c.SetNodeLevel(ctx, &citynodev1.SetNodeLevelRequest{Node: &citynodev1.NodeRef{Name: "Winstead"}, Level: 2}); err != nil || s.GetChanged() {
		t.Errorf("the same level again = %v, %v; want unchanged", s, err)
	}
	for _, bad := range []int32{-1, 5} {
		if _, err := c.SetNodeLevel(ctx, &citynodev1.SetNodeLevelRequest{Node: &citynodev1.NodeRef{Name: "Winstead"}, Level: bad}); code(err) != codes.InvalidArgument {
			t.Errorf("level %d = %v, want InvalidArgument", bad, err)
		}
	}
	x, err := c.AddNodeExperience(ctx, &citynodev1.AddNodeExperienceRequest{Node: &citynodev1.NodeRef{Name: "Winstead"}, Amount: 40_000_000})
	if err != nil || x.GetNode().GetExperience() != 40_000_000 || x.GetNode().GetLevel() != 2 || x.GetNode().GetPercent() != 100 {
		t.Fatalf("AddNodeExperience = %v, %v", x, err)
	}
	if _, err := c.AddNodeExperience(ctx, &citynodev1.AddNodeExperienceRequest{Node: &citynodev1.NodeRef{Name: "Winstead"}, Amount: -40_000_001}); code(err) != codes.InvalidArgument {
		t.Errorf("XP below zero = %v, want InvalidArgument", err)
	}

	a, err := c.AdvanceClock(ctx, &citynodev1.AdvanceClockRequest{Seconds: 120})
	if err != nil || a.GetClock().GetOffsetSeconds() != 120 {
		t.Fatalf("AdvanceClock = %v, %v", a, err)
	}
	if _, err := c.AdvanceClock(ctx, &citynodev1.AdvanceClockRequest{Seconds: 0}); code(err) != codes.InvalidArgument {
		t.Errorf("AdvanceClock(0) = %v", err)
	}
	sp, err := c.SetClockSpeed(ctx, &citynodev1.SetClockSpeedRequest{Factor: 30})
	if err != nil || sp.GetClock().GetSpeed() != 30 {
		t.Fatalf("SetClockSpeed = %v, %v", sp, err)
	}
	if _, err := c.SetClockSpeed(ctx, &citynodev1.SetClockSpeedRequest{Factor: 0}); code(err) != codes.InvalidArgument {
		t.Errorf("SetClockSpeed(0) = %v", err)
	}
	gc, err := c.GetClock(ctx, &citynodev1.GetClockRequest{})
	if err != nil || gc.GetClock().GetSpeed() != 30 {
		t.Errorf("GetClock = %v, %v", gc, err)
	}

	r, err := c.ApplySeed(ctx, &citynodev1.ApplySeedRequest{})
	if err != nil || r.GetApplied() || r.GetNodesChanged() != 0 {
		t.Errorf("ApplySeed without force = %v, %v; want not applied", r, err)
	}
	r, err = c.ApplySeed(ctx, &citynodev1.ApplySeedRequest{Force: true})
	if err != nil || !r.GetApplied() || !r.GetForced() || r.GetNodesChanged() != 1 { // Winstead back to Wilderness
		t.Errorf("ApplySeed(force) = %v, %v", r, err)
	}
	if _, err := c.ApplySeed(ctx, &citynodev1.ApplySeedRequest{Force: true, SeedJson: `{"nodes":{"Joeva":{"level":9}}}`}); code(err) != codes.InvalidArgument {
		t.Errorf("a wrong seed = %v, want InvalidArgument", err)
	}
}

// The calls need the service token, streams included.
func TestTokenRequired(t *testing.T) {
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
	cc, err := grpc.NewClient(lis.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer cc.Close()
	c := citynodev1.NewCityNodeServiceClient(cc)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := c.ListNodes(ctx, &citynodev1.ListNodesRequest{}); code(err) != codes.Unauthenticated {
		t.Errorf("ListNodes without the token = %v, want Unauthenticated", err)
	}
	if _, err := c.SetNodeLevel(ctx, &citynodev1.SetNodeLevelRequest{Node: &citynodev1.NodeRef{Name: "Joeva"}, Level: 1}); code(err) != codes.Unauthenticated {
		t.Errorf("SetNodeLevel without the token = %v, want Unauthenticated", err)
	}
	st, err := c.WatchNodes(ctx, &citynodev1.WatchNodesRequest{})
	if err == nil {
		_, err = st.Recv()
	}
	if code(err) != codes.Unauthenticated {
		t.Errorf("WatchNodes without the token = %v, want Unauthenticated", err)
	}
	if n, _ := e.Svc.GetNode(ctx, 0, "Joeva"); n.Level != 4 {
		t.Errorf("a refused call changed Joeva to level %d", n.Level)
	}
}

// Cyb / C | O | A

// WatchNodes over gRPC: the whole state first, then each change, numbered without a gap, from any caller (the in-game GM
// path and nodectl both go through the same methods); and the end of the stream when the service shuts down.
func TestWatchNodesStream(t *testing.T) {
	c, e := serve(t)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	st, err := c.WatchNodes(ctx, &citynodev1.WatchNodesRequest{})
	if err != nil {
		t.Fatal(err)
	}
	first, err := st.Recv()
	if err != nil {
		t.Fatal(err)
	}
	snap := first.GetSnapshot()
	if snap == nil || len(snap.GetNodes()) != 46 || first.GetSeq() != 0 || snap.GetClock().GetSpeed() != 1 {
		t.Fatalf("first message = %v", first)
	}

	// changes by the gRPC methods and by the service object directly (worldd's path later) arrive in the order they were made
	if _, err := c.SetNodeLevel(ctx, &citynodev1.SetNodeLevelRequest{Node: &citynodev1.NodeRef{Name: "Winstead"}, Level: 3, DisableVassalship: true}); err != nil {
		t.Fatal(err)
	}
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", 5); err != nil {
		t.Fatal(err)
	}
	if _, err := c.AdvanceClock(ctx, &citynodev1.AdvanceClockRequest{Seconds: 10}); err != nil {
		t.Fatal(err)
	}
	m, err := st.Recv()
	if err != nil || m.GetSeq() != 1 || m.GetNodeChanged() == nil {
		t.Fatalf("message 1 = %v, %v", m, err)
	}
	nc := m.GetNodeChanged()
	if nc.GetNode().GetName() != "Verra_RVR_Winstead" || nc.GetNode().GetLevel() != 3 || nc.GetPreviousLevel() != 0 ||
		nc.GetCause() != citynodev1.ChangeCause_CHANGE_CAUSE_SET_LEVEL || !nc.GetVassalshipDisabled() {
		t.Errorf("change 1 = %v", nc)
	}
	m, err = st.Recv()
	if err != nil || m.GetSeq() != 2 || m.GetNodeChanged().GetCause() != citynodev1.ChangeCause_CHANGE_CAUSE_ADD_EXPERIENCE || m.GetNodeChanged().GetNode().GetExperience() != 5 {
		t.Fatalf("message 2 = %v, %v", m, err)
	}
	m, err = st.Recv()
	if err != nil || m.GetSeq() != 3 || m.GetClockChanged().GetOffsetSeconds() != 10 {
		t.Fatalf("message 3 = %v, %v", m, err)
	}

	// shutdown ends the stream with Unavailable
	e.Svc.Shutdown()
	if _, err := st.Recv(); code(err) != codes.Unavailable {
		t.Errorf("after shutdown: %v, want Unavailable", err)
	}
}
