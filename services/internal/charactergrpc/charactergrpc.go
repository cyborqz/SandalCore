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
	// Gathering stage 2: the character's 22 professions (a profession with no row is Novice, level 1, no XP).
	Professions(ctx context.Context, id string) ([]character.Profession, error)
}

var _ Store = (*character.Store)(nil)

// WalletStore is the part of character.Store that keeps a character's money (the vendors milestone, step 0). A store without it keeps no wallet: GetCharacter sends none
// and a save that replaces the wallet is refused.
type WalletStore interface {
	Wallet(ctx context.Context, id string) ([]character.WalletEntry, error)
	SaveInventoryAndWallet(ctx context.Context, id string, items []character.Item, wallet []character.WalletEntry) error
}

var _ WalletStore = (*character.Store)(nil)

// JobStore is the part of character.Store that keeps a character's processing jobs and the recipes it was taught (the crafting milestone, steps 1 and 3). A store without it keeps none:
// GetCharacter sends none and a save that replaces the jobs or the recipes is refused.
type JobStore interface {
	ProcessingJobs(ctx context.Context, id string) ([]character.ProcessingJob, error)
	KnownRecipes(ctx context.Context, id string) ([]int64, error)
	SaveInventoryAll(ctx context.Context, id string, items []character.Item, wallet []character.WalletEntry, replaceWallet bool, jobs []character.ProcessingJob, replaceJobs bool, known []int64, replaceKnown bool) error
}

var _ JobStore = (*character.Store)(nil)

// FTUEStore is the part of character.Store that keeps the tutorials a character finished (missing systems, step 0). A store without it keeps none: GetCharacter sends none.
type FTUEStore interface {
	CompletedFTUEs(ctx context.Context, id string) ([]int64, error)
}

var _ FTUEStore = (*character.Store)(nil)

// PvP: contributed by MrJambix in #23.
// PvPStore is the part of character.Store that keeps a character's corruption and blight (migration 0025). A store without it keeps none: GetCharacter sends none and a save that
// replaces them is refused.
type PvPStore interface {
	PvPState(ctx context.Context, id string) (character.PvP, error)
}

var _ PvPStore = (*character.Store)(nil)

// WarehouseStore is the part of character.Store that keeps a character's warehouse (the storage milestone, step 0). A store without it keeps none: GetCharacter sends none and a save that
// replaces the warehouse is refused.
type WarehouseStore interface {
	WarehouseItems(ctx context.Context, id string) ([]character.WarehouseItem, error)
	SaveInventoryEverything(ctx context.Context, id string, items []character.Item, wallet []character.WalletEntry, replaceWallet bool, jobs []character.ProcessingJob, replaceJobs bool,
		known []int64, replaceKnown bool, warehouse []character.WarehouseItem, replaceWarehouse bool) error
}

var _ WarehouseStore = (*character.Store)(nil)

// AttunementStore is the part of character.Store that keeps the places a character discovered and its home (Embersprings). A store without it keeps none: GetCharacter sends none.
type AttunementStore interface {
	Attunements(ctx context.Context, id string) (character.Attunements, error)
}

var _ AttunementStore = (*character.Store)(nil)

// IgnoredStore is the part of character.Store that keeps the players and the guilds a character ignores (social, step 6). A store without it keeps none: GetCharacter sends none.
type IgnoredStore interface {
	IgnoredNames(ctx context.Context, id string) (character.IgnoredNames, error)
}

var _ IgnoredStore = (*character.Store)(nil)

// FriendsStore is the part of character.Store that keeps a character's friends and pending friend requests (social, step 7). A store without it keeps none: GetCharacter sends none.
type FriendsStore interface {
	Friends(ctx context.Context, id string) (character.Friends, error)
}

var _ FriendsStore = (*character.Store)(nil)

// GuildStore is the part of character.Store that keeps the guilds (social, step 9). A store without it keeps none: GetCharacter sends no guild and SaveGuild and
// SaveGuildCooldown are refused.
type GuildStore interface {
	SaveGuild(ctx context.Context, g character.Guild) (character.Guild, error)
	GuildByCharacter(ctx context.Context, characterID string) (character.Guild, bool, error)
	SetGuildCooldown(ctx context.Context, characterID string, untilMs int64) error
}

var _ GuildStore = (*character.Store)(nil)

// MailStore is the part of character.Store that keeps a character's mails (migration 0027). A store without it keeps none: the four mail calls are refused.
type MailStore interface {
	Mails(ctx context.Context, id string) ([]character.Mail, error)
	SaveMail(ctx context.Context, id string, mail character.Mail) error
	CreateMail(ctx context.Context, id string, mail character.NewMail) (character.Mail, error)
	UnreadMails(ctx context.Context, id string) (int32, error)
	ClaimMailItems(ctx context.Context, id string, claims []character.MailClaim, bags character.ClaimBags) (character.ClaimResult, error)
}

var _ MailStore = (*character.Store)(nil)

// MarketStore is the part of character.Store that keeps the Marketplace listings (migration 0030). A store without it keeps none: the two calls are refused.
type MarketStore interface {
	SaveInventoryAndListing(ctx context.Context, id string, items []character.Item, wallet []character.WalletEntry, jobs []character.ProcessingJob,
		known []int64, warehouse []character.WarehouseItem, listing character.MarketListing) error
	MarketListings(ctx context.Context) ([]character.MarketListing, error)
}

var _ MarketStore = (*character.Store)(nil)

// MailMoneyStore is the part of character.Store that takes a mail's money into the purse with the claim (migration 0031). A store without it refuses a claim of money.
type MailMoneyStore interface {
	ClaimMail(ctx context.Context, id string, claims []character.MailClaim, moneyMails []character.MoneyClaim, bags character.ClaimBags) (character.ClaimResult, error)
}

var _ MailMoneyStore = (*character.Store)(nil)

// MarketSalesStore is the part of character.Store that stores the Marketplace's purchases, removals and expiries and keeps the nodes' sales tax (migration 0031). A store
// without it refuses the three calls and sends no node's rate.
type MarketSalesStore interface {
	BuyListing(ctx context.Context, r character.BuyRequest) (character.BuyResult, error)
	RemoveListing(ctx context.Context, r character.RemoveRequest) (character.RemoveResult, error)
	ExpireListing(ctx context.Context, listingID, itemName string) (character.ExpireResult, error)
	NodeMarketTaxes(ctx context.Context) ([]character.NodeMarketTax, error)
}

