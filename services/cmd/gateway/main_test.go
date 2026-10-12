// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/gatewaygrpc"
	"aocrop/services/internal/ics"
	"aocrop/services/internal/svcauth"
	"aocrop/services/internal/tickets"
)

// CoA captures, C-Y-B code

// selfSigned writes a certificate for IP 127.0.0.1 (like certs/server.pem)
// and its key, and returns their paths and a pool that trusts it.
func selfSigned(t *testing.T) (certFile, keyFile string, pool *x509.CertPool) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "test gateway"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(time.Hour),
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1")},
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IsCA:                  true,
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	certFile, keyFile = filepath.Join(dir, "server.pem"), filepath.Join(dir, "server.key")
	if err := os.WriteFile(certFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0o600); err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	pool = x509.NewCertPool()
	pool.AddCert(cert)
	return certFile, keyFile, pool
}

// startICS serves an ICS server with the certificate in certFile and
// keyFile on a loopback port, and returns the server and its address.
func startICS(t *testing.T, certFile, keyFile string) (*grpc.Server, string) {
	t.Helper()
	creds, err := icsCredentials(certFile, keyFile)
	if err != nil {
		t.Fatal(err)
	}
	srv := newICSServer(creds, ics.New(ics.Deps{}, ics.DefaultConfig()))
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return srv, lis.Addr().String()
}

// keepAlive opens an ICS stream to addr and has one keep-alive answered on
// it, so the stream is live on the server.
func keepAlive(t *testing.T, addr string, pool *x509.CertPool) icsxclient.XClientService_ProcessXClientMessageClient {
	t.Helper()
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(credentials.NewTLS(&tls.Config{RootCAs: pool})))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	t.Cleanup(cancel)
	stream, err := icsxclient.NewXClientServiceClient(conn).ProcessXClientMessage(ctx)
	if err != nil {
		t.Fatal(err)
	}
	data, err := proto.Marshal(&icscommon.KeepAliveMessage{TimestampMsec: 1})
	if err != nil {
		t.Fatal(err)
	}
	if err := stream.Send(&icscommon.MessageWrapper{MessageTypeName: "ics_common.KeepAliveMessage", MessageData: data}); err != nil {
		t.Fatal(err)
	}
	m, err := stream.Recv()
	if err != nil || m.GetMessageTypeName() != "ics_common.KeepAliveMessage" {
		t.Fatalf("Recv = %v %v, want a KeepAliveMessage", m, err)
	}
	return stream
}

// The client reaches the stream over TLS (with ALPN h2, which grpc-go does)
// on an IP-literal address, as it does at 127.0.0.1:443.
func TestICSServerSpeaksTLSAndStreams(t *testing.T) {
	certFile, keyFile, pool := selfSigned(t)
	_, addr := startICS(t, certFile, keyFile)
	keepAlive(t, addr, pool)
}

func TestICSServerNeedsItsCertificate(t *testing.T) {
	dir := t.TempDir()
	_, err := icsCredentials(filepath.Join(dir, "missing.pem"), filepath.Join(dir, "missing.key"))
	if err == nil || !strings.Contains(err.Error(), "certificate") {
		t.Errorf("icsCredentials with missing files = %v, want an error about the certificate", err)
	}
}

// The key is missing (as in every worktree): a clear error naming it.
func TestICSServerNeedsItsKey(t *testing.T) {
	certFile, _, _ := selfSigned(t)
	creds, err := icsCredentials(certFile, filepath.Join(t.TempDir(), "server.key"))
	if creds != nil || err == nil || !strings.Contains(err.Error(), "server.key") {
		t.Errorf("icsCredentials without the key = %v %v", creds, err)
	}
}

// ~ C:Y:B & C.O.A ~

