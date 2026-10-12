// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package config

import (
	"testing"
	"time"
)

func TestDefaults(t *testing.T) {
	// Clear all AOC_* env vars so test is hermetic and doesn't fail if developer has them set
	for _, k := range []string{"AOC_DATABASE_URL", "AOC_AUTH_HTTP_ADDR", "AOC_AUTH_GRPC_ADDR", "AOC_CHARACTER_GRPC_ADDR", "AOC_TOKEN_TTL"} {
		t.Setenv(k, "")
	}
	c := Load()
	if c.DatabaseURL != "postgres://aoc:aoc@127.0.0.1:5432/aoc?sslmode=disable" {
		t.Errorf("DatabaseURL = %q", c.DatabaseURL)
	}
	if c.AuthHTTPAddr != "127.0.0.1:8081" || c.AuthGRPCAddr != "127.0.0.1:50051" ||
		c.CharacterGRPCAddr != "127.0.0.1:50052" {
		t.Errorf("addresses = %q %q %q", c.AuthHTTPAddr, c.AuthGRPCAddr, c.CharacterGRPCAddr)
	}
	if c.TokenTTL != 24*time.Hour {
		t.Errorf("TokenTTL = %v", c.TokenTTL)
	}
}

func TestContentDB(t *testing.T) {
	t.Setenv("AOC_CONTENT_DB", "")
	if got := Load().ContentDB; got != "dist/content/content.db" {
		t.Errorf("ContentDB = %q", got)
	}
	t.Setenv("AOC_CONTENT_DB", `elsewhere/content.db`)
	if got := Load().ContentDB; got != `elsewhere/content.db` {
		t.Errorf("ContentDB = %q", got)
	}
}

func TestOverrides(t *testing.T) {
	t.Setenv("AOC_DATABASE_URL", "postgres://x")
	t.Setenv("AOC_AUTH_HTTP_ADDR", "0.0.0.0:9000")
	t.Setenv("AOC_AUTH_GRPC_ADDR", "0.0.0.0:9001")
	t.Setenv("AOC_CHARACTER_GRPC_ADDR", "0.0.0.0:9002")
	t.Setenv("AOC_TOKEN_TTL", "90m")
	c := Load()
	if c.DatabaseURL != "postgres://x" || c.AuthHTTPAddr != "0.0.0.0:9000" || c.AuthGRPCAddr != "0.0.0.0:9001" ||
		c.CharacterGRPCAddr != "0.0.0.0:9002" || c.TokenTTL != 90*time.Minute {
		t.Errorf("got %+v", c)
	}
}

func TestBadDurationFallsBack(t *testing.T) {
	tests := []string{"soon", "0s", "-5m"} // Invalid (non-parseable), zero, and negative durations
	for _, val := range tests {
		t.Run(val, func(t *testing.T) {
			t.Setenv("AOC_TOKEN_TTL", val)
			if got := Load().TokenTTL; got != 24*time.Hour {
				t.Errorf("TokenTTL = %v, want 24h", got)
			}
		})
	}
}

// The A3 settings: mesh, gateway and join tickets.
var a3Keys = []string{
	"AOC_MESH_GRPC_ADDR", "AOC_MESH_STATIC_WORLD", "AOC_MESH_HEARTBEAT_TTL",
	"AOC_GATEWAY_ICS_ADDR", "AOC_GATEWAY_GRPC_ADDR", "AOC_GATEWAY_TLS_CERT", "AOC_GATEWAY_TLS_KEY",
	"AOC_TICKET_TTL",
}

// CoA with C Y B

func TestGatewayAndMeshDefaults(t *testing.T) {
	for _, k := range a3Keys {
		t.Setenv(k, "")
	}
	c := Load()
	if c.MeshGRPCAddr != "127.0.0.1:50053" || c.MeshStaticWorld != "" || c.MeshHeartbeatTTL != 15*time.Second {
		t.Errorf("mesh = %q %q %v", c.MeshGRPCAddr, c.MeshStaticWorld, c.MeshHeartbeatTTL)
	}
	if c.GatewayICSAddr != "127.0.0.1:443" || c.GatewayGRPCAddr != "127.0.0.1:50054" {
		t.Errorf("gateway addresses = %q %q", c.GatewayICSAddr, c.GatewayGRPCAddr)
	}
	if c.GatewayTLSCert != "certs/server.pem" || c.GatewayTLSKey != "certs/server.key" {
		t.Errorf("gateway TLS = %q %q", c.GatewayTLSCert, c.GatewayTLSKey)
	}
	if c.TicketTTL != 2*time.Minute {
		t.Errorf("TicketTTL = %v", c.TicketTTL)
	}
}

