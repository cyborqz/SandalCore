// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// worldctl is the command line of worldd's admin API (the GM travel tools): it calls the same WorldAdminService methods the admin
// app to come (a 3D viewer that controls the server) calls.
//
//	worldctl players                                  every player in the world: name, guid, position, mounted or not
//	worldctl places                                   the places tp takes by name: every city node, "login" and "garden" (the GM garden's centre, when it is placed)
//	worldctl tp <character> <place>                   moves the player to a place (joeva, "New Aela", Verra_RVR_Joeva, login ...)
//	worldctl tp <character> <x> <y> [<z>]             moves the player to a point (cm); without z, the ground height under it
//	worldctl speed <character> <multiplier>           the character's run speed, 0.5 to 10 (1 resets); until relog
//	worldctl give-money <character> <copper>          adds copper to the character's purse (100 copper = 1 silver, 100 silver = 1 gold; the purse holds 10,000,000 at most)
//	worldctl give-item <character> <item> [<count>] [<rarity>|q<NN>]
//	                                                  gives an item by its record name or Guid (count 1 by default), into the bag its kind belongs to. A rarity (Common, Uncommon, Rare,
//	                                                  Heroic, Epic, Legendary or Artifact) gives the lowest quality of that colour; q<NN> (q1 to q200) gives an exact quality. An item is
//	                                                  only given a rarity it can have: the answer lists the ones it can
//	worldctl place-station <node> <plot> <station>    puts an artisan building's station on a free service plot of a Village or a Town (it stays until removed, until the node drops below
//	                                                  a Village, or until worldd restarts)
//	worldctl remove-station <node> <plot>             takes a placed station off its plot
//	worldctl craft finish <character> [<station>]     finishes the character's processing jobs that are not over (the one running and the ones waiting), all of them or only those at one
//	                                                  station: each is over now and ready to collect
//	worldctl craft teach <character> <recipe>         teaches the character a recipe the game makes a player learn (its record name or Guid): it is known from then on and is saved
//	worldctl craft forget <character> <recipe>        takes a taught recipe away again
//	worldctl craft taught <character>                 lists the recipes the character was taught
//	worldctl tutorials reset <character>              puts the character's finished tutorials back: its tutorial hints can start again (a test command)
//	worldctl npc kill|respawn|reset|heal|despawn <id> a game master's action on one creature, named by its actor id (npc.go says what each does)
//	worldctl npc move <id> <x> <y> [<z>]              stands a creature at a point (cm; without z, the ground under it); its leash is centred there, its spawn point stays
//	worldctl watch [--area minx,miny,maxx,maxy] [--density [--cell 50000]] [--layers all|players,npcs,nodes,mounts,loot] [--seconds N]
//	                                                  watches worldd's world stream (WorldStreamService, AOC_WORLD_STREAM_ADDR, default 127.0.0.1:50057): one line a second with the
//	                                                  frames that came, the entities held and the cost of a publish (watch.go)
//
// A mounted player is dismounted first. A <place> is matched as nodectl matches a node: the record name (Verra_RVR_Joeva), the same
// without Verra_, or the display name (Joeva); case, spaces and punctuation do not count.
//
// Every command takes --addr (default $AOC_WORLD_ADMIN_ADDR or 127.0.0.1:50056) and --token-file (default $AOC_SERVICE_TOKEN_FILE,
// else service.token beside worldctl.exe, else dist/services/service.token). Exit status: 0 done, 1 worldd refused or could not be
// reached, 2 the command line is wrong.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"text/tabwriter"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/svcauth"
)

// ~ CoA & C.Y.B ~

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Stdout, os.Stderr))
}

const usage = `usage: worldctl <command> [flags] [arguments]
  players                           every player in the world
  places                            the places tp takes by name
  tp <character> <place>            move a player to a city node or to "login"
  tp <character> <x> <y> [<z>]      move a player to a point (cm); without z, the ground height
  speed <character> <multiplier>    run speed, 0.5 to 10; 1 resets (until relog)
  give-money <character> <copper>   add copper to the purse (a test command)
  give-item <character> <item> [n] [rarity|qNN]  give an item by record name or Guid, into the right bag, in a rarity (Common ... Artifact) or at a quality (q1 to q200) (a test command)
  place-station <node> <plot> <st>  put an artisan building's station on a free plot of a Village or a Town
  remove-station <node> <plot>      take a placed station off its plot
  craft finish <character> [<st>]   finish the character's processing jobs at once, ready to collect (a test command)
  craft teach|forget <character> <recipe>   teach a recipe the game makes a player learn (name or Guid), or take it away again (a test command)
  craft taught <character>          list the recipes the character was taught
  tutorials reset <character>       put the character's finished tutorials back, so its hints can start again (a test command)
  npc kill|respawn|reset|heal|despawn <id>   a game master's action on one creature, by its actor id
  npc move <id> <x> <y> [<z>]       stand a creature at a point (cm); its leash is centred there, its spawn point stays
  watch [--area minx,miny,maxx,maxy] [--density] [--layers l] [--seconds n]   watch worldd's world stream (its own address, AOC_WORLD_STREAM_ADDR): one line a second
every command takes --addr and --token-file`

