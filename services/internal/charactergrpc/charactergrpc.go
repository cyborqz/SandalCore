// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package charactergrpc serves aoc.character.v1.CharacterService.
package charactergrpc

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/customization"
)

// Store is the part of character.Store the service uses.
type Store interface {
	Create(ctx context.Context, n character.New) (character.Character, error)
	List(ctx context.Context, accountID int64) ([]character.Character, error)
	Get(ctx context.Context, id string) (character.Character, error)
	SaveState(ctx context.Context, id string, st character.State) (character.Character, error)
	Delete(ctx context.Context, accountID int64, id string) error
	// Plan B2: a character's items, and the replacement of all of them.
	Inventory(ctx context.Context, id string) ([]character.Item, error)
	SaveInventory(ctx context.Context, id string, items []character.Item) error
	// Plan D1: a character's XP per leveling type, and the save of its level, class and XP.
	Experience(ctx context.Context, id string) ([]character.Experience, error)
	SaveProgress(ctx context.Context, id string, p character.Progress) (character.Character, error)
	// Plan D1, stage 2: the ranks a character bought and its action bar.
	SkillRanks(ctx context.Context, id string) ([]character.TreeRanks, error)
	ActionBar(ctx context.Context, id string) ([]character.ActionbarSlot, error)
}

var _ Store = (*character.Store)(nil)

// Checker judges a new character before it is stored (plan B1:
// customization.Rules, which reads content.db). Check returns nil to accept it,
// an error wrapping ErrUnavailable when there is nothing to judge by, and any
// other error for a character it refuses.
type Checker interface {
	Check(customization.New) error
}

// ErrUnavailable is what a Checker returns, wrapped, when it cannot judge: the
// service has no usable content.db. The character is refused, not accepted
// unchecked.
var ErrUnavailable = errors.New("character creation is unavailable")

// Kits makes the items a new character starts with, its class's kit (plan B2: internal/kit over content.db).
type Kits = character.KitMaker

// StarterSets makes the GM starter set (plan D1: internal/kit's StarterMaker), given to a new character with its kit when
// the server was made with one (AOC_GM_STARTER_SET=1).
type StarterSets = character.StarterSetMaker

// Server implements characterv1.CharacterServiceServer.
type Server struct {
	characterv1.UnimplementedCharacterServiceServer
	store   Store
	check   Checker
	kits    Kits
	starter StarterSets
}

// [C | O | A x C:Y:B]

// Option changes how New makes a Server.
type Option func(*Server)

// WithKits gives the server the kits of the classes: a character it creates starts with its class's kit, stored in the
// same transaction as the character. Without it a new character has no items (the backfill gives it its kit at the
// next start of the service, if it has content.db then).
func WithKits(k Kits) Option { return func(s *Server) { s.kits = k } }

// WithStarterSet gives the server the GM starter set (plan D1): a character it creates gets the set in its bag with its kit
// and is marked as given it, so the backfill at the next start leaves it alone.
func WithStarterSet(m StarterSets) Option { return func(s *Server) { s.starter = m } }

// New serves store. check judges the characters CreateCharacter is asked to
// create; a nil check refuses them all (there is nothing to judge by).
func New(store Store, check Checker, opts ...Option) *Server {
	s := &Server{store: store, check: check}
	for _, o := range opts {
		o(s)
	}
	return s
}

