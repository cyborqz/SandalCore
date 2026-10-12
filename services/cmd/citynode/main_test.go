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

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/internal/citynode"
	"aocrop/services/internal/svcauth"
)

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// Every call to citynode needs the service token. A guid no node has is answered NotFound without the database.
func TestServerRequiresTheServiceToken(t *testing.T) {
	srv := newServer(testToken, citynode.New(nil, citynode.DefaultRoster(), ""))
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	get := func(opts ...grpc.DialOption) error {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
		cc, err := grpc.NewClient(lis.Addr().String(), opts...)
		if err != nil {
			t.Fatal(err)
		}
		defer cc.Close()
		_, err = citynodev1.NewCityNodeServiceClient(cc).GetNode(context.Background(), &citynodev1.GetNodeRequest{Node: &citynodev1.NodeRef{Guid: 5}})
		return err
	}
	if err := get(testToken.DialOption()); status.Code(err) != codes.NotFound {
		t.Errorf("GetNode with the token = %v, want NotFound from the roster", err)
	}
	if err := get(); status.Code(err) != codes.Unauthenticated {
		t.Errorf("GetNode without the token = %v, want Unauthenticated", err)
	}
}

// [Cyb :: C-O-A]

// run refuses a missing service token before it reaches the database. (The database is a port nothing listens on.)
func TestRunNeedsTheServiceToken(t *testing.T) {
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	dead := lis.Addr().String()
	lis.Close()
	t.Setenv("AOC_DATABASE_URL", "postgres://aoc:aoc@"+dead+"/aoc?sslmode=disable&connect_timeout=2")
	t.Setenv("AOC_SERVICE_TOKEN_FILE", filepath.Join(t.TempDir(), "service.token"))
	if err := run("seed.json"); err == nil || !strings.Contains(err.Error(), "service token") || strings.Contains(err.Error(), "database") {
		t.Errorf("run() = %v, want the missing service token refused before the database", err)
	}
}
