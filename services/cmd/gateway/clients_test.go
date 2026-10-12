// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"errors"
	"log"
	"log/slog"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/proto"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	characterv1 "aocrop/services/gen/aoc/character/v1"
	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/config"
	"aocrop/services/internal/ics"
	"aocrop/services/internal/mesh"
	"aocrop/services/internal/meshgrpc"
	"aocrop/services/internal/svcauth"
)

// fakeServices is auth, character and mesh in one gRPC server. It records the
// last request and how long its call had left, and when block is set, holds
// each call until the call's context ends.
type fakeServices struct {
	authv1.UnimplementedAuthServiceServer
	characterv1.UnimplementedCharacterServiceServer
	meshv1.UnimplementedMeshServiceServer

	mu      sync.Mutex
	block   bool
	started chan struct{} // closed when the next blocked call starts
	last    proto.Message
	left    time.Duration // -1: no deadline
}

func (f *fakeServices) record(ctx context.Context, req proto.Message) error {
	f.mu.Lock()
	f.last = req
	f.left = -1
	if d, ok := ctx.Deadline(); ok {
		f.left = time.Until(d)
	}
	block, started := f.block, f.started
	f.started = nil
	f.mu.Unlock()
	if !block {
		return nil
	}
	if started != nil {
		close(started)
	}
	<-ctx.Done()
	return status.FromContextError(ctx.Err()).Err()
}

// (C O A) (C.Y.B)

func (f *fakeServices) ValidateToken(ctx context.Context, r *authv1.ValidateTokenRequest) (*authv1.ValidateTokenResponse, error) {
	if err := f.record(ctx, r); err != nil {
		return nil, err
	}
	return &authv1.ValidateTokenResponse{AccountId: 42, Username: "cyb"}, nil
}

func (f *fakeServices) ListCharacters(ctx context.Context, r *characterv1.ListCharactersRequest) (*characterv1.ListCharactersResponse, error) {
	if err := f.record(ctx, r); err != nil {
		return nil, err
	}
	return &characterv1.ListCharactersResponse{Characters: []*characterv1.Character{{Id: "a"}, {Id: "b"}}}, nil
}

func (f *fakeServices) GetCharacter(ctx context.Context, r *characterv1.GetCharacterRequest) (*characterv1.GetCharacterResponse, error) {
	if err := f.record(ctx, r); err != nil {
		return nil, err
	}
	return &characterv1.GetCharacterResponse{Character: &characterv1.Character{Id: r.GetId(), AccountId: 42}}, nil
}

func (f *fakeServices) CreateCharacter(ctx context.Context, r *characterv1.CreateCharacterRequest) (*characterv1.CreateCharacterResponse, error) {
	if err := f.record(ctx, r); err != nil {
		return nil, err
	}
	return &characterv1.CreateCharacterResponse{Character: &characterv1.Character{Id: "new", Name: r.GetName()}}, nil
}

func (f *fakeServices) DeleteCharacter(ctx context.Context, r *characterv1.DeleteCharacterRequest) (*characterv1.DeleteCharacterResponse, error) {
	if err := f.record(ctx, r); err != nil {
		return nil, err
	}
	return &characterv1.DeleteCharacterResponse{}, nil
}

func (f *fakeServices) Place(ctx context.Context, r *meshv1.PlaceRequest) (*meshv1.PlaceResponse, error) {
	if err := f.record(ctx, r); err != nil {
		return nil, err
	}
	return &meshv1.PlaceResponse{ServerId: 60, Address: "127.0.0.1:7777"}, nil
}

// serve registers services on an in-memory gRPC server and returns a
// connection to it.
func serve(t *testing.T, register func(*grpc.Server)) *grpc.ClientConn {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	register(srv)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///fakes",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func startFakes(t *testing.T, f *fakeServices) *grpc.ClientConn {
	t.Helper()
	return serve(t, func(s *grpc.Server) {
		authv1.RegisterAuthServiceServer(s, f)
		characterv1.RegisterCharacterServiceServer(s, f)
		meshv1.RegisterMeshServiceServer(s, f)
	})
}

// fakeIssuer is the ticket store as the issuer adapter sees it.
type fakeIssuer struct {
	block bool
	got   issued
	left  time.Duration
}

type issued struct {
	accountID   int64
	characterID string
	serverID    uint32
}