// grpc-go logs a failed TLS handshake only at its own INFO level, which is
// off: a client without ALPN h2 (refused since grpc-go 1.67), with an old TLS
// version or rejecting the certificate would otherwise leave no trace.
func TestTLSHandshakeFailuresAreLogged(t *testing.T) {
	logs := captureLog(t)
	certFile, keyFile, pool := selfSigned(t)
	_, addr := startICS(t, certFile, keyFile)

	// h2 is fine, and not logged.
	keepAlive(t, addr, pool)
	if strings.Contains(logs.String(), "handshake failed") {
		t.Errorf("a good handshake was logged:\n%s", logs.String())
	}

	// waitFor waits up to 2 s for the log to contain want.
	waitFor := func(want string) string {
		deadline := time.Now().Add(2 * time.Second)
		for !strings.Contains(logs.String(), want) && time.Now().Before(deadline) {
			time.Sleep(10 * time.Millisecond)
		}
		return logs.String()
	}
	const failed = `level=WARN msg="ics: TLS handshake failed" remote=127.0.0.1:`

	// A client that does not speak TLS at all.
	raw, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	raw.Write([]byte("GET / HTTP/1.1\r\n\r\n"))
	raw.SetReadDeadline(time.Now().Add(2 * time.Second))
	raw.Read(make([]byte, 1)) // until the server closes the connection
	raw.Close()
	if got := waitFor(failed); !strings.Contains(got, failed) {
		t.Errorf("log after a client that does not speak TLS:\n%s", got)
	}

	// A client that offers no ALPN, unless grpc-go was told not to insist
	// (GRPC_ENFORCE_ALPN_ENABLED=false, the fallback for such a client).
	if strings.EqualFold(os.Getenv("GRPC_ENFORCE_ALPN_ENABLED"), "false") {
		t.Log("GRPC_ENFORCE_ALPN_ENABLED=false: a client without ALPN is accepted")
		return
	}
	c, err := tls.DialWithDialer(&net.Dialer{Timeout: 2 * time.Second}, "tcp", addr, &tls.Config{RootCAs: pool})
	if err == nil {
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		c.Read(make([]byte, 1))
		c.Close()
	}
	if got := waitFor("ALPN"); !strings.Contains(got, failed) || !strings.Contains(got, "ALPN") {
		t.Errorf("log after a handshake without ALPN:\n%s", got)
	}
}

// hangingRedeemer holds each RedeemTicket until its call ends.
type hangingRedeemer struct{ started chan struct{} }

func (h hangingRedeemer) Redeem(ctx context.Context, _, _ string, _ uint32) (int64, error) {
	h.started <- struct{}{}
	<-ctx.Done()
	return 0, ctx.Err()
}

// startInternal serves newInternalServer(store) on a loopback port.
func startInternal(t *testing.T, store gatewaygrpc.Tickets) (*grpc.Server, string) {
	t.Helper()
	srv := newInternalServer(store, testToken)
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return srv, lis.Addr().String()
}

// refuses reports whether nothing accepts connections at addr any more.
func refuses(addr string) bool {
	c, err := net.DialTimeout("tcp", addr, time.Second)
	if err == nil {
		c.Close()
	}
	return err != nil
}

// The game client's ICS stream never ends by itself, nor does a RedeemTicket
// stuck on the database: shutdown still returns after the grace, and both
// are cut.
func TestShutdownCutsLiveStreams(t *testing.T) {
	certFile, keyFile, pool := selfSigned(t)
	icsSrv, addr := startICS(t, certFile, keyFile)
	started := make(chan struct{}, 1)
	internal, internalAddr := startInternal(t, hangingRedeemer{started})
	stream := keepAlive(t, addr, pool)

	cc, err := grpc.NewClient(internalAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), testToken.DialOption())
	if err != nil {
		t.Fatal(err)
	}
	defer cc.Close()
	redeemed := make(chan error, 1)
	go func() {
		_, err := gatewayv1.NewGatewayServiceClient(cc).RedeemTicket(context.Background(), &gatewayv1.RedeemTicketRequest{})
		redeemed <- err
	}()
	<-started

	done := make(chan struct{})
	go func() { shutdown(200*time.Millisecond, icsSrv, internal); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("shutdown still waiting for live calls after 3s")
	}
	if _, err := stream.Recv(); err == nil {
		t.Error("the ICS stream outlived shutdown")
	}
	select {
	case err := <-redeemed:
		if err == nil {
			t.Error("the stuck RedeemTicket succeeded")
		}
	case <-time.After(2 * time.Second):
		t.Error("the stuck RedeemTicket outlived shutdown")
	}
	if !refuses(addr) || !refuses(internalAddr) {
		t.Error("a server still accepts connections after shutdown")
	}
}

// With nothing in flight, shutdown does not wait for the grace, and stops
// every server.
func TestShutdownOfIdleServersIsPrompt(t *testing.T) {
	certFile, keyFile, _ := selfSigned(t)
	icsSrv, addr := startICS(t, certFile, keyFile)
	internal, internalAddr := startInternal(t, tickets.NewStore(nil, 2*time.Minute))
	done := make(chan struct{})
	go func() { shutdown(time.Minute, icsSrv, internal); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("shutdown of idle servers still running after 3s")
	}
	if !refuses(addr) || !refuses(internalAddr) {
		t.Error("a server still accepts connections after shutdown")
	}
}

// ~ C | Y | B :: CoA ~

