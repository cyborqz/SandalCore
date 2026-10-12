// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package citynodegrpc serves aoc.citynode.v1.CityNodeService over internal/citynode.
package citynodegrpc

import (
	"context"
	"errors"
	"log/slog"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	"aocrop/services/internal/citynode"
)

// Server implements citynodev1.CityNodeServiceServer.
type Server struct {
	citynodev1.UnimplementedCityNodeServiceServer
	svc *citynode.Service
}

// New serves svc.
func New(svc *citynode.Service) *Server { return &Server{svc: svc} }

func (s *Server) ListNodes(ctx context.Context, _ *citynodev1.ListNodesRequest) (*citynodev1.ListNodesResponse, error) {
	snap, err := s.svc.ListNodes(ctx)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.ListNodesResponse{Nodes: toProtoNodes(snap.Nodes), Clock: toProtoClock(snap.Clock), Seq: snap.Seq, MaxNodeLevel: citynode.MaxNodeLevel}, nil
}

func (s *Server) GetNode(ctx context.Context, r *citynodev1.GetNodeRequest) (*citynodev1.GetNodeResponse, error) {
	n, err := s.svc.GetNode(ctx, r.GetNode().GetGuid(), r.GetNode().GetName())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.GetNodeResponse{Node: toProtoNode(n)}, nil
}

func (s *Server) GetClock(ctx context.Context, _ *citynodev1.GetClockRequest) (*citynodev1.GetClockResponse, error) {
	c, err := s.svc.GetClock(ctx)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.GetClockResponse{Clock: toProtoClock(c)}, nil
}

// WatchNodes sends the snapshot, then every change.
func (s *Server) WatchNodes(_ *citynodev1.WatchNodesRequest, stream grpc.ServerStreamingServer[citynodev1.WatchNodesResponse]) error {
	ctx := stream.Context()
	w, err := s.svc.Watch(ctx)
	if err != nil {
		return toStatus(ctx, err)
	}
	defer w.Close()
	snap := w.Snapshot
	if err := stream.Send(&citynodev1.WatchNodesResponse{Seq: snap.Seq, Event: &citynodev1.WatchNodesResponse_Snapshot{
		Snapshot: &citynodev1.Snapshot{Nodes: toProtoNodes(snap.Nodes), Clock: toProtoClock(snap.Clock)}}}); err != nil {
		return err
	}
	for {
		select {
		case <-ctx.Done():
			return status.FromContextError(ctx.Err()).Err()
		case ev, ok := <-w.Events():
			if !ok {
				switch err := w.Err(); {
				case errors.Is(err, citynode.ErrLagged):
					return status.Error(codes.Aborted, "the watcher fell behind the changes: watch again for the whole state")
				case errors.Is(err, citynode.ErrShutdown):
					return status.Error(codes.Unavailable, "the node service is shutting down")
				default:
					return status.Error(codes.Canceled, "watch closed")
				}
			}
			out := &citynodev1.WatchNodesResponse{Seq: ev.Seq}
			switch {
			case ev.Node != nil:
				out.Event = &citynodev1.WatchNodesResponse_NodeChanged{NodeChanged: toProtoChange(*ev.Node)}
			case ev.Clock != nil:
				out.Event = &citynodev1.WatchNodesResponse_ClockChanged{ClockChanged: toProtoClock(*ev.Clock)}
			}
			if err := stream.Send(out); err != nil {
				return err
			}
		}
	}
}

// built with C-O-A and Cyb

func (s *Server) SetNodeLevel(ctx context.Context, r *citynodev1.SetNodeLevelRequest) (*citynodev1.SetNodeLevelResponse, error) {
	n, changed, err := s.svc.SetNodeLevel(ctx, r.GetNode().GetGuid(), r.GetNode().GetName(), int(r.GetLevel()), r.GetDisableVassalship())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.SetNodeLevelResponse{Node: toProtoNode(n), Changed: changed}, nil
}

func (s *Server) AddNodeExperience(ctx context.Context, r *citynodev1.AddNodeExperienceRequest) (*citynodev1.AddNodeExperienceResponse, error) {
	n, err := s.svc.AddNodeExperience(ctx, r.GetNode().GetGuid(), r.GetNode().GetName(), r.GetAmount())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.AddNodeExperienceResponse{Node: toProtoNode(n)}, nil
}

