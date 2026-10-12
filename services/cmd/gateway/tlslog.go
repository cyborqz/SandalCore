// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"log/slog"
	"net"

	"google.golang.org/grpc/credentials"
)

// loggedCreds logs failed TLS handshakes, which grpc-go reports only at its
// own INFO level (off by default): a client that offers no ALPN h2 (refused
// since grpc-go 1.67), speaks an old TLS version or rejects the certificate
// otherwise leaves no trace.
type loggedCreds struct {
	credentials.TransportCredentials
}

func (c loggedCreds) ServerHandshake(conn net.Conn) (net.Conn, credentials.AuthInfo, error) {
	nc, info, err := c.TransportCredentials.ServerHandshake(conn)
	if err != nil {
		slog.Warn("ics: TLS handshake failed", "remote", conn.RemoteAddr().String(), "err", err)
	}
	return nc, info, err
}

func (c loggedCreds) Clone() credentials.TransportCredentials {
	return loggedCreds{c.TransportCredentials.Clone()}
}
