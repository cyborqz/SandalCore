// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package character

// Social, step 9 (migration 0026): a guild, its three ranks and its members, kept whole in one transaction and read with one character at a time, plus the join cooldown.
// A save that writes over the wrong version is refused; a member who is offline is read from its character row; a disband removes the guild and leaves the characters; a
// cooldown is set, read and cleared.

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// newGuildCharacter makes a Bard at the account, as the other social tests do.
func newGuildCharacter(t *testing.T, s *Store, acc int64, name string) Character {
	t.Helper()
	c, err := s.Create(context.Background(), New{AccountID: acc, Name: name, ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// guildMemberByID is the row of `id` in the read guild, nil when it is not a member (the rows' order is the table's, not the roster's).
func guildMemberByID(g Guild, id string) *GuildMember {
	for i := range g.Members {
		if g.Members[i].CharacterID == id {
			return &g.Members[i]
		}
	}
	return nil
}

// guildOf builds a guild with the data's three ranks and `members` at the ranks given.
func guildOf(id, name string, members ...GuildMember) Guild {
	g := Guild{ID: id, Name: name, Version: 0, Level: 1, LevelCap: 10, Capacity: 50,
		Ranks: []GuildRank{{"Guild Leader", (1 << 35) - 1}, {"Officer", 0x7ffcfc7f}, {"Member", 0x65006014}}}
	g.Members = members
	return g
}

func guildMember(id string, rank int32) GuildMember { return GuildMember{CharacterID: id, Rank: rank, JoinedMs: 1763000000000} }

func TestGuildSavedWholeIsReadWithARankAcooldownAndTheCharacterRows(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	cyb := newGuildCharacter(t, s, acc, "cyb")
	dee := newGuildCharacter(t, s, acc, "dee")
	id := "11112222333344445555666677778888"
	saved, err := s.SaveGuild(ctx, guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0), guildMember(dee.ID, 2)))
	if err != nil {
		t.Fatalf("creating the guild: %v", err)
	}
	if saved.Version != 1 {
		t.Fatalf("a create returned version %d, want 1", saved.Version)
	}
	g, inGuild, err := s.GuildByCharacter(ctx, cyb.ID)
	if err != nil || !inGuild {
		t.Fatalf("reading cyb's guild: %+v, %v, %v", g, inGuild, err)
	}
	if g.ID != id || g.Name != "Sandal Makers" || g.Version != 1 || g.SelfRank != 0 || g.Capacity != 50 || g.LevelCap != 10 {
		t.Errorf("the guild reads as %+v", g)
	}
	if len(g.Ranks) != 3 || g.Ranks[0].Name != "Guild Leader" || g.Ranks[0].Permissions != (1<<35)-1 || g.Ranks[1].Permissions != 0x7ffcfc7f ||
		g.Ranks[2].Permissions != 0x65006014 {
		t.Errorf("the ranks read as %+v", g.Ranks)
	}
	if len(g.Members) != 2 || guildMemberByID(g, cyb.ID) == nil || guildMemberByID(g, dee.ID) == nil {
		t.Errorf("the members read as %+v", g.Members)
	}
	if m := guildMemberByID(g, cyb.ID); m == nil || m.Name != "cyb" || m.Level != 1 {
		t.Errorf("the leader's row reads as %+v", m)
	}
	if g.CooldownUntilMs != 0 {
		t.Errorf("a member's cooldown is %d, want 0", g.CooldownUntilMs)
	}
	// dee's own read: the same guild and its own rank
	if dg, in, err := s.GuildByCharacter(ctx, dee.ID); err != nil || !in || dg.SelfRank != 2 || dg.Name != "Sandal Makers" {
		t.Errorf("dee's guild: %+v, %v, %v", dg, in, err)
	}
	// a character in none
	other := newGuildCharacter(t, s, acc, "mira")
	if og, in, err := s.GuildByCharacter(ctx, other.ID); err != nil || in || og.CooldownUntilMs != 0 {
		t.Errorf("a character in no guild: %+v, %v, %v", og, in, err)
	}
}

func TestGuildSavesWriteOverTheVersionAndRefuseAStaleOne(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	cyb := newGuildCharacter(t, s, acc, "cyb")
	dee := newGuildCharacter(t, s, acc, "dee")
	id := "11112222333344445555666677778888"
	first, err := s.SaveGuild(ctx, guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0)))
	if err != nil {
		t.Fatal(err)
	}
	// the current version takes the change and the version moves on
	next := guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0), guildMember(dee.ID, 2))
	next.Version = first.Version
	saved, err := s.SaveGuild(ctx, next)
	if err != nil {
		t.Fatalf("the save after the version: %v", err)
	}
	if saved.Version != first.Version+1 {
		t.Fatalf("the save returned version %d, want %d", saved.Version, first.Version+1)
	}
	// the version before it is stale now: the save is refused and writes nothing (the guild keeps its two members)
	stale := guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0))
	stale.Version = first.Version
	if _, err := s.SaveGuild(ctx, stale); !errors.Is(err, ErrGuildVersion) {
		t.Fatalf("a stale save: %v, want ErrGuildVersion", err)
	}
	if g, _, err := s.GuildByCharacter(ctx, cyb.ID); err != nil || len(g.Members) != 2 {
		t.Fatalf("the refused save changed the guild: %+v, %v", g, err)
	}
	// a member who is offline sees the change at its own read, and the roster's level is its character row's
	if _, err := s.SaveProgress(ctx, dee.ID, Progress{Level: 25}); err != nil {
		t.Fatalf("dee's level: %v", err)
	}
	if g, in, err := s.GuildByCharacter(ctx, dee.ID); err != nil || !in || len(g.Members) != 2 || g.SelfRank != 2 {
		t.Fatalf("dee's read after the change: %+v, %v, %v", g, in, err)
	} else if m := guildMemberByID(g, dee.ID); m == nil || m.Level != 25 {
		t.Fatalf("dee's row after the change: %+v", m)
	}
	// a rank change of the whole roster: the leader promotes dee to officer
	promoted := guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0), guildMember(dee.ID, 1))
	promoted.Version = saved.Version
	if _, err := s.SaveGuild(ctx, promoted); err != nil {
		t.Fatal(err)
	}
	if g, _, err := s.GuildByCharacter(ctx, dee.ID); err != nil || g.SelfRank != 1 {
		t.Fatalf("dee after the promotion: %+v, %v", g, err)
	}
}

