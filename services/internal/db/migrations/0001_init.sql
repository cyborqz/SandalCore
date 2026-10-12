-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
CREATE TABLE accounts (
    id            BIGSERIAL PRIMARY KEY,
    username      TEXT        NOT NULL CONSTRAINT accounts_username_not_empty CHECK (username <> ''),
    password_hash TEXT        NOT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX accounts_username_lower ON accounts (lower(username));

-- Launcher login tokens; only their SHA-256 is stored.
CREATE TABLE login_tokens (
    token_hash BYTEA       PRIMARY KEY CONSTRAINT login_tokens_hash_length CHECK (octet_length(token_hash) = 32),
    account_id BIGINT      NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL
);
CREATE INDEX login_tokens_account ON login_tokens (account_id);
CREATE INDEX login_tokens_expires ON login_tokens (expires_at);

CREATE TABLE characters (
    id           TEXT PRIMARY KEY CHECK (id ~ '^[0-9a-f]{32}$'),  -- the client's 128-bit GUID as hex
    account_id   BIGINT      NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    name         TEXT        NOT NULL CONSTRAINT characters_name_not_empty CHECK (name <> ''),
    race_id      INT         NOT NULL,
    gender_id    INT         NOT NULL,
    archetype_id INT         NOT NULL,             -- class archetype record id
    class_name   TEXT        NOT NULL,
    appearance   JSONB       NOT NULL DEFAULT '{}',
    create_info  BYTEA,                            -- the client's creation payload, as sent
    level        INT         NOT NULL DEFAULT 1 CONSTRAINT characters_level_positive CHECK (level >= 1),
    pos_x        DOUBLE PRECISION,                 -- NULL until first save: default spawn
    pos_y        DOUBLE PRECISION,
    pos_z        DOUBLE PRECISION,
    yaw          REAL,
    last_region  TEXT        NOT NULL DEFAULT '',
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT characters_position_all_or_nothing
        CHECK ((pos_x IS NULL) = (pos_y IS NULL) AND (pos_x IS NULL) = (pos_z IS NULL))
);
CREATE UNIQUE INDEX characters_name_lower ON characters (lower(name));
CREATE INDEX characters_account ON characters (account_id, created_at);