type usageError string

// rarityNames are the rarities give-item takes, as the game spells them.
var rarityNames = []string{"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}

// parseGiveWords reads the words after the item of give-item: a whole number is the count (1 to 100000), q<NN> an exact quality (1 to 200), and a rarity name the rarity (the lowest
// quality of that colour). Each kind may be given once, in any order.
func parseGiveWords(words []string) (count int64, rarity string, quality int64, err error) {
	count = 1
	var sawCount, sawRarity, sawQuality bool
	for _, raw := range words {
		w := strings.TrimSpace(raw)
		isDigits := func(s string) bool {
			if s == "" {
				return false
			}
			for _, c := range s {
				if c < '0' || c > '9' {
					return false
				}
			}
			return true
		}
		switch {
		case isDigits(w) || strings.HasPrefix(w, "-") || strings.HasPrefix(w, "+") || strings.ContainsAny(w, ".,"):
			n, perr := strconv.ParseInt(w, 10, 32)
			if perr != nil || n < 1 || n > 100000 || sawCount {
				return 0, "", 0, usageError(fmt.Sprintf("the count %q is not a whole number from 1 to 100000 (or it was given twice)", raw))
			}
			count, sawCount = n, true
		case len(w) > 1 && (w[0] == 'q' || w[0] == 'Q') && isDigits(w[1:]):
			n, perr := strconv.ParseInt(w[1:], 10, 32)
			if perr != nil || n < 1 || n > 200 || sawQuality {
				return 0, "", 0, usageError(fmt.Sprintf("the quality %q is not q1 to q200 (or it was given twice)", raw))
			}
			quality, sawQuality = n, true
		default:
			name := ""
			for _, r := range rarityNames {
				if strings.EqualFold(w, r) {
					name = r
				}
			}
			if name == "" || sawRarity {
				return 0, "", 0, usageError(fmt.Sprintf("%q is not a rarity (or it was given twice): use %s, or q<number> (q1 to q200) for an exact quality", raw, strings.Join(rarityNames, ", ")))
			}
			rarity, sawRarity = name, true
		}
	}
	return count, rarity, quality, nil
}

func (u usageError) Error() string { return string(u) }

// run is worldctl: it returns the exit status.
func run(ctx context.Context, args []string, out, errw io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(errw, usage)
		return 2
	}
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "players", "places", "tp", "speed", "give-money", "give-item", "place-station", "remove-station", "craft", "tutorials", "npc":
	case "watch":
		return runWatch(ctx, rest, out, errw)
	case "help", "-h", "--help":
		fmt.Fprintln(out, usage)
		return 0
	default:
		fmt.Fprintf(errw, "worldctl: unknown command %q\n%s\n", cmd, usage)
		return 2
	}
	fs := flag.NewFlagSet("worldctl "+cmd, flag.ContinueOnError)
	fs.SetOutput(errw)
	cfg := config.Load()
	addr := fs.String("addr", cfg.WorldAdminAddr, "worldd's admin API address")
	tokenFile := fs.String("token-file", defaultTokenFile(cfg.ServiceTokenFile), "the service token file")
	if err := fs.Parse(rest); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	// What the command line needs is checked before any connection is made.
	call, err := parse(cmd, fs.Args())
	if err != nil {
		fmt.Fprintf(errw, "worldctl %s: %v\n", cmd, err)
		return 2
	}
	token, err := svcauth.Load(*tokenFile)
	if err != nil {
		fmt.Fprintf(errw, "worldctl: %v\n", err)
		return 1
	}
	cc, err := grpc.NewClient(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()), token.DialOption())
	if err != nil {
		fmt.Fprintf(errw, "worldctl: %v\n", err)
		return 1
	}
	defer cc.Close()
	if err := call(ctx, worldadminv1.NewWorldAdminServiceClient(cc), out); err != nil {
		fmt.Fprintf(errw, "worldctl %s: %s\n", cmd, explain(err, *addr))
		return 1
	}
	return 0
}

