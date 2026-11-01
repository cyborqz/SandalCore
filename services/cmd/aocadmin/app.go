// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// app.go: the methods the window calls. Each one is one call to the server,
// the way worldctl calls the same admin API.
package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"google.golang.org/grpc/status"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/content"
)

// App: the window's backend.
type App struct {
	ctx    context.Context
	client *Client
}

// NewApp: the application.
func NewApp() *App { return &App{ctx: context.Background()} }

// startup: the window is up: read the token and open the connections. The
// connections are lazy (gRPC opens them when a call is made), so the status
// line asks the server itself whether it answers.
func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	c := NewClient(Options{TokenFile: tokenFile()})
	if err := c.Dial(ctx); err != nil {
		log.Printf("[aocadmin] the server is not reachable: %v", err)
		return
	}
	a.client = c
}

// shutdown: the window is closed: the connections go with it.
func (a *App) shutdown(ctx context.Context) {
	if a.client != nil {
		a.client.Close()
	}
}

// Status: what the window's status line shows. The window asks the world
// stream, so the line tells whether the server really answers.
type Status struct {
	AdminAddr     string  `json:"adminAddr"`
	CharacterAddr string  `json:"characterAddr"`
	StreamAddr    string  `json:"streamAddr"`
	CityNodeAddr  string  `json:"cityNodeAddr"`
	Connected     bool    `json:"connected"`
	Players       uint32  `json:"players"`
	WorldTime     float64 `json:"worldTime"`
	Error         string  `json:"error"`
}

// Status: the server's answer.
func (a *App) Status() Status {
	if a.client == nil {
		return Status{Error: "الخادم مو شغّال"}
	}
	ctx, cancel := context.WithTimeout(a.ctx, CallTimeout)
	defer cancel()
	out, err := a.client.stream.GetStatus(a.client.WithToken(ctx), &worldadminv1.GetStatusRequest{})
	status := Status{
		AdminAddr:     a.client.AdminAddr(),
		CharacterAddr: a.client.CharacterAddr(),
		StreamAddr:    a.client.WorldStreamAddr(),
		CityNodeAddr:  a.client.CityNodeAddr(),
	}
	if err != nil {
		status.Error = errText(err)
		return status
	}
	status.Connected = true
	status.Players = out.GetStats().GetPlayers()
	status.WorldTime = out.GetWorldTime()
	return status
}

// call: one call to the server: the service token, the window's timeout, and
// the server's own message when it refuses.
func call[T any](a *App, fn func(ctx context.Context) (T, error)) (T, error) {
	var zero T
	if a.client == nil {
		return zero, serverError{errors.New("الخادم مو شغّال")}
	}
	ctx, cancel := context.WithTimeout(a.ctx, CallTimeout)
	defer cancel()
	out, err := fn(a.client.WithToken(ctx))
	if err != nil {
		return zero, serverError{err}
	}
	return out, nil
}

// serverError: a refusal the window shows. The message is the server's own
// reason (the guid of a stat that is not the character's, the maximum a value
// is above), so the game master reads what the server said.
type serverError struct{ err error }

func (e serverError) Error() string { return errText(e.err) }

// Unwrap: the failure behind it.
func (e serverError) Unwrap() error { return e.err }

// errText: the message of a failure. A gRPC refusal carries its reason in the
// status, so the reason alone is shown.
func errText(err error) string {
	if err == nil {
		return ""
	}
	if s, ok := status.FromError(err); ok {
		return s.Message()
	}
	return err.Error()
}

// Players: the players in the world.
func (a *App) Players() ([]*worldadminv1.Player, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.ListPlayersResponse, error) {
		return a.client.admin.ListPlayers(ctx, &worldadminv1.ListPlayersRequest{})
	})
	if err != nil {
		return nil, err
	}
	return out.GetPlayers(), nil
}

// Places: the places a teleport can name.
func (a *App) Places() ([]*worldadminv1.Place, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.ListPlacesResponse, error) {
		return a.client.admin.ListPlaces(ctx, &worldadminv1.ListPlacesRequest{})
	})
	if err != nil {
		return nil, err
	}
	return out.GetPlaces(), nil
}

// StatRow: one stat of a character, in the shape the window's table wants.
// The guid is a string: the bundle's guids are bigger than a JavaScript number
// holds exactly, and a rounded guid names no stat.
type StatRow struct {
	Guid        string  `json:"guid"`
	Name        string  `json:"name"`
	Flags       uint32  `json:"flags"`
	Value       float64 `json:"value"`
	Max         float64 `json:"max"`
	HasOverride bool    `json:"has_override"`
	Override    float64 `json:"override"`
}

