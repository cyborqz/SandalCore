// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// citynode serves aoc.citynode.v1.CityNodeService, the live state of the game world's city nodes (Nodes 1): each node's level,
// XP and vassal link, the node clock, and the stream of changes. worldd, nodectl and the admin app to come all call it.
//
//	citynode [--seed deploy/citynode_seed.json]
//
// It uses the same database as the other services (AOC_DATABASE_URL; migration 0009 makes its tables) and the same service token
// (AOC_SERVICE_TOKEN_FILE). At its first start it applies the seed file once, in one transaction (city_node_seed.applied_at): a
// missing or wrong seed file stops the start and nothing is applied. Later edits to the file need `nodectl seed --force`.
// AOC_CITYNODE_GRPC_ADDR (default 127.0.0.1:50055) and AOC_CITYNODE_SEED set the address and the seed file.
package main

import (
	"context"
	"flag"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/internal/citynode"
	"aocrop/services/internal/citynodegrpc"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/svcauth"
)

func main() {
	seed := flag.String("seed", config.Load().CityNodeSeed,
		"the node seed file, applied once at the first start; default $AOC_CITYNODE_SEED or deploy/citynode_seed.json")
	flag.Parse()
	if err := run(*seed); err != nil {
		slog.Error("citynode stopped", "err", err)
		os.Exit(1)
	}
}

// == C O A | C Y B ==

func run(seedPath string) error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := config.Load()
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

	svc := citynode.New(pool, citynode.DefaultRoster(), seedPath)
	if err := svc.Start(ctx); err != nil {
		return startupErr(ctx, err)
	}
	lis, err := net.Listen("tcp", cfg.CityNodeGRPCAddr)
	if err != nil {
		return err
	}
	srv := newServer(token, svc)

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(lis) }()
	slog.Info("citynode listening", "grpc", lis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	// The watch streams end first (a graceful stop waits for them), then in-flight calls drain, but not forever.
	svc.Shutdown()
	force := time.AfterFunc(10*time.Second, srv.Stop)
	srv.GracefulStop()
	force.Stop()
	return err
}

// newServer is the gRPC server worldd, nodectl and the admin app call. It requires the service token.
func newServer(token svcauth.Token, svc *citynode.Service) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	citynodev1.RegisterCityNodeServiceServer(s, citynodegrpc.New(svc))
	return s
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
