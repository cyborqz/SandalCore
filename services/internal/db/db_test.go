// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

import (
	"context"
	"errors"
	"fmt"
	"io/fs"
	"slices"
	"strings"
	"sync"
	"testing"
	"testing/fstest"

	"aocrop/services/internal/dbtest"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// reversedFS lists directory entries in reverse name order, so the runner
// cannot get away with trusting the order it is given.
type reversedFS struct{ fstest.MapFS }

func (r reversedFS) ReadDir(name string) ([]fs.DirEntry, error) {
	entries, err := r.MapFS.ReadDir(name)
	slices.Reverse(entries)
	return entries, err
}

// sqlFiles builds a MapFS holding one file per name, each containing
// "-- <name>", so a test can tell that the right file was read.
func sqlFiles(names ...string) fstest.MapFS {
	fsys := fstest.MapFS{}
	for _, name := range names {
		fsys[name] = &fstest.MapFile{Data: []byte("-- " + name)}
	}
	return fsys
}

// C | O | A x Cyb

// The file-name rules live in loadMigrations, which touches no database, so
// they are tested here without one (these run even when
// AOC_TEST_DATABASE_URL is unset).
func TestLoadMigrations(t *testing.T) {
	type entry struct {
		version int
		name    string
	}
	type loadCase struct {
		name    string
		fsys    fs.FS
		want    []entry
		wantErr []string // substrings the error must contain
	}
	cases := []loadCase{
		{name: "empty directory", fsys: sqlFiles()},
		{
			name: "one file",
			fsys: sqlFiles("0001_init.sql"),
			want: []entry{{1, "0001_init.sql"}},
		},
		{
			name: "listed in version order",
			fsys: sqlFiles("0001_first.sql", "0002_second.sql", "0010_tenth.sql"),
			want: []entry{{1, "0001_first.sql"}, {2, "0002_second.sql"}, {10, "0010_tenth.sql"}},
		},
		{
			name: "sorted by version whatever order the file system lists them in",
			fsys: reversedFS{sqlFiles("0001_first.sql", "0002_second.sql", "0010_tenth.sql")},
			want: []entry{{1, "0001_first.sql"}, {2, "0002_second.sql"}, {10, "0010_tenth.sql"}},
		},
		{
			name: "versions may have gaps",
			fsys: sqlFiles("0001_a.sql", "0005_b.sql"),
			want: []entry{{1, "0001_a.sql"}, {5, "0005_b.sql"}},
		},
		{
			name: "non-sql files and directories are ignored",
			fsys: sqlFiles("0001_init.sql", "README.md", "0002_notes.txt", "sub/0009_nested.sql", "0003_dir.sql/x.sql"),
			want: []entry{{1, "0001_init.sql"}},
		},
		{
			name:    "duplicate version names both files",
			fsys:    sqlFiles("0001_a.sql", "0001_b.sql"),
			wantErr: []string{"0001_a.sql and 0001_b.sql", "version 1"},
		},
		{
			name:    "duplicate version is reported the same way in any listing order",
			fsys:    reversedFS{sqlFiles("0001_a.sql", "0001_b.sql")},
			wantErr: []string{"0001_a.sql and 0001_b.sql", "version 1"},
		},
		{
			name:    "duplicate version among other files",
			fsys:    reversedFS{sqlFiles("0001_a.sql", "0002_x.sql", "0002_y.sql", "0003_z.sql")},
			wantErr: []string{"0002_x.sql and 0002_y.sql", "version 2"},
		},
	}
	for _, bad := range []string{
		"12_x.sql",    // too few digits
		"00012_x.sql", // too many digits
		"abcd_x.sql",  // not digits
		"0001x_y.sql", // digits followed by junk
		"0001.sql",    // no description
		"0001-x.sql",  // wrong separator
		"0002_.sql",   // empty description
		"_x.sql",      // no version
		string([]rune{0xff10, 0xff10, 0xff10, 0xff12}) + "_x.sql", // full-width digits, not ASCII
	} {
		cases = append(cases, loadCase{
			name:    "bad name " + bad,
			fsys:    sqlFiles("0001_ok.sql", bad),
			wantErr: []string{bad},
		})
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, err := loadMigrations(c.fsys)
			if len(c.wantErr) > 0 {
				if err == nil {
					t.Fatalf("loadMigrations succeeded, want an error containing %q", c.wantErr)
				}
				for _, s := range c.wantErr {
					if !strings.Contains(err.Error(), s) {
						t.Errorf("error %q does not contain %q", err, s)
					}
				}
				return
			}
			if err != nil {
				t.Fatalf("loadMigrations: %v", err)
			}
			var gotEntries []entry
			for _, m := range got {
				gotEntries = append(gotEntries, entry{m.version, m.name})
				if m.sql != "-- "+m.name {
					t.Errorf("%s: sql = %q, want the file's own contents", m.name, m.sql)
				}
			}
			if !slices.Equal(gotEntries, c.want) {
				t.Errorf("got %v, want %v", gotEntries, c.want)
			}
		})
	}
}

