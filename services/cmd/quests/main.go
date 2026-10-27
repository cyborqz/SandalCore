// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// quests serves aoc.quest.v1.QuestService, the saved quest log of each character (quests milestone, slice 1): the quests it took, the ones it finished,
// its quest variables, and the inbox of GM commands for it. worldd and questctl call it.
//
//	quests
//
// It uses the same database as the other services (AOC_DATABASE_URL; migration 0016 makes its tables) and the same service token
// (AOC_SERVICE_TOKEN_FILE). AOC_QUESTS_GRPC_ADDR (default 127.0.0.1:50058) sets the address.
package main

import (
	"context"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"

	questv1 "aocrop/services/gen/aoc/quest/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/quests"
	"aocrop/services/internal/questsgrpc"
	"aocrop/services/internal/svcauth"
)

func main() {
	if err := run(); err != nil {
		slog.Error("quests stopped", "err", err)
		os.Exit(1)
	}
}

func run() error {
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

	lis, err := net.Listen("tcp", cfg.QuestsGRPCAddr)
	if err != nil {
		return err
	}
	srv := newServer(token, quests.New(pool))

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(lis) }()
	slog.Info("quests listening", "grpc", lis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	force := time.AfterFunc(10*time.Second, srv.Stop)
	srv.GracefulStop()
	force.Stop()
	return err
}

// newServer is the gRPC server worldd and questctl call. It requires the service token.
func newServer(token svcauth.Token, svc *quests.Service) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	questv1.RegisterQuestServiceServer(s, questsgrpc.New(svc))
	return s
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