// StatModRow: one stat mod on its way to the server. The guid is a string,
// for the same reason.
type StatModRow struct {
	Stat  string  `json:"stat"`
	Value float64 `json:"value"`
}

// statRows: the stats in the shape the window's table wants.
func statRows(stats []*worldadminv1.StatInfo) []StatRow {
	rows := make([]StatRow, 0, len(stats))
	for _, s := range stats {
		rows = append(rows, StatRow{
			Guid:        strconv.FormatInt(s.GetGuid(), 10),
			Name:        s.GetName(),
			Flags:       s.GetFlags(),
			Value:       s.GetValue(),
			Max:         s.GetMax(),
			HasOverride: s.GetHasOverride(),
			Override:    s.GetOverride(),
		})
	}
	return rows
}

// Stats: the bundle's stats with the player's current values.
func (a *App) Stats(character string) ([]StatRow, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.GetPlayerStatsResponse, error) {
		return a.client.admin.GetPlayerStats(ctx, &worldadminv1.GetPlayerStatsRequest{Character: character})
	})
	if err != nil {
		return nil, err
	}
	return statRows(out.GetStats()), nil
}

// SetStatMods: the game master's stat mods on a player, replacing the ones
// there. The server refuses a guid that is not one of the character's stats.
func (a *App) SetStatMods(character string, mods []StatModRow) ([]StatRow, error) {
	items := make([]*worldadminv1.StatMod, 0, len(mods))
	for _, m := range mods {
		guid, err := strconv.ParseInt(m.Stat, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("the stat guid %q is not a number: %w", m.Stat, err)
		}
		items = append(items, &worldadminv1.StatMod{Stat: guid, Value: m.Value})
	}
	out, err := call(a, func(ctx context.Context) (*worldadminv1.SetStatModsResponse, error) {
		return a.client.admin.SetStatMods(ctx, &worldadminv1.SetStatModsRequest{Character: character, Mods: items})
	})
	if err != nil {
		return nil, err
	}
	return statRows(out.GetStats()), nil
}

// ClearStatMods: the overrides are removed.
func (a *App) ClearStatMods(character string) ([]StatRow, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.ClearStatModsResponse, error) {
		return a.client.admin.ClearStatMods(ctx, &worldadminv1.ClearStatModsRequest{Character: character})
	})
	if err != nil {
		return nil, err
	}
	return statRows(out.GetStats()), nil
}

// SetCurrentStat: the current value of a stat the player holds (Health,
// Mana, Stamina). The server refuses a value above the stat's maximum; it
// does not cut it down.
func (a *App) SetCurrentStat(character, stat string, value float64) (float64, error) {
	guid, err := strconv.ParseInt(stat, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("the stat guid %q is not a number: %w", stat, err)
	}
	out, err := call(a, func(ctx context.Context) (*worldadminv1.SetCurrentStatResponse, error) {
		return a.client.admin.SetCurrentStat(ctx, &worldadminv1.SetCurrentStatRequest{
			Character: character,
			Stat:      guid,
			Value:     value,
		})
	})
	if err != nil {
		return 0, err
	}
	return out.GetValue(), nil
}

// GiveMoney: copper into the player's purse. The answer is the balance.
func (a *App) GiveMoney(character string, copper int64) (int64, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.GiveMoneyResponse, error) {
		return a.client.admin.GiveMoney(ctx, &worldadminv1.GiveMoneyRequest{Character: character, Copper: copper})
	})
	if err != nil {
		return 0, err
	}
	return out.GetBalance(), nil
}

// GiveItem: an item into the player's bags. The answer is the item's name.
func (a *App) GiveItem(character, item string, count int32) (string, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.GiveItemResponse, error) {
		return a.client.admin.GiveItem(ctx, &worldadminv1.GiveItemRequest{
			Character: character,
			Item:      item,
			Count:     count,
		})
	})
	if err != nil {
		return "", err
	}
	return out.GetItem(), nil
}

// ItemName: one item of the game's catalog: the record's name (what the
// server takes) and what it is called (what the game master reads).
type ItemName struct {
	Key  string `json:"key"`
	Text string `json:"text"`
}

