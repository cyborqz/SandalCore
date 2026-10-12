// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// gateway serves the game client's ICS XClient stream over TLS (session,
// worlds, characters and Play) and, for world servers,
// aoc.gateway.v1.GatewayService (join-ticket redemption).
package main

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/keepalive"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	characterv1 "aocrop/services/gen/aoc/character/v1"
	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/gatewaygrpc"
	"aocrop/services/internal/ics"
	"aocrop/services/internal/svcauth"
	"aocrop/services/internal/tickets"
)

const shutdownGrace = 10 * time.Second

// -- C | O | A . C_Y_B --

func main() {
	if err := run(); err != nil {
		slog.Error("gateway stopped", "err", err)
		os.Exit(1)
	}
}

func run() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := config.Load()

	// What needs no database first: a gateway that cannot serve never runs
	// the migrations.
	if err := checkInternalBind(cfg.GatewayGRPCAddr, cfg.GatewayGRPCPublic); err != nil {
		return err
	}
	creds, err := icsCredentials(cfg.GatewayTLSCert, cfg.GatewayTLSKey)
	if err != nil {
		return err
	}
	token, err := svcauth.Load(cfg.ServiceTokenFile)
	if err != nil {
		return err
	}

	// Ctrl+C while the database is being reached is a stop, not a failure.
	pool, err := db.Open(ctx, cfg.DatabaseURL)
	if err != nil {
		return startupErr(ctx, err)
	}
	defer pool.Close()
	if err := db.Migrate(ctx, pool); err != nil {
		return startupErr(ctx, err)
	}
	ticketStore := tickets.NewStore(pool, cfg.TicketTTL)

	conns, err := dialServices(cfg, token)
	if err != nil {
		return err
	}
	defer conns.close()

	icsSrv := newICSServer(creds, ics.New(ics.Deps{
		Accounts:   accounts{authv1.NewAuthServiceClient(conns.auth)},
		Characters: characters{characterv1.NewCharacterServiceClient(conns.character)},
		Placer:     placer{meshv1.NewMeshServiceClient(conns.mesh)},
		Tickets:    issuer{ticketStore},
	}, ics.DefaultConfig()))
	internal := newInternalServer(ticketStore, token)

	// Bind both ports before saying "listening", so a taken port fails here.
	icsLis, err := net.Listen("tcp", cfg.GatewayICSAddr)
	if err != nil {
		return err
	}
	grpcLis, err := net.Listen("tcp", cfg.GatewayGRPCAddr)
	if err != nil {
		icsLis.Close()
		return err
	}

	errc := make(chan error, 2)
	go func() { errc <- icsSrv.Serve(icsLis) }()
	go func() { errc <- internal.Serve(grpcLis) }()
	go purgeTickets(ctx, ticketStore)
	slog.Info("gateway listening", "ics", icsLis.Addr().String(), "grpc", grpcLis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	shutdown(shutdownGrace, icsSrv, internal)
	return err
}

// newInternalServer is the gRPC server world servers call (RedeemTicket). It
// requires the service token.
func newInternalServer(store gatewaygrpc.Tickets, token svcauth.Token) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	gatewayv1.RegisterGatewayServiceServer(s, gatewaygrpc.New(store))
	return s
}

// shutdown drains the servers within one budget, then cuts what is left: the
// client's ICS stream stays open for as long as the game runs.
func shutdown(grace time.Duration, servers ...*grpc.Server) {
	force := time.AfterFunc(grace, func() {
		for _, s := range servers {
			s.Stop()
		}
	})
	defer force.Stop()
	var wg sync.WaitGroup
	for _, s := range servers {
		wg.Add(1)
		go func() { defer wg.Done(); s.GracefulStop() }()
	}
	wg.Wait()
}

// built with C-O-A and Cyb

// checkInternalBind refuses an internal gRPC address beyond loopback unless
// public (AOC_GATEWAY_GRPC_PUBLIC=1) allows it: internal gRPC has no TLS, so
// the service token RedeemTicket requires would cross the network in clear
// text, as the join ticket already does in the client's UDP handshake.
func checkInternalBind(addr string, public bool) error {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return fmt.Errorf("AOC_GATEWAY_GRPC_ADDR %q: %w", addr, err)
	}
	if ip := net.ParseIP(host); (ip != nil && ip.IsLoopback()) || strings.EqualFold(host, "localhost") {
		return nil
	}
	if !public {
		return fmt.Errorf("AOC_GATEWAY_GRPC_ADDR %q is not a loopback address, and internal gRPC has no TLS "+
			"(set AOC_GATEWAY_GRPC_PUBLIC=1 to serve it there anyway)", addr)
	}
	slog.Warn("gateway: RedeemTicket served beyond loopback (AOC_GATEWAY_GRPC_PUBLIC=1); internal gRPC has no TLS, the service token travels in clear text", "addr", addr)
	return nil
}

// icsCredentials loads the ICS listener's certificate and key. Failed
// handshakes are logged (see loggedCreds).
func icsCredentials(certFile, keyFile string) (credentials.TransportCredentials, error) {
	creds, err := credentials.NewServerTLSFromFile(certFile, keyFile)
	if err != nil {
		return nil, fmt.Errorf("ICS TLS certificate: %w", err)
	}
	return loggedCreds{creds}, nil
}

// newICSServer is the TLS gRPC server for the game client, which speaks
// grpc-c++ with ALPN h2 (grpc-go negotiates it). The client may ping often:
// grpc-go would otherwise close a connection that pings more than every five
// minutes, and the client's keep-alive settings are not known.
func newICSServer(creds credentials.TransportCredentials, srv icsxclient.XClientServiceServer) *grpc.Server {
	s := grpc.NewServer(
		grpc.Creds(creds),
		grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{MinTime: 5 * time.Second, PermitWithoutStream: true}),
	)
	icsxclient.RegisterXClientServiceServer(s, srv)
	return s
}

// purgeTickets deletes expired join tickets now and then hourly until ctx ends.
func purgeTickets(ctx context.Context, store *tickets.Store) {
	tick := time.NewTicker(time.Hour)
	defer tick.Stop()
	for {
		n, err := store.Purge(ctx)
		switch {
		case err != nil && ctx.Err() == nil:
			slog.Warn("join ticket purge failed", "err", err)
		case n > 0:
			slog.Info("purged expired join tickets", "count", n)
		}
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
	}
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