func TestGuildNameAndRulesAreChecked(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	cyb := newGuildCharacter(t, s, acc, "cyb")
	id := "11112222333344445555666677778888"
	if _, err := s.SaveGuild(ctx, guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0))); err != nil {
		t.Fatal(err)
	}
	other := newGuildCharacter(t, s, acc, "dee")
	for _, tc := range []struct {
		name  string
		guild Guild
		want  error
	}{
		{"the same name in another case", guildOf("22222222222222222222222222222222", "sandal makers", guildMember(other.ID, 0)), ErrGuildNameTaken},
		{"a name with a double space", guildOf("22222222222222222222222222222222", "Sandal  Makers", guildMember(other.ID, 0)), ErrInvalid},
		{"a name with a digit", guildOf("22222222222222222222222222222222", "Sandal 2", guildMember(other.ID, 0)), ErrInvalid},
		{"two ranks", Guild{ID: "22222222222222222222222222222222", Name: "Two Ranks", Level: 1, LevelCap: 10, Capacity: 50,
			Ranks: []GuildRank{{"Leader", 0}, {"Member", 0}}, Members: []GuildMember{guildMember(other.ID, 0)}}, ErrInvalid},
		{"no leader", guildOf("22222222222222222222222222222222", "No Leader", guildMember(other.ID, 2)), ErrInvalid},
		{"a 36th permission", Guild{ID: "22222222222222222222222222222222", Name: "Too Much", Level: 1, LevelCap: 10, Capacity: 50,
			Ranks: []GuildRank{{"Guild Leader", 1 << 35}, {"Officer", 0}, {"Member", 0}}, Members: []GuildMember{guildMember(other.ID, 0)}}, ErrInvalid},
		{"a member not a character", guildOf("22222222222222222222222222222222", "Ghosts", guildMember("ffffffffffffffffffffffffffffffff", 0)), ErrInvalid},
		{"over the capacity", func() Guild {
			g := guildOf("22222222222222222222222222222222", "Too Many", guildMember(other.ID, 0))
			g.Capacity = 1
			g.Members = append(g.Members, guildMember(cyb.ID, 2))
			return g
		}(), ErrInvalid},
	} {
		if _, err := s.SaveGuild(ctx, tc.guild); !errors.Is(err, tc.want) {
			t.Errorf("%s: %v, want %v", tc.name, err, tc.want)
		}
	}
	if g, _, err := s.GuildByCharacter(ctx, other.ID); err != nil || len(g.Members) != 0 || g.Name != "" {
		t.Errorf("a refused guild left something behind: %+v, %v", g, err)
	}
}

