// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/account"
	"aocrop/services/internal/character"
	"aocrop/services/internal/charactergrpc"
	"aocrop/services/internal/content"
	"aocrop/services/internal/customization"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
	"aocrop/services/internal/svcauth"
)

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// == Cyb | C.O.A ==

// Every call to character needs the service token. An id no character can
// have is answered NotFound without the database.
func TestServerRequiresTheServiceToken(t *testing.T) {
	srv := newServer(testToken, character.NewStore(nil), nil, nil, nil)
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	get := func(opts ...grpc.DialOption) error {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
		cc, err := grpc.NewClient(lis.Addr().String(), opts...)
		if err != nil {
			t.Fatal(err)
		}
		defer cc.Close()
		_, err = characterv1.NewCharacterServiceClient(cc).GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "not-an-id"})
		return err
	}
	if err := get(testToken.DialOption()); status.Code(err) != codes.NotFound {
		t.Errorf("GetCharacter with the token = %v, want NotFound from the store", err)
	}
	if err := get(); status.Code(err) != codes.Unauthenticated {
		t.Errorf("GetCharacter without the token = %v, want Unauthenticated", err)
	}
}

// run refuses a missing service token before it reaches the database. (The
// database is a port nothing listens on.)
func TestRunNeedsTheServiceToken(t *testing.T) {
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	dead := lis.Addr().String()
	lis.Close()
	t.Setenv("AOC_DATABASE_URL", "postgres://aoc:aoc@"+dead+"/aoc?sslmode=disable&connect_timeout=2")
	t.Setenv("AOC_SERVICE_TOKEN_FILE", filepath.Join(t.TempDir(), "service.token"))
	if err := run("content.db", true, false); err == nil || !strings.Contains(err.Error(), "service token") || strings.Contains(err.Error(), "database") {
		t.Errorf("run() = %v, want the missing service token refused before the database", err)
	}
}

// Without a content.db the service has no rules: every creation is refused with
// the reason, and nothing else is affected (the store is never reached here).
func TestWithoutContentDBCreationIsRefusedWithTheReason(t *testing.T) {
	garbage := filepath.Join(t.TempDir(), "garbage.db")
	if err := os.WriteFile(garbage, []byte("this is not a database"), 0o600); err != nil {
		t.Fatal(err)
	}
	for name, path := range map[string]string{
		"a missing file":       filepath.Join(t.TempDir(), "content.db"),
		"a file that is no db": garbage,
		"an empty path":        "",
		"a directory":          t.TempDir(),
	} {
		check, kits, _, closeContent := loadContent(path, false)
		closeContent()
		if kits != nil {
			t.Errorf("%s: loadContent gave kits from a content.db that cannot be used", name)
		}
		err := check.Check(customization.New{Race: 0, Gender: 0, Class: 2, AppearanceJSON: "{}"})
		if !errors.Is(err, charactergrpc.ErrUnavailable) || !strings.Contains(err.Error(), "build it with contentc") ||
			!strings.Contains(err.Error(), "AOC_CONTENT_DB") {
			t.Errorf("%s: Check = %v, want ErrUnavailable with the way out", name, err)
		}

		srv := newServer(testToken, character.NewStore(nil), check, nil, nil)
		lis, lerr := net.Listen("tcp", "127.0.0.1:0")
		if lerr != nil {
			t.Fatal(lerr)
		}
		go srv.Serve(lis)
		cc, cerr := grpc.NewClient(lis.Addr().String(), testToken.DialOption(), grpc.WithTransportCredentials(insecure.NewCredentials()))
		if cerr != nil {
			t.Fatal(cerr)
		}
		_, err = characterv1.NewCharacterServiceClient(cc).CreateCharacter(context.Background(),
			&characterv1.CreateCharacterRequest{AccountId: 1, Name: "Zed", ClassId: 2, AppearanceJson: "{}"})
		if status.Code(err) != codes.FailedPrecondition || !strings.Contains(err.Error(), "character creation is unavailable") {
			t.Errorf("%s: CreateCharacter = %v, want FailedPrecondition", name, err)
		}
		cc.Close()
		srv.Stop()
	}
}

// A content.db built by contentc before the stat program (version 3, plan B2) opens but is refused: creation says what to rebuild it with.
func TestAContentDBFromAnOlderContentcIsRefused(t *testing.T) {
	path := filepath.Join(t.TempDir(), "content.db")
	raw, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if err := content.Create(raw); err != nil {
		t.Fatal(err)
	}
	if _, err := raw.Exec(`INSERT INTO meta (key, value) VALUES (?, '1'), (?, 'json'), (?, '3')`,
		content.MetaSchemaVersion, content.MetaRecordEncoding, content.MetaContentcVersion); err != nil {
		t.Fatal(err)
	}
	raw.Close()
	check, kits, _, closeContent := loadContent(path, false)
	closeContent()
	if kits != nil {
		t.Errorf("loadContent gave kits from a content.db built by contentc version 3")
	}
	err = check.Check(customization.New{Race: 0, Gender: 0, Class: 2, AppearanceJSON: "{}"})
	if !errors.Is(err, charactergrpc.ErrUnavailable) || !strings.Contains(err.Error(), "contentc version 3") ||
		!strings.Contains(err.Error(), "go run ./cmd/contentc") {
		t.Errorf("Check = %v, want ErrUnavailable naming contentc version 3 and the rebuild command", err)
	}
}

