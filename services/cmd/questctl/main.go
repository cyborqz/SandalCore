// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// questctl is the command line of the quest service (quests milestone, slice 1): it calls the same QuestService methods worldd calls.
//
//	questctl give <character> <quest>        the character takes the quest, whatever its rules say (a GM's start)
//	questctl finish <character> <quest>      the quest counts as done: out of the log, one more finished
//	questctl fail <character> <quest>        the quest failed: out of the log, not counted as finished
//	questctl abandon <character> <quest>     out of the log, as if the character had abandoned it
//	questctl clear <character> [<quest>]     forget the finished quests and the quest variables; with a quest, forget only that quest's finishes
//	questctl set-var <character> <name> <value>   a quest variable (the rules of the chain quests read them, for example PickedSamias)
//	questctl log <character>                 what is saved for the character
//
// A <character> is its id or its name; a <quest> is its record id or its design name (RVR_FTUE_QST_1415_OfTheRootways). A command is put in the
// character's inbox: worldd does it in the character's session within a moment, or at its next login when it is away.
//
// Every command takes --addr (default $AOC_QUESTS_GRPC_ADDR or 127.0.0.1:50058) and --token-file (default $AOC_SERVICE_TOKEN_FILE, else service.token
// beside questctl.exe, else dist/services/service.token). Exit status: 0 done, 1 the service refused or could not be reached, 2 the command line is wrong.
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

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	questv1 "aocrop/services/gen/aoc/quest/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/svcauth"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Stdout, os.Stderr))
}

const usage = `usage: questctl <command> [flags] [arguments]
  give <character> <quest>
  finish <character> <quest>
  fail <character> <quest>
  abandon <character> <quest>
  clear <character> [<quest>]
  set-var <character> <name> <value>
  log <character>
a <character> is its id or name, a <quest> its record id or design name; every command takes --addr and --token-file`

type usageError string

func (u usageError) Error() string { return string(u) }

type call func(ctx context.Context, c questv1.QuestServiceClient, out io.Writer) error

// run is questctl: it returns the exit status.
func run(ctx context.Context, args []string, out, errw io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(errw, usage)
		return 2
	}
	cmd, rest := args[0], args[1:]
	if cmd == "help" || cmd == "-h" || cmd == "--help" {
		fmt.Fprintln(out, usage)
		return 0
	}
	fs := flag.NewFlagSet("questctl "+cmd, flag.ContinueOnError)
	fs.SetOutput(errw)
	cfg := config.Load()
	addr := fs.String("addr", cfg.QuestsGRPCAddr, "the quest service's gRPC address")
	tokenFile := fs.String("token-file", defaultTokenFile(cfg.ServiceTokenFile), "the service token file")
	if err := fs.Parse(rest); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	c, err := parse(cmd, fs.Args())
	if err != nil {
		fmt.Fprintf(errw, "questctl %s: %v\n%s\n", cmd, err, usage)
		return 2
	}
	token, err := svcauth.Load(*tokenFile)
	if err != nil {
		fmt.Fprintf(errw, "questctl: %v\n", err)
		return 1
	}
	cc, err := grpc.NewClient(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()), token.DialOption())
	if err != nil {
		fmt.Fprintf(errw, "questctl: %v\n", err)
		return 1
	}
	defer cc.Close()
	if err := c(ctx, questv1.NewQuestServiceClient(cc), out); err != nil {
		fmt.Fprintf(errw, "questctl %s: %s\n", cmd, explain(err, *addr))
		return 1
	}
	return 0
}

// defaultTokenFile is $AOC_SERVICE_TOKEN_FILE; else service.token beside questctl.exe; else the services' default, relative to the current directory.
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
			return fmt.Sprintf("the quest service is not reachable at %s (is it running? scripts\\services.ps1 run): %s", addr, s.Message())
		case codes.InvalidArgument:
			return "refused: " + strings.TrimPrefix(s.Message(), "invalid: ")
		case codes.NotFound:
			return "no such character"
		}
		return fmt.Sprintf("%s: %s", strings.ToLower(s.Code().String()), s.Message())
	}
	return err.Error()
}

// parse checks a command's arguments and returns the call that runs it.
func parse(cmd string, pos []string) (call, error) {
	switch cmd {
	case "give", "finish", "fail", "abandon":
		if len(pos) != 2 {
			return nil, usageError("takes a character and a quest")
		}
		return queue(&questv1.QueueCommandRequest{CharacterId: pos[0], Kind: cmd, Quest: pos[1]}), nil
	case "clear":
		if len(pos) < 1 || len(pos) > 2 {
			return nil, usageError("takes a character and, optionally, a quest")
		}
		r := &questv1.QueueCommandRequest{CharacterId: pos[0], Kind: "clear"}
		if len(pos) == 2 {
			r.Quest = pos[1]
		}
		return queue(r), nil
	case "set-var":
		if len(pos) != 3 {
			return nil, usageError("takes a character, a variable name and a value")
		}
		v, err := strconv.ParseInt(pos[2], 10, 64)
		if err != nil {
			return nil, usageError(fmt.Sprintf("the value %q is not a whole number", pos[2]))
		}
		return queue(&questv1.QueueCommandRequest{CharacterId: pos[0], Kind: "set-var", Name: pos[1], Value: v}), nil
	case "log":
		if len(pos) != 1 {
			return nil, usageError("takes a character")
		}
		return func(ctx context.Context, c questv1.QuestServiceClient, out io.Writer) error {
			r, err := c.LoadLog(ctx, &questv1.LoadLogRequest{CharacterId: pos[0]})
			if err != nil {
				return err
			}
			printLog(out, r.GetLog())
			return nil
		}, nil
	}
	return nil, usageError("unknown command")
}

func queue(req *questv1.QueueCommandRequest) call {
	return func(ctx context.Context, c questv1.QuestServiceClient, out io.Writer) error {
		r, err := c.QueueCommand(ctx, req)
		if err != nil {
			return err
		}
		fmt.Fprintf(out, "queued command %d (%s); worldd does it within a moment when the character plays, else at its next login\n", r.GetId(), req.GetKind())
		return nil
	}
}

var statusNames = [...]string{"active", "completed", "failed", "paused"}

func printLog(out io.Writer, log *questv1.QuestLog) {
	fmt.Fprintf(out, "%d quest(s) taken, %d finished, %d variable(s)\n", len(log.GetInstances()), len(log.GetCompleted()), len(log.GetVariables()))
	for _, q := range log.GetInstances() {
		st := "?"
		if q.GetStatus() >= 0 && int(q.GetStatus()) < len(statusNames) {
			st = statusNames[q.GetStatus()]
		}
		fmt.Fprintf(out, "  taken   %d  %s  %s  %d objective(s)\n", q.GetRecordId(), q.GetInstanceGuid(), st, len(q.GetObjectives()))
	}
	for _, c := range log.GetCompleted() {
		fmt.Fprintf(out, "  done    %d  x%d\n", c.GetRecordId(), c.GetCount())
	}
	for _, v := range log.GetVariables() {
		fmt.Fprintf(out, "  var     %s = %d\n", v.GetName(), v.GetValue())
	}
}