// defaultTokenFile is $AOC_SERVICE_TOKEN_FILE; else service.token beside worldctl.exe (scripts\services.ps1 builds both into
// dist\services); else the services' default, relative to the current directory.
func defaultTokenFile(def string) string {
	if os.Getenv("AOC_SERVICE_TOKEN_FILE") != "" {
		return def
	}
	if exe, err := os.Executable(); err == nil {
		beside := filepath.Join(filepath.Dir(exe), "service.token")
		if _, err := os.Stat(beside); err == nil {
			return beside
		}
	}
	return def
}

func explain(err error, addr string) string {
	if s, ok := status.FromError(err); ok {
		switch s.Code() {
		case codes.Unavailable:
			return fmt.Sprintf("worldd's admin API is not reachable at %s (is worldd running?): %s", addr, s.Message())
		case codes.Unauthenticated:
			return "refused: the service token is wrong (use the service.token worldd was started with)"
		case codes.InvalidArgument:
			return "refused: " + s.Message()
		case codes.NotFound:
			return "not found: " + s.Message()
		case codes.FailedPrecondition:
			return "not done: " + s.Message()
		}
		return fmt.Sprintf("%s: %s", strings.ToLower(s.Code().String()), s.Message())
	}
	return err.Error()
}

type call func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error

