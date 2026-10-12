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

	authv1 "aocrop/services/gen/aoc/auth/v1"
	"aocrop/services/internal/account"
	"aocrop/services/internal/svcauth"
)

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// okTokens accepts every login token, so a call that passes the service token
// check reaches the handler without a database.
type okTokens struct{}

func (okTokens) ValidateToken(context.Context, string) (account.Account, error) {
	return account.Account{ID: 1, Username: "cyb"}, nil
}

// built by C Y B with CoA

// Every gRPC call to auth needs the service token.
func TestGRPCServerRequiresTheServiceToken(t *testing.T) {
	srv := newGRPCServer(testToken, okTokens{})
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	validate := func(opts ...grpc.DialOption) error {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
		cc, err := grpc.NewClient(lis.Addr().String(), opts...)
		if err != nil {
			t.Fatal(err)
		}
		defer cc.Close()
		_, err = authv1.NewAuthServiceClient(cc).ValidateToken(context.Background(), &authv1.ValidateTokenRequest{Token: "t"})
		return err
	}
	if err := validate(testToken.DialOption()); err != nil {
		t.Errorf("ValidateToken with the token = %v, want OK", err)
	}
	if err := validate(); status.Code(err) != codes.Unauthenticated {
		t.Errorf("ValidateToken without the token = %v, want Unauthenticated", err)
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
	t.Setenv("AOC_SERVICE_TOKEN_FILE", filepath.Join(t.TempDir(), "service.token"))
	if err := run(); err == nil || !strings.Contains(err.Error(), "service token") || strings.Contains(err.Error(), "database") {
		t.Errorf("run() = %v, want the missing service token refused before the database", err)
	}
}
