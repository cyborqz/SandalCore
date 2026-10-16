// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

// The storages a character's items are in. Backpack, Equipment and Quest are the values of character_items.container (plan B2); Resource0 to Resource2 (Gathering stage 3,
// "Resource storage") are the values of character_resource_items.container: the containers of the resource bags worn in Equipment slots 23, 24 and 25.
const (
	Backpack  = "Backpack"
	Equipment = "Equipment"
	Quest     = "Quest"
	Resource0 = "Resource0"
	Resource1 = "Resource1"
	Resource2 = "Resource2"
)

// MaxResourceSlots is the most cells a resource bag has (a slot of a resource container is the index of the top-left cell of its item, row by row): the largest bag
// in the data is the Livestream promotion bag, 8 x 30 = 240. Whether an item fits its bag's real grid is worldd's check (this service has no item data).
const MaxResourceSlots = 256

// SlotCount is the number of slots of each storage, as the client's own storage payloads have it (MaximumItems): the
// Backpack's 70, the Equipment's 83 (one for every EEquipSlot value below cCount) and the Quest storage's 100; a resource container's is the most any bag has.
var SlotCount = map[string]int32{Backpack: 70, Equipment: 83, Quest: 100, Resource0: MaxResourceSlots, Resource1: MaxResourceSlots, Resource2: MaxResourceSlots}

// MaxItems is the most items a character can have: every slot of every storage.
const MaxItems = 70 + 83 + 100 + 3*MaxResourceSlots

// IsResource says whether a container is one of the three resource containers (kept in character_resource_items).
func IsResource(container string) bool {
	return container == Resource0 || container == Resource1 || container == Resource2
}

// Limits of one item's numbers, the constraints of migration 0004.
const (
	MaxQuantity       = 1_000_000
	MaxQuality        = 200          // Artifact 101-200 in the item's quality curve
	maxDurable        = 1e9          // durability is a float32 below this
	maxPersisted      = 1e9          // a mount item's persisted Health and Mana (migration 0007)
	maxLockExpiration = 253402300799 // Unix seconds: the end of the year 9999 (migration 0007)
	minCreated        = 1_000        // created_at_ms (and Item.CreatedAt) must be after the epoch by at least this many ms
)

var guidPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

// Item is one item instance a character owns (a row of character_items).
type Item struct {
	Container    string // Backpack, Equipment, Quest, Resource0, Resource1 or Resource2
	Slot         int32  // where in the storage: 0 to SlotCount-1 (an Equipment slot is the client's EEquipSlot; a resource slot the index of the item's top-left cell)
	ItemRecordID int64  // the design database's ItemRecord Guid
	InstanceGUID string // the instance's own id: 32 lowercase hex digits, not all zero
	Quantity     int32
	Quality      int32 // an index into the item's quality curve
	Durability   float32
	CreatedAt    time.Time

	// What a mount item persists (the mounts milestone, migration 0007; retail's login shows it on every mount item): the mount's Health and Mana, and the
	// time until which the item cannot be summoned. nil and 0 for every other item.
	PersistedHealth *float32
	PersistedMana   *float32
	LockExpiration  int64 // Unix seconds; 0 or a past time: not locked
}

// [C | O | A ~ C Y B]

