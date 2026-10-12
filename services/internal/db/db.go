// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package db opens the PostgreSQL pool and applies the embedded migrations.
package db

import (
	"cmp"
	"context"
	"embed"
	"fmt"
	"io/fs"
	"log/slog"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

//go:embed migrations/*.sql
var migrations embed.FS

// migrationLock is the advisory-lock key that serialises Migrate across
// services starting together.
const migrationLock = 7_320_001

// cleanupTimeout bounds releasing the migration lock (and, failing that,
// closing the connection that holds it).
const cleanupTimeout = 5 * time.Second

// [C O A x C Y B]

// migrationName matches NNNN_name.sql: a four-digit version, an underscore and
// a non-empty description.
var migrationName = regexp.MustCompile(`^(\d{4})_.+\.sql$`)

// Open connects to the PostgreSQL server at url and checks the connection with
// a ping. Errors from pgx never include the password, so they are safe to log.
// The server's warnings, and the notices a migration raises (their text starts
// with "migration "), go to the log (logNotice).
func Open(ctx context.Context, url string) (*pgxpool.Pool, error) {
	cfg, err := pgxpool.ParseConfig(url)
	if err != nil {
		return nil, err
	}
	cfg.ConnConfig.OnNotice = logNotice
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, err
	}
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("database ping: %w", err)
	}
	return pool, nil
}

// logNotice logs what PostgreSQL says outside a result: a warning at WARN, a
// migration's own notice ("migration 0003: ...") at INFO, and every other
// notice (such as "relation already exists, skipping") at DEBUG.
func logNotice(_ *pgconn.PgConn, n *pgconn.Notice) {
	switch {
	case n.SeverityUnlocalized == "WARNING":
		slog.Warn("postgres: " + n.Message)
	case strings.HasPrefix(n.Message, "migration "):
		slog.Info("postgres: " + n.Message)
	default:
		slog.Debug("postgres: "+n.Message, "severity", n.SeverityUnlocalized, "code", n.Code)
	}
}

// Migrate applies the embedded migrations/NNNN_*.sql files that are not yet
// recorded in schema_migrations, in version order, each in its own
// transaction. It holds a PostgreSQL advisory lock while it works, so services
// starting together can all call it safely.
func Migrate(ctx context.Context, pool *pgxpool.Pool) error {
	sub, err := fs.Sub(migrations, "migrations")
	if err != nil {
		return err
	}
	return migrate(ctx, pool, sub)
}

// migration is one NNNN_name.sql file.
type migration struct {
	version int
	name    string
	sql     string
}

// loadMigrations reads every *.sql file at the root of fsys and returns them
// in version order. Every such file must be named NNNN_name.sql, and no two
// files may share a version. It touches no database, so a bad set of files is
// rejected before anything is applied.
func loadMigrations(fsys fs.FS) ([]migration, error) {
	entries, err := fs.ReadDir(fsys, ".")
	if err != nil {
		return nil, err
	}
	var list []migration
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".sql") {
			continue
		}
		m := migrationName.FindStringSubmatch(name)
		if m == nil {
			return nil, fmt.Errorf("migration %s: name must be NNNN_name.sql with a four-digit version", name)
		}
		version, err := strconv.Atoi(m[1])
		if err != nil {
			return nil, fmt.Errorf("migration %s: %w", name, err)
		}
		body, err := fs.ReadFile(fsys, name)
		if err != nil {
			return nil, err
		}
		list = append(list, migration{version: version, name: name, sql: string(body)})
	}
	slices.SortFunc(list, func(a, b migration) int {
		return cmp.Or(cmp.Compare(a.version, b.version), strings.Compare(a.name, b.name))
	})
	for i := 1; i < len(list); i++ {
		if list[i].version == list[i-1].version {
			return nil, fmt.Errorf("migrations %s and %s share version %d", list[i-1].name, list[i].name, list[i].version)
		}
	}
	return list, nil
}

// -- C | O | A x Cyb --

// migrate is Migrate over an arbitrary file system, so tests can feed it
// hand-made migrations.
func migrate(ctx context.Context, pool *pgxpool.Pool, fsys fs.FS) error {
	list, err := loadMigrations(fsys)
	if err != nil {
		return err
	}

	conn, err := pool.Acquire(ctx)
	if err != nil {
		return err
	}
	defer conn.Release()
	if _, err := conn.Exec(ctx, "SELECT pg_advisory_lock($1)", migrationLock); err != nil {
		return err
	}
	// Deferred after Release, so it runs first, while the connection is ours.
	defer unlock(conn)

	if _, err := conn.Exec(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (
		version    INT PRIMARY KEY,
		applied_at TIMESTAMPTZ NOT NULL DEFAULT now())`); err != nil {
		return err
	}
	for _, m := range list {
		var done bool
		if err := conn.QueryRow(ctx,
			"SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = $1)", m.version).Scan(&done); err != nil {
			return err
		}
		if done {
			continue
		}
		if err := apply(ctx, conn, m); err != nil {
			return err
		}
	}
	return nil
}

// apply runs one migration and records its version in a single transaction.
func apply(ctx context.Context, conn *pgxpool.Conn, m migration) error {
	tx, err := conn.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if _, err := tx.Exec(ctx, m.sql); err != nil {
		return fmt.Errorf("migration %s: %w", m.name, err)
	}
	if _, err := tx.Exec(ctx, "INSERT INTO schema_migrations (version) VALUES ($1)", m.version); err != nil {
		return fmt.Errorf("migration %s: record version: %w", m.name, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("migration %s: commit: %w", m.name, err)
	}
	return nil
}

// unlock releases the migration lock taken on conn. If that cannot be
// confirmed, it destroys the connection instead of leaving it for the pool to
// reuse, so the session, and the lock with it, dies.
func unlock(conn *pgxpool.Conn) {
	ctx, cancel := context.WithTimeout(context.Background(), cleanupTimeout)
	defer cancel()
	var released bool
	if err := conn.QueryRow(ctx, "SELECT pg_advisory_unlock($1)", migrationLock).Scan(&released); err == nil && released {
		return
	}
	// Hijack takes the connection out of the pool, so the deferred Release is
	// then a no-op.
	_ = conn.Hijack().Close(ctx)
}
