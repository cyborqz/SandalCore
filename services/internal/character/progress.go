// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sort"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"aocrop/services/internal/chardef"
)

// Plan D1: a character's progress (its level, class and XP per leveling type: character_experience, migration 0005) and the
// one-time GM starter set (characters.gm_starter_set_granted_at).

// Experience is the XP a character has in one leveling type (a row of character_experience).
type Experience struct {
	LevelingTypeGuid int64 // the design database's LevelingTypeDef Guid (Adventuring 108651085518458)
	XP               int64 // the total, as the client's XPTracker shows it
	Debt             int64
}

// Progress is what SaveProgress writes: a zero Level or ClassID leaves that value alone; every Experience replaces the
// character's row of its leveling type (plan D1, stage 2: and the ranks and the action bar, whole, when asked).
type Progress struct {
	Level      int32
	ClassID    int32
	Experience []Experience
	// Plan D1, stage 2: with ReplaceSkillRanks the character's bought ranks become exactly SkillRanks (a tree not listed has none left),
	// with ReplaceActionBar its action bar becomes exactly ActionBar; without them they are left alone.
	ReplaceSkillRanks bool
	SkillRanks        []TreeRanks
	ReplaceActionBar  bool
	ActionBar         []ActionbarSlot
	// Gathering stage 2: each profession replaces the character's row of that profession (professions.go); the others are left alone.
	Professions []Profession
}

// C-Y-B x C O A

// MaxLevel is the game's maximum level, and the highest level the store takes: Adventuring's LevelCapOverride is 25, so a
// character cannot be above it (content.db's tables go further, to 50, but no character reaches them).
const MaxLevel = 25

// ValidateLevel says whether a level can be stored: 0 (which a request reads as "unset"), or 1 to MaxLevel.
func ValidateLevel(level int32) error {
	if level < 0 || level > MaxLevel {
		return fmt.Errorf("%w: level %d is not 1 to %d (0 is unset)", ErrInvalid, level, MaxLevel)
	}
	return nil
}

// Validate says what is wrong with the progress.
func (p Progress) Validate() error {
	if p.Level < 0 || p.Level > MaxLevel {
		return fmt.Errorf("%w: level %d is not 1 to %d (0 leaves it)", ErrInvalid, p.Level, MaxLevel)
	}
	if p.ClassID < 0 || p.ClassID > 8 {
		return fmt.Errorf("%w: class %d is not an ECharacterClass id (1-8; 0 leaves it)", ErrInvalid, p.ClassID)
	}
	seen := map[int64]bool{}
	for _, e := range p.Experience {
		switch {
		case e.LevelingTypeGuid <= 0:
			return fmt.Errorf("%w: leveling type %d is no record", ErrInvalid, e.LevelingTypeGuid)
		case e.XP < 0:
			return fmt.Errorf("%w: xp %d of leveling type %d is negative", ErrInvalid, e.XP, e.LevelingTypeGuid)
		case e.Debt < 0:
			return fmt.Errorf("%w: debt %d of leveling type %d is negative", ErrInvalid, e.Debt, e.LevelingTypeGuid)
		case seen[e.LevelingTypeGuid]:
			return fmt.Errorf("%w: leveling type %d is given twice", ErrInvalid, e.LevelingTypeGuid)
		}
		seen[e.LevelingTypeGuid] = true
	}
	if err := ValidateProfessions(p.Professions); err != nil {
		return err
	}
	if p.ReplaceSkillRanks {
		if err := ValidateSkillRanks(p.SkillRanks); err != nil {
			return err
		}
	} else if len(p.SkillRanks) > 0 {
		return fmt.Errorf("%w: ranks are given without replace_skill_ranks", ErrInvalid)
	}
	if p.ReplaceActionBar {
		if err := ValidateActionBar(p.ActionBar); err != nil {
			return err
		}
	} else if len(p.ActionBar) > 0 {
		return fmt.Errorf("%w: action bar slots are given without replace_action_bar", ErrInvalid)
	}
	return nil
}

