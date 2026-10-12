// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// auth serves the launcher's login API (HTTP) and token validation (gRPC).
package main

import (
	"context"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	"aocrop/services/internal/account"
	"aocrop/services/internal/authgrpc"
	"aocrop/services/internal/authhttp"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/svcauth"
)

func main() {
	if err := run(); err != nil {
		slog.Error("auth stopped", "err", err)
		os.Exit(1)
	}
}

// [C | O | A + C Y B]

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
	store := account.NewStore(pool, cfg.TokenTTL)

	// Bind both ports before saying "listening", so a taken port fails here.
	httpLis, err := net.Listen("tcp", cfg.AuthHTTPAddr)
	if err != nil {
		return err
	}
	grpcLis, err := net.Listen("tcp", cfg.AuthGRPCAddr)
	if err != nil {
		httpLis.Close()
		return err
	}
	httpSrv := &http.Server{
		Handler:           authhttp.Handler(store),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
	grpcSrv := newGRPCServer(token, store)

	errc := make(chan error, 2)
	go func() { errc <- httpSrv.Serve(httpLis) }()
	go func() { errc <- grpcSrv.Serve(grpcLis) }()
	go purgeTokens(ctx, store)
	slog.Info("auth listening", "http", httpLis.Addr().String(), "grpc", grpcLis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	// Drain both servers within one budget, then cut what is left. Closing
	// cancels the stuck handlers' contexts, so their database queries end
	// and the deferred pool.Close() does not wait on them.
	force := time.AfterFunc(shutdownGrace, func() { grpcSrv.Stop(); httpSrv.Close() })
	drained := make(chan struct{})
	go func() { grpcSrv.GracefulStop(); close(drained) }()
	_ = httpSrv.Shutdown(context.Background())
	<-drained
	force.Stop()
	return err
}

// newGRPCServer is the token-validation server the gateway calls. It requires
// the service token.
func newGRPCServer(token svcauth.Token, tokens authgrpc.Tokens) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	authv1.RegisterAuthServiceServer(s, authgrpc.New(tokens))
	return s
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}

const shutdownGrace = 10 * time.Second

// purgeTokens deletes expired login tokens now and then hourly until ctx ends.
func purgeTokens(ctx context.Context, store *account.Store) {
	tick := time.NewTicker(time.Hour)
	defer tick.Stop()
	for {
		n, err := store.PurgeExpiredTokens(ctx)
		switch {
		case err != nil && ctx.Err() == nil:
			slog.Warn("token purge failed", "err", err)
		case n > 0:
			slog.Info("purged expired tokens", "count", n)
		}
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
	}
}
