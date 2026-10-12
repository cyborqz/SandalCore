// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package svcauth

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
)

const good = Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

func writeFile(t *testing.T, content string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "service.token")
	if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestLoad(t *testing.T) {
	got, err := Load(writeFile(t, "  "+string(good)+"\r\n"))
	if err != nil || got != good {
		t.Fatalf("Load = %q, %v; want the token", got, err)
	}
	for _, bad := range []string{"", "abc", strings.ToUpper(string(good)), string(good) + "0", "g" + string(good[1:])} {
		_, err := Load(writeFile(t, bad))
		if !errors.Is(err, ErrInvalid) {
			t.Errorf("Load(%q) = %v, want ErrInvalid", bad, err)
		}
		if bad != "" && strings.Contains(err.Error(), bad) {
			t.Errorf("the error quotes the file: %v", err)
		}
	}
	_, err = Load(filepath.Join(t.TempDir(), "missing"))
	if err == nil || !strings.Contains(err.Error(), "services.ps1") {
		t.Errorf("a missing file: %v, want a pointer to services.ps1", err)
	}
}

// built with C | O | A and C-Y-B

// serve starts a health server behind the token check; health has a unary
// Check and a streaming Watch.
func serve(t *testing.T) *bufconn.Listener {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer(good.ServerOptions()...)
	healthpb.RegisterHealthServer(srv, health.NewServer())
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return lis
}

func client(t *testing.T, lis *bufconn.Listener, opts ...grpc.DialOption) healthpb.HealthClient {
	t.Helper()
	opts = append(opts,
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	cc, err := grpc.NewClient("passthrough:///bufnet", opts...)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cc.Close() })
	return healthpb.NewHealthClient(cc)
}

func TestServerRequiresTheToken(t *testing.T) {
	lis := serve(t)
	ctx := context.Background()

	if _, err := client(t, lis).Check(ctx, &healthpb.HealthCheckRequest{}); status.Code(err) != codes.Unauthenticated {
		t.Errorf("no token: %v, want Unauthenticated", err)
	}
	other := Token(strings.Repeat("f", 64))
	if _, err := client(t, lis, other.DialOption()).Check(ctx, &healthpb.HealthCheckRequest{}); status.Code(err) != codes.Unauthenticated {
		t.Errorf("another token: %v, want Unauthenticated", err)
	}
	if _, err := client(t, lis, good.DialOption()).Check(ctx, &healthpb.HealthCheckRequest{}); err != nil {
		t.Errorf("the token: %v, want OK", err)
	}

	twice := metadata.AppendToOutgoingContext(ctx, MetadataKey, "Bearer "+string(good))
	if _, err := client(t, lis, good.DialOption()).Check(twice, &healthpb.HealthCheckRequest{}); status.Code(err) != codes.Unauthenticated {
		t.Errorf("the token sent twice: %v, want Unauthenticated", err)
	}

	stream, err := client(t, lis).Watch(ctx, &healthpb.HealthCheckRequest{})
	if err == nil {
		_, err = stream.Recv()
	}
	if status.Code(err) != codes.Unauthenticated {
		t.Errorf("a stream without the token: %v, want Unauthenticated", err)
	}
	stream, err = client(t, lis, good.DialOption()).Watch(ctx, &healthpb.HealthCheckRequest{})
	if err == nil {
		_, err = stream.Recv()
	}
	if err != nil {
		t.Errorf("a stream with the token: %v, want its first message", err)
	}
}

// worldd (C++) does not use DialOption: it adds the metadata itself, as
// "authorization: Bearer <token>". The key and the prefix are the contract.
func TestTokenAsWorlddSendsIt(t *testing.T) {
	lis := serve(t)
	ctx := metadata.AppendToOutgoingContext(context.Background(), "authorization", "Bearer "+string(good))
	if _, err := client(t, lis).Check(ctx, &healthpb.HealthCheckRequest{}); err != nil {
		t.Errorf("Check with authorization: Bearer <token> = %v, want OK", err)
	}
}

