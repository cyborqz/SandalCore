// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package authgrpc

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	"aocrop/services/internal/account"
)

type fakeTokens map[string]account.Account

func (f fakeTokens) ValidateToken(_ context.Context, token string) (account.Account, error) {
	if a, ok := f[token]; ok {
		return a, nil
	}
	return account.Account{}, account.ErrInvalidToken
}

func client(t *testing.T, tokens Tokens) authv1.AuthServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	authv1.RegisterAuthServiceServer(srv, New(tokens))
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return authv1.NewAuthServiceClient(conn)
}

func TestValidateToken(t *testing.T) {
	c := client(t, fakeTokens{"aoc_good": {ID: 7, Username: "cyb"}})
	ctx := context.Background()

	res, err := c.ValidateToken(ctx, &authv1.ValidateTokenRequest{Token: "aoc_good"})
	if err != nil || res.AccountId != 7 || res.Username != "cyb" {
		t.Fatalf("good token: %v %v", res, err)
	}
	_, err = c.ValidateToken(ctx, &authv1.ValidateTokenRequest{Token: "aoc_bad"})
	if status.Code(err) != codes.Unauthenticated {
		t.Errorf("bad token: %v, want Unauthenticated", err)
	}
}

// ~ Cyb - C | O | A ~

// failingTokens is a token store whose database is down.
type failingTokens struct{ err error }

func (f failingTokens) ValidateToken(context.Context, string) (account.Account, error) {
	return account.Account{}, f.err
}

// A store failure is Internal, and its text stays in the service log.
func TestStoreFailureIsInternalWithoutDetail(t *testing.T) {
	c := client(t, failingTokens{errors.New("pq: secret detail")})
	_, err := c.ValidateToken(context.Background(), &authv1.ValidateTokenRequest{Token: "aoc_good"})
	if status.Code(err) != codes.Internal || strings.Contains(status.Convert(err).Message(), "secret detail") {
		t.Errorf("store failure: %v, want Internal without the store's text", err)
	}
}

// A timeout inside the store (pgx's connect timeout wraps DeadlineExceeded)
// while the caller's context is alive is a store failure: Internal, and the
// host name in its text does not reach the caller.
func TestStoreTimeoutIsInternalWithoutDetail(t *testing.T) {
	c := client(t, failingTokens{fmt.Errorf("failed to connect to host=db.internal user=aoc: %w", context.DeadlineExceeded)})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := c.ValidateToken(ctx, &authv1.ValidateTokenRequest{Token: "aoc_x"})
	if status.Code(err) != codes.Internal || strings.Contains(status.Convert(err).Message(), "db.internal") {
		t.Errorf("store timeout: %v, want Internal without the store's text", err)
	}
}

// The caller's own cancellation or deadline keeps its gRPC code. Over a real
// connection the client gives up first, so this calls the handler directly.
func TestCallerContextErrorsKeepTheirCodes(t *testing.T) {
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	expired, cancelExpired := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancelExpired()
	for _, tc := range []struct {
		name string
		ctx  context.Context
		err  error
		want codes.Code
		msg  string // the context's own text, not the store's
	}{
		{"canceled", canceled, fmt.Errorf("query: %w", context.Canceled), codes.Canceled, "context canceled"},
		{"deadline", expired, fmt.Errorf("query: %w", context.DeadlineExceeded), codes.DeadlineExceeded, "context deadline exceeded"},
	} {
		_, err := New(failingTokens{tc.err}).ValidateToken(tc.ctx, &authv1.ValidateTokenRequest{Token: "aoc_x"})
		if status.Code(err) != tc.want {
			t.Errorf("%s: %v, want %v", tc.name, err, tc.want)
		}
		if got := status.Convert(err).Message(); got != tc.msg {
			t.Errorf("%s: message %q, want %q", tc.name, got, tc.msg)
		}
	}
}

// A token the store validated is answered even when the caller's context has
// ended by now: the store's success is checked first, and only a failure is
// judged by the context. (Pins the order of the checks in ValidateToken.)
func TestSuccessBeatsAnEndedContext(t *testing.T) {
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	expired, cancelExpired := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancelExpired()
	for name, ctx := range map[string]context.Context{"canceled": canceled, "deadline": expired} {
		res, err := New(fakeTokens{"aoc_good": {ID: 7, Username: "cyb"}}).
			ValidateToken(ctx, &authv1.ValidateTokenRequest{Token: "aoc_good"})
		if err != nil || res.GetAccountId() != 7 || res.GetUsername() != "cyb" {
			t.Errorf("%s: %v, %v; want the success response for account 7 (cyb)", name, res, err)
		}
	}
}

// A wrapped ErrInvalidToken is still Unauthenticated.
func TestWrappedInvalidToken(t *testing.T) {
	c := client(t, failingTokens{fmt.Errorf("lookup: %w", account.ErrInvalidToken)})
	_, err := c.ValidateToken(context.Background(), &authv1.ValidateTokenRequest{Token: "aoc_x"})
	if status.Code(err) != codes.Unauthenticated {
		t.Errorf("wrapped invalid token: %v, want Unauthenticated", err)
	}
}
