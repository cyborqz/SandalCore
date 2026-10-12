// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package authgrpc serves aoc.auth.v1.AuthService for other services.
package authgrpc

import (
	"context"
	"errors"
	"log/slog"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	"aocrop/services/internal/account"
)

// Tokens is the part of account.Store the service uses.
type Tokens interface {
	ValidateToken(ctx context.Context, token string) (account.Account, error)
}

var _ Tokens = (*account.Store)(nil)

// Server implements authv1.AuthServiceServer.
type Server struct {
	authv1.UnimplementedAuthServiceServer
	tokens Tokens
}

func New(tokens Tokens) *Server { return &Server{tokens: tokens} }

// ValidateToken resolves a launcher login token to its account. Errors:
// Unauthenticated for an unknown or expired token; Canceled or
// DeadlineExceeded when the caller's own context ended; Internal (details
// logged, not returned) for anything else.
func (s *Server) ValidateToken(ctx context.Context, req *authv1.ValidateTokenRequest) (*authv1.ValidateTokenResponse, error) {
	acc, err := s.tokens.ValidateToken(ctx, req.GetToken())
	if err == nil {
		return &authv1.ValidateTokenResponse{AccountId: acc.ID, Username: acc.Username}, nil
	}
	if errors.Is(err, account.ErrInvalidToken) {
		return nil, status.Error(codes.Unauthenticated, "invalid or expired token")
	}
	// Judge by the RPC's own context, not by what err wraps: a store timeout
	// (pgx's connect timeout) wraps DeadlineExceeded too, and its text names
	// the database host and user.
	if ctxErr := ctx.Err(); ctxErr != nil {
		return nil, status.FromContextError(ctxErr).Err()
	}
	slog.Error("token validation failed", "err", err)
	return nil, status.Error(codes.Internal, "token validation failed")
}