var _ MarketSalesStore = (*character.Store)(nil)

// TradeStore is the part of character.Store that commits atomic two-character player trades (migration 0032).
type TradeStore interface {
	CommitPlayerTrade(ctx context.Context, r character.CommitTradeRequest) (character.CommitTradeResult, error)
}

var _ TradeStore = (*character.Store)(nil)

// StorefrontStore is the part of character.Store that keeps the storefront descriptions and the nodes' page counts (migration 0034). A store without it refuses
// SetStorefrontDescription and sends no description and no count.
type StorefrontStore interface {
	SetStorefrontDescription(ctx context.Context, characterID string, node int64, description string) (character.StorefrontResult, error)
	MarketStorefronts(ctx context.Context) ([]character.StorefrontText, []character.NodeHeaderRaises, error)
}

var _ StorefrontStore = (*character.Store)(nil)

// NameStore is the part of character.Store that finds a character by its name without regard to the case of the letters (social, step 7: the name check and the request to an offline
// character). A store without it cannot answer GetCharacterByName.
type NameStore interface {
	GetByName(ctx context.Context, name string) (character.Character, error)
}

var _ NameStore = (*character.Store)(nil)

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
		MaxCharacters: character.DefaultMaxCharacters,
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
		profs, err := s.store.Professions(ctx, c.ID)
		if err != nil {
			return nil, toStatus(ctx, err)
		}
		out.Professions = toProtoProfessions(profs)
		if ws, ok := s.store.(WalletStore); ok {
			wallet, err := ws.Wallet(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.Wallet = toProtoWallet(wallet)
		}
		if js, ok := s.store.(JobStore); ok {
			jobs, err := js.ProcessingJobs(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.ProcessingJobs = toProtoJobs(jobs)
			known, err := js.KnownRecipes(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.KnownRecipes = known
		}
		if fs, ok := s.store.(FTUEStore); ok {
			done, err := fs.CompletedFTUEs(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.CompletedFtues = done
		}
		// PvP: contributed by MrJambix in #23.
		if ps, ok := s.store.(PvPStore); ok {
			pvp, err := ps.PvPState(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			if pvp != (character.PvP{}) {
				out.Pvp = &characterv1.PvpState{Corruption: pvp.Corruption, Blight: pvp.Blight}
			}
		}
		if ws, ok := s.store.(WarehouseStore); ok {
			stored, err := ws.WarehouseItems(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.WarehouseItems = toProtoWarehouse(stored)
		}
		if as, ok := s.store.(AttunementStore); ok {
			a, err := as.Attunements(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.Attunements = a.Discovered
			out.HomeAttunement = a.Home
			out.HomeLastUsedMs = a.HomeLastUsedMs
		}
		if is, ok := s.store.(IgnoredStore); ok {
			ignored, err := is.IgnoredNames(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.IgnoredCharacterNames = ignored.Characters
			out.IgnoredGuildNames = ignored.Guilds
		}
		if fs, ok := s.store.(FriendsStore); ok {
			friends, err := fs.Friends(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			out.Friends = toProtoFriends(friends.Friends)
			out.FriendRequests = toProtoFriendRequests(friends.Requests)
		}
		// Social, step 9: the guild the character is in, with its roster and the character's rank and join cooldown.
		if gs, ok := s.store.(GuildStore); ok {
			guild, inGuild, err := gs.GuildByCharacter(ctx, c.ID)
			if err != nil {
				return nil, toStatus(ctx, err)
			}
			if inGuild {
				out.Guild = toProtoGuild(guild)
			} else if guild.CooldownUntilMs != 0 {
				out.Guild = &characterv1.Guild{SelfRank: -1, CooldownUntilMs: guild.CooldownUntilMs}
			}
		}
		// Mail (migration 0027): the live, unread mails, for the social component's unread count at the login.  One count query: a
		// store that cannot answer it logs and leaves the count at 0; the login is never failed for it.
		if ms, ok := s.store.(MailStore); ok {
			unread, err := ms.UnreadMails(ctx, c.ID)
			if err != nil {
				slog.Warn("character: the unread mail count could not be read; the login goes on with 0", "character_id", c.ID, "err", err)
			} else {
				out.UnreadMailCount = unread
			}
		}
	}
	return &characterv1.GetCharacterResponse{Character: out}, nil
}

// GetCharacterByName answers the name check and the request to an offline character: the character with this name, without regard to the case of the letters (social, step 7).
func (s *Server) GetCharacterByName(ctx context.Context, r *characterv1.GetCharacterByNameRequest) (*characterv1.GetCharacterByNameResponse, error) {
	ns, ok := s.store.(NameStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store has no by-name lookup")
	}
	c, err := ns.GetByName(ctx, r.GetName())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.GetCharacterByNameResponse{Character: toProto(c)}, nil
}

// ~ C-Y-B - C | O | A ~

// SaveProgress writes the level, the class and the XP rows (plan D1; worldd saves them as its player levels).
func (s *Server) SaveProgress(ctx context.Context, r *characterv1.SaveProgressRequest) (*characterv1.SaveProgressResponse, error) {
	p := character.Progress{Level: r.GetLevel(), ClassID: r.GetClassId(), ReplaceSkillRanks: r.GetReplaceSkillRanks(), ReplaceActionBar: r.GetReplaceActionBar(),
		ReplaceCompletedFTUEs: r.GetReplaceCompletedFtues(), CompletedFTUEs: r.GetCompletedFtues(),
		ReplaceAttunements: r.GetReplaceAttunements(), Attunements: character.Attunements{Discovered: r.GetAttunements(), Home: r.GetHomeAttunement(), HomeLastUsedMs: r.GetHomeLastUsedMs()},
		ReplaceIgnoredNames: r.GetReplaceIgnoredNames(), IgnoredNames: character.IgnoredNames{Characters: r.GetIgnoredCharacterNames(), Guilds: r.GetIgnoredGuildNames()},
		ReplaceFriends: r.GetReplaceFriends(), Friends: fromProtoFriends(r.GetFriends()),
		ReplaceFriendRequests: r.GetReplaceFriendRequests(), FriendRequests: fromProtoFriendRequests(r.GetFriendRequests())}
	// PvP: contributed by MrJambix in #23.
	if r.GetReplacePvp() {
		if _, ok := s.store.(PvPStore); !ok {
			return nil, status.Error(codes.FailedPrecondition, "this character store keeps no PvP state")
		}
		p.ReplacePvP = true
		p.PvP = character.PvP{Corruption: r.GetPvp().GetCorruption(), Blight: r.GetPvp().GetBlight()}
	} else if r.GetPvp() != nil {
		return nil, status.Error(codes.InvalidArgument, "a PvP state is given without replace_pvp")
	}
	for _, e := range r.GetExperience() {
		p.Experience = append(p.Experience, character.Experience{LevelingTypeGuid: e.GetLevelingTypeGuid(), XP: e.GetXp(), Debt: e.GetDebt()})
	}
	for _, t := range r.GetSkillRanks() {
		p.SkillRanks = append(p.SkillRanks, character.TreeRanks{TreeGuid: t.GetTreeGuid(), RankGuids: t.GetRankGuids()})
	}
	for _, a := range r.GetActionBar() {
		p.ActionBar = append(p.ActionBar, fromProtoSlot(a))
	}
	for _, pr := range r.GetProfessions() {
		p.Professions = append(p.Professions, character.Profession{ProfessionGuid: pr.GetProfessionGuid(), CertificationTier: pr.GetCertificationTier(), Level: pr.GetLevel(), XP: pr.GetXp()})
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
	profs, err := s.store.Professions(ctx, c.ID)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out.Professions = toProtoProfessions(profs)
	return &characterv1.SaveProgressResponse{Character: out}, nil
}

// Social, step 9: guilds, mapped to and from the service's messages.
func toProtoGuild(g character.Guild) *characterv1.Guild {
	out := &characterv1.Guild{
		Id: g.ID, Name: g.Name, Version: g.Version, Level: g.Level, Experience: g.Experience, Capacity: g.Capacity, LevelCap: g.LevelCap,
		SelfRank: g.SelfRank, CooldownUntilMs: g.CooldownUntilMs,
	}
	for _, r := range g.Ranks {
		out.Ranks = append(out.Ranks, &characterv1.GuildRank{Name: r.Name, Permissions: r.Permissions})
	}
	for _, m := range g.Members {
		out.Members = append(out.Members, &characterv1.GuildMember{CharacterId: m.CharacterID, Name: m.Name, Rank: m.Rank, Level: m.Level, ClassId: m.ClassID,
			JoinedMs: m.JoinedMs, LastOnlineMs: m.LastOnlineMs})
	}
	return out
}

func fromProtoGuild(g *characterv1.Guild) character.Guild {
	out := character.Guild{ID: g.GetId(), Name: g.GetName(), Version: g.GetVersion(), Level: g.GetLevel(), Experience: g.GetExperience(),
		Capacity: g.GetCapacity(), LevelCap: g.GetLevelCap()}
	for _, r := range g.GetRanks() {
		out.Ranks = append(out.Ranks, character.GuildRank{Name: r.GetName(), Permissions: r.GetPermissions()})
	}
	for _, m := range g.GetMembers() {
		out.Members = append(out.Members, character.GuildMember{CharacterID: m.GetCharacterId(), Name: m.GetName(), Rank: m.GetRank(), Level: m.GetLevel(),
			ClassID: m.GetClassId(), JoinedMs: m.GetJoinedMs(), LastOnlineMs: m.GetLastOnlineMs()})
	}
	return out
}

// SaveGuild writes a guild whole (social, step 9, migration 0026): version 0 creates it, any other version must be the guild's current one. The save makes the version one
// more and the reply carries it. AlreadyExists for a name another guild has; FailedPrecondition for a stale save; NotFound for a guild the version names and the store
// does not hold; InvalidArgument for a row the store's rules refuse.
func (s *Server) SaveGuild(ctx context.Context, r *characterv1.SaveGuildRequest) (*characterv1.SaveGuildResponse, error) {
	gs, ok := s.store.(GuildStore)
	if !ok {
		return nil, status.Error(codes.FailedPrecondition, "this character store keeps no guilds")
	}
	if r.GetGuild() == nil {
		return nil, status.Error(codes.InvalidArgument, "no guild is given")
	}
	g := fromProtoGuild(r.GetGuild())
	g.Disband = r.GetDisband()
	saved, err := gs.SaveGuild(ctx, g)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveGuildResponse{Version: saved.Version}, nil
}

// SaveGuildCooldown writes the time before which a character cannot create or join a guild again (social, step 9, migration 0026). InvalidArgument for a negative time.
func (s *Server) SaveGuildCooldown(ctx context.Context, r *characterv1.SaveGuildCooldownRequest) (*characterv1.SaveGuildCooldownResponse, error) {
	gs, ok := s.store.(GuildStore)
	if !ok {
		return nil, status.Error(codes.FailedPrecondition, "this character store keeps no guild cooldowns")
	}
	if err := gs.SetGuildCooldown(ctx, r.GetCharacterId(), r.GetUntilMs()); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveGuildCooldownResponse{}, nil
}

// GetMails returns a character's live mails, newest first, with their attachments and status (migration 0027). A store that keeps no mails answers
// Unimplemented; NotFound for an unknown character.
func (s *Server) GetMails(ctx context.Context, r *characterv1.GetMailsRequest) (*characterv1.GetMailsResponse, error) {
	ms, ok := s.store.(MailStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no mails")
	}
	mails, err := ms.Mails(ctx, r.GetCharacterId())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.GetMailsResponse{Mails: toProtoMails(mails)}, nil
}

// SaveMail writes one mail whole: its content, its status and its attachments' claimed flags. FailedPrecondition for a stale save that would clear a stored
// mark or claim; NotFound for an unknown character or mail; InvalidArgument, with nothing changed, for a mail the rules refuse.
func (s *Server) SaveMail(ctx context.Context, r *characterv1.SaveMailRequest) (*characterv1.SaveMailResponse, error) {
	ms, ok := s.store.(MailStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no mails")
	}
	if r.GetMail() == nil {
		return nil, status.Error(codes.InvalidArgument, "no mail is given")
	}
	if err := ms.SaveMail(ctx, r.GetCharacterId(), fromProtoMail(r.GetMail())); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveMailResponse{}, nil
}

// CreateMail adds one mail for the admin send and returns the stored mail with its id, issue time and absolute expiry (migration 0027). NotFound for an unknown
// character; InvalidArgument, with nothing changed, for a mail the rules refuse or a full mailbox.
func (s *Server) CreateMail(ctx context.Context, r *characterv1.CreateMailRequest) (*characterv1.CreateMailResponse, error) {
	ms, ok := s.store.(MailStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no mails")
	}
	mail, err := ms.CreateMail(ctx, r.GetCharacterId(), character.NewMail{Sender: r.GetSender(), Subject: r.GetSubject(), Body: r.GetBody(),
		ExpiresInMs: r.GetExpiresInMs(), Items: fromProtoMailItems(r.GetItems())})
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.CreateMailResponse{Mail: toProtoMail(mail)}, nil
}

// ClaimMailItems claims attachments into the bags in one transaction (see character.Store.ClaimMailItems): the bags and the claimed marks are written together or not at
// all. NotFound for an unknown character, mail or attachment (another character's mail looks the same); AlreadyExists for an attachment claimed before by a request with other
// bags, and for a mail whose money is another amount than the one the request credited (money_expected_copper: a sale was added to the mail meanwhile); InvalidArgument, with
// nothing changed, for names or bags the rules refuse, and for money mail ids that do not each have an expected amount. A request that repeats an earlier one whole answers Ok
// with replayed set.
func (s *Server) ClaimMailItems(ctx context.Context, r *characterv1.ClaimMailItemsRequest) (*characterv1.ClaimMailItemsResponse, error) {
	ms, ok := s.store.(MailStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no mails")
	}
	inv := r.GetInventory()
	if inv == nil {
		return nil, status.Error(codes.InvalidArgument, "no bags are given")
	}
	claims := make([]character.MailClaim, 0, len(r.GetClaims()))
	for _, c := range r.GetClaims() {
		claims = append(claims, character.MailClaim{MailID: c.GetMailId(), InstanceGUID: c.GetInstanceGuid()})
	}
	bags, err := claimBagsOf(inv)
	if err != nil {
		return nil, err
	}
	if len(r.GetMoneyExpectedCopper()) != len(r.GetMoneyMailIds()) {
		return nil, status.Errorf(codes.InvalidArgument, "%d money mail ids but %d expected amounts: each money names the copper it credits", len(r.GetMoneyMailIds()), len(r.GetMoneyExpectedCopper()))
	}
	var res character.ClaimResult
	if len(r.GetMoneyMailIds()) > 0 {
		mm, ok := s.store.(MailMoneyStore)
		if !ok {
			return nil, status.Error(codes.Unimplemented, "this character store keeps no mail money")
		}
		money := make([]character.MoneyClaim, 0, len(r.GetMoneyMailIds()))
		for i, id := range r.GetMoneyMailIds() {
			money = append(money, character.MoneyClaim{MailID: id, Copper: r.GetMoneyExpectedCopper()[i]})
		}
		res, err = mm.ClaimMail(ctx, r.GetCharacterId(), claims, money, bags)
	} else {
		res, err = ms.ClaimMailItems(ctx, r.GetCharacterId(), claims, bags)
	}
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.ClaimMailItemsResponse{Claimed: int32(res.Claimed), Replayed: res.Replayed}, nil
}

// claimBagsOf reads the bags a claim or a Marketplace step carries (the items, and the purse, jobs, recipes and warehouse when their replace flags say so).
func claimBagsOf(inv *characterv1.SaveInventoryRequest) (character.ClaimBags, error) {
	bags := character.ClaimBags{Items: make([]character.Item, 0, len(inv.GetItems()))}
	for i, it := range inv.GetItems() {
		item, err := fromProtoItem(it)
		if err != nil {
			return character.ClaimBags{}, status.Errorf(codes.InvalidArgument, "item %d: %s", i, err)
		}
		bags.Items = append(bags.Items, item)
	}
	if inv.GetReplaceWallet() {
		bags.ReplaceWallet = true
		bags.Wallet = make([]character.WalletEntry, 0, len(inv.GetWallet()))
		for _, e := range inv.GetWallet() {
			bags.Wallet = append(bags.Wallet, character.WalletEntry{CurrencyGuid: e.GetCurrencyGuid(), Amount: e.GetAmount()})
		}
	}
	if inv.GetReplaceProcessingJobs() {
		bags.ReplaceJobs = true
		bags.Jobs = fromProtoJobs(inv.GetProcessingJobs())
	}
	if inv.GetReplaceKnownRecipes() {
		bags.ReplaceKnown = true
		bags.KnownRecipes = inv.GetKnownRecipes()
	}
	if inv.GetReplaceWarehouse() {
		bags.ReplaceWarehouse = true
		bags.Warehouse = make([]character.WarehouseItem, 0, len(inv.GetWarehouseItems()))
		for _, w := range inv.GetWarehouseItems() {
			bags.Warehouse = append(bags.Warehouse, fromProtoWarehouse(w))
		}
	}
	return bags, nil
}

// SaveInventoryAndListing writes the seller's bags, wallet, jobs, recipes and warehouse AND one Market listing in one transaction (migration 0030). NotFound for
// an unknown character; InvalidArgument, with nothing changed, for a bag or a listing the rules refuse; AlreadyExists, with nothing changed, for a listing id the seller
// already has (an earlier call stored it: worldd asks again after an answer it could not read, and takes AlreadyExists as saved).
func (s *Server) SaveInventoryAndListing(ctx context.Context, r *characterv1.SaveInventoryAndListingRequest) (*characterv1.SaveInventoryAndListingResponse, error) {
	ms, ok := s.store.(MarketStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no market listings")
	}
	if r.GetListing() == nil {
		return nil, status.Error(codes.InvalidArgument, "no listing is given")
	}
	items := make([]character.Item, 0, len(r.GetItems()))
	for i, it := range r.GetItems() {
		item, err := fromProtoItem(it)
		if err != nil {
			return nil, status.Errorf(codes.InvalidArgument, "item %d: %s", i, err)
		}
		items = append(items, item)
	}
	wallet := make([]character.WalletEntry, 0, len(r.GetWallet()))
	for _, e := range r.GetWallet() {
		wallet = append(wallet, character.WalletEntry{CurrencyGuid: e.GetCurrencyGuid(), Amount: e.GetAmount()})
	}
	stored := make([]character.WarehouseItem, 0, len(r.GetWarehouseItems()))
	for _, w := range r.GetWarehouseItems() {
		stored = append(stored, fromProtoWarehouse(w))
	}
	if err := ms.SaveInventoryAndListing(ctx, r.GetCharacterId(), items, wallet, fromProtoJobs(r.GetProcessingJobs()), r.GetKnownRecipes(), stored,
		fromProtoMarketListing(r.GetListing())); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SaveInventoryAndListingResponse{}, nil
}

// ListMarketListings returns every listed listing of the world with its seller's name, oldest first (migration 0030), and the rate of every node that has one (0031).
func (s *Server) ListMarketListings(ctx context.Context, _ *characterv1.ListMarketListingsRequest) (*characterv1.ListMarketListingsResponse, error) {
	ms, ok := s.store.(MarketStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no market listings")
	}
	listings, err := ms.MarketListings(ctx)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	resp := &characterv1.ListMarketListingsResponse{Listings: toProtoMarketListings(listings)}
	if sales, ok := s.store.(MarketSalesStore); ok {
		taxes, err := sales.NodeMarketTaxes(ctx)
		if err != nil {
			return nil, toStatus(ctx, err)
		}
		for _, t := range taxes {
			resp.NodeTaxes = append(resp.NodeTaxes, &characterv1.NodeMarketTax{Node: t.Node, Rate: t.Rate, CollectedCopper: t.CollectedCopper})
		}
	}
	if fronts, ok := s.store.(StorefrontStore); ok {
		texts, raises, err := fronts.MarketStorefronts(ctx)
		if err != nil {
			return nil, toStatus(ctx, err)
		}
		for _, t := range texts {
			resp.Storefronts = append(resp.Storefronts, &characterv1.StorefrontText{Node: t.Node, SellerId: t.SellerID, Description: t.Description})
		}
		for _, r := range raises {
			resp.NodeHeaders = append(resp.NodeHeaders, &characterv1.NodeHeaderRaises{Node: r.Node, Raises: r.Raises})
		}
	}
	return resp, nil
}

// storefrontOutcomeOf is the wire's outcome of SetStorefrontDescription's.
func storefrontOutcomeOf(o character.StorefrontOutcome) characterv1.StorefrontOutcome {
	switch o {
	case character.StorefrontDone:
		return characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_DONE
	case character.StorefrontUnchanged:
		return characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_UNCHANGED
	case character.StorefrontNoListing:
		return characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_NO_LISTING
	}
	return characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_UNSPECIFIED
}

// SetStorefrontDescription stores a seller's description of his storefront at one Marketplace and raises that Marketplace's page version, in one transaction (migration 0034; see
// character.Store.SetStorefrontDescription). NotFound for an unknown character; InvalidArgument, with nothing changed, for a node of 0 or a text the rules refuse; Ok with the
// outcome NO_LISTING (nothing written) for a seller who has no listing there.
func (s *Server) SetStorefrontDescription(ctx context.Context, r *characterv1.SetStorefrontDescriptionRequest) (*characterv1.SetStorefrontDescriptionResponse, error) {
	fs, ok := s.store.(StorefrontStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no storefront descriptions")
	}
	res, err := fs.SetStorefrontDescription(ctx, r.GetCharacterId(), r.GetNode(), r.GetDescription())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.SetStorefrontDescriptionResponse{Outcome: storefrontOutcomeOf(res.Outcome), Description: res.Description, HeaderRaises: res.HeaderRaises}, nil
}

// marketOutcomeOf is the wire's outcome of a step's.
func marketOutcomeOf(o character.MarketOutcome) characterv1.MarketOutcome {
	switch o {
	case character.OutcomeDone:
		return characterv1.MarketOutcome_MARKET_OUTCOME_DONE
	case character.OutcomeReplayed:
		return characterv1.MarketOutcome_MARKET_OUTCOME_REPLAYED
	case character.OutcomeListingGone:
		return characterv1.MarketOutcome_MARKET_OUTCOME_LISTING_GONE
	case character.OutcomeListingExpired:
		return characterv1.MarketOutcome_MARKET_OUTCOME_LISTING_EXPIRED
	case character.OutcomeQuantity:
		return characterv1.MarketOutcome_MARKET_OUTCOME_QUANTITY
	case character.OutcomePriceChanged:
		return characterv1.MarketOutcome_MARKET_OUTCOME_PRICE_CHANGED
	case character.OutcomeRateChanged:
		return characterv1.MarketOutcome_MARKET_OUTCOME_RATE_CHANGED
	case character.OutcomeOwnListing:
		return characterv1.MarketOutcome_MARKET_OUTCOME_OWN_LISTING
	case character.OutcomeNotExpired:
		return characterv1.MarketOutcome_MARKET_OUTCOME_NOT_EXPIRED
	}
	return characterv1.MarketOutcome_MARKET_OUTCOME_UNSPECIFIED
}

// BuyListing stores one purchase in one transaction (migration 0031; see character.Store.BuyListing). NotFound for an unknown character; InvalidArgument, with nothing
// changed, for a request or bags the rules refuse; Ok with the outcome of a listing that cannot be bought (nothing was written) or of a purchase stored before (REPLAYED).
func (s *Server) BuyListing(ctx context.Context, r *characterv1.BuyListingRequest) (*characterv1.BuyListingResponse, error) {
	ms, ok := s.store.(MarketSalesStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no market sales")
	}
	if r.GetInventory() == nil {
		return nil, status.Error(codes.InvalidArgument, "no bags are given")
	}
	bags, err := claimBagsOf(r.GetInventory())
	if err != nil {
		return nil, err
	}
	res, err := ms.BuyListing(ctx, character.BuyRequest{PurchaseID: r.GetPurchaseId(), BuyerID: r.GetCharacterId(), ListingID: r.GetListingId(), Quantity: r.GetQuantity(),
		PricePerUnit: r.GetPricePerUnit(), TaxRate: r.GetTaxRate(), Tax: r.GetTax(), ItemName: r.GetItemName(), Bags: bags})
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out := &characterv1.BuyListingResponse{Outcome: marketOutcomeOf(res.Outcome), Available: res.Available, SellerId: res.SellerID}
	if res.Outcome == character.OutcomeDone {
		out.SellerMail = toProtoMail(res.SellerMail)
	}
	return out, nil
}

// RemoveListing stores a seller's removal of a listing in one transaction (migration 0031; see character.Store.RemoveListing).
func (s *Server) RemoveListing(ctx context.Context, r *characterv1.RemoveListingRequest) (*characterv1.RemoveListingResponse, error) {
	ms, ok := s.store.(MarketSalesStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no market sales")
	}
	if r.GetInventory() == nil {
		return nil, status.Error(codes.InvalidArgument, "no bags are given")
	}
	bags, err := claimBagsOf(r.GetInventory())
	if err != nil {
		return nil, err
	}
	res, err := ms.RemoveListing(ctx, character.RemoveRequest{SellerID: r.GetCharacterId(), ListingID: r.GetListingId(), ExpectedQuantity: r.GetExpectedQuantity(), Bags: bags})
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.RemoveListingResponse{Outcome: marketOutcomeOf(res.Outcome), Available: res.Available}, nil
}

// ExpireListing ends a listing whose time has come and mails its stack to the seller, in one transaction (migration 0031; see character.Store.ExpireListing).
func (s *Server) ExpireListing(ctx context.Context, r *characterv1.ExpireListingRequest) (*characterv1.ExpireListingResponse, error) {
	ms, ok := s.store.(MarketSalesStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no market sales")
	}
	res, err := ms.ExpireListing(ctx, r.GetListingId(), r.GetItemName())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	out := &characterv1.ExpireListingResponse{Outcome: marketOutcomeOf(res.Outcome), SellerId: res.SellerID}
	if res.Outcome == character.OutcomeDone {
		out.SellerMail = toProtoMail(res.SellerMail)
	}
	return out, nil
}

// CommitPlayerTrade commits a two-character trade in one transaction (migration 0032; see character.Store.CommitPlayerTrade).
func (s *Server) CommitPlayerTrade(ctx context.Context, r *characterv1.CommitPlayerTradeRequest) (*characterv1.CommitPlayerTradeResponse, error) {
	ts, ok := s.store.(TradeStore)
	if !ok {
		return nil, status.Error(codes.Unimplemented, "this character store keeps no player trades")
	}
	if r.GetSideA() == nil || r.GetSideB() == nil {
		return nil, status.Error(codes.InvalidArgument, "both trade sides are required")
	}
	bagsA, err := claimBagsOf(r.GetSideA())
	if err != nil {
		return nil, err
	}
	bagsB, err := claimBagsOf(r.GetSideB())
	if err != nil {
		return nil, err
	}
	res, err := ts.CommitPlayerTrade(ctx, character.CommitTradeRequest{
		TradeID:       r.GetTradeId(),
		SideA:         character.TradeSide{ID: r.GetSideA().GetId(), Bags: bagsA},
		SideB:         character.TradeSide{ID: r.GetSideB().GetId(), Bags: bagsB},
		CopperAToB:    r.GetCopperAToB(),
		CopperBToA:    r.GetCopperBToA(),
		ItemsAToB:     r.GetItemsAToB(),
		ItemsBToA:     r.GetItemsBToA(),
		CommittedAtMs: r.GetCommittedAtMs(),
	})
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &characterv1.CommitPlayerTradeResponse{Replayed: res.Replayed}, nil
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

func toProtoProfessions(ps []character.Profession) []*characterv1.ProfessionState {
	if len(ps) == 0 {
		return nil
	}
	out := make([]*characterv1.ProfessionState, 0, len(ps))
	for _, p := range ps {
		out = append(out, &characterv1.ProfessionState{ProfessionGuid: p.ProfessionGuid, CertificationTier: p.CertificationTier, Level: p.Level, Xp: p.XP})
	}
	return out
}

func toProtoWallet(w []character.WalletEntry) []*characterv1.WalletEntry {
	out := make([]*characterv1.WalletEntry, 0, len(w))
	for _, e := range w {
		out = append(out, &characterv1.WalletEntry{CurrencyGuid: e.CurrencyGuid, Amount: e.Amount})
	}
	return out
}

func toProtoJobs(jobs []character.ProcessingJob) []*characterv1.ProcessingJob {
	if len(jobs) == 0 {
		return nil
	}
	out := make([]*characterv1.ProcessingJob, 0, len(jobs))
	for _, j := range jobs {
		p := &characterv1.ProcessingJob{JobGuid: j.JobGUID, StationKey: j.StationKey, RecipeGuid: j.RecipeGUID, SubRecipeGuid: j.SubRecipeGUID, Quantity: j.Quantity,
			QueuedAtMs: j.QueuedAt.UnixMilli(), StartedAtMs: j.StartedAt.UnixMilli(), DurationMs: j.DurationMs, ClientTxn: j.ClientTxn,
			RatingXp: j.RatingXP, RatingQuantity: j.RatingQuantity, RatingRarity: j.RatingRarity}
		for _, r := range j.Resources {
			res := &characterv1.JobResource{RecordId: r.RecordID}
			for _, ra := range r.Rarities {
				res.Rarities = append(res.Rarities, &characterv1.JobRarity{Rarity: ra.Rarity, Count: ra.Count, FromWarehouse: ra.FromWarehouse})
			}
			p.Resources = append(p.Resources, res)
		}
		out = append(out, p)
	}
	return out
}

func fromProtoJobs(jobs []*characterv1.ProcessingJob) []character.ProcessingJob {
	out := make([]character.ProcessingJob, 0, len(jobs))
	for _, p := range jobs {
		j := character.ProcessingJob{JobGUID: p.GetJobGuid(), StationKey: p.GetStationKey(), RecipeGUID: p.GetRecipeGuid(), SubRecipeGUID: p.GetSubRecipeGuid(), Quantity: p.GetQuantity(),
			QueuedAt: time.UnixMilli(p.GetQueuedAtMs()).UTC(), StartedAt: time.UnixMilli(p.GetStartedAtMs()).UTC(), DurationMs: p.GetDurationMs(), ClientTxn: p.GetClientTxn(),
			RatingXP: p.GetRatingXp(), RatingQuantity: p.GetRatingQuantity(), RatingRarity: p.GetRatingRarity()}
		for _, r := range p.GetResources() {
			res := character.JobResource{RecordID: r.GetRecordId()}
			for _, ra := range r.GetRarities() {
				res.Rarities = append(res.Rarities, character.JobRarity{Rarity: ra.GetRarity(), Count: ra.GetCount(), FromWarehouse: ra.GetFromWarehouse()})
			}
			j.Resources = append(j.Resources, res)
		}
		out = append(out, j)
	}
	return out
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

// C O A with Cyb

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
	if r.GetReplaceWarehouse() {
		ws, ok := s.store.(WarehouseStore)
		if !ok {
			return nil, status.Error(codes.Unimplemented, "this character store keeps no warehouse")
		}
		wallet := make([]character.WalletEntry, 0, len(r.GetWallet()))
		for _, e := range r.GetWallet() {
			wallet = append(wallet, character.WalletEntry{CurrencyGuid: e.GetCurrencyGuid(), Amount: e.GetAmount()})
		}
		stored := make([]character.WarehouseItem, 0, len(r.GetWarehouseItems()))
		for _, w := range r.GetWarehouseItems() {
			stored = append(stored, fromProtoWarehouse(w))
		}
		if err := ws.SaveInventoryEverything(ctx, r.GetId(), items, wallet, r.GetReplaceWallet(), fromProtoJobs(r.GetProcessingJobs()), r.GetReplaceProcessingJobs(), r.GetKnownRecipes(),
			r.GetReplaceKnownRecipes(), stored, true); err != nil {
			return nil, toStatus(ctx, err)
		}
		return &characterv1.SaveInventoryResponse{Saved: int32(len(items))}, nil
	}
	if r.GetReplaceProcessingJobs() || r.GetReplaceKnownRecipes() {
		js, ok := s.store.(JobStore)
		if !ok {
			return nil, status.Error(codes.Unimplemented, "this character store keeps no processing jobs or known recipes")
		}
		wallet := make([]character.WalletEntry, 0, len(r.GetWallet()))
		for _, e := range r.GetWallet() {
			wallet = append(wallet, character.WalletEntry{CurrencyGuid: e.GetCurrencyGuid(), Amount: e.GetAmount()})
		}
		if err := js.SaveInventoryAll(ctx, r.GetId(), items, wallet, r.GetReplaceWallet(), fromProtoJobs(r.GetProcessingJobs()), r.GetReplaceProcessingJobs(), r.GetKnownRecipes(), r.GetReplaceKnownRecipes()); err != nil {
			return nil, toStatus(ctx, err)
		}
		return &characterv1.SaveInventoryResponse{Saved: int32(len(items))}, nil
	}
	if r.GetReplaceWallet() {
		ws, ok := s.store.(WalletStore)
		if !ok {
			return nil, status.Error(codes.Unimplemented, "this character store keeps no wallet")
		}
		wallet := make([]character.WalletEntry, 0, len(r.GetWallet()))
		for _, e := range r.GetWallet() {
			wallet = append(wallet, character.WalletEntry{CurrencyGuid: e.GetCurrencyGuid(), Amount: e.GetAmount()})
		}
		if err := ws.SaveInventoryAndWallet(ctx, r.GetId(), items, wallet); err != nil {
			return nil, toStatus(ctx, err)
		}
		return &characterv1.SaveInventoryResponse{Saved: int32(len(items))}, nil
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
	case errors.Is(err, character.ErrCharacterLimit):
		return status.Error(codes.ResourceExhausted, err.Error())
	case errors.Is(err, character.ErrNameTaken), errors.Is(err, character.ErrIDTaken), errors.Is(err, character.ErrGuildNameTaken), errors.Is(err, character.ErrListingSaved):
		return status.Error(codes.AlreadyExists, err.Error())
	case errors.Is(err, character.ErrGuildVersion), errors.Is(err, character.ErrMailStale), errors.Is(err, character.ErrTradeCommitted):
		return status.Error(codes.FailedPrecondition, err.Error())
	case errors.Is(err, character.ErrMailClaimed):
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

// Social, step 7: a character's friends and pending requests, as GetCharacter sends them and as SaveProgress takes them back.
func toProtoFriends(fs []character.Friend) []*characterv1.Friend {
	if len(fs) == 0 {
		return nil
	}
	out := make([]*characterv1.Friend, 0, len(fs))
	for _, f := range fs {
		out = append(out, &characterv1.Friend{Guid: f.GUID, Name: f.Name, Level: f.Level, ClassId: f.ClassID, SubArchetype: f.SubArchetype,
			GuildName: f.GuildName, StillFriend: f.StillFriend})
	}
	return out
}

func fromProtoFriends(fs []*characterv1.Friend) []character.Friend {
	out := make([]character.Friend, 0, len(fs))
	for _, f := range fs {
		out = append(out, character.Friend{GUID: f.GetGuid(), Name: f.GetName(), Level: f.GetLevel(), ClassID: f.GetClassId(), SubArchetype: f.GetSubArchetype(),
			GuildName: f.GetGuildName(), StillFriend: f.GetStillFriend()})
	}
	return out
}

func toProtoFriendRequests(rs []character.FriendRequest) []*characterv1.FriendRequest {
	if len(rs) == 0 {
		return nil
	}
	out := make([]*characterv1.FriendRequest, 0, len(rs))
	for _, r := range rs {
		out = append(out, &characterv1.FriendRequest{SenderGuid: r.SenderGUID, SenderName: r.SenderName, RecipientGuid: r.RecipientGUID, ExpiresAtMs: r.ExpiresAtMs})
	}
	return out
}

func fromProtoFriendRequests(rs []*characterv1.FriendRequest) []character.FriendRequest {
	out := make([]character.FriendRequest, 0, len(rs))
	for _, r := range rs {
		out = append(out, character.FriendRequest{SenderGUID: r.GetSenderGuid(), SenderName: r.GetSenderName(), RecipientGUID: r.GetRecipientGuid(), ExpiresAtMs: r.GetExpiresAtMs()})
	}
	return out
}

// Mail (migration 0027): a character's mails and their attachments, as the three calls send and take them.
func toProtoMails(mails []character.Mail) []*characterv1.Mail {
	if len(mails) == 0 {
		return nil
	}
	out := make([]*characterv1.Mail, 0, len(mails))
	for _, m := range mails {
		out = append(out, toProtoMail(m))
	}
	return out
}

func toProtoMail(m character.Mail) *characterv1.Mail {
	return &characterv1.Mail{Id: m.ID, Sender: m.Sender, Subject: m.Subject, Body: m.Body, Status: m.Status, IssuedAtMs: m.IssuedAtMs,
		ExpiresAtMs: m.ExpiresAtMs, Items: toProtoMailItems(m.Items), MoneyCopper: m.MoneyCopper, MoneyClaimed: m.MoneyClaimed}
}

func fromProtoMail(m *characterv1.Mail) character.Mail {
	return character.Mail{ID: m.GetId(), Sender: m.GetSender(), Subject: m.GetSubject(), Body: m.GetBody(), Status: m.GetStatus(), IssuedAtMs: m.GetIssuedAtMs(),
		ExpiresAtMs: m.GetExpiresAtMs(), Items: fromProtoMailItems(m.GetItems())}
}

func toProtoMailItems(items []character.MailItem) []*characterv1.MailItem {
	if len(items) == 0 {
		return nil
	}
	out := make([]*characterv1.MailItem, 0, len(items))
	for _, it := range items {
		out = append(out, &characterv1.MailItem{ItemRecordId: it.ItemRecordID, InstanceGuid: it.InstanceGUID, Quantity: it.Quantity, Quality: it.Quality,
			Durability: it.Durability, CreatedAtMs: unixMilli(it.CreatedAt), PersistedHealth: it.PersistedHealth, PersistedMana: it.PersistedMana,
			LockExpiration: it.LockExpiration, Claimed: it.Claimed})
	}
	return out
}

func fromProtoMailItems(items []*characterv1.MailItem) []character.MailItem {
	if len(items) == 0 {
		return nil
	}
	out := make([]character.MailItem, 0, len(items))
	for _, it := range items {
		out = append(out, character.MailItem{ItemRecordID: it.GetItemRecordId(), InstanceGUID: it.GetInstanceGuid(), Quantity: it.GetQuantity(), Quality: it.GetQuality(),
			Durability: it.GetDurability(), CreatedAt: time.UnixMilli(it.GetCreatedAtMs()), PersistedHealth: it.PersistedHealth, PersistedMana: it.PersistedMana,
			LockExpiration: it.GetLockExpiration(), Claimed: it.GetClaimed()})
	}
	return out
}

var containerNames = map[characterv1.ItemContainer]string{
	characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK:  character.Backpack,
	characterv1.ItemContainer_ITEM_CONTAINER_EQUIPMENT: character.Equipment,
	characterv1.ItemContainer_ITEM_CONTAINER_QUEST:     character.Quest,
	characterv1.ItemContainer_ITEM_CONTAINER_RESOURCE0: character.Resource0, // Gathering stage 3, "Resource storage"
	characterv1.ItemContainer_ITEM_CONTAINER_RESOURCE1: character.Resource1,
	characterv1.ItemContainer_ITEM_CONTAINER_RESOURCE2: character.Resource2,
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

// fromProtoItem is the item as the store takes it; an error for a container that is none of the six.
func fromProtoItem(p *characterv1.Item) (character.Item, error) {
	name, ok := containerNames[p.GetContainer()]
	if !ok {
		return character.Item{}, fmt.Errorf("container %s is not the Backpack, the Equipment, the Quest storage or a resource container", p.GetContainer())
	}
	return character.Item{
		Container: name, Slot: p.GetSlot(), ItemRecordID: p.GetItemRecordId(), InstanceGUID: p.GetInstanceGuid(),
		Quantity: p.GetQuantity(), Quality: p.GetQuality(), Durability: p.GetDurability(),
		CreatedAt:       time.UnixMilli(p.GetCreatedAtMs()),
		PersistedHealth: p.PersistedHealth, PersistedMana: p.PersistedMana, LockExpiration: p.GetLockExpiration(),
	}, nil
}

// toProtoWarehouse is the warehouse items as GetCharacter sends them.
func toProtoWarehouse(items []character.WarehouseItem) []*characterv1.WarehouseItem {
	if len(items) == 0 {
		return nil
	}
	out := make([]*characterv1.WarehouseItem, 0, len(items))
	for _, w := range items {
		out = append(out, &characterv1.WarehouseItem{
			Area: w.Area, NodeGuid: w.NodeGUID, ContainerGuid: w.ContainerGUID, Slot: w.Slot, ItemRecordId: w.ItemRecordID, InstanceGuid: w.InstanceGUID,
			Quantity: w.Quantity, Quality: w.Quality, Durability: w.Durability, CreatedAtMs: unixMilli(w.CreatedAt),
			PersistedHealth: w.PersistedHealth, PersistedMana: w.PersistedMana, LockExpiration: w.LockExpiration,
		})
	}
	return out
}

// fromProtoWarehouse is a warehouse item as the store takes it (the store checks it).
func fromProtoWarehouse(p *characterv1.WarehouseItem) character.WarehouseItem {
	return character.WarehouseItem{
		Area: p.GetArea(), NodeGUID: p.GetNodeGuid(), ContainerGUID: p.GetContainerGuid(), Slot: p.GetSlot(),
		Item: character.Item{
			ItemRecordID: p.GetItemRecordId(), InstanceGUID: p.GetInstanceGuid(), Quantity: p.GetQuantity(), Quality: p.GetQuality(), Durability: p.GetDurability(),
			CreatedAt: time.UnixMilli(p.GetCreatedAtMs()), PersistedHealth: p.PersistedHealth, PersistedMana: p.PersistedMana, LockExpiration: p.GetLockExpiration(),
		},
	}
}

// Market (migration 0030): a listing as the two calls send and take it.
func toProtoMarketListings(listings []character.MarketListing) []*characterv1.MarketListing {
	if len(listings) == 0 {
		return nil
	}
	out := make([]*characterv1.MarketListing, 0, len(listings))
	for _, l := range listings {
		out = append(out, toProtoMarketListing(l))
	}
	return out
}

func toProtoMarketListing(l character.MarketListing) *characterv1.MarketListing {
	p := &characterv1.MarketListing{Id: l.ID, SellerId: l.SellerID, SellerName: l.SellerName, MarketNode: l.Node, PricePerUnit: l.PricePerUnit,
		Status: l.Status, IssueDateMs: l.IssuedAtMs, ExpiryDateMs: l.ExpiresAtMs}
	if items := toProtoItems([]character.Item{l.Item}); len(items) == 1 {
		p.Item = items[0]
	}
	return p
}

func fromProtoMarketListing(l *characterv1.MarketListing) character.MarketListing {
	item, _ := fromProtoItem(l.GetItem())
	return character.MarketListing{ID: l.GetId(), SellerID: l.GetSellerId(), SellerName: l.GetSellerName(), Node: l.GetMarketNode(), Item: item,
		PricePerUnit: l.GetPricePerUnit(), Status: l.GetStatus(), IssuedAtMs: l.GetIssueDateMs(), ExpiresAtMs: l.GetExpiryDateMs()}
}

// unixMilli is t in Unix milliseconds, and 0 for the zero time.
func unixMilli(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.UnixMilli()
}
