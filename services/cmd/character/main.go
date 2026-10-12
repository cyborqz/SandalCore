// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// character serves aoc.character.v1.CharacterService, the runtime writer of
// character records. (charimport is the offline exception: it writes them
// straight to the database, before the services run.)
//
//	character [--content-db dist/content/content.db] [--no-kit-backfill]
//
// With AOC_GM_STARTER_SET=1 (plan D1; off by default) the same file gives a new character the GM starter set (the level
// scrolls, the class scrolls and a gear scroll: internal/kit's StarterMaker) in its bag with the kit, and, at a start of the
// service, gives it once to every character that has not had it (characters.gm_starter_set_granted_at), logged per
// character; what does not fit a full bag is left out and logged.
//
// The content.db that contentc builds (AOC_CONTENT_DB, or --content-db) is what
// a new character's race, gender, class and customization are checked against
// (internal/customization). Without a usable one the service still starts and
// serves everything else, and refuses to create characters, with that reason,
// until it is restarted with one: nothing is accepted unchecked.
//
// The same file gives a new character its class's starting kit (plan B2: internal/kit), stored with the
// character, and, when the service starts, gives the kit to every character that has no items (the characters
// made before items existed: each grant is its own transaction, never touches a character that has an item, and is
// logged; --no-kit-backfill turns it off). Without a usable content.db no kit can be made: new characters are refused
// anyway and the backfill says it has nothing to give with.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/charactergrpc"
	"aocrop/services/internal/config"
	"aocrop/services/internal/content"
	"aocrop/services/internal/customization"
	"aocrop/services/internal/db"
	"aocrop/services/internal/kit"
	"aocrop/services/internal/svcauth"
)

// (CoA) (C | Y | B)

func main() {
	contentDB := flag.String("content-db", config.Load().ContentDB,
		"the content.db (contentc's output) that new characters are checked against; default $AOC_CONTENT_DB or dist/content/content.db")
	noBackfill := flag.Bool("no-kit-backfill", false,
		"do not give the starting kit to the characters that have no items when the service starts (plan B2)")
	flag.Parse()
	if err := run(*contentDB, !*noBackfill, config.Load().GMStarterSet); err != nil {
		slog.Error("character stopped", "err", err)
		os.Exit(1)
	}
}

func run(contentDB string, backfill bool, starterSet bool) error {
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

	lis, err := net.Listen("tcp", cfg.CharacterGRPCAddr)
	if err != nil {
		return err
	}
	check, kits, starter, closeContent := loadContent(contentDB, starterSet)
	defer closeContent()
	store := character.NewStore(pool)
	if backfill {
		backfillKits(ctx, store, kits)
	}
	if starterSet {
		backfillStarterSets(ctx, store, starter)
	}
	srv := newServer(token, store, check, kits, starter)

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(lis) }()
	slog.Info("character listening", "grpc", lis.Addr().String())

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

// newServer is the gRPC server the gateway and worldd call. It requires the
// service token.
func newServer(token svcauth.Token, store charactergrpc.Store, check charactergrpc.Checker, kits charactergrpc.Kits, starter charactergrpc.StarterSets) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	var opts []charactergrpc.Option
	if kits != nil {
		opts = append(opts, charactergrpc.WithKits(kits))
	}
	if starter != nil {
		opts = append(opts, charactergrpc.WithStarterSet(starter))
	}
	characterv1.RegisterCharacterServiceServer(s, charactergrpc.New(store, check, opts...))
	return s
}

// backfillKits gives the characters that have no items their class's kit (plan B2). Nothing to give with (no kit
// maker) is a warning that says how many characters are waiting; a database error stops the pass and is logged, and the
// service goes on (the pass runs again at the next start).
func backfillKits(ctx context.Context, store *character.Store, kits charactergrpc.Kits) {
	if kits == nil {
		slog.Warn("character: the starting kit backfill is skipped: there is no usable content.db to make kits from; characters without items stay as they are until the service starts with one")
		return
	}
	if _, err := store.GrantMissingKits(ctx, kits); err != nil {
		slog.Error("character: the starting kit backfill stopped on an error (characters already given their kit keep it; it runs again at the next start)", "err", err)
	}
}

// backfillStarterSets gives the characters that have not had the GM starter set theirs (plan D1, AOC_GM_STARTER_SET=1), once.
// No maker (no usable content.db) is a warning; a database error stops the pass and is logged, and the service goes on.
func backfillStarterSets(ctx context.Context, store *character.Store, sets charactergrpc.StarterSets) {
	if sets == nil {
		slog.Warn("character: the GM starter set backfill is skipped: there is no usable content.db to make the set from")
		return
	}
	if _, err := store.GrantMissingStarterSets(ctx, sets); err != nil {
		slog.Error("character: the GM starter set backfill stopped on an error (characters already given the set keep it; it runs again at the next start)", "err", err)
	}
	// the mounts milestone, stage 3: a character that had the set before it held the test mounts gets them (a top-up at every start: a test mount it holds nowhere is given again; review B M8)
	if mk, ok := sets.(character.MountSetMaker); ok {
		if _, err := store.GrantMissingStarterMounts(ctx, mk); err != nil {
			slog.Error("character: the test mounts backfill stopped on an error (it runs again at the next start)", "err", err)
		}
	}
}

// Cyb with C-O-A

// loadContent reads the rules of character creation, the starting kits and (with starterSet) the GM starter set from the
// content.db at path and returns them with the function that closes it. It never fails: when the file is missing or is not a
// content.db this service can work from, it logs why and returns a Checker that refuses every creation with that reason
// (charactergrpc.ErrUnavailable) and no kits (nil), so the rest of the service still runs.
func loadContent(path string, starterSet bool) (charactergrpc.Checker, charactergrpc.Kits, charactergrpc.StarterSets, func()) {
	db, err := content.Open(path)
	var rules *customization.Rules
	var kits *kit.Maker
	var starter *kit.StarterMaker
	if err == nil {
		// A content.db that contentc wrote before plan D1's tables is refused here, as worldd refuses it: the two services
		// must be on the same file, and the message says what to rebuild it with.
		if err = db.RequireVersion(); err == nil {
			if rules, err = customization.Load(db); err == nil {
				kits, err = kit.Load(db)
			}
		}
		if err == nil && starterSet {
			starter, err = kit.LoadStarterSet(db)
		}
		if err != nil {
			db.Close()
		}
	}
	if err != nil {
		reason := fmt.Sprintf("the content.db %q cannot be used (%v): build it with contentc (services/README.md, \"Content\") and restart, or set AOC_CONTENT_DB / --content-db", path, err)
		slog.Warn("character creation is refused until the character service has a usable content.db", "path", path, "err", err)
		return unavailable(reason), nil, nil, func() {}
	}
	if starterSet {
		slog.Info("character creation is checked against content.db; new characters start with their class's kit and the GM starter set (AOC_GM_STARTER_SET=1)", "path", path)
		return rules, kits, starter, func() { db.Close() }
	}
	slog.Info("character creation is checked against content.db, and new characters start with their class's kit", "path", path)
	return rules, kits, nil, func() { db.Close() }
}

// unavailable is the Checker of a service with no usable content.db: it
// refuses every character, saying why.
type unavailable string

func (u unavailable) Check(customization.New) error {
	return fmt.Errorf("%w: %s", charactergrpc.ErrUnavailable, string(u))
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
