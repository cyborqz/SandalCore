// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"strconv"
	"strings"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/svcauth"
)

// watch is the reference consumer of worldd's world stream (WorldStreamService): it subscribes to an area, keeps the entities the frames
// describe, checks the frames' sequence and, once a second, prints one line: the frames that came, what they changed, how many entities it
// holds, and the cost of a publish that worldd reports in GetStatus. A frame out of sequence makes it subscribe again, as the stream's
// contract asks of any reader.
//
//	worldctl watch [--area minx,miny,maxx,maxy] [--density [--cell 50000]] [--layers all|players,npcs,nodes,mounts,loot] [--seconds N]

const watchUsage = `usage: worldctl watch [flags]
  --area minx,miny,maxx,maxy   the area, in cm (default: 100 km square around the origin; at most 100 km on a side)
  --density                    counts per cell instead of single entities (--cell 20000, 50000 or 100000 cm)
  --layers all|players,npcs,nodes,mounts,loot
  --seconds N                  stop after N seconds (default: until Ctrl+C)
  --addr, --token-file`

// layerBits are StreamLayer's numbers.
var layerBits = map[string]uint32{"players": 1, "npcs": 2, "nodes": 4, "mounts": 8, "loot": 16}

func parseArea(s string) (*worldadminv1.AreaBox, error) {
	parts := strings.Split(s, ",")
	if len(parts) != 4 {
		return nil, errors.New("--area wants four numbers: minx,miny,maxx,maxy (cm)")
	}
	var v [4]int32
	for i, p := range parts {
		n, err := strconv.ParseInt(strings.TrimSpace(p), 10, 32)
		if err != nil {
			return nil, fmt.Errorf("--area: %q is not a whole number of cm", p)
		}
		v[i] = int32(n)
	}
	return &worldadminv1.AreaBox{MinXCm: v[0], MinYCm: v[1], MaxXCm: v[2], MaxYCm: v[3]}, nil
}

func parseLayers(s string) (uint32, error) {
	if s == "" || s == "all" {
		return 0, nil // 0 asks for every layer
	}
	var bits uint32
	for _, name := range strings.Split(s, ",") {
		b, ok := layerBits[strings.ToLower(strings.TrimSpace(name))]
		if !ok {
			return 0, fmt.Errorf("--layers: %q is none of players, npcs, nodes, mounts, loot", name)
		}
		bits |= b
	}
	return bits, nil
}

// explainWatch turns a stream error into a sentence.
func explainWatch(err error, addr string) string {
	if s, ok := status.FromError(err); ok {
		switch s.Code() {
		case codes.Unavailable:
			return fmt.Sprintf("worldd's world stream is not reachable at %s (is worldd running, and not started with --no-world-stream?): %s", addr, s.Message())
		case codes.Unauthenticated:
			return "refused: the service token is wrong (use the service.token worldd was started with)"
		case codes.InvalidArgument, codes.ResourceExhausted:
			return "refused: " + s.Message()
		}
		return fmt.Sprintf("%s: %s", strings.ToLower(s.Code().String()), s.Message())
	}
	return err.Error()
}

// tally is what the frames of one second added up to.
type tally struct {
	frames, snapshots, deltas, densities, changed, removed, bytes int
}

type watcher struct {
	addr   string
	client worldadminv1.WorldStreamServiceClient
	req    *worldadminv1.SubscribeRequest
	out    io.Writer

	mu       sync.Mutex
	t        tally
	entities map[uint64]*worldadminv1.Entity // what the frames have described so far
	grid     *worldadminv1.DensityGrid       // the newest density frame
	resubs   int
	total    int // frames since the start, whatever second they came in
}

// apply takes one frame into the picture.
func (w *watcher) apply(resp *worldadminv1.SubscribeResponse) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.t.frames++
	w.total++
	w.t.bytes += proto.Size(resp)
	switch body := resp.GetBody().(type) {
	case *worldadminv1.SubscribeResponse_Snapshot:
		w.t.snapshots++
		w.entities = make(map[uint64]*worldadminv1.Entity, len(body.Snapshot.GetEntities()))
		for _, e := range body.Snapshot.GetEntities() {
			w.entities[e.GetId()] = e
			w.t.changed++
		}
	case *worldadminv1.SubscribeResponse_Delta:
		w.t.deltas++
		for _, e := range body.Delta.GetChanged() {
			if old, ok := w.entities[e.GetId()]; ok && e.GetName() == "" {
				e.Name = old.GetName() // a name comes once, in the frame in which the player first appears
			}
			w.entities[e.GetId()] = e
			w.t.changed++
		}
		for _, id := range body.Delta.GetRemoved() {
			delete(w.entities, id)
			w.t.removed++
		}
	case *worldadminv1.SubscribeResponse_Density:
		w.t.densities++
		w.grid = body.Density
	}
}

// subscribe reads one stream until it ends. A frame out of sequence ends it with errGap.
var errGap = errors.New("a frame is out of sequence")

func (w *watcher) subscribe(ctx context.Context) error {
	stream, err := w.client.Subscribe(ctx, w.req)
	if err != nil {
		return err
	}
	expect := uint64(1)
	for {
		resp, err := stream.Recv()
		if err != nil {
			return err
		}
		if resp.GetSeq() != expect {
			fmt.Fprintf(w.out, "frame %d came where %d was due: subscribing again\n", resp.GetSeq(), expect)
			return errGap
		}
		expect++
		w.apply(resp)
	}
}

