// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package questsgrpc serves aoc.quest.v1.QuestService over internal/quests.
package questsgrpc

import (
	"context"
	"errors"
	"log/slog"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	questv1 "aocrop/services/gen/aoc/quest/v1"
	"aocrop/services/internal/quests"
)

// Server implements questv1.QuestServiceServer.
type Server struct {
	questv1.UnimplementedQuestServiceServer
	svc *quests.Service
}

// New serves svc.
func New(svc *quests.Service) *Server { return &Server{svc: svc} }

func (s *Server) LoadLog(ctx context.Context, r *questv1.LoadLogRequest) (*questv1.LoadLogResponse, error) {
	log, err := s.svc.LoadLog(ctx, r.GetCharacterId())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &questv1.LoadLogResponse{Log: log}, nil
}

func (s *Server) SaveLog(ctx context.Context, r *questv1.SaveLogRequest) (*questv1.SaveLogResponse, error) {
	if err := s.svc.SaveLog(ctx, r.GetCharacterId(), r.GetLog()); err != nil {
		return nil, toStatus(ctx, err)
	}
	return &questv1.SaveLogResponse{}, nil
}

func (s *Server) QueueCommand(ctx context.Context, r *questv1.QueueCommandRequest) (*questv1.QueueCommandResponse, error) {
	id, err := s.svc.QueueCommand(ctx, r)
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &questv1.QueueCommandResponse{Id: id}, nil
}

func (s *Server) TakeCommands(ctx context.Context, r *questv1.TakeCommandsRequest) (*questv1.TakeCommandsResponse, error) {
	cmds, err := s.svc.TakeCommands(ctx, r.GetCharacterId())
	if err != nil {
		return nil, toStatus(ctx, err)
	}
	return &questv1.TakeCommandsResponse{Commands: cmds}, nil
}

// toStatus maps a service error to the gRPC code a caller can act on.
func toStatus(ctx context.Context, err error) error {
	switch {
	case errors.Is(err, quests.ErrNotFound):
		return status.Error(codes.NotFound, err.Error())
	case errors.Is(err, quests.ErrInvalid):
		return status.Error(codes.InvalidArgument, err.Error())
	}
	if ctxErr := ctx.Err(); ctxErr != nil {
		return status.FromContextError(ctxErr).Err()
	}
	m, _ := grpc.Method(ctx) // empty when the handler is called without a server
	slog.Error("quests failed", "method", m, "err", err)
	return status.Error(codes.Internal, "quests failed")
}
