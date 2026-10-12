-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- World servers registered with mesh. ServerIds start at 1: 0 is what mesh
-- reports for the configured static world (the legacy aoc_server), which
-- never registers, and worldd must not mint NetGUIDs under it.
-- last_heartbeat comes from mesh's clock, which Place compares it against.
CREATE TABLE world_servers (
    server_id      INT         GENERATED ALWAYS AS IDENTITY PRIMARY KEY
                               CONSTRAINT world_servers_id_positive CHECK (server_id > 0),
    address        TEXT        NOT NULL CONSTRAINT world_servers_address_not_empty CHECK (address <> ''),
    regions        TEXT[]      NOT NULL DEFAULT '{}',
    registered_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_heartbeat TIMESTAMPTZ NOT NULL
);
CREATE INDEX world_servers_heartbeat ON world_servers (last_heartbeat);

-- One-time join tickets: issued by the gateway at Play, redeemed by the world
-- server the client joins (RedeemTicket). Tickets are stored as issued, unlike
-- login tokens (only a hash): a ticket is single-use and dead after its two
-- minutes. target_server_id has no foreign key because 0, the static world,
-- has no row. created_at and expires_at come from the gateway's clock, which
-- Redeem and Purge compare against (one gateway writes them).
CREATE TABLE sessions (
    ticket           TEXT        PRIMARY KEY CONSTRAINT sessions_ticket_hex CHECK (ticket ~ '^[0-9a-f]{32}$'),
    account_id       BIGINT      NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    character_id     TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    target_server_id INT         NOT NULL CONSTRAINT sessions_server_id_not_negative CHECK (target_server_id >= 0),
    created_at       TIMESTAMPTZ NOT NULL,
    expires_at       TIMESTAMPTZ NOT NULL,
    used_at          TIMESTAMPTZ,
    CONSTRAINT sessions_expires_after_created CHECK (expires_at > created_at)
);
CREATE INDEX sessions_expires ON sessions (expires_at);
CREATE INDEX sessions_character ON sessions (character_id); -- the character delete cascade
