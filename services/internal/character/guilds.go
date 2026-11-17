// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package character

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

// Guilds (social, step 9, migration 0026): a guild is one row of guilds, its three ranks are guild_ranks and its members guild_members; a character's join cooldown is
// character_guild_cooldowns.  worldd reads the guild of a character with the character at the login (GetCharacter) and saves the whole guild after every change
// (SaveGuild), whose version says which state it wrote over.  The rank names and permissions are the design data's DefaultRanks (Guild_Information_Base); the
// store only checks what the wire and the client can carry.

const (
	// GuildRanks is how many ranks a guild has: the design data's DefaultRanks (Guild Leader, Officer, Member).
	GuildRanks = 3
	// MaxGuildMembers bounds a guild: the design data's StartingCapacity.  The size perks (15 more each) are not built, so no guild grows past it.
	MaxGuildMembers = 50
	// MinGuildNameLength bounds a guild's name's shortest form, in bytes.  The game client checks its own minimum and maximum (from a data asset this project does
	// not extract) and the letters-and-single-spaces form; the store keeps the form, this minimum and MaxGuildNameLength (friends.go), and worldd checks the client's
	// bounds before it sends a create.
	MinGuildNameLength = 3
	// MaxGuildRankNameLength bounds a rank's name, in bytes.
	MaxGuildRankNameLength = 32
	// GuildPermissionBits is the number of permission booleans a rank carries (the design data's CharacterAlignmentRecordBase permissions).
	GuildPermissionBits = 35
	// GuildCooldownMs is the join cooldown a character has after leaving a guild: the data's GuildCooldownDurationSeconds (432,000 s) in milliseconds.
	GuildCooldownMs int64 = 432000 * 1000
)

// ErrGuildNameTaken is returned by SaveGuild when another guild already has the name (the case of the letters does not count).
var ErrGuildNameTaken = errors.New("guild name taken")

// ErrGuildVersion is returned by SaveGuild when the version it writes over is not the guild's current one: another save (a second session, another world) wrote in
// between, and its state is newer.  The caller reads the guild again rather than writing over the change.
var ErrGuildVersion = errors.New("guild version is stale")

// guildNamePattern is the client's own name form: ASCII letters with single spaces between the words (the client also refuses a bad word and a double space; the
// store checks the form and the length).
var guildNamePattern = regexp.MustCompile(`^[A-Za-z]+( [A-Za-z]+)*$`)

// GuildRank is one rank of a guild: its name and its 35 permissions as a mask, bit 0 the first of the record's order (bInvitePlayer ... bit 34 bAdjustGuildPoints).
type GuildRank struct {
	Name        string
	Permissions uint64
}

// GuildMember is one member of a guild: its character GUID and rank, when it joined and when it was last seen online (Unix milliseconds).  Its name, level and class
// are read from the characters table when the guild is read (not stored here), so they are current for a member that is offline.
type GuildMember struct {
	CharacterID  string
	Name         string
	Rank         int32
	Level        int32
	ClassID      int32
	JoinedMs     int64
	LastOnlineMs int64
}

// Guild is a guild: its GUID and name, the save version (0: a create), its ranks, its level, experience, capacity and level cap, and its members.  A read for one
// character also fills that character's rank (SelfRank; -1: not a member) and its join cooldown (CooldownUntilMs; 0: none).  Disband asks SaveGuild to remove the guild.
type Guild struct {
	ID              string
	Name            string
	Version         int64
	Ranks           []GuildRank
	Level           int32
	Experience      int32
	Capacity        int32
	LevelCap        int32
	Members         []GuildMember
	SelfRank        int32
	CooldownUntilMs int64
	Disband         bool
}