func TestGuildDisbandTakesTheGuildAwayAndLeavesTheMembers(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	cyb := newGuildCharacter(t, s, acc, "cyb")
	dee := newGuildCharacter(t, s, acc, "dee")
	id := "11112222333344445555666677778888"
	saved, err := s.SaveGuild(ctx, guildOf(id, "Sandal Makers", guildMember(cyb.ID, 0), guildMember(dee.ID, 2)))
	if err != nil {
		t.Fatal(err)
	}
	disbanded := Guild{ID: id, Name: "Sandal Makers", Version: saved.Version, Disband: true}
	if _, err := s.SaveGuild(ctx, disbanded); err != nil {
		t.Fatalf("the disband: %v", err)
	}
	for _, who := range []string{cyb.ID, dee.ID} {
		if g, in, err := s.GuildByCharacter(ctx, who); err != nil || in || g.Name != "" {
			t.Errorf("%s after the disband: %+v, %v, %v", who, g, in, err)
		}
	}
	if _, err := s.SaveGuild(ctx, disbanded); !errors.Is(err, ErrNotFound) {
		t.Errorf("a second disband: %v, want ErrNotFound", err)
	}
}

func TestGuildCooldownIsSetReadAndCleared(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	cyb := newGuildCharacter(t, s, acc, "cyb")
	const until = 1763432000000
	if err := s.SetGuildCooldown(ctx, cyb.ID, until); err != nil {
		t.Fatalf("setting the cooldown: %v", err)
	}
	if g, in, err := s.GuildByCharacter(ctx, cyb.ID); err != nil || in || g.CooldownUntilMs != until {
		t.Fatalf("a character in no guild with a cooldown: %+v, %v, %v", g, in, err)
	}
	if err := s.SetGuildCooldown(ctx, cyb.ID, 0); err != nil {
		t.Fatalf("clearing the cooldown: %v", err)
	}
	if g, _, err := s.GuildByCharacter(ctx, cyb.ID); err != nil || g.CooldownUntilMs != 0 {
		t.Fatalf("the cleared cooldown: %+v, %v", g, err)
	}
	if err := s.SetGuildCooldown(ctx, cyb.ID, -1); !errors.Is(err, ErrInvalid) {
		t.Errorf("a negative cooldown: %v, want ErrInvalid", err)
	}
	if err := s.SetGuildCooldown(ctx, "ffffffffffffffffffffffffffffffff", until); !errors.Is(err, ErrNotFound) {
		t.Errorf("a cooldown for an unknown character: %v, want ErrNotFound", err)
	}
}

func TestGuildValidationOfTheNameAndTheRanksWithoutADatabase(t *testing.T) {
	cyb := "0123456789abcdef0123456789abcdef"
	base := guildOf("11112222333344445555666677778888", "Sandal Makers", guildMember(cyb, 0))
	if err := validateGuild(base); err != nil {
		t.Fatalf("a valid guild: %v", err)
	}
	for _, tc := range []struct {
		name  string
		edit  func(Guild) Guild
	}{
		{"a two-letter name", func(g Guild) Guild { g.Name = "Ab"; return g }},
		{"a name over 64 bytes", func(g Guild) Guild { g.Name = strings.Repeat("a", 65); return g }},
		{"a name with a leading space", func(g Guild) Guild { g.Name = " Sandal Makers"; return g }},
		{"an empty rank name", func(g Guild) Guild { g.Ranks[1].Name = ""; return g }},
		{"a member twice", func(g Guild) Guild { g.Members = append(g.Members, guildMember(cyb, 2)); return g }},
		{"a rank 3", func(g Guild) Guild { g.Members[0].Rank = 3; return g }},
		{"two leaders", func(g Guild) Guild { g.Members = append(g.Members, guildMember("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)); return g }},
		{"no members", func(g Guild) Guild { g.Members = nil; return g }},
	} {
		if err := validateGuild(tc.edit(base)); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", tc.name, err)
		}
	}
}
