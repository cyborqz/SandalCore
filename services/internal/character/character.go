// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package character owns character records: identity, race, gender, class,
// appearance, level, and where the character was last saved.
//
// Race, gender and class are the client's enums (internal/chardef): race 0-8
// (Kaelar ... Tulnar), gender 0-1, class 1-8. They have columns of their own
// (race, gender, class_id); the columns race_id and gender_id of the first
// schema are the legacy backup of plan B1's migration and are neither read nor
// written. A row the migration could not derive a value for holds NULL, which
// the store reads as 0.
package character

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

var (
	ErrNameTaken = errors.New("character name taken")
	ErrIDTaken   = errors.New("character id taken")
	ErrNotFound  = errors.New("character not found")
	ErrInvalid   = errors.New("invalid character data")
)

// [CoA + C.Y.B]

type Position struct {
	X, Y, Z float64
	Yaw     float32
}

type Character struct {
	ID             string
	AccountID      int64
	Name           string
	RaceID         int32 // ECharacterRace
	GenderID       int32 // ECharacterGender
	ClassID        int32 // ECharacterClass; 0 when the migration could not derive it
	ArchetypeID    int32
	ClassName      string
	AppearanceJSON string // a JSON object as the database normalized it (key order, spacing)
	CreateInfo     []byte
	Level          int32
	Position       *Position // nil until the first save
	LastRegion     string
	CreatedAt      time.Time // when the record was created
}

// New describes a character to create; ID and Level are optional.
type New struct {
	ID             string
	AccountID      int64
	Name           string
	RaceID         int32 // ECharacterRace, 0-8
	GenderID       int32 // ECharacterGender, 0-1
	ClassID        int32 // ECharacterClass, 1-8, or 0 for none (stored as NULL)
	ArchetypeID    int32
	ClassName      string
	AppearanceJSON string
	CreateInfo     []byte
	Level          int32
	// Items are the character's first items, its class's starting kit (plan B2), stored in the same transaction as the
	// character: either both exist or neither.
	Items []Item
	// GMStarterSetGranted says the Items hold the GM starter set too (plan D1, AOC_GM_STARTER_SET=1): the character is
	// marked as given it (gm_starter_set_granted_at), so the backfill at the next start leaves it alone.
	GMStarterSetGranted bool
}

// State is what play changes; a zero Level leaves the level alone.
type State struct {
	Position   *Position
	LastRegion string
	Level      int32
}