func (s *Server) AdvanceClock(ctx context.Context, r *citynodev1.AdvanceClockRequest) (*citynodev1.AdvanceClockResponse, error) {
	c, err := s.svc.AdvanceClock(ctx, r.GetSeconds())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.AdvanceClockResponse{Clock: toProtoClock(c)}, nil
}

func (s *Server) SetClockSpeed(ctx context.Context, r *citynodev1.SetClockSpeedRequest) (*citynodev1.SetClockSpeedResponse, error) {
	c, err := s.svc.SetClockSpeed(ctx, r.GetFactor())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.SetClockSpeedResponse{Clock: toProtoClock(c)}, nil
}

func (s *Server) ApplySeed(ctx context.Context, r *citynodev1.ApplySeedRequest) (*citynodev1.ApplySeedResponse, error) {
	var body []byte
	if r.GetSeedJson() != "" {
		body = []byte(r.GetSeedJson())
	}
	res, err := s.svc.ApplySeed(ctx, r.GetForce(), body)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &citynodev1.ApplySeedResponse{Applied: res.Applied, Forced: res.Forced, NodesChanged: uint32(res.Changed), AppliedAtMs: res.AppliedAt.UnixMilli()}, nil
}

// toStatus maps the service's errors to gRPC codes. Anything else is logged and reported without its detail.
func toStatus(ctx context.Context, err error) error {
	switch {
	case errors.Is(err, citynode.ErrNotFound):
		return status.Error(codes.NotFound, err.Error())
	case errors.Is(err, citynode.ErrInvalid):
		return status.Error(codes.InvalidArgument, err.Error())
	case errors.Is(err, citynode.ErrShutdown):
		return status.Error(codes.Unavailable, err.Error())
	}
	if ctxErr := ctx.Err(); ctxErr != nil {
		return status.FromContextError(ctxErr).Err()
	}
	m, _ := grpc.Method(ctx) // empty when the handler is called without a server
	slog.Error("citynode failed", "method", m, "err", err)
	return status.Error(codes.Internal, "citynode failed")
}

func toProtoNode(n citynode.Node) *citynodev1.Node {
	return &citynodev1.Node{
		Guid: n.Guid, Name: n.Name, DisplayName: n.DisplayName,
		Level: uint32(n.Level), LevelName: citynode.LevelName(n.Level),
		Experience: n.Experience, Percent: uint32(n.Percent), ExperienceToNextLevel: n.ToNext,
		ParentGuid: n.ParentGuid, UpdatedAtMs: n.UpdatedAt.UnixMilli(),
	}
}

func toProtoNodes(ns []citynode.Node) []*citynodev1.Node {
	out := make([]*citynodev1.Node, 0, len(ns))
	for _, n := range ns {
		out = append(out, toProtoNode(n))
	}
	return out
}

func toProtoClock(c citynode.Clock) *citynodev1.Clock {
	return &citynodev1.Clock{Speed: c.Speed, OffsetSeconds: c.OffsetSeconds, RealUnixMs: c.RealUnixMs, NodeUnixMs: c.NodeUnixMs}
}

var causes = map[citynode.Cause]citynodev1.ChangeCause{
	citynode.CauseSetLevel:      citynodev1.ChangeCause_CHANGE_CAUSE_SET_LEVEL,
	citynode.CauseAddExperience: citynodev1.ChangeCause_CHANGE_CAUSE_ADD_EXPERIENCE,
	citynode.CauseSeed:          citynodev1.ChangeCause_CHANGE_CAUSE_SEED,
}

func toProtoChange(c citynode.NodeChange) *citynodev1.NodeChange {
	return &citynodev1.NodeChange{
		Node: toProtoNode(c.Node), PreviousLevel: uint32(c.PreviousLevel), PreviousExperience: c.PreviousExperience,
		PreviousParentGuid: c.PreviousParentGuid, Cause: causes[c.Cause], VassalshipDisabled: c.VassalshipDisabled,
	}
}