// validateGuild checks a guild before it is written: its id and name, its three ranks (names and masks), the leader, the caps, and the members' ids and ranks.
func validateGuild(g Guild) error {
	if !idPattern.MatchString(g.ID) || g.ID == strings.Repeat("0", 32) {
		return fmt.Errorf("%w: guild id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, g.ID)
	}
	if g.Name == "" || len(g.Name) > MaxGuildNameLength || !utf8.ValidString(g.Name) || g.Name != strings.TrimSpace(g.Name) {
		return fmt.Errorf("%w: guild name %q is empty, over %d bytes or not trimmed text", ErrInvalid, g.Name, MaxGuildNameLength)
	}
	if !guildNamePattern.MatchString(g.Name) {
		return fmt.Errorf("%w: guild name %q is not letters with single spaces between the words", ErrInvalid, g.Name)
	}
	if len(g.Name) < MinGuildNameLength {
		return fmt.Errorf("%w: guild name %q is shorter than %d bytes", ErrInvalid, g.Name, MinGuildNameLength)
	}
	if len(g.Ranks) != GuildRanks {
		return fmt.Errorf("%w: a guild has %d ranks, not %d", ErrInvalid, GuildRanks, len(g.Ranks))
	}
	for i, r := range g.Ranks {
		if r.Name == "" || len(r.Name) > MaxGuildRankNameLength || !utf8.ValidString(r.Name) || r.Name != strings.TrimSpace(r.Name) {
			return fmt.Errorf("%w: rank %d's name %q is empty, over %d bytes or not trimmed text", ErrInvalid, i, r.Name, MaxGuildRankNameLength)
		}
		if r.Permissions>>GuildPermissionBits != 0 {
			return fmt.Errorf("%w: rank %d has permissions %#x (below 2^%d)", ErrInvalid, i, r.Permissions, GuildPermissionBits)
		}
	}
	if g.Level < 1 || g.Level > 10 {
		return fmt.Errorf("%w: guild level %d is outside 1 to 10", ErrInvalid, g.Level)
	}
	if g.Experience < 0 {
		return fmt.Errorf("%w: guild experience %d is negative", ErrInvalid, g.Experience)
	}
	if g.Capacity < 1 || g.Capacity > 200 {
		return fmt.Errorf("%w: guild capacity %d is outside 1 to 200", ErrInvalid, g.Capacity)
	}
	if g.LevelCap < 1 || g.LevelCap > 50 {
		return fmt.Errorf("%w: guild level cap %d is outside 1 to 50", ErrInvalid, g.LevelCap)
	}
	if len(g.Members) == 0 || len(g.Members) > MaxGuildMembers {
		return fmt.Errorf("%w: a guild has 1 to %d members, not %d", ErrInvalid, MaxGuildMembers, len(g.Members))
	}
	if len(g.Members) > int(g.Capacity) {
		return fmt.Errorf("%w: the guild holds %d members but its capacity is %d", ErrInvalid, len(g.Members), g.Capacity)
	}
	seen := make(map[string]bool, len(g.Members))
	leaders := 0
	for i, m := range g.Members {
		if !idPattern.MatchString(m.CharacterID) || m.CharacterID == strings.Repeat("0", 32) {
			return fmt.Errorf("%w: member %d's character id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, i, m.CharacterID)
		}
		if seen[m.CharacterID] {
			return fmt.Errorf("%w: member %d: %q again", ErrInvalid, i, m.CharacterID)
		}
		seen[m.CharacterID] = true
		if m.Rank < 0 || m.Rank >= GuildRanks {
			return fmt.Errorf("%w: member %d has rank %d (0 to %d)", ErrInvalid, i, m.Rank, GuildRanks-1)
		}
		if m.Rank == 0 {
			leaders++
		}
		if m.JoinedMs < 0 || m.LastOnlineMs < 0 {
			return fmt.Errorf("%w: member %d has a negative time", ErrInvalid, i)
		}
	}
	if leaders != 1 {
		return fmt.Errorf("%w: the guild has %d leaders, not one", ErrInvalid, leaders)
	}
	return nil
}

// SaveGuild writes a guild whole: its row, its three ranks and its members, in one transaction.  Version 0 creates it (the name must be free) and any other version
// must be the guild's current one; the save writes one more and returns it.  ErrGuildNameTaken for a name another guild has; ErrGuildVersion for a stale save;
// ErrNotFound for a guild the version names and the table does not hold; ErrInvalid for a row the rules or the character table refuse.
func (s *Store) SaveGuild(ctx context.Context, g Guild) (Guild, error) {
	if g.Disband {
		if !idPattern.MatchString(g.ID) || g.Version <= 0 {
			return Guild{}, fmt.Errorf("%w: a disband names the guild's id and its version", ErrInvalid)
		}
		tx, err := s.pool.Begin(ctx)
		if err != nil {
			return Guild{}, err
		}
		defer tx.Rollback(ctx)
		var current int64
		if err := tx.QueryRow(ctx, `SELECT version FROM guilds WHERE id = $1 FOR UPDATE`, g.ID).Scan(&current); err != nil {
			if errors.Is(err, pgx.ErrNoRows) {
				return Guild{}, ErrNotFound
			}
			return Guild{}, err
		}
		if current != g.Version {
			return Guild{}, fmt.Errorf("%w: it holds %d, the disband was read at %d", ErrGuildVersion, current, g.Version)
		}
		if _, err := tx.Exec(ctx, `DELETE FROM guilds WHERE id = $1`, g.ID); err != nil {
			return Guild{}, err
		}
		if err := tx.Commit(ctx); err != nil {
			return Guild{}, err
		}
		g.Version = current + 1
		return g, nil
	}
	if err := validateGuild(g); err != nil {
		return Guild{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Guild{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if g.Version == 0 {
		if _, err := tx.Exec(ctx, `INSERT INTO guilds (id, name, version, level, experience, capacity, level_cap) VALUES ($1, $2, 1, $3, $4, $5, $6)`,
			g.ID, g.Name, g.Level, g.Experience, g.Capacity, g.LevelCap); err != nil {
			return Guild{}, guildError(err)
		}
	} else {
		var current int64
		if err := tx.QueryRow(ctx, `SELECT version FROM guilds WHERE id = $1 FOR UPDATE`, g.ID).Scan(&current); err != nil {
			if errors.Is(err, pgx.ErrNoRows) {
				return Guild{}, ErrNotFound
			}
			return Guild{}, err
		}
		if current != g.Version {
			return Guild{}, fmt.Errorf("%w: it holds %d, the save was read at %d", ErrGuildVersion, current, g.Version)
		}
		if _, err := tx.Exec(ctx, `UPDATE guilds SET name = $2, version = version + 1, level = $3, experience = $4, capacity = $5, level_cap = $6, updated_at = now()
			WHERE id = $1`, g.ID, g.Name, g.Level, g.Experience, g.Capacity, g.LevelCap); err != nil {
			return Guild{}, guildError(err)
		}
	}
	// The ranks and the members are the wanted state: the rows are replaced as a whole.
	if _, err := tx.Exec(ctx, `DELETE FROM guild_ranks WHERE guild_id = $1`, g.ID); err != nil {
		return Guild{}, err
	}
	for i, r := range g.Ranks {
		if _, err := tx.Exec(ctx, `INSERT INTO guild_ranks (guild_id, rank_index, rank_name, permissions) VALUES ($1, $2, $3, $4)`, g.ID, i, r.Name, r.Permissions); err != nil {
			return Guild{}, guildError(err)
		}
	}
	if _, err := tx.Exec(ctx, `DELETE FROM guild_members WHERE guild_id = $1`, g.ID); err != nil {
		return Guild{}, err
	}
	for _, m := range g.Members {
		if _, err := tx.Exec(ctx, `INSERT INTO guild_members (guild_id, character_id, rank_index, joined_ms, last_online_ms) VALUES ($1, $2, $3, $4, $5)`,
			g.ID, m.CharacterID, m.Rank, m.JoinedMs, m.LastOnlineMs); err != nil {
			return Guild{}, guildError(err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return Guild{}, err
	}
	out := g
	if g.Version == 0 {
		out.Version = 1
	} else {
		out.Version = g.Version + 1
	}
	return out, nil
}

// GuildByCharacter reads the guild a character is in, its ranks, its members (with the character row's current name, level and class), the character's rank and its
// join cooldown.  inGuild is false when the character is in no guild (the returned Guild then carries only the cooldown); an unknown character is ErrNotFound.
func (s *Store) GuildByCharacter(ctx context.Context, characterID string) (Guild, bool, error) {
	if !idPattern.MatchString(characterID) {
		return Guild{}, false, ErrNotFound
	}
	var exists bool
	if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return Guild{}, false, err
	}
	if !exists {
		return Guild{}, false, ErrNotFound
	}
	var out Guild
	err := s.pool.QueryRow(ctx, `SELECT g.id, g.name, g.version, g.level, g.experience, g.capacity, g.level_cap, m.rank_index
		FROM guild_members m JOIN guilds g ON g.id = m.guild_id WHERE m.character_id = $1`, characterID).
		Scan(&out.ID, &out.Name, &out.Version, &out.Level, &out.Experience, &out.Capacity, &out.LevelCap, &out.SelfRank)
	if errors.Is(err, pgx.ErrNoRows) {
		// no guild: the cooldown may still be running
		out = Guild{SelfRank: -1}
		if err := s.pool.QueryRow(ctx, `SELECT until_ms FROM character_guild_cooldowns WHERE character_id = $1`, characterID).Scan(&out.CooldownUntilMs); err != nil && !errors.Is(err, pgx.ErrNoRows) {
			return Guild{}, false, err
		}
		return out, false, nil
	}
	if err != nil {
		return Guild{}, false, err
	}
	if err := s.pool.QueryRow(ctx, `SELECT until_ms FROM character_guild_cooldowns WHERE character_id = $1`, characterID).Scan(&out.CooldownUntilMs); err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return Guild{}, false, err
	}
	rows, err := s.pool.Query(ctx, `SELECT rank_index, rank_name, permissions FROM guild_ranks WHERE guild_id = $1 ORDER BY rank_index`, out.ID)
	if err != nil {
		return Guild{}, false, err
	}
	for rows.Next() {
		var idx int32
		var r GuildRank
		if err := rows.Scan(&idx, &r.Name, &r.Permissions); err != nil {
			rows.Close()
			return Guild{}, false, err
		}
		out.Ranks = append(out.Ranks, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return Guild{}, false, err
	}
	rows, err = s.pool.Query(ctx, `SELECT m.character_id, c.name, m.rank_index, c.level, c.class_id, m.joined_ms, m.last_online_ms
		FROM guild_members m JOIN characters c ON c.id = m.character_id WHERE m.guild_id = $1 ORDER BY m.joined_ms, m.character_id`, out.ID)
	if err != nil {
		return Guild{}, false, err
	}
	for rows.Next() {
		var m GuildMember
		if err := rows.Scan(&m.CharacterID, &m.Name, &m.Rank, &m.Level, &m.ClassID, &m.JoinedMs, &m.LastOnlineMs); err != nil {
			rows.Close()
			return Guild{}, false, err
		}
		out.Members = append(out.Members, m)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return Guild{}, false, err
	}
	return out, true, nil
}

// SetGuildCooldown writes the time before which a character cannot create or join a guild again (Unix milliseconds; 0 clears it).  ErrNotFound for an unknown
// character, ErrInvalid for a negative time.
func (s *Store) SetGuildCooldown(ctx context.Context, characterID string, untilMs int64) error {
	if !idPattern.MatchString(characterID) {
		return ErrNotFound
	}
	if untilMs < 0 {
		return fmt.Errorf("%w: a cooldown of %d ms is negative", ErrInvalid, untilMs)
	}
	if untilMs == 0 {
		_, err := s.pool.Exec(ctx, `DELETE FROM character_guild_cooldowns WHERE character_id = $1`, characterID)
		return err
	}
	tag, err := s.pool.Exec(ctx, `INSERT INTO character_guild_cooldowns (character_id, guild_id, until_ms)
		SELECT $1, NULL, $2 WHERE EXISTS (SELECT 1 FROM characters WHERE id = $1)
		ON CONFLICT (character_id) DO UPDATE SET guild_id = NULL, until_ms = EXCLUDED.until_ms`, characterID, untilMs)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

// guildError maps the store's constraint violations: a guild name another guild has is ErrGuildNameTaken, a row the rules refuse is ErrInvalid, and a member the
// character table does not know is ErrInvalid too (a delete of a member's character during the save).
func guildError(err error) error {
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		switch pgErr.Code {
		case "23505":
			if pgErr.ConstraintName == "guilds_name_lower" {
				return fmt.Errorf("%w: %s", ErrGuildNameTaken, pgErr.Detail)
			}
			return fmt.Errorf("%w: %s", ErrInvalid, pgErr.Message)
		case "23503":
			return fmt.Errorf("%w: %s", ErrInvalid, pgErr.Detail)
		case "23514":
			return fmt.Errorf("%w: %s", ErrInvalid, pgErr.Message)
		}
		if strings.HasPrefix(pgErr.Code, "22") {
			return fmt.Errorf("%w: %s", ErrInvalid, pgErr.Message)
		}
	}
	return err
}