// An empty Token (never Load's result, but the zero value) must not become a
// server that accepts "Bearer " from anyone.
func TestEmptyTokenRefusesEverything(t *testing.T) {
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer(Token("").ServerOptions()...)
	healthpb.RegisterHealthServer(srv, health.NewServer())
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	ctx := metadata.AppendToOutgoingContext(context.Background(), MetadataKey, "Bearer ")
	if _, err := client(t, lis).Check(ctx, &healthpb.HealthCheckRequest{}); status.Code(err) != codes.Unauthenticated {
		t.Errorf("an empty token accepted \"Bearer \": %v, want Unauthenticated", err)
	}
}

// captureLog sends slog's output to the returned buffer until the test ends.
func captureLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	prev := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, nil)))
	t.Cleanup(func() { slog.SetDefault(prev) })
	return &buf
}

// TestMain keeps the shared refusal log from scheduling real timers: one a
// test left behind would write its line into whichever test is running a second
// later. Tests that look at the log's timer use fakeClock.
func TestMain(m *testing.M) {
	refusals = &refusalLog{now: time.Now, after: func(time.Duration, func()) {}}
	os.Exit(m.Run())
}

// fakeTimers stands in for time.AfterFunc: it records what was scheduled and
// runs it when the test says so.
type fakeTimers struct {
	delays []time.Duration // the delay of each timer scheduled and not yet fired
	funcs  []func()
}

func (f *fakeTimers) after(d time.Duration, fn func()) {
	f.delays = append(f.delays, d)
	f.funcs = append(f.funcs, fn)
}

// fire runs the oldest timer scheduled, as its time coming.
func (f *fakeTimers) fire(t *testing.T) {
	t.Helper()
	if len(f.funcs) == 0 {
		t.Fatal("no timer is scheduled")
	}
	fn := f.funcs[0]
	f.funcs, f.delays = f.funcs[1:], f.delays[1:]
	fn()
}

// fakeClock gives the refusal log a fresh state and a clock the test moves,
// until the test ends. Without it, a refusal an earlier test made a moment ago
// would hold this test's first line back.
func fakeClock(t *testing.T) (advance func(time.Duration)) {
	advance, _ = fakeClockAndTimers(t)
	return advance
}

// ~ C-O-A & C-Y-B ~

// fakeClockAndTimers is fakeClock, and the refusal log's timers are the returned ones.
func fakeClockAndTimers(t *testing.T) (advance func(time.Duration), timers *fakeTimers) {
	t.Helper()
	var ns atomic.Int64
	ns.Store(time.Unix(1_000_000, 0).UnixNano())
	timers = &fakeTimers{}
	prev := refusals
	refusals = &refusalLog{now: func() time.Time { return time.Unix(0, ns.Load()) }, after: timers.after}
	t.Cleanup(func() { refusals = prev })
	return func(d time.Duration) { ns.Add(int64(d)) }, timers
}

func TestRefusalLogNamesTheMethodNotTheToken(t *testing.T) {
	buf := captureLog(t)
	fakeClock(t)

	lis := serve(t)
	wrong := Token(strings.Repeat("e", 64))
	client(t, lis, wrong.DialOption()).Check(context.Background(), &healthpb.HealthCheckRequest{})
	out := buf.String()
	if !strings.Contains(out, "/grpc.health.v1.Health/Check") {
		t.Errorf("log %q does not name the method", out)
	}
	// bufconn's address is "bufconn"; over TCP the line names the caller's.
	if !strings.Contains(out, "peer=bufconn") {
		t.Errorf("log %q does not name the peer", out)
	}
	if strings.Contains(out, string(wrong)) || strings.Contains(out, string(good)) {
		t.Errorf("log %q contains a token", out)
	}
}

