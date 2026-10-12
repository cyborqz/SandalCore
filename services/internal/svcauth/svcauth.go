// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package svcauth is the shared secret the internal services require of each
// other (plan A4). scripts\services.ps1 writes 32 random bytes as 64
// lowercase hex digits to dist\services\service.token; every internal gRPC
// server refuses calls that do not carry it as "authorization: Bearer
// <token>", and the gateway and worldd send it. The game client's ICS
// stream does not use it.
package svcauth

import (
	"context"
	"crypto/subtle"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/peer"
	"google.golang.org/grpc/status"
)

// MetadataKey is the gRPC metadata the token travels in.
const MetadataKey = "authorization"

// Token is a service token: 64 lowercase hex digits. Never log it.
type Token string

var tokenPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

// ErrInvalid means the token file does not hold a token.
var ErrInvalid = errors.New("not a service token (64 lowercase hex digits)")

// Load reads a token file; surrounding whitespace is ignored. Errors never
// quote the file's contents.
func Load(path string) (Token, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("service token: %w (scripts\\services.ps1 run creates it)", err)
	}
	t := strings.TrimSpace(string(b))
	if !tokenPattern.MatchString(t) {
		return "", fmt.Errorf("service token %s: %w", path, ErrInvalid)
	}
	return Token(t), nil
}

// C.O.A : Cyb

// ServerOptions make a gRPC server refuse, with Unauthenticated, every call
// that does not carry the token.
func (t Token) ServerOptions() []grpc.ServerOption {
	return []grpc.ServerOption{
		grpc.ChainUnaryInterceptor(func(ctx context.Context, req any, info *grpc.UnaryServerInfo, h grpc.UnaryHandler) (any, error) {
			if err := t.check(ctx, info.FullMethod); err != nil {
				return nil, err
			}
			return h(ctx, req)
		}),
		grpc.ChainStreamInterceptor(func(srv any, ss grpc.ServerStream, info *grpc.StreamServerInfo, h grpc.StreamHandler) error {
			if err := t.check(ss.Context(), info.FullMethod); err != nil {
				return err
			}
			return h(srv, ss)
		}),
	}
}

// DialOption makes a client send the token with every call.
func (t Token) DialOption() grpc.DialOption {
	return grpc.WithPerRPCCredentials(credentials{t})
}

func (t Token) check(ctx context.Context, method string) error {
	want := []byte("Bearer " + string(t))
	md, _ := metadata.FromIncomingContext(ctx)
	// An empty Token refuses everything, rather than accept a bare "Bearer ".
	if got := md.Get(MetadataKey); t != "" && len(got) == 1 && subtle.ConstantTimeCompare([]byte(got[0]), want) == 1 {
		return nil
	}
	from := "unknown"
	if p, ok := peer.FromContext(ctx); ok {
		from = p.Addr.String()
	}
	refusals.note(method, from)
	return status.Error(codes.Unauthenticated, "missing or wrong service token")
}

// refusalInterval is the least time between two refusal log lines.
const refusalInterval = time.Second

// refusalLog rate-limits the refusal warning: a caller that retries with a
// missing or wrong token would otherwise write a line per call. A line says
// how many refusals it stands for, itself and those held back since the last
// line, and names the latest one's method and peer.
//
// The refusals held back when a burst ends are not left for the next refusal,
// which may never come: the first one held back arms a timer for the end of the
// interval, and its line stands for all of them. A refusal that comes due first
// takes the held-back count with it, and the timer then finds nothing to write.
type refusalLog struct {
	mu           sync.Mutex
	now          func() time.Time
	after        func(d time.Duration, f func()) // runs f once, d from now; time.AfterFunc when nil
	last         time.Time                       // when the last line was written
	pending      int                             // refusals since then not yet in a line
	method, peer string                          // of the latest of those
	armed        bool                            // a timer is set to call flush
}

// note counts a refusal, and logs it or holds it back for the timer's line.
func (l *refusalLog) note(method, peer string) {
	l.mu.Lock()
	l.pending++
	l.method, l.peer = method, peer
	count, due := l.takeLocked()
	if !due {
		l.armLocked()
	}
	l.mu.Unlock()
	if due {
		logRefusals(count, method, peer)
	}
}

// takeLocked returns how many refusals a line stands for if one is due now (none
// has been written yet, or the interval since the last is over), and resets the
// count.
func (l *refusalLog) takeLocked() (count int, due bool) {
	now := l.now()
	if l.pending == 0 || (!l.last.IsZero() && now.Sub(l.last) < refusalInterval) {
		return 0, false
	}
	count, l.pending, l.last = l.pending, 0, now
	return count, true
}

// armLocked sets the timer for the end of the interval, if refusals wait and no
// timer is set.
func (l *refusalLog) armLocked() {
	if l.armed || l.pending == 0 {
		return
	}
	l.armed = true
	after := l.after
	if after == nil {
		after = func(d time.Duration, f func()) { time.AfterFunc(d, f) }
	}
	after(l.last.Add(refusalInterval).Sub(l.now()), l.flush)
}

// flush is the timer: it writes the line for the refusals held back, or, when
// the interval is not over after all, sets the timer for the rest of it.
func (l *refusalLog) flush() {
	l.mu.Lock()
	l.armed = false
	method, peer := l.method, l.peer
	count, due := l.takeLocked()
	if !due {
		l.armLocked()
	}
	l.mu.Unlock()
	if due {
		logRefusals(count, method, peer)
	}
}

// ~ Cyb - C | O | A ~

func logRefusals(count int, method, peer string) {
	slog.Warn("refused internal calls with a missing or wrong service token", "count", count, "method", method, "peer", peer)
}

var refusals = &refusalLog{now: time.Now}

type credentials struct{ t Token }

func (c credentials) GetRequestMetadata(context.Context, ...string) (map[string]string, error) {
	return map[string]string{MetadataKey: "Bearer " + string(c.t)}, nil
}

// RequireTransportSecurity is false: the internal services talk over
// loopback or a private network without TLS.
func (credentials) RequireTransportSecurity() bool { return false }
