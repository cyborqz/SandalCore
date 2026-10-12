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
every command takes --addr and --token-file`

type usageError string

func (u usageError) Error() string { return string(u) }

// run is worldctl: it returns the exit status.
func run(ctx context.Context, args []string, out, errw io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(errw, usage)
		return 2
	}
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "players", "places", "tp", "speed":
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