// Experience returns the character's XP rows by leveling type. A character with none has an empty list; an unknown
// character is ErrNotFound.
func (s *Store) Experience(ctx context.Context, characterID string) ([]Experience, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT leveling_type_guid, xp, debt FROM character_experience WHERE character_id = $1 ORDER BY leveling_type_guid`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Experience
	for rows.Next() {
		var e Experience
		if err := rows.Scan(&e.LevelingTypeGuid, &e.XP, &e.Debt); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return nil, err
		}
		if !exists {
			return nil, ErrNotFound
		}
	}
	return out, nil
}

// SaveProgress writes the level, the class (with its name and archetype id) and the experience rows in one transaction
// under the character's row lock (the latest save wins; worldd is the only writer while a character plays). ErrNotFound for
// an unknown character; ErrInvalid, with nothing changed, for a progress Validate refuses.
func (s *Store) SaveProgress(ctx context.Context, characterID string, p Progress) (Character, error) {
	if !idPattern.MatchString(characterID) {
		return Character{}, ErrNotFound
	}
	if err := p.Validate(); err != nil {
		return Character{}, err
	}
	className, archetype := "", int32(0)
	if p.ClassID != 0 {
		className, _ = chardef.ClassName(p.ClassID)
		archetype = chardef.ArchetypeID(p.ClassID)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Character{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		return Character{}, err
	}
	c, err := scan(tx.QueryRow(ctx, `
		UPDATE characters SET
			level = CASE WHEN $2 = 0 THEN level ELSE $2 END,
			class_id = CASE WHEN $3 = 0 THEN class_id ELSE $3 END,
			class_name = CASE WHEN $3 = 0 THEN class_name ELSE $4 END,
			archetype_id = CASE WHEN $3 = 0 THEN archetype_id ELSE $5 END,
			updated_at = now()
		WHERE id = $1
		RETURNING `+columns, characterID, p.Level, p.ClassID, className, archetype))
	if errors.Is(err, pgx.ErrNoRows) {
		return Character{}, ErrNotFound
	}
	if err != nil {
		return Character{}, invalidData(err)
	}
	for _, e := range p.Experience {
		if _, err := tx.Exec(ctx, `INSERT INTO character_experience (character_id, leveling_type_guid, xp, debt) VALUES ($1, $2, $3, $4)
			ON CONFLICT (character_id, leveling_type_guid) DO UPDATE SET xp = EXCLUDED.xp, debt = EXCLUDED.debt, updated_at = now()`,
			characterID, e.LevelingTypeGuid, e.XP, e.Debt); err != nil {
			return Character{}, itemsError(err)
		}
	}
	if err := saveProfessions(ctx, tx, characterID, p.Professions); err != nil {
		return Character{}, itemsError(err)
	}
	if p.ReplaceSkillRanks {
		if err := replaceSkillRanks(ctx, tx, characterID, p.SkillRanks); err != nil {
			return Character{}, itemsError(err)
		}
	}
	if p.ReplaceActionBar {
		if err := replaceActionBar(ctx, tx, characterID, p.ActionBar); err != nil {
			return Character{}, itemsError(err)
		}
	}
	return c, tx.Commit(ctx)
}

// ---- the GM starter set -------------------------------------------------------------------------------------------------------

// StarterSetMaker makes the GM starter set of a character of a class at a level (internal/kit over content.db): Backpack
// items whose slots the caller assigns (PlaceInBackpack).
type StarterSetMaker interface {
	StarterSet(classID, level int32) ([]Item, error)
}

// CoA / C Y B

// MountSetMaker is what a StarterSetMaker may also be: it makes the test mounts alone (the mounts milestone, stage 3), for the
// characters that had the set before it held mounts (GrantMissingStarterMounts).
type MountSetMaker interface {
	StarterMounts() ([]Item, error)
}

// PlaceInBackpack puts `add` into the lowest free Backpack slots of `existing`, in order, and returns the placed items (with
// their slots) and how many did not fit (the bag was full).
func PlaceInBackpack(existing []Item, add []Item) (placed []Item, skipped int) {
	taken := map[int32]bool{}
	for _, it := range existing {
		if it.Container == Backpack {
			taken[it.Slot] = true
		}
	}
	slot := int32(0)
	for _, it := range add {
		for slot < SlotCount[Backpack] && taken[slot] {
			slot++
		}
		if slot >= SlotCount[Backpack] {
			skipped++
			continue
		}
		it.Container = Backpack
		it.Slot = slot
		taken[slot] = true
		placed = append(placed, it)
	}
	return placed, skipped
}

// StarterSetResult says what GrantMissingStarterSets did.
type StarterSetResult struct {
	Waiting int      // characters that had not been given the set
	Granted int      // given it (marked, items placed)
	Items   int      // items placed in all
	Skipped []string // "name (id): why" for each left for the next start
	NoRoom  int      // items that did not fit a full bag (the character is marked anyway: the set is given once)
}

// [CoA :: C.Y.B]

// GrantMissingStarterSets gives the GM starter set to every character whose gm_starter_set_granted_at is NULL (plan D1,
// AOC_GM_STARTER_SET=1): once per character. Each grant is its own transaction that locks the character, checks the column
// again, puts the set into the free bag slots (what does not fit is logged and left out) and sets the column, so running it
// again changes nothing and it cannot race a save of worldd's. A row that stays locked past grantLockTimeout is skipped for
// this start; a character whose class is NULL is skipped with the reason.
func (s *Store) GrantMissingStarterSets(ctx context.Context, sets StarterSetMaker) (StarterSetResult, error) {
	var res StarterSetResult
	type target struct {
		id, name string
		class    int32
		level    int32
	}
	rows, err := s.pool.Query(ctx, `SELECT id, name, COALESCE(class_id, 0), level FROM characters WHERE gm_starter_set_granted_at IS NULL ORDER BY created_at, id`)
	if err != nil {
		return res, err
	}
	var targets []target
	for rows.Next() {
		var t target
		if err := rows.Scan(&t.id, &t.name, &t.class, &t.level); err != nil {
			rows.Close()
			return res, err
		}
		targets = append(targets, t)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return res, err
	}
	res.Waiting = len(targets)
	for _, t := range targets {
		skip := func(why string, args ...any) {
			res.Skipped = append(res.Skipped, fmt.Sprintf("%s (%s): %s", t.name, t.id, why))
			slog.Warn("character: no GM starter set granted (backfill)", append([]any{"character_id", t.id, "name", t.name, "why", why}, args...)...)
		}
		if t.class == 0 {
			skip("its class is not known (class_id is NULL)")
			continue
		}
		set, err := sets.StarterSet(t.class, t.level)
		if err != nil {
			skip(err.Error(), "class_id", t.class, "level", t.level)
			continue
		}
		placed, noRoom, err := s.grantBag(ctx, t.id, set, "gm_starter_set_granted_at", "gm_starter_set_granted_at = now(), gm_starter_mounts_granted_at = now()", false)
		if errors.Is(err, errKitLockTimeout) {
			skip(fmt.Sprintf("its row stayed locked by another transaction for %s (a save in flight?); the next start grants it", grantLockTimeout))
			continue
		}
		if err != nil {
			return res, fmt.Errorf("GM starter set of %s (%s): %w", t.name, t.id, err)
		}
		if placed < 0 {
			continue // granted meanwhile
		}
		res.Granted++
		res.Items += placed
		res.NoRoom += noRoom
		slog.Info("character: GM starter set granted (backfill)", "character_id", t.id, "name", t.name, "class_id", t.class, "level", t.level,
			"items", placed, "no_room", noRoom)
	}
	slog.Info("character: GM starter set backfill", "characters_waiting", res.Waiting, "granted", res.Granted, "items", res.Items, "no_room", res.NoRoom,
		"skipped", len(res.Skipped))
	return res, nil
}

// grantBag places `set` in the character's bag and marks the character (`mark` is the SET clause), under its row lock; placed is -1
// when the character's `guard` column was set meanwhile (or the character is gone) and nothing was written.  With onlyMissing the guard
// is not asked and an item of the set whose record the character already holds anywhere (bag, worn, quest storage) is left out of it;
// nothing is written (nor marked) when nothing is missing.
func (s *Store) grantBag(ctx context.Context, characterID string, set []Item, guard, mark string, onlyMissing bool) (placed, noRoom int, err error) {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return 0, 0, err
	}
	defer tx.Rollback(ctx)
	if _, err := tx.Exec(ctx, fmt.Sprintf(`SET LOCAL lock_timeout = '%dms'`, grantLockTimeout.Milliseconds())); err != nil {
		return 0, 0, err
	}
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		if errors.Is(err, ErrNotFound) {
			return -1, 0, nil
		}
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "55P03" {
			return 0, 0, errKitLockTimeout
		}
		return 0, 0, err
	}
	if !onlyMissing {
		var granted *time.Time
		if err := tx.QueryRow(ctx, `SELECT `+guard+` FROM characters WHERE id = $1`, characterID).Scan(&granted); err != nil {
			return 0, 0, err
		}
		if granted != nil {
			return -1, 0, nil
		}
	}
	rows, err := tx.Query(ctx, allItemsSQL, characterID)
	if err != nil {
		return 0, 0, err
	}
	var existing []Item
	for rows.Next() {
		var it Item
		if err := rows.Scan(&it.Container, &it.Slot, &it.ItemRecordID, &it.InstanceGUID, &it.Quantity, &it.Quality, &it.Durability, &it.CreatedAt,
			&it.PersistedHealth, &it.PersistedMana, &it.LockExpiration); err != nil {
			rows.Close()
			return 0, 0, err
		}
		existing = append(existing, it)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return 0, 0, err
	}
	if onlyMissing {
		held := map[int64]bool{}
		for _, it := range existing {
			held[it.ItemRecordID] = true
		}
		var missing []Item
		for _, it := range set {
			if !held[it.ItemRecordID] {
				missing = append(missing, it)
			}
		}
		if len(missing) == 0 {
			return 0, 0, nil // nothing to give: no write, no mark
		}
		set = missing
	}
	items, skipped := PlaceInBackpack(existing, set)
	all := append(append([]Item(nil), existing...), items...)
	sort.SliceStable(all, func(i, j int) bool { return all[i].Slot < all[j].Slot })
	if err := ValidateItems(all); err != nil {
		return 0, 0, err
	}
	if err := insertItems(ctx, tx, characterID, items); err != nil {
		return 0, 0, itemsError(err)
	}
	if _, err := tx.Exec(ctx, `UPDATE characters SET `+mark+`, updated_at = now() WHERE id = $1`, characterID); err != nil {
		return 0, 0, err
	}
	return len(items), skipped, tx.Commit(ctx)
}

// StarterMountsResult says what GrantMissingStarterMounts did.
type StarterMountsResult struct {
	Waiting int // characters that have the set (each is looked at)
	Granted int // given a mount (items placed)
	Items   int // items placed in all
	NoRoom  int // items that did not fit a full bag (they are asked for again at the next start)
}

// (C | O | A : Cyb)

// GrantMissingStarterMounts keeps the test mounts of the GM starter set (the mounts milestone, stage 3) stocked: every character
// that has the set (gm_starter_set_granted_at) is given each mount of the maker that it holds nowhere (bag, worn, quest storage).
// A character that has not had the set gets the mounts with it (GrantMissingStarterSets); one that holds them all is not written to.
// It is a top-up, not a once-only grant (fix 2 of stage 3: the set changed, the six flyers only a node's mayor can use were
// replaced by the two a client lets anyone summon, and the characters that had the old set must get them), so a test mount that was
// thrown away comes back at the next start.  Each grant is its own transaction under the row lock, as the set's is; what does not
// fit the bag is logged and left out.
func (s *Store) GrantMissingStarterMounts(ctx context.Context, maker MountSetMaker) (StarterMountsResult, error) {
	var res StarterMountsResult
	rows, err := s.pool.Query(ctx, `SELECT id, name FROM characters WHERE gm_starter_set_granted_at IS NOT NULL ORDER BY created_at, id`)
	if err != nil {
		return res, err
	}
	type target struct{ id, name string }
	var targets []target
	for rows.Next() {
		var t target
		if err := rows.Scan(&t.id, &t.name); err != nil {
			rows.Close()
			return res, err
		}
		targets = append(targets, t)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return res, err
	}
	res.Waiting = len(targets)
	for _, t := range targets {
		mounts, err := maker.StarterMounts()
		if err != nil {
			slog.Warn("character: no test mounts granted (backfill)", "character_id", t.id, "name", t.name, "why", err.Error())
			continue
		}
		placed, noRoom, err := s.grantBag(ctx, t.id, mounts, "", "gm_starter_mounts_granted_at = COALESCE(gm_starter_mounts_granted_at, now())", true)
		if errors.Is(err, errKitLockTimeout) {
			slog.Warn("character: no test mounts granted (backfill): its row stayed locked; the next start grants them", "character_id", t.id, "name", t.name)
			continue
		}
		if err != nil {
			return res, fmt.Errorf("test mounts of %s (%s): %w", t.name, t.id, err)
		}
		if placed <= 0 && noRoom == 0 {
			continue // holds them all (or is gone)
		}
		res.NoRoom += noRoom
		if placed > 0 {
			res.Granted++
			res.Items += placed
		}
		slog.Info("character: GM starter set test mounts granted (top-up)", "character_id", t.id, "name", t.name, "items", placed, "no_room", noRoom)
	}
	if res.Granted > 0 || res.NoRoom > 0 {
		slog.Info("character: GM starter set test mounts top-up", "characters_with_the_set", res.Waiting, "granted", res.Granted, "items", res.Items, "no_room", res.NoRoom)
	}
	return res, nil
}