func (f *fakeIssuer) Issue(ctx context.Context, accountID int64, characterID string, serverID uint32) (string, error) {
	f.got = issued{accountID, characterID, serverID}
	f.left = -1
	if d, ok := ctx.Deadline(); ok {
		f.left = time.Until(d)
	}
	if f.block {
		<-ctx.Done()
		return "", ctx.Err()
	}
	return "7bc658710f9d3f333441c089d974dece", nil
}

// aboutCallTimeout reports whether a call that had left of its deadline was
// given callTimeout (5 s).
func aboutCallTimeout(left time.Duration) bool { return left > 4*time.Second && left <= 5*time.Second }

// (C_Y_B ~ C O A)

// Each adapter sends the request the service expects, returns its answer,
// and gives the call its own callTimeout.
func TestAdaptersPassRequestsAndBoundEachCall(t *testing.T) {
	f := &fakeServices{}
	conn := startFakes(t, f)
	a := accounts{authv1.NewAuthServiceClient(conn)}
	c := characters{characterv1.NewCharacterServiceClient(conn)}
	p := placer{meshv1.NewMeshServiceClient(conn)}
	ctx := context.Background() // no deadline of its own: the adapter must add one

	check := func(name string, want proto.Message) {
		t.Helper()
		f.mu.Lock()
		defer f.mu.Unlock()
		if !proto.Equal(f.last, want) {
			t.Errorf("%s sent %v, want %v", name, f.last, want)
		}
		if !aboutCallTimeout(f.left) {
			t.Errorf("%s: the call had %v left, want about 5s", name, f.left)
		}
	}

	acc, err := a.ValidateToken(ctx, "tok")
	if err != nil || acc != (ics.Account{ID: 42, Username: "cyb"}) {
		t.Errorf("ValidateToken = %+v %v", acc, err)
	}
	check("ValidateToken", &authv1.ValidateTokenRequest{Token: "tok"})

	list, err := c.List(ctx, 42)
	if err != nil || len(list) != 2 {
		t.Errorf("List = %v %v", list, err)
	}
	check("List", &characterv1.ListCharactersRequest{AccountId: 42})

	got, err := c.Get(ctx, "abc")
	if err != nil || got.GetId() != "abc" {
		t.Errorf("Get = %v %v", got, err)
	}
	check("Get", &characterv1.GetCharacterRequest{Id: "abc"})

	req := &characterv1.CreateCharacterRequest{AccountId: 42, Name: "Zed", ClassName: "Bard", Level: 1}
	made, err := c.Create(ctx, req)
	if err != nil || made.GetName() != "Zed" {
		t.Errorf("Create = %v %v", made, err)
	}
	check("Create", req)

	if err := c.Delete(ctx, 42, "abc"); err != nil {
		t.Errorf("Delete = %v", err)
	}
	check("Delete", &characterv1.DeleteCharacterRequest{Id: "abc", AccountId: 42})

	id, addr, err := p.Place(ctx, "abc")
	if err != nil || id != 60 || addr != "127.0.0.1:7777" {
		t.Errorf("Place = %d %q %v", id, addr, err)
	}
	check("Place", &meshv1.PlaceRequest{CharacterId: "abc"})

	store := &fakeIssuer{}
	ticket, err := issuer{store}.Issue(ctx, 42, "abc", 60)
	if err != nil || ticket != "7bc658710f9d3f333441c089d974dece" || store.got != (issued{42, "abc", 60}) {
		t.Errorf("Issue = %q %v; the store got %+v", ticket, err, store.got)
	}
	// A paused database would otherwise hold Play, and the stream's
	// keep-alives, without end.
	if !aboutCallTimeout(store.left) {
		t.Errorf("Issue: the call had %v left, want about 5s", store.left)
	}
}