// ItemNames: the items the extracted game data knows, so the window can offer
// them to click instead of asking for a record name. The record's name is what
// the server's GiveItem takes (the UI names in item_names.tsv are not).
func (a *App) ItemNames() []ItemName {
	root := repoRoot()
	if root == "" {
		return []ItemName{}
	}
	db, err := content.Open(filepath.Join(root, "dist", "content", "content.db"))
	if err != nil {
		return []ItemName{}
	}
	defer db.Close()

	var typeID int64
	if err := db.SQL().QueryRow("SELECT type_id FROM record_table WHERE schema = 'ItemRecordBase'").Scan(&typeID); err != nil {
		return []ItemName{}
	}
	rows, err := db.SQL().Query(
		"SELECT name FROM record WHERE type_id = ? AND name <> '' AND name NOT LIKE 'NOTUSED%' ORDER BY name", typeID)
	if err != nil {
		return []ItemName{}
	}
	defer rows.Close()

	out := make([]ItemName, 0, 4096)
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			continue
		}
		out = append(out, ItemName{Key: name, Text: name})
	}
	return out
}

// Teleport: the player to a place (by its name) or to a point.
func (a *App) Teleport(character, place string, x, y, z float64, hasZ bool) (string, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.TeleportResponse, error) {
		req := &worldadminv1.TeleportRequest{Character: character}
		if place != "" {
			req.Target = &worldadminv1.TeleportRequest_Place{Place: place}
		} else {
			req.Target = &worldadminv1.TeleportRequest_Point{
				Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: hasZ},
			}
		}
		return a.client.admin.Teleport(ctx, req)
	})
	if err != nil {
		return "", err
	}
	return out.GetCharacter(), nil
}

// SetSpeed: the player's run speed (0.5 to 10; 1 puts it back). The answer is
// the multiplier that is now in force.
func (a *App) SetSpeed(character string, multiplier float64) (float64, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.SetMoveSpeedResponse, error) {
		return a.client.admin.SetMoveSpeed(ctx, &worldadminv1.SetMoveSpeedRequest{
			Character:  character,
			Multiplier: multiplier,
		})
	})
	if err != nil {
		return 0, err
	}
	return out.GetMultiplier(), nil
}

// NpcAction: kill, respawn, reset, heal, despawn or move a creature. The
// answer is the sentence the server says about what it did.
func (a *App) NpcAction(action string, id uint64, x, y, z float64, hasZ bool) (string, error) {
	out, err := call(a, func(ctx context.Context) (string, error) {
		switch action {
		case "kill":
			r, err := a.client.admin.KillNpc(ctx, &worldadminv1.KillNpcRequest{Id: id})
			if err != nil {
				return "", err
			}
			return r.GetResult(), nil
		case "respawn":
			r, err := a.client.admin.RespawnNpc(ctx, &worldadminv1.RespawnNpcRequest{Id: id})
			if err != nil {
				return "", err
			}
			return r.GetResult(), nil
		case "reset":
			r, err := a.client.admin.ResetNpc(ctx, &worldadminv1.ResetNpcRequest{Id: id})
			if err != nil {
				return "", err
			}
			return r.GetResult(), nil
		case "heal":
			r, err := a.client.admin.HealNpc(ctx, &worldadminv1.HealNpcRequest{Id: id})
			if err != nil {
				return "", err
			}
			return r.GetResult(), nil
		case "despawn":
			r, err := a.client.admin.DespawnNpc(ctx, &worldadminv1.DespawnNpcRequest{Id: id})
			if err != nil {
				return "", err
			}
			return r.GetResult(), nil
		case "move":
			r, err := a.client.admin.MoveNpc(ctx, &worldadminv1.MoveNpcRequest{
				Id:    id,
				Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: hasZ},
			})
			if err != nil {
				return "", err
			}
			return r.GetResult(), nil
		}
		return "", fmt.Errorf("ما فيه أمر %q", action)
	})
	if err != nil {
		return "", err
	}
	return out, nil
}

// EntityRow: one entity of the world stream's search, in the shape the
// window's table wants.
type EntityRow struct {
	ID        uint64  `json:"id"`
	Name      string  `json:"name"`
	Kind      string  `json:"kind"`
	Level     uint32  `json:"level"`
	HealthPct uint32  `json:"health_pct"` // 0..100; 255: the health is not tracked
	X         float64 `json:"x"`
	Y         float64 `json:"y"`
	Z         float64 `json:"z"`
}

