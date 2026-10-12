// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// nodectl is the command line of the city node service (Nodes 1): it calls the same CityNodeService methods the game's GM
// commands and the admin app to come call.
//
//	nodectl list                                       every node: level, XP, vassal link, and the node clock
//	nodectl get <node>                                 one node
//	nodectl set-level [--disable-vassalship] <node> <level>   level 0 (Wilderness) to 4 (Town)
//	nodectl add-xp <node> <amount>                     stores XP; the level does NOT change (no level-up from XP in Nodes 1)
//	nodectl clock                                      the node clock
//	nodectl clock advance <seconds>                    skip the node clock ahead
//	nodectl clock speed <factor>                       the speed knob (1 is real time)
//	nodectl seed [--force] [--file seed.json]          applies the seed if none was applied; --force overwrites EVERY level and parent
//	nodectl watch                                      prints the whole state, then every change, until Ctrl+C
//
// A <node> is its record name (Verra_RVR_Joeva), the same without Verra_ (RVR_Joeva), its display name (Joeva) or its guid.
//
// Every command takes --addr (default $AOC_CITYNODE_GRPC_ADDR or 127.0.0.1:50055) and --token-file (default $AOC_SERVICE_TOKEN_FILE,
// else service.token beside nodectl.exe, else dist/services/service.token). Exit status: 0 done, 1 the service refused or could
// not be reached, 2 the command line is wrong.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/svcauth"
)

// -- C.O.A x C.Y.B --

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Stdout, os.Stderr))
}

const usage = `usage: nodectl <command> [flags] [arguments]
  list                              every node, and the node clock
  get <node>                        one node
  set-level [--disable-vassalship] <node> <level>
  add-xp <node> <amount>
  clock [advance <seconds> | speed <factor>]
  seed [--force] [--file seed.json]
  watch
a <node> is Verra_RVR_Joeva, RVR_Joeva, Joeva or a guid; every command takes --addr and --token-file`

type usageError string

func (u usageError) Error() string { return string(u) }

// run is nodectl: it returns the exit status.
func run(ctx context.Context, args []string, out, errw io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(errw, usage)
		return 2
	}
	cmd, rest := args[0], args[1:]
	fs := flag.NewFlagSet("nodectl "+cmd, flag.ContinueOnError)
	fs.SetOutput(errw)
	cfg := config.Load()
	addr := fs.String("addr", cfg.CityNodeGRPCAddr, "the city node service's gRPC address")
	tokenFile := fs.String("token-file", defaultTokenFile(cfg.ServiceTokenFile), "the service token file")
	var (
		disable = new(bool)
		force   = new(bool)
		file    = new(string)
	)
	switch cmd {
	case "set-level":
		disable = fs.Bool("disable-vassalship", false, "the client's GM flag bDisableVassalshipSystem (Nodes 1 has no vassal rules: it only travels with the change)")
	case "seed":
		force = fs.Bool("force", false, "apply the seed again, overwriting EVERY node's level and parent")
		file = fs.String("file", "", "read the seed from this file (default: the service's own seed file)")
	case "list", "get", "add-xp", "clock", "watch":
	case "help", "-h", "--help":
		fmt.Fprintln(out, usage)
		return 0
	default:
		fmt.Fprintf(errw, "nodectl: unknown command %q\n%s\n", cmd, usage)
		return 2
	}
	if err := fs.Parse(rest); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	pos := fs.Args()

	// What the command line needs is checked before any connection is made.
	call, err := parse(cmd, pos, *disable, *force, *file)
	if err != nil {
		fmt.Fprintf(errw, "nodectl %s: %v\n", cmd, err)
		return 2
	}
	token, err := svcauth.Load(*tokenFile)
	if err != nil {
		fmt.Fprintf(errw, "nodectl: %v\n", err)
		return 1
	}
	cc, err := grpc.NewClient(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()), token.DialOption())
	if err != nil {
		fmt.Fprintf(errw, "nodectl: %v\n", err)
		return 1
	}
	defer cc.Close()
	if err := call(ctx, citynodev1.NewCityNodeServiceClient(cc), out); err != nil {
		if errors.Is(ctx.Err(), context.Canceled) && cmd == "watch" {
			return 0
		}
		fmt.Fprintf(errw, "nodectl %s: %s\n", cmd, explain(err, *addr))
		return 1
	}
	return 0
}

// defaultTokenFile is $AOC_SERVICE_TOKEN_FILE; else service.token beside nodectl.exe (scripts\services.ps1 builds both into
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
		if s.Code() == codes.Unavailable {
			return fmt.Sprintf("the node service is not reachable at %s (is it running? scripts\\services.ps1 run): %s", addr, s.Message())
		}
		if s.Code() == codes.InvalidArgument {
			return "refused: " + strings.TrimPrefix(s.Message(), "invalid: ")
		}
		return fmt.Sprintf("%s: %s", strings.ToLower(s.Code().String()), s.Message())
	}
	return err.Error()
}