// A service that hangs costs the stream callTimeout at most, and the stream's
// own end (the client going away) ends the call at once.
func TestAdapterCallsEndWithTheStream(t *testing.T) {
	f := &fakeServices{block: true}
	conn := startFakes(t, f)
	a := accounts{authv1.NewAuthServiceClient(conn)}
	c := characters{characterv1.NewCharacterServiceClient(conn)}
	p := placer{meshv1.NewMeshServiceClient(conn)}
	for _, tc := range []struct {
		name string
		call func(context.Context) error
		want codes.Code
	}{
		{"ValidateToken", func(ctx context.Context) error { _, err := a.ValidateToken(ctx, "t"); return err }, codes.Canceled},
		{"List", func(ctx context.Context) error { _, err := c.List(ctx, 42); return err }, codes.Canceled},
		{"Get", func(ctx context.Context) error { _, err := c.Get(ctx, "x"); return err }, codes.Canceled},
		{"Create", func(ctx context.Context) error {
			_, err := c.Create(ctx, &characterv1.CreateCharacterRequest{})
			return err
		}, codes.Canceled},
		{"Delete", func(ctx context.Context) error { return c.Delete(ctx, 42, "x") }, codes.Canceled},
		// Anything but mesh's own "no world server" is a failure to place.
		{"Place", func(ctx context.Context) error { _, _, err := p.Place(ctx, "x"); return err }, codes.Internal},
	} {
		ctx, cancel := context.WithCancel(context.Background())
		f.mu.Lock()
		f.started = make(chan struct{})
		started := f.started
		f.mu.Unlock()
		go func() { <-started; cancel() }()
		errc := make(chan error, 1)
		go func() { errc <- tc.call(ctx) }()
		select {
		case err := <-errc:
			if status.Code(err) != tc.want {
				t.Errorf("%s on a stream that ended = %v, want %v", tc.name, err, tc.want)
			}
		case <-time.After(2 * time.Second):
			t.Errorf("%s still running 2s after its stream ended", tc.name)
		}
		cancel()
	}

	ctx, cancel := context.WithCancel(context.Background())
	errc := make(chan error, 1)
	go func() { _, err := (issuer{&fakeIssuer{block: true}}).Issue(ctx, 42, "x", 60); errc <- err }()
	cancel()
	select {
	case err := <-errc:
		if !errors.Is(err, context.Canceled) {
			t.Errorf("Issue on a stream that ended = %v, want context.Canceled", err)
		}
	case <-time.After(2 * time.Second):
		t.Error("Issue still running 2s after its stream ended")
	}
}

// C.O.A and C | Y | B

// Each connection goes to its own service.
func TestDialServicesTargets(t *testing.T) {
	c, err := dialServices(config.Config{AuthGRPCAddr: "127.0.0.1:1", CharacterGRPCAddr: "127.0.0.1:2", MeshGRPCAddr: "127.0.0.1:3"}, testToken)
	if err != nil {
		t.Fatal(err)
	}
	defer c.close()
	if c.auth.Target() != "127.0.0.1:1" || c.character.Target() != "127.0.0.1:2" || c.mesh.Target() != "127.0.0.1:3" {
		t.Errorf("targets %s %s %s", c.auth.Target(), c.character.Target(), c.mesh.Target())
	}
}

// tcpFakes serves fakeServices behind the token check on a loopback port.
func tcpFakes(t *testing.T, token svcauth.Token) string {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeServices{}
	srv := grpc.NewServer(token.ServerOptions()...)
	authv1.RegisterAuthServiceServer(srv, f)
	characterv1.RegisterCharacterServiceServer(srv, f)
	meshv1.RegisterMeshServiceServer(srv, f)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return lis.Addr().String()
}

// Every connection dialServices makes sends the service token.
func TestDialServicesSendTheToken(t *testing.T) {
	addr := tcpFakes(t, testToken)
	cfg := config.Config{AuthGRPCAddr: addr, CharacterGRPCAddr: addr, MeshGRPCAddr: addr}
	ctx := context.Background()

	c, err := dialServices(cfg, testToken)
	if err != nil {
		t.Fatal(err)
	}
	defer c.close()
	if _, err := (accounts{authv1.NewAuthServiceClient(c.auth)}).ValidateToken(ctx, "t"); err != nil {
		t.Errorf("auth with the token: %v, want OK", err)
	}
	if _, err := (characters{characterv1.NewCharacterServiceClient(c.character)}).List(ctx, 42); err != nil {
		t.Errorf("character with the token: %v, want OK", err)
	}
	if _, _, err := (placer{meshv1.NewMeshServiceClient(c.mesh)}).Place(ctx, "abc"); err != nil {
		t.Errorf("mesh with the token: %v, want OK", err)
	}

	other, err := dialServices(cfg, svcauth.Token(strings.Repeat("f", 64)))
	if err != nil {
		t.Fatal(err)
	}
	defer other.close()
	if _, err := (accounts{authv1.NewAuthServiceClient(other.auth)}).ValidateToken(ctx, "t"); status.Code(err) != codes.Unauthenticated {
		t.Errorf("auth with another token: %v, want Unauthenticated", err)
	}
}

// acceptAndClose counts the connections made to a port that accepts each one
// and hangs up.
func acceptAndClose(t *testing.T) (string, *atomic.Int32) {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { lis.Close() })
	var n atomic.Int32
	go func() {
		for {
			c, err := lis.Accept()
			if err != nil {
				return
			}
			n.Add(1)
			c.Close()
		}
	}()
	return lis.Addr().String(), &n
}

