// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package gatewaygrpc

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"

	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
	"aocrop/services/internal/tickets"
)

type fakeTickets struct {
	err                error
	gotTicket, gotChar string
	gotServer          uint32
}

func (f *fakeTickets) Redeem(_ context.Context, ticket, characterID string, serverID uint32) (int64, error) {
	f.gotTicket, f.gotChar, f.gotServer = ticket, characterID, serverID
	if f.err != nil {
		return 0, f.err
	}
	return 7, nil
}

func client(t *testing.T, tk Tickets) gatewayv1.GatewayServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	gatewayv1.RegisterGatewayServiceServer(srv, New(tk))
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return gatewayv1.NewGatewayServiceClient(conn)
}

const (
	aTicket = "7bc658710f9d3f333441c089d974dece"
	aChar   = "bbec5c86eb101fe5f38c87f0fa22e7d0"
)

func TestRedeemTicket(t *testing.T) {
	tk := &fakeTickets{}
	resp, err := client(t, tk).RedeemTicket(context.Background(),
		&gatewayv1.RedeemTicketRequest{Ticket: aTicket, CharacterId: aChar, ServerId: 60})
	if err != nil || resp.GetAccountId() != 7 || resp.GetCharacterId() != aChar {
		t.Fatalf("RedeemTicket = %v %v", resp, err)
	}
	if tk.gotTicket != aTicket || tk.gotChar != aChar || tk.gotServer != 60 {
		t.Errorf("store got %q %q %d", tk.gotTicket, tk.gotChar, tk.gotServer)
	}
}

// (C O A ~ C | Y | B)

func TestRedeemTicketFailures(t *testing.T) {
	for _, tc := range []struct {
		err  error
		want codes.Code
	}{
		{tickets.ErrInvalid, codes.PermissionDenied},
		{errors.New("pq: secret detail"), codes.Internal},
	} {
		_, err := client(t, &fakeTickets{err: tc.err}).RedeemTicket(context.Background(),
			&gatewayv1.RedeemTicketRequest{Ticket: aTicket, CharacterId: aChar, ServerId: 60})
		if status.Code(err) != tc.want || strings.Contains(status.Convert(err).Message(), "secret detail") {
			t.Errorf("store error %q: %v, want %v without the store's text", tc.err, err, tc.want)
		}
	}
}

func TestCallerCancellationKeepsItsCode(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := New(&fakeTickets{err: context.Canceled}).RedeemTicket(ctx, &gatewayv1.RedeemTicketRequest{})
	if status.Code(err) != codes.Canceled {
		t.Errorf("RedeemTicket = %v, want Canceled", err)
	}
}

// A store error that wraps ErrInvalid is still "not valid", and the reason it
// wraps is not passed on to the caller.
func TestWrappedErrInvalidIsPermissionDeniedWithoutReason(t *testing.T) {
	err := fmt.Errorf("%w: expired at 12:02", tickets.ErrInvalid)
	_, got := New(&fakeTickets{err: err}).RedeemTicket(context.Background(),
		&gatewayv1.RedeemTicketRequest{Ticket: aTicket, CharacterId: aChar, ServerId: 60})
	if status.Code(got) != codes.PermissionDenied || strings.Contains(status.Convert(got).Message(), "expired") {
		t.Errorf("RedeemTicket = %v, want PermissionDenied without the reason", got)
	}
}

// The caller's deadline keeps its own code too, not Canceled.
func TestCallerDeadlineKeepsItsCode(t *testing.T) {
	ctx, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancel()
	_, err := New(&fakeTickets{err: context.DeadlineExceeded}).RedeemTicket(ctx, &gatewayv1.RedeemTicketRequest{})
	if status.Code(err) != codes.DeadlineExceeded {
		t.Errorf("RedeemTicket = %v, want DeadlineExceeded", err)
	}
}

type syncBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuf) Write(p []byte) (int, error) { s.mu.Lock(); defer s.mu.Unlock(); return s.b.Write(p) }
func (s *syncBuf) String() string              { s.mu.Lock(); defer s.mu.Unlock(); return s.b.String() }

// captureLog points slog's default logger at a buffer (all levels) until the
// test ends.
func captureLog(t *testing.T) *syncBuf {
	t.Helper()
	var buf syncBuf
	old := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, &slog.HandlerOptions{Level: slog.LevelDebug})))
	t.Cleanup(func() { slog.SetDefault(old) })
	return &buf
}

// The join ticket is a credential: it is in no log line, whatever the outcome,
// while the failure the caller is not told about is logged.
func TestTheTicketIsNeverLogged(t *testing.T) {
	buf := captureLog(t)
	req := &gatewayv1.RedeemTicketRequest{Ticket: aTicket, CharacterId: aChar, ServerId: 60}
	for _, store := range []*fakeTickets{{}, {err: tickets.ErrInvalid}, {err: fmt.Errorf("pq: secret detail")}} {
		New(store).RedeemTicket(context.Background(), req)
	}
	logged := buf.String()
	if strings.Contains(logged, aTicket) {
		t.Errorf("the ticket is in the log:\n%s", logged)
	}
	if !strings.Contains(logged, "secret detail") || !strings.Contains(logged, aChar) {
		t.Errorf("the log lacks the store's error or the character:\n%s", logged)
	}
}

// worldd forwards whatever an unauthenticated UDP client sent, so a refusal
// logs the character id only when it is well-formed (32 lowercase hex digits);
// anything else is logged by its length alone.
func TestRefusalLogsOnlyWellFormedCharacterIDs(t *testing.T) {
	for _, tc := range []struct {
		name, id string
		want     string // must be in the log
		leak     string // must not be
	}{
		{"well-formed", aChar, "character_id=" + aChar, ""},
		{"1000 characters", strings.Repeat("Z", 1000), "character_id_len=1000", strings.Repeat("Z", 20)},
		{"empty", "", "character_id_len=0", ""},
		{"uppercase hex", strings.ToUpper(aChar), "character_id_len=32", strings.ToUpper(aChar)},
		{"one digit too long", aChar + "0", "character_id_len=33", aChar},
		{"control characters", aChar[:30] + "\r\n", "character_id_len=32", aChar[:30]},
	} {
		t.Run(tc.name, func(t *testing.T) {
			buf := captureLog(t)
			_, err := New(&fakeTickets{err: tickets.ErrInvalid}).RedeemTicket(context.Background(),
				&gatewayv1.RedeemTicketRequest{Ticket: aTicket, CharacterId: tc.id, ServerId: 60})
			if status.Code(err) != codes.PermissionDenied {
				t.Fatalf("RedeemTicket = %v, want PermissionDenied", err)
			}
			logged := buf.String()
			if !strings.Contains(logged, "join ticket refused") || !strings.Contains(logged, tc.want) {
				t.Errorf("the log lacks %q:\n%s", tc.want, logged)
			}
			if tc.leak != "" && strings.Contains(logged, tc.leak) {
				t.Errorf("the character id leaked into the log:\n%.300s", logged)
			}
			if tc.id != aChar && strings.Contains(logged, "character_id=") {
				t.Errorf("a malformed id was logged as character_id:\n%.300s", logged)
			}
		})
	}
}