type call func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error

// parse checks a command's arguments and returns the call that runs it.
func parse(cmd string, pos []string, disable, force bool, file string) (call, error) {
	switch cmd {
	case "list":
		if len(pos) != 0 {
			return nil, usageError("takes no arguments")
		}
		return doList, nil
	case "get":
		if len(pos) != 1 {
			return nil, usageError("needs one node: nodectl get <node>")
		}
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.GetNode(ctx, &citynodev1.GetNodeRequest{Node: ref(pos[0])})
			if err != nil {
				return err
			}
			printNode(out, r.GetNode())
			return nil
		}, nil
	case "set-level":
		if len(pos) != 2 {
			return nil, usageError("needs a node and a level: nodectl set-level [--disable-vassalship] <node> <level>")
		}
		level, err := strconv.ParseInt(pos[1], 10, 32)
		if err != nil {
			return nil, usageError(fmt.Sprintf("the level %q is not a number (0 Wilderness ... 4 Town)", pos[1]))
		}
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.SetNodeLevel(ctx, &citynodev1.SetNodeLevelRequest{Node: ref(pos[0]), Level: int32(level), DisableVassalship: disable})
			if err != nil {
				return err
			}
			if !r.GetChanged() {
				fmt.Fprintf(out, "%s was already level %d (%s): nothing changed\n", r.GetNode().GetName(), r.GetNode().GetLevel(), r.GetNode().GetLevelName())
			} else {
				fmt.Fprintf(out, "%s is now level %d (%s)\n", r.GetNode().GetName(), r.GetNode().GetLevel(), r.GetNode().GetLevelName())
			}
			return nil
		}, nil
	case "add-xp":
		if len(pos) != 2 {
			return nil, usageError("needs a node and an amount: nodectl add-xp <node> <amount>")
		}
		amount, err := strconv.ParseInt(pos[1], 10, 64)
		if err != nil {
			return nil, usageError(fmt.Sprintf("the amount %q is not a whole number", pos[1]))
		}
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.AddNodeExperience(ctx, &citynodev1.AddNodeExperienceRequest{Node: ref(pos[0]), Amount: amount})
			if err != nil {
				return err
			}
			n := r.GetNode()
			fmt.Fprintf(out, "%s holds %d XP: %d%% through level %d (%s); the level is unchanged\n", n.GetName(), n.GetExperience(), n.GetPercent(), n.GetLevel(), n.GetLevelName())
			return nil
		}, nil
	case "clock":
		return parseClock(pos)
	case "seed":
		if len(pos) != 0 {
			return nil, usageError("takes no arguments: nodectl seed [--force] [--file seed.json]")
		}
		var body string
		if file != "" {
			b, err := os.ReadFile(file)
			if err != nil {
				return nil, usageError(err.Error())
			}
			if len(b) == 0 {
				return nil, usageError(file + " is empty")
			}
			body = string(b)
		}
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.ApplySeed(ctx, &citynodev1.ApplySeedRequest{Force: force, SeedJson: body})
			if err != nil {
				return err
			}
			at := time.UnixMilli(r.GetAppliedAtMs()).Format(time.RFC3339)
			switch {
			case !r.GetApplied():
				fmt.Fprintf(out, "the seed was applied before (%s); nothing changed. nodectl seed --force applies it again and overwrites EVERY node's level and parent\n", at)
			case r.GetForced():
				fmt.Fprintf(out, "FORCED: the seed was applied again and overwrote the levels and parents of all nodes; %d node(s) changed (XP untouched)\n", r.GetNodesChanged())
			default:
				fmt.Fprintf(out, "the seed was applied; %d node(s) changed\n", r.GetNodesChanged())
			}
			return nil
		}, nil
	case "watch":
		if len(pos) != 0 {
			return nil, usageError("takes no arguments")
		}
		return doWatch, nil
	}
	return nil, usageError("unknown command")
}

func parseClock(pos []string) (call, error) {
	switch {
	case len(pos) == 0:
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.GetClock(ctx, &citynodev1.GetClockRequest{})
			if err != nil {
				return err
			}
			printClock(out, r.GetClock())
			return nil
		}, nil
	case pos[0] == "advance" && len(pos) == 2:
		sec, err := strconv.ParseInt(pos[1], 10, 64)
		if err != nil {
			return nil, usageError(fmt.Sprintf("the seconds %q are not a whole number", pos[1]))
		}
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.AdvanceClock(ctx, &citynodev1.AdvanceClockRequest{Seconds: sec})
			if err != nil {
				return err
			}
			printClock(out, r.GetClock())
			return nil
		}, nil
	case pos[0] == "speed" && len(pos) == 2:
		f, err := strconv.ParseFloat(pos[1], 64)
		if err != nil {
			return nil, usageError(fmt.Sprintf("the factor %q is not a number", pos[1]))
		}
		return func(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
			r, err := c.SetClockSpeed(ctx, &citynodev1.SetClockSpeedRequest{Factor: f})
			if err != nil {
				return err
			}
			printClock(out, r.GetClock())
			return nil
		}, nil
	}
	return nil, usageError("nodectl clock | nodectl clock advance <seconds> | nodectl clock speed <factor>")
}