// SearchEntities: the players, creatures and resource nodes whose name holds
// the query (the stream wants two characters at least).
func (a *App) SearchEntities(query string, limit uint32) ([]EntityRow, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.SearchResponse, error) {
		return a.client.stream.Search(ctx, &worldadminv1.SearchRequest{Query: query, Limit: limit})
	})
	if err != nil {
		return nil, err
	}
	rows := make([]EntityRow, 0, len(out.GetResults()))
	for _, r := range out.GetResults() {
		e := r.GetEntity()
		rows = append(rows, EntityRow{
			ID:        e.GetId(),
			Name:      r.GetName(),
			Kind:      entityKind(e.GetKind()),
			Level:     e.GetLevel(),
			HealthPct: e.GetHealthPct(),
			X:         float64(e.GetXCm()),
			Y:         float64(e.GetYCm()),
			Z:         float64(e.GetZCm()),
		})
	}
	return rows, nil
}

// EntityDetail: what the world stream says about one entity.
type EntityDetail struct {
	ID        uint64  `json:"id"`
	Name      string  `json:"name"`
	Kind      string  `json:"kind"`
	Level     uint32  `json:"level"`
	Health    float64 `json:"health"`
	MaxHealth float64 `json:"max_health"`
	AIState   string  `json:"ai_state"`
	Archetype string  `json:"archetype"`
	Source    string  `json:"source"`
	Corpse    bool    `json:"corpse"`
	RespawnIn float64 `json:"respawn_in_seconds"`
	Home      string  `json:"home"`
	Text      string  `json:"text"`
}

// InspectEntity: the state of one entity, as worldd knows it now.
func (a *App) InspectEntity(id uint64) (*EntityDetail, error) {
	out, err := call(a, func(ctx context.Context) (*worldadminv1.InspectResponse, error) {
		return a.client.stream.Inspect(ctx, &worldadminv1.InspectRequest{Id: id})
	})
	if err != nil {
		return nil, err
	}
	e := out.GetEntity()
	detail := &EntityDetail{
		ID:    e.GetId(),
		Name:  e.GetName(),
		Kind:  entityKind(e.GetKind()),
		Level: e.GetLevel(),
	}
	switch d := out.GetDetail().(type) {
	case *worldadminv1.InspectResponse_Actor:
		detail.Name = d.Actor.GetName()
		detail.Level = d.Actor.GetLevel()
		detail.Archetype = d.Actor.GetArchetype()
		detail.Source = d.Actor.GetSource()
		detail.Health = d.Actor.GetHealth()
		detail.MaxHealth = d.Actor.GetMaxHealth()
		detail.AIState = d.Actor.GetAiState()
		detail.Corpse = d.Actor.GetCorpse()
		detail.RespawnIn = d.Actor.GetRespawnInSeconds()
		detail.Home = fmt.Sprintf("%d, %d, %d", d.Actor.GetHomeXCm(), d.Actor.GetHomeYCm(), d.Actor.GetHomeZCm())
		detail.Text = d.Actor.GetArchetype()
	case *worldadminv1.InspectResponse_Player:
		detail.Name = d.Player.GetName()
		detail.Level = d.Player.GetLevel()
		detail.Text = "لاعب"
	}
	return detail, nil
}

// entityKind: the kind of an entity, in words.
func entityKind(k worldadminv1.EntityKind) string {
	switch k {
	case worldadminv1.EntityKind_ENTITY_KIND_PLAYER:
		return "لاعب"
	case worldadminv1.EntityKind_ENTITY_KIND_NPC:
		return "مخلوق"
	case worldadminv1.EntityKind_ENTITY_KIND_RESOURCE_NODE:
		return "مورد"
	case worldadminv1.EntityKind_ENTITY_KIND_MOUNT:
		return "مركبة"
	case worldadminv1.EntityKind_ENTITY_KIND_LOOT_PILE:
		return "غنيمة"
	}
	return "غير معروف"
}

// CityRow: one city node, in the shape the window's table wants.
type CityRow struct {
	Guid      int64  `json:"guid"`
	Name      string `json:"name"`
	Display   string `json:"display_name"`
	Level     uint32 `json:"level"`
	LevelName string `json:"level_name"`
	XP        int64  `json:"xp"`
	Percent   uint32 `json:"percent"`
	Clock     string `json:"clock"` // the node clock's time
}