// After a failed connection the gateway tries again within 200 ms, then 320,
// 512 and 819 ms: at least four attempts in the first 2 s, even with the 20 %
// jitter. grpc-go's default backoff (1 s, then 1.6 s) makes two.
func TestDialServicesRetryWithinSeconds(t *testing.T) {
	addr, attempts := acceptAndClose(t)
	c, err := dialServices(config.Config{AuthGRPCAddr: addr, CharacterGRPCAddr: "127.0.0.1:1", MeshGRPCAddr: "127.0.0.1:1"}, testToken)
	if err != nil {
		t.Fatal(err)
	}
	defer c.close()
	c.auth.Connect()
	deadline := time.Now().Add(2 * time.Second)
	for attempts.Load() < 4 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if n := attempts.Load(); n < 4 {
		t.Errorf("%d connection attempts in 2 s, want at least 4 (200 ms backoff)", n)
	}
}

// silent accepts connections and never speaks: a service that is up but slow
// to answer the HTTP/2 preface.
func silent(t *testing.T) string {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	var (
		mu    sync.Mutex
		conns []net.Conn
		done  = make(chan struct{})
	)
	go func() {
		defer close(done)
		for {
			c, err := lis.Accept()
			if err != nil {
				return
			}
			mu.Lock()
			conns = append(conns, c)
			mu.Unlock()
		}
	}()
	t.Cleanup(func() {
		lis.Close()
		<-done
		mu.Lock()
		defer mu.Unlock()
		for _, c := range conns {
			c.Close()
		}
	})
	return lis.Addr().String()
}

// [Cyb + C-O-A]

// A service that is up but slow to answer the HTTP/2 preface still gets the
// call's callTimeout. Left at its default, MinConnectTimeout gives a connection
// attempt only the backoff delay (200 ms the first time), and the call in
// flight ends Unavailable at once instead of waiting.
func TestDialServicesWaitForASlowService(t *testing.T) {
	c, err := dialServices(config.Config{AuthGRPCAddr: silent(t), CharacterGRPCAddr: "127.0.0.1:1", MeshGRPCAddr: "127.0.0.1:1"}, testToken)
	if err != nil {
		t.Fatal(err)
	}
	defer c.close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	errc := make(chan error, 1)
	go func() {
		_, err := (accounts{authv1.NewAuthServiceClient(c.auth)}).ValidateToken(ctx, "t")
		errc <- err
	}()
	select {
	case err := <-errc:
		t.Fatalf("the call ended within 1 s: %v, want it still waiting for the service", err)
	case <-time.After(time.Second):
	}
	cancel()
	select {
	case err := <-errc:
		if status.Code(err) != codes.Canceled {
			t.Errorf("the call ended with %v, want Canceled", err)
		}
	case <-time.After(2 * time.Second):
		t.Error("the call still running 2 s after its context ended")
	}
}

// meshStore answers Place with err.
type meshStore struct{ err error }

func (m meshStore) Register(context.Context, string, []string) (uint32, error) { return 0, m.err }
func (m meshStore) Heartbeat(context.Context, uint32) error                    { return m.err }
func (m meshStore) Place(context.Context, string) (uint32, string, error)      { return 0, "", m.err }
func (m meshStore) HeartbeatTTL() time.Duration                                { return 15 * time.Second }

// closedPort is an address nothing listens on.
func closedPort(t *testing.T) string {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := lis.Addr().String()
	lis.Close()
	return addr
}