// World servers reach RedeemTicket on the internal server, with the service
// token. Server 0 (the static world) is refused before the store touches the
// database, so no pool is needed.
func TestInternalServerServesRedeemTicket(t *testing.T) {
	srv := newInternalServer(tickets.NewStore(nil, 2*time.Minute), testToken)
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	redeem := func(opts ...grpc.DialOption) error {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
		cc, err := grpc.NewClient(lis.Addr().String(), opts...)
		if err != nil {
			t.Fatal(err)
		}
		defer cc.Close()
		_, err = gatewayv1.NewGatewayServiceClient(cc).RedeemTicket(context.Background(), &gatewayv1.RedeemTicketRequest{})
		return err
	}
	if err := redeem(testToken.DialOption()); status.Code(err) != codes.PermissionDenied {
		t.Errorf("RedeemTicket = %v, want PermissionDenied from the ticket store", err)
	}
	if err := redeem(); status.Code(err) != codes.Unauthenticated {
		t.Errorf("RedeemTicket without the service token = %v, want Unauthenticated", err)
	}
}

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// RedeemTicket has no authentication and the ticket crosses the network in
// clear text in the client's UDP handshake: the internal server binds
// loopback only, unless AOC_GATEWAY_GRPC_PUBLIC=1 says otherwise.
func TestInternalBindMustBeLoopback(t *testing.T) {
	for _, addr := range []string{"127.0.0.1:50054", "127.1.2.3:1", "[::1]:50054", "localhost:50054", "LocalHost:1"} {
		if err := checkInternalBind(addr, false); err != nil {
			t.Errorf("checkInternalBind(%q) = %v, want nil", addr, err)
		}
	}
	public := []string{"0.0.0.0:50054", ":50054", "[::]:50054", "192.0.2.5:50054", "198.51.100.2:1", "gateway.lan:50054", "[fe80::1%eth0]:1"}
	for _, addr := range public {
		err := checkInternalBind(addr, false)
		if err == nil || !strings.Contains(err.Error(), "AOC_GATEWAY_GRPC_PUBLIC=1") {
			t.Errorf("checkInternalBind(%q) = %v, want a refusal naming AOC_GATEWAY_GRPC_PUBLIC=1", addr, err)
		}
	}
	if err := checkInternalBind("no-port", true); err == nil {
		t.Error("checkInternalBind(no-port) = nil, want an error")
	}

	logs := captureLog(t)
	if err := checkInternalBind("127.0.0.1:50054", true); err != nil || logs.String() != "" {
		t.Errorf("loopback with the flag = %v, log %q; want nil and no warning", err, logs.String())
	}
	for _, addr := range public {
		if err := checkInternalBind(addr, true); err != nil {
			t.Errorf("checkInternalBind(%q) with the flag = %v, want nil", addr, err)
		}
	}
	if n := strings.Count(logs.String(), "level=WARN"); n != len(public) || !strings.Contains(logs.String(), "no TLS") {
		t.Errorf("%d warnings for %d public addresses:\n%s", n, len(public), logs.String())
	}
}

// -- C.O.A & C-Y-B --

// run checks what it can before it reaches the database, so a gateway that
// cannot serve never runs the migrations: the internal bind, then the TLS
// files. (The database is a port nothing listens on.)
func TestRunChecksBeforeTheDatabase(t *testing.T) {
	certFile, keyFile, _ := selfSigned(t)
	missingKey := filepath.Join(t.TempDir(), "server.key")
	for _, tc := range []struct {
		name, grpcAddr, key, want string
	}{
		{"public internal address", "0.0.0.0:50054", missingKey, "AOC_GATEWAY_GRPC_PUBLIC"},
		{"missing key", "127.0.0.1:0", missingKey, "server.key"},
		{"missing service token", "127.0.0.1:0", keyFile, "service token"},
	} {
		t.Setenv("AOC_DATABASE_URL", "postgres://aoc:aoc@"+closedPort(t)+"/aoc?sslmode=disable&connect_timeout=2")
		t.Setenv("AOC_GATEWAY_GRPC_ADDR", tc.grpcAddr)
		t.Setenv("AOC_GATEWAY_GRPC_PUBLIC", "")
		t.Setenv("AOC_GATEWAY_TLS_CERT", certFile)
		t.Setenv("AOC_GATEWAY_TLS_KEY", tc.key)
		t.Setenv("AOC_SERVICE_TOKEN_FILE", filepath.Join(t.TempDir(), "service.token"))
		if err := run(); err == nil || !strings.Contains(err.Error(), tc.want) || strings.Contains(err.Error(), "database") {
			t.Errorf("%s: run() = %v, want an error about %s before the database", tc.name, err, tc.want)
		}
	}
}