// parse checks a command's arguments and returns the call that runs it.
func parse(cmd string, pos []string) (call, error) {
	switch cmd {
	case "players":
		if len(pos) != 0 {
			return nil, usageError("takes no arguments")
		}
		return doPlayers, nil
	case "places":
		if len(pos) != 0 {
			return nil, usageError("takes no arguments")
		}
		return doPlaces, nil
	case "tp":
		return parseTeleport(pos)
	case "npc":
		return parseNpc(pos)
	case "craft":
		if len(pos) >= 1 && (pos[0] == "teach" || pos[0] == "forget") {
			if len(pos) != 3 {
				return nil, usageError("needs a character and a recipe: worldctl craft " + pos[0] + " <character> <recipe>, such as: worldctl craft " + pos[0] + " cyb Armorsmithing_T1_Armor_Deepward_Waist")
			}
			return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
				if pos[0] == "teach" {
					r, err := c.TeachRecipe(ctx, &worldadminv1.TeachRecipeRequest{Character: pos[1], Recipe: pos[2]})
					if err != nil {
						return err
					}
					fmt.Fprintf(out, "%s was taught %s (%d); %d recipe(s) taught in all\n", r.GetCharacter(), r.GetRecipe(), r.GetRecipeGuid(), r.GetTaught())
					return nil
				}
				r, err := c.ForgetRecipe(ctx, &worldadminv1.ForgetRecipeRequest{Character: pos[1], Recipe: pos[2]})
				if err != nil {
					return err
				}
				fmt.Fprintf(out, "%s forgot %s (%d); %d recipe(s) taught in all\n", r.GetCharacter(), r.GetRecipe(), r.GetRecipeGuid(), r.GetTaught())
				return nil
			}, nil
		}
		if len(pos) == 2 && pos[0] == "taught" {
			return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
				r, err := c.ListTaughtRecipes(ctx, &worldadminv1.ListTaughtRecipesRequest{Character: pos[1]})
				if err != nil {
					return err
				}
				fmt.Fprintf(out, "%s was taught %d recipe(s)\n", r.GetCharacter(), len(r.GetRecipes()))
				for _, rec := range r.GetRecipes() {
					fmt.Fprintf(out, "  %s (%d)\n", rec.GetRecipe(), rec.GetRecipeGuid())
				}
				return nil
			}, nil
		}
		if len(pos) < 2 || len(pos) > 3 || pos[0] != "finish" {
			return nil, usageError("needs finish, teach, forget or taught and a character: worldctl craft finish <character> [<station>], worldctl craft teach <character> <recipe>, worldctl craft taught <character>")
		}
		station := ""
		if len(pos) == 3 {
			station = pos[2]
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.FinishJobs(ctx, &worldadminv1.FinishJobsRequest{Character: pos[1], Station: station})
			if err != nil {
				return err
			}
			where := ""
			if station != "" {
				where = " at " + station
			}
			fmt.Fprintf(out, "%s: %d job(s) finished%s; they are ready to collect\n", r.GetCharacter(), r.GetFinished(), where)
			return nil
		}, nil
	case "tutorials":
		if len(pos) != 2 || pos[0] != "reset" {
			return nil, usageError("needs reset and a character: worldctl tutorials reset <character>")
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.ResetTutorials(ctx, &worldadminv1.ResetTutorialsRequest{Character: pos[1]})
			if err != nil {
				return err
			}
			fmt.Fprintf(out, "%s: %d finished tutorial(s) put back; the hints can start again\n", r.GetCharacter(), r.GetCleared())
			return nil
		}, nil
	case "speed":
		if len(pos) != 2 {
			return nil, usageError("needs a character and a multiplier: worldctl speed <character> <multiplier>")
		}
		m, err := parseNumber(pos[1])
		if err != nil {
			return nil, usageError(fmt.Sprintf("the multiplier %q is not a number", pos[1]))
		}
		if m < 0.5 || m > 10 {
			return nil, usageError(fmt.Sprintf("the multiplier %g is outside 0.5 to 10 (1 resets)", m))
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.SetMoveSpeed(ctx, &worldadminv1.SetMoveSpeedRequest{Character: pos[0], Multiplier: m})
			if err != nil {
				return err
			}
			if r.GetMultiplier() == 1 {
				fmt.Fprintf(out, "%s: run speed back to normal (was x%g)\n", r.GetCharacter(), r.GetPrevious())
			} else {
				fmt.Fprintf(out, "%s: run speed x%g (was x%g); it lasts until relog or `worldctl speed %s 1`\n", r.GetCharacter(), r.GetMultiplier(), r.GetPrevious(), r.GetCharacter())
			}
			return nil
		}, nil
	case "give-money":
		if len(pos) != 2 {
			return nil, usageError("needs a character and an amount of copper: worldctl give-money <character> <copper>")
		}
		copper, err := strconv.ParseInt(strings.TrimSpace(pos[1]), 10, 64)
		if err != nil || copper < 1 {
			return nil, usageError(fmt.Sprintf("the amount %q is not a whole number of copper of 1 or more (100 copper = 1 silver, 10,000 copper = 1 gold)", pos[1]))
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.GiveMoney(ctx, &worldadminv1.GiveMoneyRequest{Character: pos[0], Copper: copper})
			if err != nil {
				return err
			}
			note := ""
			if r.GetAdded() < copper {
				note = fmt.Sprintf("; the purse is full, only %d of the %d copper fitted", r.GetAdded(), copper)
			}
			fmt.Fprintf(out, "%s: +%d copper, the purse holds %d (was %d)%s\n", r.GetCharacter(), r.GetAdded(), r.GetBalance(), r.GetPrevious(), note)
			return nil
		}, nil
	case "give-item":
		if len(pos) < 2 || len(pos) > 5 {
			return nil, usageError("needs a character and an item: worldctl give-item <character> <item name or Guid> [<count>] [<rarity>|q<NN>]")
		}
		count, rarity, quality, err := parseGiveWords(pos[2:])
		if err != nil {
			return nil, err
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.GiveItem(ctx, &worldadminv1.GiveItemRequest{Character: pos[0], Item: pos[1], Count: int32(count), Rarity: rarity, Quality: int32(quality)})
			if err != nil {
				return err
			}
			as := ""
			if r.GetRarity() != "" { // (an older worldd does not say)
				as = fmt.Sprintf(" as %s (quality %d)", r.GetRarity(), r.GetQuality())
			}
			fmt.Fprintf(out, "%s: %d x %s (%d) given%s, into %s\n", r.GetCharacter(), r.GetCount(), r.GetItem(), r.GetItemGuid(), as, r.GetContainer())
			return nil
		}, nil
	case "place-station":
		if len(pos) != 3 {
			return nil, usageError("needs a node, a plot and a station: worldctl place-station <node> <plot> <station>, such as: worldctl place-station Joeva Plot_17_Service CraftingStation_Weaponsmithing_T3")
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.PlaceStation(ctx, &worldadminv1.PlaceStationRequest{Node: pos[0], Plot: pos[1], Station: pos[2]})
			if err != nil {
				return err
			}
			at := r.GetPosition()
			fmt.Fprintf(out, "%s %s: %s of %s placed (actor %d) at %.0f %.0f %.0f\n", r.GetNode(), r.GetPlot(), r.GetStation(), r.GetBuilding(), r.GetActor(), at.GetX(), at.GetY(), at.GetZ())
			return nil
		}, nil
	case "remove-station":
		if len(pos) != 2 {
			return nil, usageError("needs a node and a plot: worldctl remove-station <node> <plot>")
		}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.RemoveStation(ctx, &worldadminv1.RemoveStationRequest{Node: pos[0], Plot: pos[1]})
			if err != nil {
				return err
			}
			fmt.Fprintf(out, "%s %s: %s removed\n", r.GetNode(), r.GetPlot(), r.GetStation())
			return nil
		}, nil
	}
	return nil, usageError("unknown command")
}