// ref is a <node> argument: a number is a guid, anything else a name.
func ref(s string) *citynodev1.NodeRef {
	if g, err := strconv.ParseInt(s, 10, 64); err == nil && g != 0 {
		return &citynodev1.NodeRef{Guid: g}
	}
	return &citynodev1.NodeRef{Name: s}
}

// C-O-A with Cyb

func doList(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
	r, err := c.ListNodes(ctx, &citynodev1.ListNodesRequest{})
	if err != nil {
		return err
	}
	names := map[int64]string{}
	for _, n := range r.GetNodes() {
		names[n.GetGuid()] = n.GetName()
	}
	tw := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	fmt.Fprintln(tw, "NODE\tLEVEL\tXP\t%\tVASSAL OF")
	for _, n := range r.GetNodes() {
		parent := ""
		if p := n.GetParentGuid(); p != 0 {
			parent = names[p]
			if parent == "" {
				parent = strconv.FormatInt(p, 10)
			}
		}
		fmt.Fprintf(tw, "%s\t%d %s\t%d\t%d\t%s\n", n.GetName(), n.GetLevel(), n.GetLevelName(), n.GetExperience(), n.GetPercent(), parent)
	}
	tw.Flush()
	fmt.Fprintf(out, "%d nodes (max level %d); change #%d\n", len(r.GetNodes()), r.GetMaxNodeLevel(), r.GetSeq())
	printClock(out, r.GetClock())
	return nil
}

func printNode(out io.Writer, n *citynodev1.Node) {
	fmt.Fprintf(out, "%s (%s), guid %d\n", n.GetName(), n.GetDisplayName(), n.GetGuid())
	fmt.Fprintf(out, "  level %d (%s)\n", n.GetLevel(), n.GetLevelName())
	fmt.Fprintf(out, "  XP %d: %d%% through the level, %d to the next\n", n.GetExperience(), n.GetPercent(), n.GetExperienceToNextLevel())
	if p := n.GetParentGuid(); p != 0 {
		fmt.Fprintf(out, "  vassal of guid %d\n", p)
	} else {
		fmt.Fprintln(out, "  no lord")
	}
	fmt.Fprintf(out, "  changed %s\n", time.UnixMilli(n.GetUpdatedAtMs()).Format(time.RFC3339))
}

func printClock(out io.Writer, c *citynodev1.Clock) {
	fmt.Fprintf(out, "node clock: %s (speed x%g, offset %ds; real time %s)\n",
		time.UnixMilli(c.GetNodeUnixMs()).UTC().Format("2006-01-02 15:04:05 UTC"), c.GetSpeed(), c.GetOffsetSeconds(),
		time.UnixMilli(c.GetRealUnixMs()).UTC().Format("15:04:05 UTC"))
}

func doWatch(ctx context.Context, c citynodev1.CityNodeServiceClient, out io.Writer) error {
	st, err := c.WatchNodes(ctx, &citynodev1.WatchNodesRequest{})
	if err != nil {
		return err
	}
	for {
		m, err := st.Recv()
		if err != nil {
			return err
		}
		switch ev := m.GetEvent().(type) {
		case *citynodev1.WatchNodesResponse_Snapshot:
			fmt.Fprintf(out, "#%d snapshot: %d nodes\n", m.GetSeq(), len(ev.Snapshot.GetNodes()))
			for _, n := range ev.Snapshot.GetNodes() {
				if n.GetLevel() > 0 || n.GetParentGuid() != 0 || n.GetExperience() > 0 {
					fmt.Fprintf(out, "      %s level %d xp %d\n", n.GetName(), n.GetLevel(), n.GetExperience())
				}
			}
		case *citynodev1.WatchNodesResponse_NodeChanged:
			n := ev.NodeChanged.GetNode()
			fmt.Fprintf(out, "#%d %s: %s level %d -> %d, xp %d (parent %d)\n", m.GetSeq(), strings.TrimPrefix(ev.NodeChanged.GetCause().String(), "CHANGE_CAUSE_"),
				n.GetName(), ev.NodeChanged.GetPreviousLevel(), n.GetLevel(), n.GetExperience(), n.GetParentGuid())
		case *citynodev1.WatchNodesResponse_ClockChanged:
			fmt.Fprintf(out, "#%d clock: ", m.GetSeq())
			printClock(out, ev.ClockChanged)
		}
	}
}