// follow keeps a stream open: it subscribes again after a gap or when worldd goes away for a moment, and gives up on anything else.
func (w *watcher) follow(ctx context.Context) error {
	for ctx.Err() == nil {
		err := w.subscribe(ctx)
		switch {
		case ctx.Err() != nil:
			return nil
		case errors.Is(err, errGap):
		case status.Code(err) == codes.Unavailable || errors.Is(err, io.EOF):
			fmt.Fprintf(w.out, "the stream was lost (%s): subscribing again\n", explainWatch(err, w.addr))
			select {
			case <-ctx.Done():
				return nil
			case <-time.After(time.Second):
			}
		default:
			return err
		}
		w.mu.Lock()
		w.resubs++
		w.mu.Unlock()
	}
	return nil
}

// line prints the second that has passed.
func (w *watcher) line(ctx context.Context) {
	w.mu.Lock()
	t := w.t
	w.t = tally{}
	held := len(w.entities)
	var byKind [6]int
	for _, e := range w.entities {
		if k := int(e.GetKind()); k > 0 && k < len(byKind) {
			byKind[k]++
		}
	}
	grid := w.grid
	resubs := w.resubs
	w.mu.Unlock()

	var what string
	if w.req.GetDetail() == worldadminv1.StreamDetail_STREAM_DETAIL_DENSITY {
		var players, npcs, nodes uint32
		var cells int
		if grid != nil {
			cells = int(grid.GetCols()) * int(grid.GetRows())
			for _, v := range grid.GetPlayers() {
				players += v
			}
			for _, v := range grid.GetNpcs() {
				npcs += v
			}
			for _, v := range grid.GetResourceNodes() {
				nodes += v
			}
		}
		what = fmt.Sprintf("grid %d cells: players %d npcs %d nodes %d", cells, players, npcs, nodes)
	} else {
		what = fmt.Sprintf("changed %d removed %d held %d (players %d npcs %d nodes %d mounts %d piles %d)", t.changed, t.removed, held, byKind[1], byKind[2], byKind[3], byKind[4], byKind[5])
	}
	line := fmt.Sprintf("%s  frames %d (snapshots %d deltas %d density %d)  %s  %.1f KB/s", time.Now().Format("15:04:05"), t.frames, t.snapshots, t.deltas, t.densities, what, float64(t.bytes)/1024)
	if resubs > 0 {
		line += fmt.Sprintf("  resubscribed %d", resubs)
	}
	sctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	if st, err := w.client.GetStatus(sctx, &worldadminv1.GetStatusRequest{}); err == nil {
		s := st.GetStats()
		line += fmt.Sprintf("  | publish %.3f ms (worst %.3f) loop %.2f ms | worldd: players %d npcs %d nodes %d mounts %d | streams %d", s.GetPublishMs(), s.GetPublishMaxMs(), s.GetTickMs(),
			s.GetPlayers(), s.GetNpcs(), s.GetResourceNodes(), s.GetMounts(), st.GetSubscribers())
	} else if ctx.Err() == nil {
		line += "  | status: " + explainWatch(err, w.addr)
	}
	fmt.Fprintln(w.out, line)
}

// runWatch is `worldctl watch`: it returns the exit status.
func runWatch(ctx context.Context, args []string, out, errw io.Writer) int {
	fs := flag.NewFlagSet("worldctl watch", flag.ContinueOnError)
	fs.SetOutput(errw)
	cfg := config.Load()
	addr := fs.String("addr", cfg.WorldStreamAddr, "worldd's world stream address")
	tokenFile := fs.String("token-file", defaultTokenFile(cfg.ServiceTokenFile), "the service token file")
	area := fs.String("area", "-5000000,-5000000,5000000,5000000", "the area in cm: minx,miny,maxx,maxy")
	density := fs.Bool("density", false, "counts per cell instead of single entities")
	cell := fs.Uint("cell", 50000, "with --density: the cell size in cm (20000, 50000 or 100000)")
	layers := fs.String("layers", "all", "all, or a comma list of players, npcs, nodes, mounts, loot")
	seconds := fs.Int("seconds", 0, "stop after this many seconds (0: until Ctrl+C)")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintf(errw, "worldctl watch: takes no arguments\n%s\n", watchUsage)
		return 2
	}
	box, err := parseArea(*area)
	if err != nil {
		fmt.Fprintf(errw, "worldctl watch: %v\n", err)
		return 2
	}
	bits, err := parseLayers(*layers)
	if err != nil {
		fmt.Fprintf(errw, "worldctl watch: %v\n", err)
		return 2
	}
	if *seconds < 0 {
		fmt.Fprintln(errw, "worldctl watch: --seconds cannot be negative")
		return 2
	}
	req := &worldadminv1.SubscribeRequest{Area: box, Layers: bits}
	if *density {
		req.Detail = worldadminv1.StreamDetail_STREAM_DETAIL_DENSITY
		req.DensityCellCm = uint32(*cell)
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

	if *seconds > 0 {
		var stop context.CancelFunc
		ctx, stop = context.WithTimeout(ctx, time.Duration(*seconds)*time.Second)
		defer stop()
	}
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	w := &watcher{addr: *addr, client: worldadminv1.NewWorldStreamServiceClient(cc), req: req, out: out, entities: map[uint64]*worldadminv1.Entity{}}
	failed := make(chan error, 1)
	go func() { failed <- w.follow(ctx) }()
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			w.mu.Lock()
			got := w.total
			w.mu.Unlock()
			if got == 0 {
				fmt.Fprintf(errw, "worldctl watch: no frame came from %s\n", *addr)
				return 1
			}
			return 0
		case err := <-failed:
			if err != nil {
				fmt.Fprintf(errw, "worldctl watch: %s\n", explainWatch(err, *addr))
				return 1
			}
			return 0
		case <-tick.C:
			w.line(ctx)
		}
	}
}