// ~ C-Y-B :: C.O.A ~

func parseTeleport(pos []string) (call, error) {
	const help = "worldctl tp <character> <place> | worldctl tp <character> <x> <y> [<z>]"
	if len(pos) < 2 || len(pos) > 4 {
		return nil, usageError("needs a character and a place or a point: " + help)
	}
	req := &worldadminv1.TeleportRequest{Character: pos[0]}
	if len(pos) == 2 {
		// one word after the character is a place; "tp cyb 100" is not a point
		if _, err := parseNumber(pos[1]); err == nil {
			return nil, usageError(fmt.Sprintf("%q is not a place, and a point needs x and y: %s", pos[1], help))
		}
		req.Target = &worldadminv1.TeleportRequest_Place{Place: pos[1]}
	} else {
		var v [3]float64
		for i, s := range pos[1:] {
			n, err := parseNumber(s)
			if err != nil {
				return nil, usageError(fmt.Sprintf("%q is not a number: %s", s, help))
			}
			v[i] = n
		}
		req.Target = &worldadminv1.TeleportRequest_Point{Point: &worldadminv1.Point{X: v[0], Y: v[1], Z: v[2], HasZ: len(pos) == 4}}
	}
	return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
		r, err := c.Teleport(ctx, req)
		if err != nil {
			return err
		}
		to := r.GetTo()
		where := fmt.Sprintf("(%.0f, %.0f, %.0f)", to.GetX(), to.GetY(), to.GetZ())
		if r.GetPlace() != "" {
			where = r.GetPlace() + " " + where
		}
		note := ""
		if r.GetDismounted() {
			note += "; dismounted first"
		}
		if r.GetGroundHeight() {
			note += "; z is the ground height"
		}
		from := r.GetFrom()
		fmt.Fprintf(out, "%s: (%.0f, %.0f, %.0f) -> %s%s\n", r.GetCharacter(), from.GetX(), from.GetY(), from.GetZ(), where, note)
		return nil
	}, nil
}

// parseNumber is a finite number (Go's ParseFloat also takes "NaN", "Inf" and hex floats, which are not coordinates).
func parseNumber(s string) (float64, error) {
	f, err := strconv.ParseFloat(strings.TrimSpace(s), 64)
	if err != nil {
		return 0, err
	}
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return 0, errors.New("not finite")
	}
	return f, nil
}

func doPlayers(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
	r, err := c.ListPlayers(ctx, &worldadminv1.ListPlayersRequest{})
	if err != nil {
		return err
	}
	if len(r.GetPlayers()) == 0 {
		fmt.Fprintln(out, "no players in the world")
		return nil
	}
	tw := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	fmt.Fprintln(tw, "NAME\tGUID\tX\tY\tZ\tMOUNTED\tSPEED")
	for _, p := range r.GetPlayers() {
		mounted := "no"
		if p.GetMounted() {
			mounted = "yes"
		}
		state := ""
		if p.GetDead() {
			state = " (dead)"
		}
		fmt.Fprintf(tw, "%s\t%s\t%.0f\t%.0f\t%.0f\t%s\tx%g%s\n", p.GetName(), p.GetCharacterId(), p.GetPosition().GetX(), p.GetPosition().GetY(), p.GetPosition().GetZ(), mounted,
			p.GetMoveSpeedMultiplier(), state)
	}
	return tw.Flush()
}

func doPlaces(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
	r, err := c.ListPlaces(ctx, &worldadminv1.ListPlacesRequest{})
	if err != nil {
		return err
	}
	tw := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	fmt.Fprintln(tw, "PLACE\tNAME\tX\tY\tZ")
	for _, p := range r.GetPlaces() {
		fmt.Fprintf(tw, "%s\t%s\t%.0f\t%.0f\t%.0f\n", p.GetDisplayName(), p.GetName(), p.GetPosition().GetX(), p.GetPosition().GetY(), p.GetPosition().GetZ())
	}
	if err := tw.Flush(); err != nil {
		return err
	}
	fmt.Fprintf(out, "%d places\n", len(r.GetPlaces()))
	return nil
}
