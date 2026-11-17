// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package charactergrpc

// Social, step 9: the guild handlers.  GetCharacter carries the guild of a character in one (its ranks, its members, the character's rank and its cooldown), SaveGuild maps
// the request's guild and its disband to the store and answers the version, SaveGuildCooldown passes the time through, and the store's refusals map to the gRPC statuses.

import (
	"context"
	"strings"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
)

// The fake store's guild side: one guild, keyed by id, and the cooldown set through SaveGuildCooldown.
func (f *fakeStore) SaveGuild(_ context.Context, g character.Guild) (character.Guild, error) {
	if f.guilds == nil {
		f.guilds = map[string]character.Guild{}
	}
	stored, ok := f.guilds[g.ID]
	if g.Version == 0 {
		for _, other := range f.guilds {
			if strings.EqualFold(other.Name, g.Name) {
				return character.Guild{}, character.ErrGuildNameTaken
			}
		}
		if ok {
			return character.Guild{}, character.ErrIDTaken
		}
		g.Version = 1
	} else {
		if !ok {
			return character.Guild{}, character.ErrNotFound
		}
		if stored.Version != g.Version {
			return character.Guild{}, character.ErrGuildVersion
		}
		g.Version++
	}
	f.guilds[g.ID] = g
	return g, nil
}

func (f *fakeStore) GuildByCharacter(_ context.Context, id string) (character.Guild, bool, error) {
	if _, ok := f.byID[id]; !ok {
		return character.Guild{}, false, character.ErrNotFound
	}
	for _, g := range f.guilds {
		for _, m := range g.Members {
			if m.CharacterID == id {
				g.SelfRank = m.Rank
				return g, true, nil
			}
		}
	}
	return character.Guild{SelfRank: -1}, false, nil
}

func (f *fakeStore) SetGuildCooldown(_ context.Context, id string, untilMs int64) error {
	if _, ok := f.byID[id]; !ok {
		return character.ErrNotFound
	}
	if untilMs < 0 {
		return character.ErrInvalid
	}
	return nil
}

var _ GuildStore = (*fakeStore)(nil)

func guildFake() *fakeStore {
	id := "0123456789abcdef0123456789abcdef"
	return &fakeStore{byID: map[string]character.Character{id: {ID: id, AccountID: 1, Name: "cyb", Level: 25}},
		guilds: map[string]character.Guild{
			"11112222333344445555666677778888": {
				ID: "11112222333344445555666677778888", Name: "Sandal Makers", Version: 3, Level: 1, Experience: 0, Capacity: 50, LevelCap: 10,
				Ranks:  []character.GuildRank{{Name: "Guild Leader", Permissions: (1 << 35) - 1}, {Name: "Officer", Permissions: 0x7ffcfc7f}, {Name: "Member", Permissions: 0x65006014}},
				Members: []character.GuildMember{{CharacterID: id, Name: "cyb", Rank: 0, Level: 25, ClassID: 2, JoinedMs: 1763000000000}},
				SelfRank: 0, CooldownUntilMs: 0,
			},
		}}
}

func TestGetCharacterCarriesTheGuild(t *testing.T) {
	f := guildFake()
	s := New(f, acceptAll{})
	r, err := s.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	g := r.GetCharacter().GetGuild()
	if g == nil || g.GetId() != "11112222333344445555666677778888" || g.GetName() != "Sandal Makers" || g.GetVersion() != 3 || g.GetSelfRank() != 0 {
		t.Fatalf("the guild in the reply: %+v", g)
	}
	if len(g.GetRanks()) != 3 || g.GetRanks()[0].GetPermissions() != (1<<35)-1 || g.GetRanks()[2].GetPermissions() != 0x65006014 {
		t.Errorf("the ranks: %+v", g.GetRanks())
	}
	if len(g.GetMembers()) != 1 || g.GetMembers()[0].GetLevel() != 25 || g.GetMembers()[0].GetClassId() != 2 || g.GetMembers()[0].GetJoinedMs() != 1763000000000 {
		t.Errorf("the members: %+v", g.GetMembers())
	}
}

func TestSaveGuildAndCooldownMapTheStore(t *testing.T) {
	f := guildFake()
	s := New(f, acceptAll{})
	ctx := context.Background()
	newGuild := &characterv1.Guild{Id: "22222222222222222222222222222222", Name: "Second Guild",
		Ranks:   []*characterv1.GuildRank{{Name: "Guild Leader", Permissions: (1 << 35) - 1}, {Name: "Officer", Permissions: 0x7ffcfc7f}, {Name: "Member", Permissions: 0x65006014}},
		Level:   1, Capacity: 50, LevelCap: 10,
		Members: []*characterv1.GuildMember{{CharacterId: "0123456789abcdef0123456789abcdef", Name: "cyb", Rank: 0, Level: 25, ClassId: 2, JoinedMs: 1763000000000}}}
	resp, err := s.SaveGuild(ctx, &characterv1.SaveGuildRequest{Guild: newGuild})
	if err != nil || resp.GetVersion() != 1 {
		t.Fatalf("creating a guild: %v, %v", resp, err)
	}
	// an update writes over version 1, and the same version again is stale
	newGuild.Version = 1
	if update, err := s.SaveGuild(ctx, &characterv1.SaveGuildRequest{Guild: newGuild}); err != nil || update.GetVersion() != 2 {
		t.Fatalf("updating the guild: %v, %v", update, err)
	}
	if _, err := s.SaveGuild(ctx, &characterv1.SaveGuildRequest{Guild: newGuild}); status.Code(err) != codes.FailedPrecondition {
		t.Errorf("a stale save: %v, want FailedPrecondition", err)
	}
	// the same name in another case is taken
	dup := &characterv1.Guild{Id: "33333333333333333333333333333333", Name: "second guild", Ranks: newGuild.Ranks, Members: newGuild.Members, Level: 1, Capacity: 50, LevelCap: 10}
	if _, err := s.SaveGuild(ctx, &characterv1.SaveGuildRequest{Guild: dup}); status.Code(err) != codes.AlreadyExists {
		t.Errorf("a taken name: %v, want AlreadyExists", err)
	}
	// a disband of a guild the store does not hold
	if _, err := s.SaveGuild(ctx, &characterv1.SaveGuildRequest{Guild: &characterv1.Guild{Id: "44444444444444444444444444444444", Version: 1}, Disband: true}); status.Code(err) != codes.NotFound {
		t.Errorf("a disband of an unknown guild: %v, want NotFound", err)
	}
	// the cooldown: a character that exists, one that does not, and a negative time
	if _, err := s.SaveGuildCooldown(ctx, &characterv1.SaveGuildCooldownRequest{CharacterId: "0123456789abcdef0123456789abcdef", UntilMs: 1763432000000}); err != nil {
		t.Errorf("a cooldown: %v", err)
	}
	if _, err := s.SaveGuildCooldown(ctx, &characterv1.SaveGuildCooldownRequest{CharacterId: "ffffffffffffffffffffffffffffffff"}); status.Code(err) != codes.NotFound {
		t.Errorf("a cooldown for an unknown character: %v, want NotFound", err)
	}
	if _, err := s.SaveGuildCooldown(ctx, &characterv1.SaveGuildCooldownRequest{CharacterId: "0123456789abcdef0123456789abcdef", UntilMs: -1}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a negative cooldown: %v, want InvalidArgument", err)
	}
}