// A caller that keeps retrying with a wrong or missing token gets one line a
// second, which says how many refusals it stands for.
func TestRefusalLogIsRateLimited(t *testing.T) {
	buf := captureLog(t)
	advance := fakeClock(t)
	lis := serve(t)
	hc := client(t, lis)
	refuse := func(n int) {
		t.Helper()
		for i := 0; i < n; i++ {
			if _, err := hc.Check(context.Background(), &healthpb.HealthCheckRequest{}); status.Code(err) != codes.Unauthenticated {
				t.Fatalf("Check without the token = %v, want Unauthenticated", err)
			}
		}
	}
	// wantLines checks how many lines the log holds and that the last one
	// stands for count refusals.
	wantLines := func(n, count int) {
		t.Helper()
		out := strings.TrimSpace(buf.String())
		lines := strings.Split(out, "\n")
		if out == "" {
			lines = nil
		}
		if len(lines) != n {
			t.Fatalf("%d log lines, want %d:\n%s", len(lines), n, out)
		}
		if last := lines[n-1]; !strings.Contains(last, fmt.Sprintf(" count=%d ", count)) ||
			!strings.Contains(last, `msg="refused internal calls with a missing or wrong service token"`) {
			t.Fatalf("last line %q, want the refusal message and count=%d", last, count)
		}
	}

	refuse(50) // the first is logged at once, the rest are held back
	wantLines(1, 1)
	advance(999 * time.Millisecond)
	refuse(1)
	wantLines(1, 1)
	advance(time.Millisecond) // a second since the line
	refuse(1)
	wantLines(2, 51) // the 49 and the 1 held back, and this one
	refuse(3)
	wantLines(2, 51)
	advance(time.Second)
	refuse(1)
	wantLines(3, 4)
}

// The refusals held back when a burst ends are not left for the next refusal,
// which may never come: a timer writes the line for them at the end of the
// interval.
func TestRefusalLogFlushesTheEndOfABurst(t *testing.T) {
	buf := captureLog(t)
	advance, timers := fakeClockAndTimers(t)
	lis := serve(t)
	hc := client(t, lis)
	refuse := func(n int) {
		t.Helper()
		for i := 0; i < n; i++ {
			if _, err := hc.Check(context.Background(), &healthpb.HealthCheckRequest{}); status.Code(err) != codes.Unauthenticated {
				t.Fatalf("Check without the token = %v, want Unauthenticated", err)
			}
		}
	}
	lines := func() []string {
		out := strings.TrimSpace(buf.String())
		if out == "" {
			return nil
		}
		return strings.Split(out, "\n")
	}

	refuse(1)
	if len(lines()) != 1 || len(timers.funcs) != 0 {
		t.Fatalf("the first refusal: %d lines, %d timers; want its line and no timer\n%s", len(lines()), len(timers.funcs), buf)
	}
	refuse(4) // held back
	if len(lines()) != 1 {
		t.Fatalf("%d lines after four refusals held back, want 1\n%s", len(lines()), buf)
	}
	if len(timers.funcs) != 1 || timers.delays[0] != refusalInterval {
		t.Fatalf("timers %v, want one for the rest of the interval (%v)", timers.delays, refusalInterval)
	}

	advance(400 * time.Millisecond)
	refuse(1)
	if len(timers.funcs) != 1 {
		t.Fatalf("%d timers, want the one armed (a held-back refusal does not arm another)", len(timers.funcs))
	}

	advance(300 * time.Millisecond) // the timer fires early (a clock that disagrees): it waits for the rest
	timers.fire(t)
	if len(lines()) != 1 {
		t.Fatalf("a timer firing before the interval is over wrote a line\n%s", buf)
	}
	if len(timers.funcs) != 1 || timers.delays[0] != 300*time.Millisecond {
		t.Fatalf("timers %v, want one for the remaining 300ms", timers.delays)
	}

	advance(300 * time.Millisecond) // the interval is over
	timers.fire(t)
	got := lines()
	if len(got) != 2 {
		t.Fatalf("%d lines after the timer, want 2\n%s", len(got), buf)
	}
	last := got[1]
	if !strings.Contains(last, " count=5 ") ||
		!strings.Contains(last, `msg="refused internal calls with a missing or wrong service token"`) ||
		!strings.Contains(last, "method=/grpc.health.v1.Health/Check") || !strings.Contains(last, "peer=bufconn") {
		t.Fatalf("last line %q, want the refusal message, count=5, and the method and peer", last)
	}
	if len(timers.funcs) != 0 {
		t.Fatalf("%d timers left after the flush", len(timers.funcs))
	}

	// Nothing is left pending: a line goes out only for new refusals.
	advance(time.Hour)
	if len(timers.funcs) != 0 || len(lines()) != 2 {
		t.Fatalf("after the flush: %d timers, %d lines; want none, 2", len(timers.funcs), len(lines()))
	}
	refuse(1) // due at once, standing for itself
	if got := lines(); len(got) != 3 || !strings.Contains(got[2], " count=1 ") {
		t.Fatalf("a refusal long after the flush: %d lines, last %q; want 3 and count=1", len(got), got[len(got)-1])
	}
}