// The migrations compiled into the binary must themselves obey the naming
// rules; this catches a bad file name without needing a database.
func TestEmbeddedMigrationsAreValid(t *testing.T) {
	if versions := embeddedVersions(t); len(versions) == 0 {
		t.Fatal("no migrations are embedded")
	}
}

func TestMigrateCreatesTablesOnce(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()

	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("first migrate: %v", err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("second migrate: %v", err)
	}

	for _, table := range []string{"accounts", "login_tokens", "characters", "world_servers", "sessions"} {
		var n int
		err := pool.QueryRow(ctx,
			"SELECT count(*) FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = $1",
			table).Scan(&n)
		if err != nil || n != 1 {
			t.Errorf("table %s: n=%d err=%v", table, n, err)
		}
	}
	// Running Migrate twice must record each embedded migration exactly once.
	want := embeddedVersions(t)
	if got := appliedVersions(t, pool); !slices.Equal(got, want) {
		t.Errorf("applied versions = %v, want the embedded %v", got, want)
	}
}

// Services start together and all call Migrate; the advisory lock must make
// that safe and must not be left behind.
func TestMigrateConcurrent(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()

	const workers = 8
	errs := make([]error, workers)
	var wg sync.WaitGroup
	for i := range errs {
		wg.Add(1)
		go func() {
			defer wg.Done()
			errs[i] = Migrate(ctx, pool)
		}()
	}
	wg.Wait()

	for i, err := range errs {
		if err != nil {
			t.Errorf("worker %d: %v", i, err)
		}
	}
	want := embeddedVersions(t)
	if got := appliedVersions(t, pool); !slices.Equal(got, want) {
		t.Errorf("applied versions = %v, want the embedded %v", got, want)
	}
	if n := advisoryLocksHeld(t, pool); n != 0 {
		t.Errorf("%d advisory locks still held", n)
	}
}

// C Y B - C | O | A

func TestMigrateFailingMigrationRollsBack(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	fsys := fstest.MapFS{
		"0001_ok.sql":  {Data: []byte("CREATE TABLE a (id INT PRIMARY KEY);")},
		"0002_bad.sql": {Data: []byte("CREATE TABLE b (id INT PRIMARY KEY); THIS IS NOT SQL;")},
	}

	err := migrate(ctx, pool, fsys)
	if err == nil || !strings.Contains(err.Error(), "0002_bad.sql") {
		t.Fatalf("migrate error = %v, want one naming 0002_bad.sql", err)
	}
	if !tableExists(t, pool, "a") {
		t.Error("table a (migration 1) should have been kept")
	}
	if tableExists(t, pool, "b") {
		t.Error("table b (failed migration 2) should have been rolled back")
	}
	if got := appliedVersions(t, pool); !slices.Equal(got, []int{1}) {
		t.Errorf("applied versions = %v, want [1]", got)
	}
	if n := advisoryLocksHeld(t, pool); n != 0 {
		t.Errorf("%d advisory locks still held", n)
	}
}

func TestMigrateOrdersByVersion(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	fsys := reversedFS{fstest.MapFS{
		"0002_second.sql": {Data: []byte("CREATE TABLE child (parent_id INT REFERENCES parent (id));")},
		"0001_first.sql":  {Data: []byte("CREATE TABLE parent (id INT PRIMARY KEY);")},
	}}

	if err := migrate(ctx, pool, fsys); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if got := appliedVersions(t, pool); !slices.Equal(got, []int{1, 2}) {
		t.Errorf("applied versions = %v, want [1 2]", got)
	}
	for _, table := range []string{"parent", "child"} {
		if !tableExists(t, pool, table) {
			t.Errorf("table %s missing", table)
		}
	}
}

// The file-name rules are tested database-free in TestLoadMigrations; the two
// tests below only prove that migrate applies nothing when the set is bad.

func TestMigrateRejectsDuplicateVersion(t *testing.T) {
	pool := dbtest.Pool(t)
	fsys := fstest.MapFS{
		"0001_a.sql": {Data: []byte("CREATE TABLE a (id INT);")},
		"0001_b.sql": {Data: []byte("CREATE TABLE b (id INT);")},
	}

	if err := migrate(context.Background(), pool, fsys); err == nil {
		t.Fatal("migrate succeeded with two files at version 1")
	}
	for _, table := range []string{"a", "b"} {
		if tableExists(t, pool, table) {
			t.Errorf("table %s was created despite the duplicate version", table)
		}
	}
}

