// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"net"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	"aocrop/services/internal/mesh"
	"aocrop/services/internal/svcauth"
)

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// AOC_MESH_STATIC_WORLD goes to the client as it is: it must be an address
// the client can dial, and a refusal says why as Register would.
func TestStaticWorldMustBeDialable(t *testing.T) {
	for _, addr := range []string{"", "127.0.0.1:7777", "aoc_worldd_1:7777"} {
		if err := checkStaticWorld(addr); err != nil {
			t.Errorf("checkStaticWorld(%q) = %v, want nil", addr, err)
		}
	}
	for addr, want := range map[string]string{
		"nb\u00a0sp:7777": "the host is neither an IP address nor a plain ASCII host name",
		"0.0.0.0:7777":    "bind addresses",
		"host:0x50":       "port is not a number from 1 to 65535",
		"no-port":         "missing port",
	} {
		err := checkStaticWorld(addr)
		if err == nil || !strings.Contains(err.Error(), "AOC_MESH_STATIC_WORLD") || !strings.Contains(err.Error(), want) {
			t.Errorf("checkStaticWorld(%q) = %v, want an error naming AOC_MESH_STATIC_WORLD and %q", addr, err, want)
		}
	}
}

// CoA captures, C-Y-B code

// Every call to mesh needs the service token. Server 0 never has a
// registration, so Heartbeat(0) is answered without the database.
func TestServerRequiresTheServiceToken(t *testing.T) {
	srv := newServer(testToken, mesh.NewStore(nil, 15*time.Second, ""))
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	heartbeat := func(opts ...grpc.DialOption) error {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
		cc, err := grpc.NewClient(lis.Addr().String(), opts...)
		if err != nil {
			t.Fatal(err)
		}
		defer cc.Close()
		_, err = meshv1.NewMeshServiceClient(cc).Heartbeat(context.Background(), &meshv1.HeartbeatRequest{ServerId: 0})
		return err
	}
	if err := heartbeat(testToken.DialOption()); status.Code(err) != codes.NotFound {
		t.Errorf("Heartbeat(0) with the token = %v, want NotFound from the store", err)
	}
	if err := heartbeat(); status.Code(err) != codes.Unauthenticated {
		t.Errorf("Heartbeat(0) without the token = %v, want Unauthenticated", err)
	}
}

// run refuses a missing service token before it reaches the database. (The
// database is a port nothing listens on.)
func TestRunNeedsTheServiceToken(t *testing.T) {
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	dead := lis.Addr().String()
	lis.Close()
	t.Setenv("AOC_DATABASE_URL", "postgres://aoc:aoc@"+dead+"/aoc?sslmode=disable&connect_timeout=2")
	t.Setenv("AOC_MESH_STATIC_WORLD", "")
	t.Setenv("AOC_SERVICE_TOKEN_FILE", filepath.Join(t.TempDir(), "service.token"))
	if err := run(); err == nil || !strings.Contains(err.Error(), "service token") || strings.Contains(err.Error(), "database") {
		t.Errorf("run() = %v, want the missing service token refused before the database", err)
	}
}

// run refuses a bad AOC_MESH_STATIC_WORLD before it reaches the database.
// (The database is a port nothing listens on.)
func TestRunChecksTheStaticWorldFirst(t *testing.T) {
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	dead := lis.Addr().String()
	lis.Close()
	t.Setenv("AOC_DATABASE_URL", "postgres://aoc:aoc@"+dead+"/aoc?sslmode=disable&connect_timeout=2")
	t.Setenv("AOC_MESH_STATIC_WORLD", "0.0.0.0:7777")
	if err := run(); err == nil || !strings.Contains(err.Error(), "AOC_MESH_STATIC_WORLD") || strings.Contains(err.Error(), "database") {
		t.Errorf("run() = %v, want the static world refused before the database", err)
	}
}