// Validate says what is wrong with the item: the constraints of the table, checked where the message can say which.
func (it Item) Validate() error {
	n, known := SlotCount[it.Container]
	switch {
	case !known:
		return fmt.Errorf("%w: item container %q is not Backpack, Equipment, Quest, Resource0, Resource1 or Resource2", ErrInvalid, it.Container)
	case it.Slot < 0 || it.Slot >= n:
		return fmt.Errorf("%w: slot %d is not a slot of the %s (0 to %d)", ErrInvalid, it.Slot, it.Container, n-1)
	case it.ItemRecordID <= 0:
		return fmt.Errorf("%w: item record id %d is not a record", ErrInvalid, it.ItemRecordID)
	case !guidPattern.MatchString(it.InstanceGUID) || it.InstanceGUID == strings.Repeat("0", 32):
		return fmt.Errorf("%w: instance guid %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, it.InstanceGUID)
	case it.Quantity < 1 || it.Quantity > MaxQuantity:
		return fmt.Errorf("%w: quantity %d is not 1 to %d", ErrInvalid, it.Quantity, MaxQuantity)
	case it.Quality < 1 || it.Quality > MaxQuality:
		return fmt.Errorf("%w: quality %d is not 1 to %d", ErrInvalid, it.Quality, MaxQuality)
	case math.IsNaN(float64(it.Durability)) || it.Durability < 0 || float64(it.Durability) > maxDurable:
		return fmt.Errorf("%w: durability %v is not a number from 0 to %g", ErrInvalid, it.Durability, maxDurable)
	case it.CreatedAt.IsZero() || it.CreatedAt.UnixMilli() < minCreated:
		return fmt.Errorf("%w: the item has no creation time", ErrInvalid)
	case !validPersisted(it.PersistedHealth):
		return fmt.Errorf("%w: persisted health %v is not a number from 0 to %g", ErrInvalid, *it.PersistedHealth, maxPersisted)
	case !validPersisted(it.PersistedMana):
		return fmt.Errorf("%w: persisted mana %v is not a number from 0 to %g", ErrInvalid, *it.PersistedMana, maxPersisted)
	case it.LockExpiration < 0 || it.LockExpiration > maxLockExpiration:
		return fmt.Errorf("%w: lock expiration %d is not 0 to %d (Unix seconds)", ErrInvalid, it.LockExpiration, int64(maxLockExpiration))
	}
	return nil
}

// validPersisted says whether a persisted stat is none or a number from 0 to maxPersisted.
func validPersisted(v *float32) bool {
	return v == nil || (!math.IsNaN(float64(*v)) && *v >= 0 && float64(*v) <= maxPersisted)
}

// ValidateItems checks every item and that no two share a slot or an instance guid, and that there are not more than
// MaxItems. An empty set is valid here; SaveInventory refuses it (see there).
func ValidateItems(items []Item) error {
	if len(items) > MaxItems {
		return fmt.Errorf("%w: %d items, a character can have %d", ErrInvalid, len(items), MaxItems)
	}
	slots := make(map[string]bool, len(items))
	guids := make(map[string]bool, len(items))
	for i, it := range items {
		if err := it.Validate(); err != nil {
			return fmt.Errorf("item %d: %w", i, err)
		}
		key := fmt.Sprintf("%s/%d", it.Container, it.Slot)
		if slots[key] {
			return fmt.Errorf("%w: item %d: two items in %s slot %d", ErrInvalid, i, it.Container, it.Slot)
		}
		slots[key] = true
		if guids[it.InstanceGUID] {
			return fmt.Errorf("%w: item %d: two items with instance guid %s", ErrInvalid, i, it.InstanceGUID)
		}
		guids[it.InstanceGUID] = true
	}
	return nil
}

// SortItems puts items in the order Inventory returns them: by storage (Backpack, Equipment, Quest, Resource0, Resource1, Resource2) and slot.
func SortItems(items []Item) {
	order := map[string]int{Backpack: 0, Equipment: 1, Quest: 2, Resource0: 3, Resource1: 4, Resource2: 5}
	sort.SliceStable(items, func(i, j int) bool {
		a, b := items[i], items[j]
		if order[a.Container] != order[b.Container] {
			return order[a.Container] < order[b.Container]
		}
		return a.Slot < b.Slot
	})
}

const itemColumns = `container, slot, item_record_id, instance_guid, quantity, quality, durability, created_at, persisted_health, persisted_mana, lock_expiration`

// itemTable is the table an item is kept in: the Backpack, the Equipment and the Quest storage in character_items, a resource container in character_resource_items.
func itemTable(container string) string {
	if IsResource(container) {
		return "character_resource_items"
	}
	return "character_items"
}

// allItemsSQL reads the items of one character ($1) from both tables, in the order Inventory returns them.
const allItemsSQL = `SELECT ` + itemColumns + ` FROM (
	SELECT ` + itemColumns + `, CASE container WHEN 'Backpack' THEN 0 WHEN 'Equipment' THEN 1 ELSE 2 END AS storage_order FROM character_items WHERE character_id = $1
	UNION ALL
	SELECT ` + itemColumns + `, CASE container WHEN 'Resource0' THEN 3 WHEN 'Resource1' THEN 4 ELSE 5 END FROM character_resource_items WHERE character_id = $1
) all_items ORDER BY storage_order, slot`

// insertItems adds the items of a character in tx, in one batch: each into the table of its container.
func insertItems(ctx context.Context, tx pgx.Tx, characterID string, items []Item) error {
	if len(items) == 0 {
		return nil
	}
	var batch pgx.Batch
	for _, it := range items {
		batch.Queue(`INSERT INTO `+itemTable(it.Container)+` (character_id, `+itemColumns+`) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)`,
			characterID, it.Container, it.Slot, it.ItemRecordID, it.InstanceGUID, it.Quantity, it.Quality, it.Durability, it.CreatedAt,
			it.PersistedHealth, it.PersistedMana, it.LockExpiration)
	}
	res := tx.SendBatch(ctx, &batch)
	for range items {
		if _, err := res.Exec(); err != nil {
			res.Close()
			return err
		}
	}
	return res.Close()
}

// CoA and C | Y | B

// Inventory returns the items of a character: the Backpack's by slot, then the Equipment's, the Quest storage's and the three resource containers'.
// A character with none (it never got a kit) has an empty list; an unknown character is ErrNotFound.
func (s *Store) Inventory(ctx context.Context, characterID string) ([]Item, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, allItemsSQL, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Item
	for rows.Next() {
		var it Item
		if err := rows.Scan(&it.Container, &it.Slot, &it.ItemRecordID, &it.InstanceGUID, &it.Quantity, &it.Quality, &it.Durability, &it.CreatedAt,
			&it.PersistedHealth, &it.PersistedMana, &it.LockExpiration); err != nil {
			return nil, err
		}
		out = append(out, it)
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

// SaveInventory replaces all the items of the character, in both tables, with items, in one transaction (the latest save wins: worldd is
// the only writer while a character plays). ErrNotFound for an unknown character; ErrInvalid, with nothing changed, for items
// that break the rules (ValidateItems) and for no items at all: a character keeps its items until they can be destroyed
// (not in part 1 of the complete character), so an empty save is a bug in the caller, and it would erase the character's
// kit.
func (s *Store) SaveInventory(ctx context.Context, characterID string, items []Item) error {
	if !idPattern.MatchString(characterID) {
		return ErrNotFound
	}
	// Part 2 of the complete character (destroy, sell, consume) is where an inventory can first become empty. It must add an
	// explicit "replace with an empty set" (its own call or a flag) before then: this refusal stays for every caller that does
	// not mean it, so a buggy save of no items can never erase a character's kit.
	if len(items) == 0 {
		return fmt.Errorf("%w: an inventory is never saved empty (a character keeps its items; saving none would erase them)", ErrInvalid)
	}
	if err := ValidateItems(items); err != nil {
		return err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		return err
	}
	for _, table := range []string{"character_items", "character_resource_items"} {
		if _, err := tx.Exec(ctx, `DELETE FROM `+table+` WHERE character_id = $1`, characterID); err != nil {
			return err
		}
	}
	if err := insertItems(ctx, tx, characterID, items); err != nil {
		return itemsError(err)
	}
	return tx.Commit(ctx)
}

// lockCharacter takes the row lock of the character (what serialises a save, a kit grant and a delete), ErrNotFound when
// there is none.
func lockCharacter(ctx context.Context, tx pgx.Tx, characterID string) error {
	var one int
	err := tx.QueryRow(ctx, `SELECT 1 FROM characters WHERE id = $1 FOR UPDATE`, characterID).Scan(&one)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	return err
}

// itemsError turns a violation of the table's constraints (a caller's bad data the checks above missed) into ErrInvalid.
func itemsError(err error) error {
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && (pgErr.Code == "23514" || pgErr.Code == "23505" || strings.HasPrefix(pgErr.Code, "22")) {
		return fmt.Errorf("%w: %s", ErrInvalid, pgErr.Message)
	}
	return err
}

// KitMaker makes the items of a new character: its class's starting kit. Kit returns fresh instances (new guids, durability,
// creation time) each call.
type KitMaker interface {
	Kit(classID int32) ([]Item, error)
}

// BackfillResult says what GrantMissingKits did.
type BackfillResult struct {
	Without int      // characters that had no items
	Granted int      // given their kit
	Items   int      // items granted in all
	Skipped []string // "name (id): why" for each that could not be given one
}

// (C-O-A | C.Y.B)

// GrantMissingKits gives every character that has no items its class's starting kit (plan B2's backfill of the characters
// made before items existed). It never touches a character that has any item: each grant is its own transaction that locks
// the character, checks again that it has none and inserts the kit, so running it again changes nothing, and it cannot
// race a save of the character's items. It is logged: one line per character granted (with the items' count) or skipped,
// and a summary. A character whose class is unknown (class_id NULL) or that the maker cannot make a kit for is skipped, with
// the reason; it is the same next time. So is one whose row stays locked past grantLockTimeout (a save in flight): the
// backfill goes on with the others, and the next run grants it.
func (s *Store) GrantMissingKits(ctx context.Context, kits KitMaker) (BackfillResult, error) {
	var res BackfillResult
	type target struct {
		id, name string
		class    int32
	}
	rows, err := s.pool.Query(ctx, `SELECT c.id, c.name, COALESCE(c.class_id, 0) FROM characters c
		WHERE NOT EXISTS (SELECT 1 FROM character_items i WHERE i.character_id = c.id)
		  AND NOT EXISTS (SELECT 1 FROM character_resource_items r WHERE r.character_id = c.id) ORDER BY c.created_at, c.id`)
	if err != nil {
		return res, err
	}
	var targets []target
	for rows.Next() {
		var t target
		if err := rows.Scan(&t.id, &t.name, &t.class); err != nil {
			rows.Close()
			return res, err
		}
		targets = append(targets, t)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return res, err
	}
	res.Without = len(targets)
	for _, t := range targets {
		skip := func(why string, args ...any) {
			res.Skipped = append(res.Skipped, fmt.Sprintf("%s (%s): %s", t.name, t.id, why))
			slog.Warn("character: no starter kit granted (backfill)", append([]any{"character_id", t.id, "name", t.name, "why", why}, args...)...)
		}
		if t.class == 0 {
			skip("its class is not known (class_id is NULL)")
			continue
		}
		items, err := kits.Kit(t.class)
		if err != nil {
			skip(err.Error(), "class_id", t.class)
			continue
		}
		granted, err := s.grantKit(ctx, t.id, items)
		if errors.Is(err, errKitLockTimeout) {
			skip(fmt.Sprintf("its row stayed locked by another transaction for %s (a save in flight?); the next backfill grants it", grantLockTimeout),
				"class_id", t.class)
			continue
		}
		if err != nil {
			return res, fmt.Errorf("starter kit of %s (%s): %w", t.name, t.id, err)
		}
		if !granted {
			continue // it got items meanwhile
		}
		res.Granted++
		res.Items += len(items)
		slog.Info("character: starter kit granted (backfill)", "character_id", t.id, "name", t.name, "class_id", t.class, "items", len(items))
	}
	slog.Info("character: starter kit backfill", "characters_without_items", res.Without, "granted", res.Granted, "items", res.Items, "skipped", len(res.Skipped))
	return res, nil
}

// grantLockTimeout is how long grantKit waits for the character's row lock (a save of its items in flight holds it for a
// moment); a variable so a test can shorten it.
var grantLockTimeout = 5 * time.Second

// errKitLockTimeout: the row lock was not free within grantLockTimeout (Postgres' lock_not_available, 55P03).
var errKitLockTimeout = errors.New("character: the row lock was not free in time")

// grantKit inserts items for the character when it has none, under the character's row lock; false when it had some (or is
// gone) and nothing was written. It waits for the lock at most grantLockTimeout, then errKitLockTimeout (nothing written).
func (s *Store) grantKit(ctx context.Context, characterID string, items []Item) (bool, error) {
	if err := ValidateItems(items); err != nil {
		return false, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return false, err
	}
	defer tx.Rollback(ctx)
	// the duration is formatted from an integer: SET takes no parameters
	if _, err := tx.Exec(ctx, fmt.Sprintf(`SET LOCAL lock_timeout = '%dms'`, grantLockTimeout.Milliseconds())); err != nil {
		return false, err
	}
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		if errors.Is(err, ErrNotFound) {
			return false, nil
		}
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "55P03" {
			return false, errKitLockTimeout
		}
		return false, err
	}
	var has bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM character_items WHERE character_id = $1) OR EXISTS (SELECT 1 FROM character_resource_items WHERE character_id = $1)`, characterID).Scan(&has); err != nil {
		return false, err
	}
	if has {
		return false, nil
	}
	if err := insertItems(ctx, tx, characterID, items); err != nil {
		return false, itemsError(err)
	}
	return true, tx.Commit(ctx)
}
