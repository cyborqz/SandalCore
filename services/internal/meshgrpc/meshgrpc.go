// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package meshgrpc serves aoc.mesh.v1.MeshService.
package meshgrpc

import (
	"context"
	"errors"
	"log/slog"
	"time"

	"google.golang.org/genproto/googleapis/rpc/errdetails"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	"aocrop/services/internal/mesh"
)

// Store is the part of mesh.Store the service uses.
type Store interface {
	Register(ctx context.Context, address string, regions []string) (uint32, error)
	Heartbeat(ctx context.Context, id uint32) error
	Place(ctx context.Context, characterID string) (uint32, string, error)
	HeartbeatTTL() time.Duration
}

var _ Store = (*mesh.Store)(nil)

// Server implements meshv1.MeshServiceServer.
type Server struct {
	meshv1.UnimplementedMeshServiceServer
	store Store
}

func New(store Store) *Server { return &Server{store: store} }

// [C-O-A ~ Cyb]

// The google.rpc.ErrorInfo that Place's Unavailable carries when no world
// server is live. grpc-go answers Unavailable too when mesh is not listening:
// only this detail says "no world server".
const (
	ErrorDomain         = "mesh.aoc"
	ReasonNoWorldServer = "NO_WORLD_SERVER"
)

// IsNoWorldServer reports whether err is mesh's answer that no world server is
// live, and not some other Unavailable.
func IsNoWorldServer(err error) bool {
	st, ok := status.FromError(err)
	if !ok || st.Code() != codes.Unavailable {
		return false
	}
	for _, d := range st.Details() {
		if info, ok := d.(*errdetails.ErrorInfo); ok && info.GetReason() == ReasonNoWorldServer && info.GetDomain() == ErrorDomain {
			return true
		}
	}
	return false
}

// noWorldServer is the Unavailable status for err (mesh.ErrNoServer), with its
// ErrorInfo.
func noWorldServer(err error) error {
	st, derr := status.New(codes.Unavailable, err.Error()).WithDetails(
		&errdetails.ErrorInfo{Reason: ReasonNoWorldServer, Domain: ErrorDomain})
	if derr != nil { // only for an OK code or a detail that does not marshal
		return status.Error(codes.Unavailable, err.Error())
	}
	return st.Err()
}

func (s *Server) Register(ctx context.Context, r *meshv1.RegisterRequest) (*meshv1.RegisterResponse, error) {
	id, err := s.store.Register(ctx, r.GetAddress(), r.GetRegions())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	slog.Info("world server registered", "server_id", id, "address", r.GetAddress())
	// Three heartbeats fit in the TTL, so one lost heartbeat does not unplace
	// the server. Never 0: a worldd told 0 would heartbeat in a busy loop.
	interval := max(s.store.HeartbeatTTL()/3, time.Millisecond)
	return &meshv1.RegisterResponse{ServerId: id, HeartbeatIntervalMs: interval.Milliseconds()}, nil
}

func (s *Server) Heartbeat(ctx context.Context, r *meshv1.HeartbeatRequest) (*meshv1.HeartbeatResponse, error) {
	if err := s.store.Heartbeat(ctx, r.GetServerId()); err != nil {
		if errors.Is(err, mesh.ErrUnknownServer) {
			slog.Warn("heartbeat from an unregistered server", "server_id", r.GetServerId())
		}
		return nil, toStatus(ctx, err)
	}
	return &meshv1.HeartbeatResponse{}, nil
}

func (s *Server) Place(ctx context.Context, r *meshv1.PlaceRequest) (*meshv1.PlaceResponse, error) {
	id, address, err := s.store.Place(ctx, r.GetCharacterId())
	if err != nil {
		if errors.Is(err, mesh.ErrNoServer) {
			slog.Warn("no live world server to place on")
		}
		return nil, toStatus(ctx, err)
	}
	return &meshv1.PlaceResponse{ServerId: id, Address: address}, nil
}

// toStatus maps store errors to gRPC codes, in this order:
//   - The mesh errors describe the request and pass through with their text;
//     ErrNoServer also with its ErrorInfo.
//   - Otherwise, when the RPC's own context has ended, its code wins (judged by
//     ctx, not by what err wraps: a database timeout wraps DeadlineExceeded
//     too, and its text names the database).
//   - Anything else is logged here and reaches the caller only as a generic
//     Internal.
func toStatus(ctx context.Context, err error) error {
	switch {
	case errors.Is(err, mesh.ErrInvalid):
		return status.Error(codes.InvalidArgument, err.Error())
	case errors.Is(err, mesh.ErrUnknownServer):
		return status.Error(codes.NotFound, err.Error())
	case errors.Is(err, mesh.ErrNoServer):
		return noWorldServer(err)
	}
	if ctxErr := ctx.Err(); ctxErr != nil {
		return status.FromContextError(ctxErr).Err()
	}
	m, _ := grpc.Method(ctx)
	slog.Error("mesh store failed", "method", m, "err", err)
	return status.Error(codes.Internal, "mesh store failed")
}
