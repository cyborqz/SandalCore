// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package config reads service configuration from AOC_* environment
// variables, with defaults for a single-machine development setup.
package config

import (
	"os"
	"time"
)

type Config struct {
	DatabaseURL       string        // AOC_DATABASE_URL
	AuthHTTPAddr      string        // AOC_AUTH_HTTP_ADDR: the launcher's login API
	AuthGRPCAddr      string        // AOC_AUTH_GRPC_ADDR: internal token validation
	CharacterGRPCAddr string        // AOC_CHARACTER_GRPC_ADDR
	CityNodeGRPCAddr  string        // AOC_CITYNODE_GRPC_ADDR: the city node service (Nodes 1)
	CityNodeSeed      string        // AOC_CITYNODE_SEED: the node seed file citynode applies once (default deploy/citynode_seed.json)
	WorldAdminAddr    string        // AOC_WORLD_ADMIN_ADDR: worldd's admin API (worldctl, the admin app to come); worldd listens here, default 127.0.0.1:50056
	TokenTTL          time.Duration // AOC_TOKEN_TTL, e.g. "24h"
	ServiceTokenFile  string        // AOC_SERVICE_TOKEN_FILE: the internal services' shared token (internal/svcauth)
	ContentDB         string        // AOC_CONTENT_DB: the content.db contentc builds; the character service checks new characters against it
	GMStarterSet      bool          // AOC_GM_STARTER_SET=1: new characters get the GM starter set, existing ones once at a start (plan D1)

	MeshGRPCAddr     string        // AOC_MESH_GRPC_ADDR
	MeshStaticWorld  string        // AOC_MESH_STATIC_WORLD: a world server that never registers (the legacy aoc_server); "" for none
	MeshHeartbeatTTL time.Duration // AOC_MESH_HEARTBEAT_TTL: a world server silent this long is not placed

	GatewayICSAddr    string        // AOC_GATEWAY_ICS_ADDR: the game client's ICS gRPC, over TLS
	GatewayGRPCAddr   string        // AOC_GATEWAY_GRPC_ADDR: internal (RedeemTicket), on loopback unless GatewayGRPCPublic
	GatewayGRPCPublic bool          // AOC_GATEWAY_GRPC_PUBLIC=1: allow any GatewayGRPCAddr (the ticket crosses the network in clear text)
	GatewayTLSCert    string        // AOC_GATEWAY_TLS_CERT: PEM certificate for the ICS listener
	GatewayTLSKey     string        // AOC_GATEWAY_TLS_KEY: its private key
	TicketTTL         time.Duration // AOC_TICKET_TTL: how long a join ticket stays redeemable
}

func Load() Config {
	return Config{
		DatabaseURL:       str("AOC_DATABASE_URL", "postgres://aoc:aoc@127.0.0.1:5432/aoc?sslmode=disable"),
		AuthHTTPAddr:      str("AOC_AUTH_HTTP_ADDR", "127.0.0.1:8081"),
		AuthGRPCAddr:      str("AOC_AUTH_GRPC_ADDR", "127.0.0.1:50051"),
		CharacterGRPCAddr: str("AOC_CHARACTER_GRPC_ADDR", "127.0.0.1:50052"),
		CityNodeGRPCAddr:  str("AOC_CITYNODE_GRPC_ADDR", "127.0.0.1:50055"),
		CityNodeSeed:      str("AOC_CITYNODE_SEED", "deploy/citynode_seed.json"),
		WorldAdminAddr:    str("AOC_WORLD_ADMIN_ADDR", "127.0.0.1:50056"),
		TokenTTL:          duration("AOC_TOKEN_TTL", 24*time.Hour),
		ServiceTokenFile:  str("AOC_SERVICE_TOKEN_FILE", "dist/services/service.token"),
		ContentDB:         str("AOC_CONTENT_DB", "dist/content/content.db"),
		GMStarterSet:      os.Getenv("AOC_GM_STARTER_SET") == "1",

		MeshGRPCAddr:     str("AOC_MESH_GRPC_ADDR", "127.0.0.1:50053"),
		MeshStaticWorld:  str("AOC_MESH_STATIC_WORLD", ""),
		MeshHeartbeatTTL: duration("AOC_MESH_HEARTBEAT_TTL", 15*time.Second),

		GatewayICSAddr:    str("AOC_GATEWAY_ICS_ADDR", "127.0.0.1:443"),
		GatewayGRPCAddr:   str("AOC_GATEWAY_GRPC_ADDR", "127.0.0.1:50054"),
		GatewayGRPCPublic: os.Getenv("AOC_GATEWAY_GRPC_PUBLIC") == "1",
		GatewayTLSCert:    str("AOC_GATEWAY_TLS_CERT", "certs/server.pem"),
		GatewayTLSKey:     str("AOC_GATEWAY_TLS_KEY", "certs/server.key"),
		TicketTTL:         duration("AOC_TICKET_TTL", 2*time.Minute),
	}
}

// == C:Y:B | C O A ==

func str(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func duration(key string, def time.Duration) time.Duration {
	if d, err := time.ParseDuration(os.Getenv(key)); err == nil && d > 0 {
		return d
	}
	return def
}
