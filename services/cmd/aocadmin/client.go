// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// client.go: the calls the dashboard makes to the server: the administrative API
// of worldd, the live world stream, the character service and the city nodes.
// Every call carries the service token, the way the other services authenticate
// to each other.
package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/svcauth"
)

// CallTimeout: how long a call waits.
const CallTimeout = 5 * time.Second

// Options: where the services are and where the token is.
type Options struct {
	AdminAddr     string // worldd's admin API (AOC_WORLD_ADMIN_ADDR)
	StreamAddr    string // worldd's world stream (AOC_WORLD_STREAM_ADDR)
	CharacterAddr string // the character service (AOC_CHARACTER_GRPC_ADDR)
	CityNodeAddr  string // the city node service (AOC_CITYNODE_GRPC_ADDR)
	TokenFile     string // the service token (AOC_SERVICE_TOKEN_FILE)
}

// Client: the connections to the server. The calls take the token with
// WithToken, the way the other services authenticate to each other.
type Client struct {
	opts       Options
	adminConn  *grpc.ClientConn
	streamConn *grpc.ClientConn
	charConn   *grpc.ClientConn
	cityConn   *grpc.ClientConn
	admin      worldadminv1.WorldAdminServiceClient
	stream     worldadminv1.WorldStreamServiceClient
	character  characterv1.CharacterServiceClient
	citynode   citynodev1.CityNodeServiceClient
	token      svcauth.Token
}

// NewClient: the client with the defaults the services use.
func NewClient(opts Options) *Client {
	if opts.AdminAddr == "" {
		opts.AdminAddr = envOr("AOC_WORLD_ADMIN_ADDR", "127.0.0.1:50056")
	}
	if opts.StreamAddr == "" {
		opts.StreamAddr = envOr("AOC_WORLD_STREAM_ADDR", "127.0.0.1:50057")
	}
	if opts.CharacterAddr == "" {
		opts.CharacterAddr = envOr("AOC_CHARACTER_GRPC_ADDR", "127.0.0.1:50062")
	}
	if opts.CityNodeAddr == "" {
		opts.CityNodeAddr = envOr("AOC_CITYNODE_GRPC_ADDR", "127.0.0.1:50055")
	}
	if opts.TokenFile == "" {
		opts.TokenFile = envOr("AOC_SERVICE_TOKEN_FILE", defaultTokenFile())
	}
	return &Client{opts: opts}
}

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

// defaultTokenFile: the token scripts\services.ps1 writes: service.token
// beside the dashboard's exe (the script builds every service into
// dist\services), else the services' own default, relative to the current
// directory.
func defaultTokenFile() string {
	if exe, err := os.Executable(); err == nil {
		beside := filepath.Join(filepath.Dir(exe), "service.token")
		if _, err := os.Stat(beside); err == nil {
			return beside
		}
	}
	return "dist/services/service.token"
}

// Dial: reads the token and opens the connections. The context is not used
// yet: gRPC opens the connections lazily, so a call is what finds out whether
// the server is there.
func (c *Client) Dial(ctx context.Context) error {
	token, err := svcauth.Load(c.opts.TokenFile)
	if err != nil {
		return fmt.Errorf("the service token cannot be read: %w", err)
	}
	c.token = token

	dial := func(addr string) (*grpc.ClientConn, error) {
		return grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	}
	if c.adminConn, err = dial(c.opts.AdminAddr); err != nil {
		return fmt.Errorf("the admin API at %s cannot be opened: %w", c.opts.AdminAddr, err)
	}
	if c.streamConn, err = dial(c.opts.StreamAddr); err != nil {
		c.Close()
		return fmt.Errorf("the world stream at %s cannot be opened: %w", c.opts.StreamAddr, err)
	}
	if c.charConn, err = dial(c.opts.CharacterAddr); err != nil {
		c.Close()
		return fmt.Errorf("the character service at %s cannot be opened: %w", c.opts.CharacterAddr, err)
	}
	if c.cityConn, err = dial(c.opts.CityNodeAddr); err != nil {
		c.Close()
		return fmt.Errorf("the city nodes at %s cannot be opened: %w", c.opts.CityNodeAddr, err)
	}
	c.admin = worldadminv1.NewWorldAdminServiceClient(c.adminConn)
	c.stream = worldadminv1.NewWorldStreamServiceClient(c.streamConn)
	c.character = characterv1.NewCharacterServiceClient(c.charConn)
	c.citynode = citynodev1.NewCityNodeServiceClient(c.cityConn)
	return nil
}

// Close: closes the connections.
func (c *Client) Close() {
	for _, conn := range []*grpc.ClientConn{c.adminConn, c.streamConn, c.charConn, c.cityConn} {
		if conn != nil {
			conn.Close()
		}
	}
}

// The addresses, for the window's status line.
func (c *Client) AdminAddr() string       { return c.opts.AdminAddr }
func (c *Client) CharacterAddr() string   { return c.opts.CharacterAddr }
func (c *Client) WorldStreamAddr() string { return c.opts.StreamAddr }
func (c *Client) CityNodeAddr() string    { return c.opts.CityNodeAddr }

// WithToken: the context a call carries the service token in. The internal
// services read it from the "authorization" metadata, as "Bearer <token>"
// (internal/svcauth); the token is the only thing that lets a call in.
func (c *Client) WithToken(ctx context.Context) context.Context {
	md, _ := metadata.FromOutgoingContext(ctx)
	out := md.Copy()
	out.Set(svcauth.MetadataKey, "Bearer "+string(c.token))
	return metadata.NewOutgoingContext(ctx, out)
}