// CreateCharacter checks the race, gender, class and customization (see
// internal/customization), then stores the character with its class's name and
// archetype id.
func (s *Server) CreateCharacter(ctx context.Context, r *characterv1.CreateCharacterRequest) (*characterv1.CreateCharacterResponse, error) {
	if s.check == nil {
		return nil, status.Error(codes.FailedPrecondition, ErrUnavailable.Error()+": the character service has no rules to check it against")
	}
	if err := s.check.Check(customization.New{
		Race: r.GetRaceId(), Gender: r.GetGenderId(), Class: r.GetClassId(),
		ClassName: r.GetClassName(), AppearanceJSON: r.GetAppearanceJson(),
	}); err != nil {
		if errors.Is(err, ErrUnavailable) {
			return nil, status.Error(codes.FailedPrecondition, err.Error())
		}
		slog.Info("character not created: refused by the rules", "account_id", r.GetAccountId(), "err", err)
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	// The game's maximum level is 25: checked before the starter set, which is chosen by the level.
	if err := character.ValidateLevel(r.GetLevel()); err != nil {
		slog.Info("character not created: refused", "account_id", r.GetAccountId(), "err", err)
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	// The class is checked: its name and archetype id are the class's own.
	className, _ := chardef.ClassName(r.GetClassId())
	archetype := chardef.ArchetypeID(r.GetClassId())
	if got := r.GetArchetypeId(); got != 0 && got != archetype {
		return nil, status.Errorf(codes.InvalidArgument, "archetype %d is not the archetype of class %d (%s), %d", got, r.GetClassId(), className, archetype)
	}
	// The character's class kit (plan B2) is made before anything is stored: a class with no kit is a service that cannot
	// finish what it was asked, not a character without gear.
	var items []character.Item
	if s.kits != nil {
		var err error
		if items, err = s.kits.Kit(r.GetClassId()); err != nil {
			slog.Error("character not created: no starting kit", "account_id", r.GetAccountId(), "class_id", r.GetClassId(), "err", err)
			return nil, status.Error(codes.Internal, "the starting kit cannot be made")
		}
	}
	// The GM starter set (plan D1) goes into the bag's free slots after the kit; what does not fit is logged and left out.
	granted := false
	if s.starter != nil {
		level := r.GetLevel()
		if level == 0 {
			level = 1
		}
		set, err := s.starter.StarterSet(r.GetClassId(), level)
		if err != nil {
			slog.Error("character not created: no GM starter set", "account_id", r.GetAccountId(), "class_id", r.GetClassId(), "err", err)
			return nil, status.Error(codes.Internal, "the GM starter set cannot be made")
		}
		placed, noRoom := character.PlaceInBackpack(items, set)
		if noRoom > 0 {
			slog.Warn("character: the GM starter set does not fit the new character's bag whole", "name", r.GetName(), "left_out", noRoom)
		}
		items = append(items, placed...)
		granted = true
	}
	// What is stored is what worldd can build the pawn from at join: the creator's JSON carries every member a
	// preset resolved to, and worldd takes no real preset (customization.Normalize), so presetGuid is stored as 0.
	// create_info stays as the client sent it.
	c, err := s.store.Create(ctx, character.New{
		ID: r.GetId(), AccountID: r.GetAccountId(), Name: r.GetName(),
		RaceID: r.GetRaceId(), GenderID: r.GetGenderId(), ClassID: r.GetClassId(), ArchetypeID: archetype,
		ClassName: className, AppearanceJSON: customization.Normalize(r.GetAppearanceJson()),
		CreateInfo: r.GetCreateInfo(), Level: r.GetLevel(), Items: items, GMStarterSetGranted: granted,
	})
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out := toProto(c)
	out.Items = toProtoItems(items) // the create reply carries the kit it made: the caller need not ask again
	return &characterv1.CreateCharacterResponse{Character: out}, nil
}

func (s *Server) ListCharacters(ctx context.Context, r *characterv1.ListCharactersRequest) (*characterv1.ListCharactersResponse, error) {
	list, err := s.store.List(ctx, r.GetAccountId())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out := &characterv1.ListCharactersResponse{}
	for _, c := range list {
		out.Characters = append(out.Characters, toProto(c))
	}
	return out, nil
}

func (s *Server) GetCharacter(ctx context.Context, r *characterv1.GetCharacterRequest) (*characterv1.GetCharacterResponse, error) {
	c, err := s.store.Get(ctx, r.GetId())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out := toProto(c)
	if r.GetIncludeItems() {
		items, err := s.store.Inventory(ctx, c.ID)
		if err != nil {
			return nil, toStatus(ctx, err)
		}
		out.Items = toProtoItems(items)
		xp, err := s.store.Experience(ctx, c.ID)
		if err != nil {
			return nil, toStatus(ctx, err)
		}
		out.Experience = toProtoExperience(xp)
		if err := s.addTalents(ctx, c.ID, out); err != nil {
			return nil, toStatus(ctx, err)
		}
	}
	return &characterv1.GetCharacterResponse{Character: out}, nil
}

// ~ C-Y-B - C | O | A ~

// SaveProgress writes the level, the class and the XP rows (plan D1; worldd saves them as its player levels).
func (s *Server) SaveProgress(ctx context.Context, r *characterv1.SaveProgressRequest) (*characterv1.SaveProgressResponse, error) {
	p := character.Progress{Level: r.GetLevel(), ClassID: r.GetClassId(), ReplaceSkillRanks: r.GetReplaceSkillRanks(), ReplaceActionBar: r.GetReplaceActionBar()}
	for _, e := range r.GetExperience() {
		p.Experience = append(p.Experience, character.Experience{LevelingTypeGuid: e.GetLevelingTypeGuid(), XP: e.GetXp(), Debt: e.GetDebt()})
	}
	for _, t := range r.GetSkillRanks() {
		p.SkillRanks = append(p.SkillRanks, character.TreeRanks{TreeGuid: t.GetTreeGuid(), RankGuids: t.GetRankGuids()})
	}
	for _, a := range r.GetActionBar() {
		p.ActionBar = append(p.ActionBar, fromProtoSlot(a))
	}
	c, err := s.store.SaveProgress(ctx, r.GetId(), p)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out := toProto(c)
	xp, err := s.store.Experience(ctx, c.ID)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out.Experience = toProtoExperience(xp)
	if err := s.addTalents(ctx, c.ID, out); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveProgressResponse{Character: out}, nil
}

// addTalents puts the character's bought ranks and its action bar (plan D1, stage 2) into a reply.
func (s *Server) addTalents(ctx context.Context, id string, out *characterv1.Character) error {
	ranks, err := s.store.SkillRanks(ctx, id)
	if err != nil {
		return err
	}
	for _, t := range ranks {
		out.SkillRanks = append(out.SkillRanks, &characterv1.TreeRanks{TreeGuid: t.TreeGuid, RankGuids: t.RankGuids})
	}
	bar, err := s.store.ActionBar(ctx, id)
	if err != nil {
		return err
	}
	for _, a := range bar {
		out.ActionBar = append(out.ActionBar, toProtoSlot(a))
	}
	return nil
}

func toProtoSlot(a character.ActionbarSlot) *characterv1.ActionbarSlot {
	return &characterv1.ActionbarSlot{Bank: a.Bank, Slot: a.Slot, UseType: a.UseType, Instigator: a.Instigator, AbilityId: a.AbilityID,
		SkillRankId: a.SkillRankID, PetCommandId: a.PetCommandID, ItemRecordId: a.ItemRecordID, ItemGuid: a.ItemGUID, ItemRarity: a.ItemRarity,
		ItemQuality: a.ItemQuality, ComboButton: a.ComboButton}
}

func fromProtoSlot(a *characterv1.ActionbarSlot) character.ActionbarSlot {
	return character.ActionbarSlot{Bank: a.GetBank(), Slot: a.GetSlot(), UseType: a.GetUseType(), Instigator: a.GetInstigator(), AbilityID: a.GetAbilityId(),
		SkillRankID: a.GetSkillRankId(), PetCommandID: a.GetPetCommandId(), ItemRecordID: a.GetItemRecordId(), ItemGUID: a.GetItemGuid(),
		ItemRarity: a.GetItemRarity(), ItemQuality: a.GetItemQuality(), ComboButton: a.GetComboButton()}
}

func toProtoExperience(xp []character.Experience) []*characterv1.Experience {
	if len(xp) == 0 {
		return nil
	}
	out := make([]*characterv1.Experience, 0, len(xp))
	for _, e := range xp {
		out = append(out, &characterv1.Experience{LevelingTypeGuid: e.LevelingTypeGuid, Xp: e.XP, Debt: e.Debt})
	}
	return out
}

// SaveInventory replaces all the items of a character (worldd saves them as its player plays; the latest save wins).
func (s *Server) SaveInventory(ctx context.Context, r *characterv1.SaveInventoryRequest) (*characterv1.SaveInventoryResponse, error) {
	items := make([]character.Item, 0, len(r.GetItems()))
	for i, it := range r.GetItems() {
		item, err := fromProtoItem(it)
		if err != nil {
			return nil, status.Errorf(codes.InvalidArgument, "item %d: %s", i, err)
		}
		items = append(items, item)
	}
	if err := s.store.SaveInventory(ctx, r.GetId(), items); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveInventoryResponse{Saved: int32(len(items))}, nil
}

func (s *Server) SaveCharacterState(ctx context.Context, r *characterv1.SaveCharacterStateRequest) (*characterv1.SaveCharacterStateResponse, error) {
	st := character.State{LastRegion: r.GetLastRegion(), Level: r.GetLevel()}
	if p := r.GetPosition(); p != nil {
		st.Position = &character.Position{X: p.GetX(), Y: p.GetY(), Z: p.GetZ(), Yaw: p.GetYaw()}
	}
	c, err := s.store.SaveState(ctx, r.GetId(), st)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveCharacterStateResponse{Character: toProto(c)}, nil
}

func (s *Server) DeleteCharacter(ctx context.Context, r *characterv1.DeleteCharacterRequest) (*characterv1.DeleteCharacterResponse, error) {
	if err := s.store.Delete(ctx, r.GetAccountId(), r.GetId()); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.DeleteCharacterResponse{}, nil
}

// toStatus maps store errors to gRPC codes, in this order:
//   - A character error describes the caller's input. It passes through with
//     its text, even when the caller has since gone.
//   - Otherwise, when the RPC's own context has ended, its code wins. That is
//     judged by ctx, not by what err wraps: a store timeout (pgx's connect
//     timeout) wraps DeadlineExceeded too, and its text names the database.
//   - Anything else is logged here, with the RPC's method, and reaches the
//     caller only as a generic Internal.
func toStatus(ctx context.Context, err error) error {
	switch {
	case errors.Is(err, character.ErrNameTaken), errors.Is(err, character.ErrIDTaken):
		return status.Error(codes.AlreadyExists, err.Error())
	case errors.Is(err, character.ErrNotFound):
		return status.Error(codes.NotFound, err.Error())
	case errors.Is(err, character.ErrInvalid):
		return status.Error(codes.InvalidArgument, err.Error())
	}
	if ctxErr := ctx.Err(); ctxErr != nil {
		return status.FromContextError(ctxErr).Err()
	}
	m, _ := grpc.Method(ctx) // empty when the handler is called without a server
	slog.Error("character store failed", "method", m, "err", err)
	return status.Error(codes.Internal, "character store failed")
}

func toProto(c character.Character) *characterv1.Character {
	out := &characterv1.Character{
		Id: c.ID, AccountId: c.AccountID, Name: c.Name,
		RaceId: c.RaceID, GenderId: c.GenderID, ClassId: c.ClassID, ArchetypeId: c.ArchetypeID, ClassName: c.ClassName,
		AppearanceJson: c.AppearanceJSON, CreateInfo: c.CreateInfo, Level: c.Level, LastRegion: c.LastRegion,
		CreatedAtMs: unixMilli(c.CreatedAt),
	}
	if p := c.Position; p != nil {
		out.Position = &characterv1.Position{X: p.X, Y: p.Y, Z: p.Z, Yaw: p.Yaw}
	}
	return out
}

var containerNames = map[characterv1.ItemContainer]string{
	characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK:  character.Backpack,
	characterv1.ItemContainer_ITEM_CONTAINER_EQUIPMENT: character.Equipment,
	characterv1.ItemContainer_ITEM_CONTAINER_QUEST:     character.Quest,
}

func toProtoItems(items []character.Item) []*characterv1.Item {
	if len(items) == 0 {
		return nil
	}
	out := make([]*characterv1.Item, 0, len(items))
	for _, it := range items {
		p := &characterv1.Item{
			Slot: it.Slot, ItemRecordId: it.ItemRecordID, InstanceGuid: it.InstanceGUID, Quantity: it.Quantity,
			Quality: it.Quality, Durability: it.Durability, CreatedAtMs: unixMilli(it.CreatedAt),
			PersistedHealth: it.PersistedHealth, PersistedMana: it.PersistedMana, LockExpiration: it.LockExpiration,
		}
		for k, name := range containerNames {
			if name == it.Container {
				p.Container = k
			}
		}
		out = append(out, p)
	}
	return out
}

// (C-Y-B + C | O | A)

// fromProtoItem is the item as the store takes it; an error for a container that is none of the three.
func fromProtoItem(p *characterv1.Item) (character.Item, error) {
	name, ok := containerNames[p.GetContainer()]
	if !ok {
		return character.Item{}, fmt.Errorf("container %s is not the Backpack, the Equipment or the Quest storage", p.GetContainer())
	}
	return character.Item{
		Container: name, Slot: p.GetSlot(), ItemRecordID: p.GetItemRecordId(), InstanceGUID: p.GetInstanceGuid(),
		Quantity: p.GetQuantity(), Quality: p.GetQuality(), Durability: p.GetDurability(),
		CreatedAt:       time.UnixMilli(p.GetCreatedAtMs()),
		PersistedHealth: p.PersistedHealth, PersistedMana: p.PersistedMana, LockExpiration: p.GetLockExpiration(),
	}, nil
}

// unixMilli is t in Unix milliseconds, and 0 for the zero time.
func unixMilli(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.UnixMilli()
}
