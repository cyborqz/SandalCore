// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"fmt"
	"io"
	"strconv"
	"strings"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

// worldctl npc: a game master's tools on one creature, named by its actor id (the number the world stream and Inspect give; `worldctl watch` does not print ids, the admin app
// does). Every action answers with the one plain sentence worldd says of what it did.
//
//	worldctl npc kill <id>               kills it as a blow would: a corpse, the loot a kill gives (open to every player in the world, no experience), its usual respawn timer
//	worldctl npc respawn <id>            a dead creature (corpse or only its timer left) is made again at its spawn point now; a living one is replaced by a fresh one there
//	worldctl npc reset <id>              back to its spawn point at a run, at full health, its threat cleared, its leash centred on the spawn point again
//	worldctl npc heal <id>               full health
//	worldctl npc despawn <id>            takes it out of the world now; its spawn point makes it again on its usual timer
//	worldctl npc move <id> <x> <y> [<z>] stands it at the point (cm; without z, the ground under it); its leash and its walk home are centred there, its spawn point stays

const npcHelp = "worldctl npc kill|respawn|reset|heal|despawn <id> | worldctl npc move <id> <x> <y> [<z>]"

// parseNpc checks the arguments of `worldctl npc` and returns the call that runs it.
func parseNpc(pos []string) (call, error) {
	if len(pos) < 2 {
		return nil, usageError("needs an action and a creature's actor id: " + npcHelp)
	}
	action := pos[0]
	id, err := strconv.ParseUint(strings.TrimSpace(pos[1]), 10, 64)
	if err != nil || id == 0 {
		return nil, usageError(fmt.Sprintf("%q is not an actor id (the number the world stream and Inspect give): %s", pos[1], npcHelp))
	}
	if action != "move" && len(pos) != 2 {
		return nil, usageError(fmt.Sprintf("takes only the creature's id after %q: %s", action, npcHelp))
	}
	say := func(out io.Writer, sentence string) error {
		_, err := fmt.Fprintln(out, sentence)
		return err
	}
	switch action {
	case "kill":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.KillNpc(ctx, &worldadminv1.KillNpcRequest{Id: id})
			if err != nil {
				return err
			}
			return say(out, r.GetResult())
		}, nil
	case "respawn":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.RespawnNpc(ctx, &worldadminv1.RespawnNpcRequest{Id: id})
			if err != nil {
				return err
			}
			return say(out, r.GetResult())
		}, nil
	case "reset":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.ResetNpc(ctx, &worldadminv1.ResetNpcRequest{Id: id})
			if err != nil {
				return err
			}
			return say(out, r.GetResult())
		}, nil
	case "heal":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.HealNpc(ctx, &worldadminv1.HealNpcRequest{Id: id})
			if err != nil {
				return err
			}
			return say(out, r.GetResult())
		}, nil
	case "despawn":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.DespawnNpc(ctx, &worldadminv1.DespawnNpcRequest{Id: id})
			if err != nil {
				return err
			}
			return say(out, r.GetResult())
		}, nil
	case "move":
		if len(pos) < 4 || len(pos) > 5 {
			return nil, usageError("needs the creature's id and a point: worldctl npc move <id> <x> <y> [<z>]")
		}
		var v [3]float64
		for i, s := range pos[2:] {
			n, err := parseNumber(s)
			if err != nil {
				return nil, usageError(fmt.Sprintf("%q is not a number: worldctl npc move <id> <x> <y> [<z>]", s))
			}
			v[i] = n
		}
		req := &worldadminv1.MoveNpcRequest{Id: id, Point: &worldadminv1.Point{X: v[0], Y: v[1], Z: v[2], HasZ: len(pos) == 5}}
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.MoveNpc(ctx, req)
			if err != nil {
				return err
			}
			return say(out, r.GetResult())
		}, nil
	}
	return nil, usageError(fmt.Sprintf("%q is not an NPC action: %s", action, npcHelp))
}