// [Cyb + C | O | A]

// A refusal that comes due before the timer takes the held-back count with it;
// the timer then has nothing to write, and the next burst arms a new one.
func TestRefusalLogTimerAfterADueRefusal(t *testing.T) {
	buf := captureLog(t)
	advance, timers := fakeClockAndTimers(t)
	refuse := func(n int) {
		for i := 0; i < n; i++ {
			refusals.note("/svc/Method", "192.0.2.1:1")
		}
	}
	count := func() int { return strings.Count(buf.String(), "\n") }

	refuse(4)
	advance(refusalInterval)
	refuse(1) // due: the 3 held back, and this one
	if count() != 2 || !strings.Contains(buf.String(), " count=4 ") {
		t.Fatalf("log:\n%s\nwant two lines, the second count=4", buf)
	}
	timers.fire(t) // the timer armed by the burst
	if count() != 2 || len(timers.funcs) != 0 {
		t.Fatalf("the stale timer wrote a line or armed another: %d lines, %d timers\n%s", count(), len(timers.funcs), buf)
	}

	refuse(2) // the first is held back (inside the interval of the line just written)
	if len(timers.funcs) != 1 || timers.delays[0] != refusalInterval {
		t.Fatalf("timers %v, want one armed for the new burst", timers.delays)
	}
	advance(refusalInterval)
	timers.fire(t)
	if count() != 3 || !strings.Contains(buf.String(), " count=2 ") {
		t.Fatalf("log:\n%s\nwant a third line with count=2", buf)
	}
}

// The real timer (time.AfterFunc) writes the line, too.
func TestRefusalLogRealTimerFlushes(t *testing.T) {
	if testing.Short() {
		t.Skip("waits for a second")
	}
	buf := &syncBuffer{}
	prev := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(buf, nil)))
	t.Cleanup(func() { slog.SetDefault(prev) })
	l := &refusalLog{now: time.Now} // after is nil: time.AfterFunc
	l.note("/svc/Method", "192.0.2.1:1")
	l.note("/svc/Method", "192.0.2.1:1")
	l.note("/svc/Method", "192.0.2.1:1")
	deadline := time.Now().Add(refusalInterval + 2*time.Second)
	for time.Now().Before(deadline) {
		if strings.Contains(buf.String(), " count=2 ") {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("no line for the two refusals held back, log:\n%s", buf.String())
}

// syncBuffer is a log destination a timer's goroutine may write while the test reads.
type syncBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func TestCredentials(t *testing.T) {
	md, err := credentials{good}.GetRequestMetadata(context.Background())
	if err != nil || md[MetadataKey] != "Bearer "+string(good) {
		t.Errorf("GetRequestMetadata = %v, %v", md, err)
	}
	if (credentials{}).RequireTransportSecurity() {
		t.Error("RequireTransportSecurity = true, want false (loopback)")
	}
}