func TestServiceTokenFile(t *testing.T) {
	t.Setenv("AOC_SERVICE_TOKEN_FILE", "")
	if got := Load().ServiceTokenFile; got != "dist/services/service.token" {
		t.Errorf("default ServiceTokenFile = %q", got)
	}
	t.Setenv("AOC_SERVICE_TOKEN_FILE", "x/token")
	if got := Load().ServiceTokenFile; got != "x/token" {
		t.Errorf("ServiceTokenFile = %q, want x/token", got)
	}
}

func TestGatewayAndMeshOverrides(t *testing.T) {
	values := map[string]string{
		"AOC_MESH_GRPC_ADDR": "0.0.0.0:9003", "AOC_MESH_STATIC_WORLD": "127.0.0.1:7777", "AOC_MESH_HEARTBEAT_TTL": "30s",
		"AOC_GATEWAY_ICS_ADDR": "0.0.0.0:9443", "AOC_GATEWAY_GRPC_ADDR": "0.0.0.0:9004",
		"AOC_GATEWAY_TLS_CERT": "x.pem", "AOC_GATEWAY_TLS_KEY": "x.key", "AOC_TICKET_TTL": "5m",
	}
	for k, v := range values {
		t.Setenv(k, v)
	}
	c := Load()
	if c.MeshGRPCAddr != "0.0.0.0:9003" || c.MeshStaticWorld != "127.0.0.1:7777" || c.MeshHeartbeatTTL != 30*time.Second ||
		c.GatewayICSAddr != "0.0.0.0:9443" || c.GatewayGRPCAddr != "0.0.0.0:9004" ||
		c.GatewayTLSCert != "x.pem" || c.GatewayTLSKey != "x.key" || c.TicketTTL != 5*time.Minute {
		t.Errorf("got %+v", c)
	}
}

func TestBadA3DurationsFallBack(t *testing.T) {
	for _, val := range []string{"soon", "0s", "-5m"} {
		t.Run(val, func(t *testing.T) {
			t.Setenv("AOC_MESH_HEARTBEAT_TTL", val)
			t.Setenv("AOC_TICKET_TTL", val)
			if c := Load(); c.MeshHeartbeatTTL != 15*time.Second || c.TicketTTL != 2*time.Minute {
				t.Errorf("MeshHeartbeatTTL = %v, TicketTTL = %v", c.MeshHeartbeatTTL, c.TicketTTL)
			}
		})
	}
}

// Only AOC_GATEWAY_GRPC_PUBLIC=1 lets the internal gRPC bind beyond loopback.
func TestGatewayGRPCPublic(t *testing.T) {
	for val, want := range map[string]bool{"": false, "1": true, "0": false, "true": false, "yes": false} {
		t.Setenv("AOC_GATEWAY_GRPC_PUBLIC", val)
		if got := Load().GatewayGRPCPublic; got != want {
			t.Errorf("AOC_GATEWAY_GRPC_PUBLIC=%q: GatewayGRPCPublic = %v, want %v", val, got, want)
		}
	}
}

// Nodes 1: the city node service's address and seed file.
func TestCityNodeSettings(t *testing.T) {
	t.Setenv("AOC_CITYNODE_GRPC_ADDR", "")
	t.Setenv("AOC_CITYNODE_SEED", "")
	if c := Load(); c.CityNodeGRPCAddr != "127.0.0.1:50055" || c.CityNodeSeed != "deploy/citynode_seed.json" {
		t.Errorf("defaults = %q %q", c.CityNodeGRPCAddr, c.CityNodeSeed)
	}
	t.Setenv("AOC_CITYNODE_GRPC_ADDR", "127.0.0.1:9055")
	t.Setenv("AOC_CITYNODE_SEED", `elsewhere/seed.json`)
	if c := Load(); c.CityNodeGRPCAddr != "127.0.0.1:9055" || c.CityNodeSeed != `elsewhere/seed.json` {
		t.Errorf("overrides = %q %q", c.CityNodeGRPCAddr, c.CityNodeSeed)
	}
}

// The GM travel tools: worldd's admin API address (worldctl dials it, worldd listens on it).
func TestWorldAdminSettings(t *testing.T) {
	t.Setenv("AOC_WORLD_ADMIN_ADDR", "")
	if c := Load(); c.WorldAdminAddr != "127.0.0.1:50056" {
		t.Errorf("default = %q", c.WorldAdminAddr)
	}
	t.Setenv("AOC_WORLD_ADMIN_ADDR", "127.0.0.1:9056")
	if c := Load(); c.WorldAdminAddr != "127.0.0.1:9056" {
		t.Errorf("override = %q", c.WorldAdminAddr)
	}
}