// grpc-go answers Unavailable when mesh is not listening, the code mesh
// itself uses for "no live world server": only mesh's own answer reaches ics
// as Unavailable (51, PlayReservationNoServer, for the player); everything else is Internal (22).
func TestPlaceTellsNoWorldServerFromMeshDown(t *testing.T) {
	down, err := grpc.NewClient(closedPort(t), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer down.Close()
	_, _, err = placer{meshv1.NewMeshServiceClient(down)}.Place(context.Background(), "abc")
	if status.Code(err) != codes.Internal || !strings.Contains(status.Convert(err).Message(), "mesh unreachable") {
		t.Errorf("Place with mesh down = %v, want Internal, mesh unreachable", err)
	}

	for _, tc := range []struct {
		name  string
		store error
		want  codes.Code
	}{
		{"no world server", mesh.ErrNoServer, codes.Unavailable},
		{"store failure", errors.New("database down"), codes.Internal},
	} {
		conn := serve(t, func(s *grpc.Server) { meshv1.RegisterMeshServiceServer(s, meshgrpc.New(meshStore{tc.store})) })
		_, _, err := placer{meshv1.NewMeshServiceClient(conn)}.Place(context.Background(), "abc")
		if status.Code(err) != tc.want {
			t.Errorf("%s: Place = %v, want %v", tc.name, err, tc.want)
		}
	}

	// A bare Unavailable from a mesh that answers is not "no world server" either.
	bare := serve(t, func(s *grpc.Server) { meshv1.RegisterMeshServiceServer(s, bareUnavailable{}) })
	if _, _, err := (placer{meshv1.NewMeshServiceClient(bare)}).Place(context.Background(), "abc"); status.Code(err) != codes.Internal {
		t.Errorf("Place answered a bare Unavailable = %v, want Internal", err)
	}
}

// [C.O.A x Cyb]

type bareUnavailable struct {
	meshv1.UnimplementedMeshServiceServer
}

func (bareUnavailable) Place(context.Context, *meshv1.PlaceRequest) (*meshv1.PlaceResponse, error) {
	return nil, status.Error(codes.Unavailable, "no live world server")
}

type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

// captureLog sends slog's output to the returned buffer until the test ends,
// then restores the default logger and the log package's output and flags,
// which slog.SetDefault also changes.
func captureLog(t *testing.T) *syncBuffer {
	t.Helper()
	buf := &syncBuffer{}
	prev, out, flags := slog.Default(), log.Writer(), log.Flags()
	slog.SetDefault(slog.New(slog.NewTextHandler(buf, nil)))
	t.Cleanup(func() {
		slog.SetDefault(prev)
		log.SetOutput(out)
		log.SetFlags(flags)
	})
	return buf
}

// The whole path with mesh not running: the player gets 22, not 51 (no
// world server), and the operator reads that mesh is unreachable.
func TestPlayWithMeshDown(t *testing.T) {
	logs := captureLog(t)
	conn := startFakes(t, &fakeServices{})
	meshConn, err := grpc.NewClient(closedPort(t), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer meshConn.Close()
	cfg := ics.DefaultConfig()
	cfg.PlayPause = 0
	cc := serve(t, func(s *grpc.Server) {
		icsxclient.RegisterXClientServiceServer(s, ics.New(ics.Deps{
			Accounts:   accounts{authv1.NewAuthServiceClient(conn)},
			Characters: characters{characterv1.NewCharacterServiceClient(conn)},
			Placer:     placer{meshv1.NewMeshServiceClient(meshConn)},
		}, cfg))
	})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	st, err := icsxclient.NewXClientServiceClient(cc).ProcessXClientMessage(ctx)
	if err != nil {
		t.Fatal(err)
	}
	send := func(typeName string, body proto.Message, tags map[string]string) *icscommon.MessageWrapper {
		t.Helper()
		data, _ := proto.Marshal(body)
		if err := st.Send(&icscommon.MessageWrapper{MessageTypeName: typeName, MessageData: data, SystemData: &icscommon.SystemData{Tags: tags}}); err != nil {
			t.Fatal(err)
		}
		m, err := st.Recv()
		if err != nil {
			t.Fatal(err)
		}
		return m
	}
	if m := send("ics_common.SessionRequest", &icscommon.SessionRequest{RequestType: icscommon.SessionRequestType_SESSION_REQUEST_TYPE_OPEN},
		map[string]string{"auth_token": "t"}); m.GetStatusCode() != icscommon.IcsStatusCode_ICS_STATUS_CODE_SUCCESS {
		t.Fatalf("session: %v", m.GetStatusCode())
	}
	if m := send("ics_xclient.PlayRequest", &icsxclient.PlayRequest{CharacterId: "0123456789abcdef0123456789abcdef"}, nil); m.GetStatusCode() != icscommon.IcsStatusCode_ICS_STATUS_CODE_ACKNOWLEDGED {
		t.Fatalf("first PlayReply: %v", m.GetStatusCode())
	}
	m, err := st.Recv()
	if err != nil {
		t.Fatal(err)
	}
	if m.GetStatusCode() != icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_INTERNAL_ERROR {
		t.Errorf("PlayReply with mesh down: %v, want 22 (InternalError)", m.GetStatusCode())
	}
	if !strings.Contains(logs.String(), `level=ERROR msg="ics: placement failed"`) || !strings.Contains(logs.String(), "mesh unreachable") ||
		strings.Contains(logs.String(), "no world server") {
		t.Errorf("log with mesh down:\n%s", logs.String())
	}
}
