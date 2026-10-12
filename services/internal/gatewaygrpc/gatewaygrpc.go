// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package gatewaygrpc serves aoc.gateway.v1.GatewayService: what world
// servers ask the gateway.
package gatewaygrpc

import (
	"context"
	"errors"
	"log/slog"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
	"aocrop/services/internal/tickets"
)

// Tickets is the part of tickets.Store the service uses.
type Tickets interface {
	Redeem(ctx context.Context, ticket, characterID string, serverID uint32) (int64, error)
}

var _ Tickets = (*tickets.Store)(nil)

// Server implements gatewayv1.GatewayServiceServer.
type Server struct {
	gatewayv1.UnimplementedGatewayServiceServer
	tickets Tickets
}

func New(t Tickets) *Server { return &Server{tickets: t} }

// RedeemTicket spends a join ticket. PermissionDenied for any ticket that is
// not valid (the reason is not given); Canceled or DeadlineExceeded when the
// caller's own context ended; Internal, with the details logged, otherwise.
func (s *Server) RedeemTicket(ctx context.Context, r *gatewayv1.RedeemTicketRequest) (*gatewayv1.RedeemTicketResponse, error) {
	accountID, err := s.tickets.Redeem(ctx, r.GetTicket(), r.GetCharacterId(), r.GetServerId())
	if err == nil {
		slog.Info("join ticket redeemed", "account_id", accountID, "character_id", r.GetCharacterId(), "server_id", r.GetServerId())
		return &gatewayv1.RedeemTicketResponse{AccountId: accountID, CharacterId: r.GetCharacterId()}, nil
	}
	if errors.Is(err, tickets.ErrInvalid) {
		slog.Info("join ticket refused", refusedCharacter(r.GetCharacterId()), "server_id", r.GetServerId())
		return nil, status.Error(codes.PermissionDenied, tickets.ErrInvalid.Error())
	}
	if ctxErr := ctx.Err(); ctxErr != nil {
		return nil, status.FromContextError(ctxErr).Err()
	}
	slog.Error("redeeming a join ticket failed", "err", err)
	return nil, status.Error(codes.Internal, "redeeming the join ticket failed")
}

// [CoA x C.Y.B]

// refusedCharacter is the log attribute for the character id of a refused
// request. World servers forward whatever an unauthenticated UDP client sent,
// so the id is logged only when it is well-formed (32 lowercase hex digits, as
// in tickets); anything else is logged by its length alone.
func refusedCharacter(id string) slog.Attr {
	if isHex32(id) {
		return slog.String("character_id", id)
	}
	return slog.Int("character_id_len", len(id))
}

func isHex32(s string) bool {
	if len(s) != 32 {
		return false
	}
	for i := 0; i < len(s); i++ {
		if c := s[i]; (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			return false
		}
	}
	return true
}