func TestMigrateRejectsBadName(t *testing.T) {
	pool := dbtest.Pool(t)
	fsys := fstest.MapFS{
		"0001_ok.sql": {Data: []byte("CREATE TABLE ok (id INT);")},
		"12_x.sql":    {Data: []byte("CREATE TABLE other (id INT);")},
	}

	if err := migrate(context.Background(), pool, fsys); err == nil {
		t.Fatal("migrate succeeded with a badly named file")
	}
	for _, table := range []string{"ok", "other"} {
		if tableExists(t, pool, table) {
			t.Errorf("table %s was created although a file name was invalid", table)
		}
	}
}

// If the advisory lock cannot be confirmed released, the connection must be
// destroyed rather than handed back to the pool still holding the session.
func TestMigrateDestroysConnectionWhenUnlockFails(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	fsys := fstest.MapFS{
		// Releases the lock early, so Migrate's own unlock then returns false.
		"0001_unlock.sql": {Data: []byte(fmt.Sprintf("SELECT pg_advisory_unlock(%d);", migrationLock))},
	}

	if err := migrate(ctx, pool, fsys); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if n := pool.Stat().TotalConns(); n != 0 {
		t.Errorf("pool still has %d connections; the one whose unlock failed should be gone", n)
	}
	if n := advisoryLocksHeld(t, pool); n != 0 {
		t.Errorf("%d advisory locks still held", n)
	}
}

// (Cyb + C | O | A)

func TestSchemaConstraints(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var accountID int64
	if err := pool.QueryRow(ctx,
		"INSERT INTO accounts (username, password_hash) VALUES ('Alice', 'x') RETURNING id").Scan(&accountID); err != nil {
		t.Fatal(err)
	}

	insertAccount := func(username string) error {
		_, err := pool.Exec(ctx, "INSERT INTO accounts (username, password_hash) VALUES ($1, 'x')", username)
		return err
	}
	insertChar := func(id, name string, level int, x, y, z any) error {
		_, err := pool.Exec(ctx, `INSERT INTO characters
			(id, account_id, name, race_id, gender_id, archetype_id, class_name, level, pos_x, pos_y, pos_z)
			VALUES ($1, $2, $3, 1, 1, 1, 'Fighter', $4, $5, $6, $7)`,
			id, accountID, name, level, x, y, z)
		return err
	}
	insertToken := func(hash []byte) error {
		_, err := pool.Exec(ctx,
			"INSERT INTO login_tokens (token_hash, account_id, expires_at) VALUES ($1, $2, now())", hash, accountID)
		return err
	}
	insertServer := func(address string) error {
		_, err := pool.Exec(ctx, "INSERT INTO world_servers (address, last_heartbeat) VALUES ($1, now())", address)
		return err
	}
	insertServerID := func(id int) error {
		_, err := pool.Exec(ctx, `INSERT INTO world_servers (server_id, address, last_heartbeat)
			OVERRIDING SYSTEM VALUE VALUES ($1, '127.0.0.1:7777', now())`, id)
		return err
	}
	// Character 0123... (Bob) is inserted by an earlier case, which runs first.
	insertSession := func(ticket string, serverID int, ttl string) error {
		_, err := pool.Exec(ctx, `INSERT INTO sessions
			(ticket, account_id, character_id, target_server_id, created_at, expires_at)
			VALUES ($1, $2, '0123456789abcdef0123456789abcdef', $3, now(), now() + $4::interval)`,
			ticket, accountID, serverID, ttl)
		return err
	}

	const (
		checkViolation  = "23514"
		uniqueViolation = "23505"
	)
	cases := []struct {
		name       string
		err        error
		wantCode   string // empty: must succeed
		wantConstr string // the constraint or index that fired, when it has a stable name
	}{
		{"account: empty username", insertAccount(""), checkViolation, "accounts_username_not_empty"},
		{"account: username differs only by case", insertAccount("ALICE"), uniqueViolation, "accounts_username_lower"},
		{"character: no position", insertChar("0123456789abcdef0123456789abcdef", "Bob", 1, nil, nil, nil), "", ""},
		{"character: full position", insertChar("fedcba9876543210fedcba9876543210", "Carol", 1, 1.0, 2.0, 3.0), "", ""},
		{"character: position with a missing axis", insertChar("00000000000000000000000000000001", "Dave", 1, 1.0, nil, 3.0), checkViolation, "characters_position_all_or_nothing"},
		{"character: level 0", insertChar("00000000000000000000000000000002", "Erin", 0, nil, nil, nil), checkViolation, "characters_level_positive"},
		{"character: empty name", insertChar("00000000000000000000000000000003", "", 1, nil, nil, nil), checkViolation, "characters_name_not_empty"},
		{"character: id is not 32 hex digits", insertChar("XYZ", "Frank", 1, nil, nil, nil), checkViolation, ""},
		{"character: name differs only by case", insertChar("00000000000000000000000000000004", "BOB", 1, nil, nil, nil), uniqueViolation, "characters_name_lower"},
		{"token: 32-byte hash", insertToken(make([]byte, 32)), "", ""},
		{"token: 31-byte hash", insertToken(make([]byte, 31)), checkViolation, "login_tokens_hash_length"},
		{"world server: address", insertServer("127.0.0.1:7777"), "", ""},
		{"world server: empty address", insertServer(""), checkViolation, "world_servers_address_not_empty"},
		{"world server: id 0 is the static world's", insertServerID(0), checkViolation, "world_servers_id_positive"},
		{"session: ticket", insertSession("0123456789abcdef0123456789abcdef", 60, "2 minutes"), "", ""},
		{"session: static world (server 0)", insertSession("fedcba9876543210fedcba9876543210", 0, "2 minutes"), "", ""},
		{"session: ticket in upper case", insertSession("0123456789ABCDEF0123456789ABCDEF", 60, "2 minutes"), checkViolation, "sessions_ticket_hex"},
		{"session: ticket with a trailing newline", insertSession("0123456789abcdef0123456789abcde\n", 60, "2 minutes"), checkViolation, "sessions_ticket_hex"},
		{"session: same ticket twice", insertSession("0123456789abcdef0123456789abcdef", 60, "2 minutes"), uniqueViolation, "sessions_pkey"},
		{"session: negative server id", insertSession("00000000000000000000000000000001", -1, "2 minutes"), checkViolation, "sessions_server_id_not_negative"},
		{"session: expires when created", insertSession("00000000000000000000000000000002", 60, "0 seconds"), checkViolation, "sessions_expires_after_created"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if c.wantCode == "" {
				if c.err != nil {
					t.Fatalf("want success, got %v", c.err)
				}
				return
			}
			var pgErr *pgconn.PgError
			if !errors.As(c.err, &pgErr) || pgErr.Code != c.wantCode {
				t.Fatalf("want SQLSTATE %s, got %v", c.wantCode, c.err)
			}
			if c.wantConstr != "" && pgErr.ConstraintName != c.wantConstr {
				t.Errorf("constraint = %q, want %q", pgErr.ConstraintName, c.wantConstr)
			}
		})
	}

	var n int
	if err := pool.QueryRow(ctx,
		"SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'login_tokens_expires'").Scan(&n); err != nil || n != 1 {
		t.Errorf("index login_tokens_expires: n=%d err=%v", n, err)
	}
}

