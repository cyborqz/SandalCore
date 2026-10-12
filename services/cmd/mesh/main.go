// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// mesh serves aoc.mesh.v1.MeshService: the registry of world servers and
// where characters play.
package main

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"

	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/mesh"
	"aocrop/services/internal/meshgrpc"
	"aocrop/services/internal/svcauth"
)

func main() {
	if err := run(); err != nil {
		slog.Error("mesh stopped", "err", err)
		os.Exit(1)
	}
}

// C-O-A.C.Y.B

func run() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := config.Load()
	if err := checkStaticWorld(cfg.MeshStaticWorld); err != nil {
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

	lis, err := net.Listen("tcp", cfg.MeshGRPCAddr)
	if err != nil {
		return err
	}
	srv := newServer(token, mesh.NewStore(pool, cfg.MeshHeartbeatTTL, cfg.MeshStaticWorld))

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(lis) }()
	slog.Info("mesh listening", "grpc", lis.Addr().String(), "static_world", cfg.MeshStaticWorld,
		"heartbeat_ttl", cfg.MeshHeartbeatTTL)

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	// Drain in-flight RPCs, but not forever: one stuck on an unreachable
	// database would otherwise hold shutdown.
	force := time.AfterFunc(10*time.Second, srv.Stop)
	srv.GracefulStop()
	force.Stop()
	return err
}

// newServer is the gRPC server the gateway and world servers call. It
// requires the service token.
func newServer(token svcauth.Token, store meshgrpc.Store) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	meshv1.RegisterMeshServiceServer(s, meshgrpc.New(store))
	return s
}

// checkStaticWorld checks AOC_MESH_STATIC_WORLD ("" for none), whose address
// Place hands to the client as it is.
func checkStaticWorld(addr string) error {
	if addr == "" {
		return nil
	}
	if err := mesh.ValidAddress(addr); err != nil {
		return fmt.Errorf("AOC_MESH_STATIC_WORLD %q is not an address a game client can dial: %w", addr, err)
	}
	return nil
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