var (
	namePattern = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9]{2,19}$`)
	idPattern   = regexp.MustCompile(`^[0-9a-f]{32}$`)
)

func ValidateName(name string) error {
	if !namePattern.MatchString(name) {
		return fmt.Errorf("%w: name must be 3-20 letters or digits, starting with a letter", ErrInvalid)
	}
	return nil
}

type Store struct{ pool *pgxpool.Pool }

func NewStore(pool *pgxpool.Pool) *Store { return &Store{pool: pool} }

// columns is what scan reads; the last one says that race or gender is NULL (COALESCEd to 0 above it).
const columns = `id, account_id, name, COALESCE(race, 0), COALESCE(gender, 0), COALESCE(class_id, 0),
	archetype_id, class_name, appearance::text, create_info, level, pos_x, pos_y, pos_z, yaw, last_region, created_at,
	(race IS NULL OR gender IS NULL)`

// loggedUnderived holds the ids of the characters whose NULL race or gender was reported, each is reported once.
var loggedUnderived sync.Map

func (s *Store) Create(ctx context.Context, n New) (Character, error) {
	if err := ValidateName(n.Name); err != nil {
		return Character{}, err
	}
	if n.AccountID <= 0 {
		return Character{}, fmt.Errorf("%w: account id must be positive", ErrInvalid)
	}
	if err := ValidateLevel(n.Level); err != nil {
		return Character{}, err
	}
	if n.RaceID < 0 || n.RaceID > 8 {
		return Character{}, fmt.Errorf("%w: race %d is not an ECharacterRace id (0-8)", ErrInvalid, n.RaceID)
	}
	if n.GenderID < 0 || n.GenderID > 1 {
		return Character{}, fmt.Errorf("%w: gender %d is not an ECharacterGender id (0-1)", ErrInvalid, n.GenderID)
	}
	if n.ClassID < 0 || n.ClassID > 8 {
		return Character{}, fmt.Errorf("%w: class %d is not an ECharacterClass id (1-8, or 0 for none)", ErrInvalid, n.ClassID)
	}
	if n.ID == "" {
		var raw [16]byte
		if _, err := rand.Read(raw[:]); err != nil {
			return Character{}, err
		}
		n.ID = hex.EncodeToString(raw[:])
	} else if !idPattern.MatchString(n.ID) {
		return Character{}, fmt.Errorf("%w: id must be 32 lowercase hex digits", ErrInvalid)
	}
	if n.AppearanceJSON == "" {
		n.AppearanceJSON = "{}"
	} else if !isJSONObject(n.AppearanceJSON) {
		return Character{}, fmt.Errorf("%w: appearance must be a JSON object", ErrInvalid)
	}
	if n.Level == 0 {
		n.Level = 1
	}
	if err := ValidateItems(n.Items); err != nil {
		return Character{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Character{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	row := tx.QueryRow(ctx, `
		INSERT INTO characters (id, account_id, name, race, gender, class_id, archetype_id, class_name,
		                        appearance, create_info, level, gm_starter_set_granted_at, gm_starter_mounts_granted_at)
		VALUES ($1, $2, $3, $4, $5, NULLIF($6::int, 0), $7, $8, $9::jsonb, $10, $11, CASE WHEN $12 THEN now() ELSE NULL END, CASE WHEN $12 THEN now() ELSE NULL END)
		RETURNING `+columns,
		n.ID, n.AccountID, n.Name, n.RaceID, n.GenderID, n.ClassID, n.ArchetypeID, n.ClassName,
		n.AppearanceJSON, n.CreateInfo, n.Level, n.GMStarterSetGranted)
	c, err := scan(row)
	if err == nil {
		if err = insertItems(ctx, tx, n.ID, n.Items); err != nil {
			err = itemsError(err)
		}
	}
	if err == nil {
		err = tx.Commit(ctx)
	}
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		switch {
		case pgErr.Code == "23505" && pgErr.ConstraintName == "characters_name_lower":
			return Character{}, ErrNameTaken
		case pgErr.Code == "23505" && pgErr.ConstraintName == "characters_pkey":
			return Character{}, ErrIDTaken
		case pgErr.Code == "23503" && pgErr.ConstraintName == "characters_account_id_fkey":
			return Character{}, fmt.Errorf("%w: no account %d", ErrInvalid, n.AccountID)
		}
	}
	return c, invalidData(err)
}

// C.Y.B with C | O | A

// invalidData turns PostgreSQL data exceptions (SQLSTATE class 22, e.g. a
// NUL byte in text or JSON) into ErrInvalid: the value was the caller's.
func invalidData(err error) error {
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && strings.HasPrefix(pgErr.Code, "22") {
		return fmt.Errorf("%w: %s", ErrInvalid, pgErr.Message)
	}
	return err
}

// isJSONObject reports whether s is a JSON object ("null" and arrays are not).
func isJSONObject(s string) bool {
	var obj map[string]json.RawMessage
	return json.Unmarshal([]byte(s), &obj) == nil && obj != nil
}

func (s *Store) List(ctx context.Context, accountID int64) ([]Character, error) {
	rows, err := s.pool.Query(ctx,
		"SELECT "+columns+" FROM characters WHERE account_id = $1 ORDER BY created_at, id", accountID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Character
	for rows.Next() {
		c, err := scan(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

func (s *Store) Get(ctx context.Context, id string) (Character, error) {
	if !idPattern.MatchString(id) {
		return Character{}, ErrNotFound // no character has such an id; keep NUL and bad UTF-8 away from PostgreSQL
	}
	c, err := scan(s.pool.QueryRow(ctx, "SELECT "+columns+" FROM characters WHERE id = $1", id))
	if errors.Is(err, pgx.ErrNoRows) {
		return Character{}, ErrNotFound
	}
	return c, err
}

func (s *Store) SaveState(ctx context.Context, id string, st State) (Character, error) {
	if !idPattern.MatchString(id) {
		return Character{}, ErrNotFound // no character has such an id; keep NUL and bad UTF-8 away from PostgreSQL
	}
	if err := ValidateLevel(st.Level); err != nil {
		return Character{}, err
	}
	// NaN and infinities pass the column types and the NULL-only check
	// constraint, and a stored NaN spawn can never be cleared.
	if p := st.Position; p != nil && !finite(p.X, p.Y, p.Z, float64(p.Yaw)) {
		return Character{}, fmt.Errorf("%w: position must be finite", ErrInvalid)
	}
	var x, y, z *float64
	var yaw *float32
	if st.Position != nil {
		x, y, z, yaw = &st.Position.X, &st.Position.Y, &st.Position.Z, &st.Position.Yaw
	}
	c, err := scan(s.pool.QueryRow(ctx, `
		UPDATE characters SET
			pos_x = COALESCE($2, pos_x), pos_y = COALESCE($3, pos_y), pos_z = COALESCE($4, pos_z),
			yaw = COALESCE($5, yaw),
			last_region = CASE WHEN $6 = '' THEN last_region ELSE $6 END,
			level = CASE WHEN $7 = 0 THEN level ELSE $7 END,
			updated_at = now()
		WHERE id = $1
		RETURNING `+columns, id, x, y, z, yaw, st.LastRegion, st.Level))
	if errors.Is(err, pgx.ErrNoRows) {
		return Character{}, ErrNotFound
	}
	return c, invalidData(err)
}

// (C O A ~ C.Y.B)

// Delete removes the character if it belongs to accountID. ErrNotFound when
// that account has no such character: another account's character looks the
// same as a missing one.
func (s *Store) Delete(ctx context.Context, accountID int64, id string) error {
	if !idPattern.MatchString(id) {
		return ErrNotFound // no character has such an id; keep NUL and bad UTF-8 away from PostgreSQL
	}
	tag, err := s.pool.Exec(ctx, "DELETE FROM characters WHERE id = $1 AND account_id = $2", id, accountID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func scan(row pgx.Row) (Character, error) {
	var c Character
	var x, y, z *float64
	var yaw *float32
	var underived bool
	err := row.Scan(&c.ID, &c.AccountID, &c.Name, &c.RaceID, &c.GenderID, &c.ClassID, &c.ArchetypeID, &c.ClassName,
		&c.AppearanceJSON, &c.CreateInfo, &c.Level, &x, &y, &z, &yaw, &c.LastRegion, &c.CreatedAt, &underived)
	if err != nil {
		return Character{}, err
	}
	if underived {
		if _, reported := loggedUnderived.LoadOrStore(c.ID, true); !reported {
			slog.Warn("character: race or gender is NULL (migration 0003 could not derive it): read as 0, Kaelar and Male",
				"character_id", c.ID, "name", c.Name)
		}
	}
	if x != nil && y != nil && z != nil {
		c.Position = &Position{X: *x, Y: *y, Z: *z}
		if yaw != nil {
			c.Position.Yaw = *yaw
		}
	}
	return c, nil
}

func finite(vs ...float64) bool {
	for _, v := range vs {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return false
		}
	}
	return true
}