// embeddedVersions returns the versions of the migrations compiled into the
// package, so assertions keep holding when a new migration is added.
func embeddedVersions(t *testing.T) []int {
	t.Helper()
	sub, err := fs.Sub(migrations, "migrations")
	if err != nil {
		t.Fatal(err)
	}
	list, err := loadMigrations(sub)
	if err != nil {
		t.Fatal(err)
	}
	versions := make([]int, len(list))
	for i, m := range list {
		versions[i] = m.version
	}
	return versions
}

func tableExists(t *testing.T, pool *pgxpool.Pool, name string) bool {
	t.Helper()
	var n int
	err := pool.QueryRow(context.Background(),
		"SELECT count(*) FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = $1",
		name).Scan(&n)
	if err != nil {
		t.Fatal(err)
	}
	return n == 1
}

// C.Y.B - C | O | A

func appliedVersions(t *testing.T, pool *pgxpool.Pool) []int {
	t.Helper()
	rows, err := pool.Query(context.Background(), "SELECT version FROM schema_migrations ORDER BY version")
	if err != nil {
		t.Fatal(err)
	}
	versions, err := pgx.CollectRows(rows, pgx.RowTo[int])
	if err != nil {
		t.Fatal(err)
	}
	return versions
}

// advisoryLocksHeld counts advisory locks held by this test's own sessions
// (dbtest.Pool sets application_name to the schema name), so other tests
// migrating against the same database at the same time cannot disturb it.
func advisoryLocksHeld(t *testing.T, pool *pgxpool.Pool) int {
	t.Helper()
	var n int
	err := pool.QueryRow(context.Background(), `SELECT count(*)
		FROM pg_locks l JOIN pg_stat_activity a ON a.pid = l.pid
		WHERE l.locktype = 'advisory' AND a.application_name = current_setting('application_name')`).Scan(&n)
	if err != nil {
		t.Fatal(err)
	}
	return n
}
