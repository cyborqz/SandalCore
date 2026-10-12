// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/backoff"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	characterv1 "aocrop/services/gen/aoc/character/v1"
	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/ics"
	"aocrop/services/internal/meshgrpc"
	"aocrop/services/internal/svcauth"
)

// callTimeout bounds each call to another service, so a service that hangs
// does not hold the client's stream forever.
const callTimeout = 5 * time.Second

// accounts is the auth service as ics.Accounts.
type accounts struct{ c authv1.AuthServiceClient }

func (a accounts) ValidateToken(ctx context.Context, token string) (ics.Account, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	r, err := a.c.ValidateToken(ctx, &authv1.ValidateTokenRequest{Token: token})
	if err != nil {
		return ics.Account{}, err
	}
	return ics.Account{ID: r.GetAccountId(), Username: r.GetUsername()}, nil
}

// characters is the character service as ics.Characters.
type characters struct {
	c characterv1.CharacterServiceClient
}

func (c characters) List(ctx context.Context, accountID int64) ([]*characterv1.Character, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	r, err := c.c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: accountID})
	return r.GetCharacters(), err
}

func (c characters) Get(ctx context.Context, id string) (*characterv1.Character, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	r, err := c.c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id})
	return r.GetCharacter(), err
}

func (c characters) Create(ctx context.Context, req *characterv1.CreateCharacterRequest) (*characterv1.Character, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	r, err := c.c.CreateCharacter(ctx, req)
	return r.GetCharacter(), err
}

func (c characters) Delete(ctx context.Context, accountID int64, id string) error {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	_, err := c.c.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{Id: id, AccountId: accountID})
	return err
}

// (C.Y.B | CoA)

// placer is the mesh service as ics.Placer, for which Unavailable means there
// is no world server. grpc-go answers Unavailable too when mesh is not
// listening, so only mesh's own answer (with its ErrorInfo) stays Unavailable;
// any other error is Internal.
type placer struct{ c meshv1.MeshServiceClient }

func (p placer) Place(ctx context.Context, characterID string) (uint32, string, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	r, err := p.c.Place(ctx, &meshv1.PlaceRequest{CharacterId: characterID})
	if err != nil && !meshgrpc.IsNoWorldServer(err) {
		return 0, "", status.Errorf(codes.Internal, "mesh unreachable: %v", err)
	}
	return r.GetServerId(), r.GetAddress(), err
}

// issuer is the ticket store as ics.Tickets, with callTimeout on each Issue
// like every other call: a database that hangs would otherwise hold Play, and
// the stream's keep-alives behind it, without end.
type issuer struct{ t ics.Tickets }

func (i issuer) Issue(ctx context.Context, accountID int64, characterID string, serverID uint32) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	return i.t.Issue(ctx, accountID, characterID, serverID)
}

var (
	_ ics.Accounts   = accounts{}
	_ ics.Characters = characters{}
	_ ics.Placer     = placer{}
	_ ics.Tickets    = issuer{}
)

// serviceConns are the gateway's connections to the other services. They
// connect lazily, so the gateway starts even while one of them is down; its
// requests then fail until it is up.
type serviceConns struct{ auth, character, mesh *grpc.ClientConn }

func dialServices(cfg config.Config, token svcauth.Token) (*serviceConns, error) {
	dial := func(addr string) (*grpc.ClientConn, error) {
		return grpc.NewClient(addr,
			grpc.WithTransportCredentials(insecure.NewCredentials()),
			token.DialOption(),
			// Come back within seconds after a service restarts: gRPC's
			// default backoff grows to 120 s, failing Play all that time.
			// MinConnectTimeout is set as well: left at 0, each attempt gets
			// only the backoff delay (200 ms the first time) to connect and
			// exchange the HTTP/2 preface, and a call in flight would end
			// Unavailable then instead of waiting its callTimeout.
			grpc.WithConnectParams(grpc.ConnectParams{
				MinConnectTimeout: callTimeout,
				Backoff: backoff.Config{
					BaseDelay: 200 * time.Millisecond, Multiplier: 1.6, Jitter: 0.2, MaxDelay: 5 * time.Second,
				},
			}))
	}
	c := &serviceConns{}
	var err error
	if c.auth, err = dial(cfg.AuthGRPCAddr); err != nil {
		return nil, err
	}
	if c.character, err = dial(cfg.CharacterGRPCAddr); err != nil {
		c.close()
		return nil, err
	}
	if c.mesh, err = dial(cfg.MeshGRPCAddr); err != nil {
		c.close()
		return nil, err
	}
	return c, nil
}

func (c *serviceConns) close() {
	for _, cc := range []*grpc.ClientConn{c.auth, c.character, c.mesh} {
		if cc != nil {
			cc.Close()
		}
	}
}