// Nodes: the city nodes and the node clock.
func (a *App) Nodes() ([]CityRow, error) {
	out, err := call(a, func(ctx context.Context) (*citynodev1.ListNodesResponse, error) {
		return a.client.citynode.ListNodes(ctx, &citynodev1.ListNodesRequest{})
	})
	if err != nil {
		return nil, err
	}
	clock := nodeClock(out.GetClock())
	rows := make([]CityRow, 0, len(out.GetNodes()))
	for _, n := range out.GetNodes() {
		rows = append(rows, CityRow{
			Guid:      n.GetGuid(),
			Name:      n.GetName(),
			Display:   n.GetDisplayName(),
			Level:     n.GetLevel(),
			LevelName: n.GetLevelName(),
			XP:        n.GetExperience(),
			Percent:   n.GetPercent(),
			Clock:     clock,
		})
	}
	return rows, nil
}

// NodeXp: adds XP to a node (a negative amount takes XP away).
func (a *App) NodeXp(node string, amount int64) (string, error) {
	out, err := call(a, func(ctx context.Context) (*citynodev1.AddNodeExperienceResponse, error) {
		return a.client.citynode.AddNodeExperience(ctx, &citynodev1.AddNodeExperienceRequest{
			Node:   &citynodev1.NodeRef{Name: node},
			Amount: amount,
		})
	})
	if err != nil {
		return "", err
	}
	return fmt.Sprintf("%s: %d خبرة", out.GetNode().GetDisplayName(), out.GetNode().GetExperience()), nil
}

// NodeClock: skips the node clock ahead (a game master's skip). The clock is
// the one of every node.
func (a *App) NodeClock(seconds int64) (string, error) {
	out, err := call(a, func(ctx context.Context) (*citynodev1.AdvanceClockResponse, error) {
		return a.client.citynode.AdvanceClock(ctx, &citynodev1.AdvanceClockRequest{Seconds: seconds})
	})
	if err != nil {
		return "", err
	}
	return nodeClock(out.GetClock()), nil
}

// nodeClock: the node clock's time, in words.
func nodeClock(c *citynodev1.Clock) string {
	if c == nil {
		return ""
	}
	return time.UnixMilli(c.GetNodeUnixMs()).Format("2006-01-02 15:04:05")
}

// LogTail: the last lines of a service's log (the ones under
// dist\services\logs).
func (a *App) LogTail(name string, lines int) []string {
	if lines <= 0 {
		lines = 60
	}
	root := repoRoot()
	if root == "" {
		return []string{}
	}
	raw, err := os.ReadFile(filepath.Join(root, "dist", "services", "logs", filepath.Base(name)))
	if err != nil {
		return []string{}
	}
	all := strings.Split(strings.TrimRight(string(raw), "\n"), "\n")
	if len(all) > lines {
		all = all[len(all)-lines:]
	}
	return all
}

// StartStack: starts the server stack (build\start-stack.ps1), so the window
// can bring it up when it is down.
func (a *App) StartStack() (string, error) {
	root := repoRoot()
	if root == "" {
		return "", errors.New("ما لقيت جذر المستودع (scripts\\services.ps1)")
	}
	script := filepath.Join(root, "build", "start-stack.ps1")
	cmd := exec.Command("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("the stack did not start: %w: %s", err, string(out))
	}
	return "شغّلنا الستاك", nil
}

// tokenFile: the service token's path. The client knows the environment
// variable and the file beside the exe; when neither is there, the token is
// the one scripts\services.ps1 writes: dist\services\service.token under
// the checkout's root.
func tokenFile() string {
	if v := os.Getenv("AOC_SERVICE_TOKEN_FILE"); v != "" {
		return v
	}
	if exe, err := os.Executable(); err == nil {
		beside := filepath.Join(filepath.Dir(exe), "service.token")
		if _, err := os.Stat(beside); err == nil {
			return beside
		}
	}
	if root := repoRoot(); root != "" {
		p := filepath.Join(root, "dist", "services", "service.token")
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return defaultTokenFile()
}

// repoRoot: the checkout's root: the nearest ancestor of the exe (or of the
// working directory) that holds the stack's scripts. The dashboard finds the
// stack that way, so it works from any working directory.
func repoRoot() string {
	starts := make([]string, 0, 2)
	if exe, err := os.Executable(); err == nil {
		starts = append(starts, filepath.Dir(exe))
	}
	if cwd, err := os.Getwd(); err == nil {
		starts = append(starts, cwd)
	}
	for _, start := range starts {
		dir := start
		for i := 0; i < 6; i++ {
			if _, err := os.Stat(filepath.Join(dir, "scripts", "services.ps1")); err == nil {
				return dir
			}
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	return ""
}