// With the real content.db (AOC_CONTENT_DB, as scripts\services.ps1 test sets it) the rules load.
func TestWithContentDBTheRulesLoad(t *testing.T) {
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB not set (the content.db contentc builds)")
	}
	check, kits, _, closeContent := loadContent(path, false)
	defer closeContent()
	if _, ok := check.(*customization.Rules); !ok {
		t.Fatalf("loadContent(%q) = %T (%v), want the rules", path, check, check.Check(customization.New{}))
	}
	if kits == nil {
		t.Fatalf("loadContent(%q) gave no starting kits", path)
	}
	if bard, err := kits.Kit(2); err != nil || len(bard) != 13 {
		t.Errorf("the Bard's kit = %d items, %v, want the captured Bard's 13", len(bard), err)
	}
	if err := check.Check(customization.New{Race: 0, Gender: 0, Class: 2, AppearanceJSON: "{}"}); err != nil {
		t.Errorf("a Kaelar male Bard with nothing chosen is refused: %v", err)
	}
	if err := check.Check(customization.New{Race: 8, Class: 2, AppearanceJSON: "{}"}); err == nil || !strings.Contains(err.Error(), "Tulnar") {
		t.Errorf("Tulnar: %v", err)
	}
}

// Without a usable content.db there is nothing to make kits from: the backfill says so, writes nothing and does not stop the service.
func TestBackfillWithoutKitsSaysSoAndDoesNothing(t *testing.T) {
	var logs bytes.Buffer
	saved := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&logs, nil)))
	defer slog.SetDefault(saved)
	// a nil store would panic if the backfill touched the database
	backfillKits(context.Background(), nil, nil)
	if !strings.Contains(logs.String(), "level=WARN") || !strings.Contains(logs.String(), "backfill is skipped") || !strings.Contains(logs.String(), "content.db") {
		t.Errorf("the log does not say the backfill is skipped for want of content.db:\n%s", logs.String())
	}
}

// [C.O.A + Cyb]

// The real kits on the real store: a character made before items existed (cyb, a Kaelar male Bard) is given the captured Bard's
// kit when the service starts, once; a character that has items is left alone.
func TestBackfillGivesCybTheBardsKitOnce(t *testing.T) {
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB not set (the content.db contentc builds)")
	}
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	acc, err := account.NewStore(pool, 0).Register(ctx, "owner", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	store := character.NewStore(pool)
	cyb, err := store.Create(ctx, character.New{AccountID: acc.ID, Name: "cyb", RaceID: 0, GenderID: 0, ClassID: 2, ClassName: "Bard", ArchetypeID: 17747})
	if err != nil {
		t.Fatal(err)
	}
	tank, err := store.Create(ctx, character.New{AccountID: acc.ID, Name: "Dunir", RaceID: 6, GenderID: 0, ClassID: 7, ClassName: "Tank", ArchetypeID: 17754})
	if err != nil {
		t.Fatal(err)
	}
	// one with a bag item already
	kept := []character.Item{{Container: character.Backpack, Slot: 0, ItemRecordID: 5, InstanceGUID: "11111111111111111111111111111111", Quantity: 1,
		Quality: 1, CreatedAt: time.UnixMilli(1_790_000_000_000)}}
	has, err := store.Create(ctx, character.New{AccountID: acc.ID, Name: "Has", ClassID: 2, Items: kept})
	if err != nil {
		t.Fatal(err)
	}

	check, kits, _, closeContent := loadContent(path, false)
	defer closeContent()
	if _, ok := check.(*customization.Rules); !ok || kits == nil {
		t.Fatalf("loadContent(%q) = %T, %v", path, check, kits)
	}
	var logs bytes.Buffer
	saved := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&logs, nil)))
	defer slog.SetDefault(saved)
	backfillKits(ctx, store, kits)

	items, err := store.Inventory(ctx, cyb.ID)
	if err != nil || len(items) != 13 {
		t.Fatalf("cyb has %d items (%v), want the Bard's 13", len(items), err)
	}
	for _, it := range items {
		if it.Container != character.Equipment || it.Quantity != 1 || it.Quality != 1 {
			t.Errorf("cyb's item %+v", it)
		}
	}
	if items[0].Slot != 1 || items[0].ItemRecordID != 6064630736269279236 || items[1].Slot != 2 || items[1].Durability != 11 || items[12].Slot != 38 || items[12].Durability != 38 {
		t.Errorf("cyb's first, second and last items: %+v %+v %+v", items[0], items[1], items[12])
	}
	if t2, _ := store.Inventory(ctx, tank.ID); len(t2) != 12 {
		t.Errorf("the Tank has %d items, want 12", len(t2))
	}
	if h, _ := store.Inventory(ctx, has.ID); len(h) != 1 || h[0].ItemRecordID != 5 {
		t.Errorf("the character with items was changed: %+v", h)
	}
	for _, want := range []string{"starter kit granted (backfill)", "name=cyb", "items=13", "granted=2"} {
		if !strings.Contains(logs.String(), want) {
			t.Errorf("the log lacks %q:\n%s", want, logs.String())
		}
	}
	// once: the second start changes nothing
	backfillKits(ctx, store, kits)
	if again, _ := store.Inventory(ctx, cyb.ID); len(again) != 13 || again[1].InstanceGUID != items[1].InstanceGUID {
		t.Errorf("a second backfill changed cyb's items")
	}
}
