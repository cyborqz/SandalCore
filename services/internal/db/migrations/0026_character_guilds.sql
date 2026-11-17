-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Social, step 9, "the guilds": a guild, its ranks, its members and the cooldown a character has after leaving one.
--
-- FOUR NEW TABLES. guilds holds one row per guild: its GUID, its name, the save version (every save makes it one more, so two sessions cannot write over each
-- other) and the guild's level, experience, capacity and level cap (the design data's Guild_Information_Base and GuildLevelingDef; no perk is bought yet, so
-- level and experience stay at their first values). guild_ranks holds the guild's three ranks (the data's DefaultRanks): the rank's place, its name and its
-- 35 permission booleans as a bit mask, bit 0 the first of the record's order (bInvitePlayer) to bit 34 (bAdjustGuildPoints). guild_members holds one row per
-- member: the character's GUID, the rank's place, when it joined and when it was last seen online (Unix milliseconds; 0: never recorded). A character is in at
-- most one guild (the unique index), and deleting the character removes its membership (ON DELETE CASCADE) while the guild stays for its other members.
-- character_guild_cooldowns holds a character's join cooldown: the time before which it cannot create or join a guild again (the data's
-- GuildCooldownDurationSeconds, 432,000 s, counted from a leave, a kick or a disband).
--
-- Nothing is backfilled here: a character with no guild has no row. worldd reads the guild with the character at the login (GetCharacter) and saves a guild whole
-- after every change (SaveGuild, which checks the version), and a character's cooldown with SaveGuildCooldown. The levels and the class the roster shows are read
-- from the characters table when the guild is read (GuildByCharacter), so an offline member's row is current.
--
-- The version is 0026. 0025 is the PvP change of pull request 23; 0027 to 0029 belong to this lane's migrations and are left free. The runner (internal/db/db.go)
-- skips a version it has recorded, applies every other one in version order and allows gaps.
--
-- This ADDS FOUR TABLES AND ONLY WRITES TO THEM: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented
-- block in services/README.md ("Rolling back migration 0026").
--
-- The caps are the store's (guilds.go): at most 50 members (the data's StartingCapacity; the perks that raise it are not built), three ranks exactly, and a
-- guild name of at most 64 bytes. The permission mask is below 2^35; the rank and level checks keep a row from holding a value the client's struct cannot carry.

SET LOCAL lock_timeout = '10s';

CREATE TABLE guilds (
    id         TEXT PRIMARY KEY
               CONSTRAINT guilds_id_hex CHECK (id ~ '^[0-9a-f]{32}$' AND id <> repeat('0', 32)),
    name       TEXT NOT NULL
               CONSTRAINT guilds_name CHECK (name <> '' AND name = btrim(name) AND length(name) <= 64),
    version    BIGINT NOT NULL DEFAULT 1
               CONSTRAINT guilds_version CHECK (version >= 1),
    level      INTEGER NOT NULL DEFAULT 1
               CONSTRAINT guilds_level CHECK (level >= 1 AND level <= 10),
    experience INTEGER NOT NULL DEFAULT 0
               CONSTRAINT guilds_experience CHECK (experience >= 0),
    capacity   INTEGER NOT NULL DEFAULT 50
               CONSTRAINT guilds_capacity CHECK (capacity >= 1 AND capacity <= 200),
    level_cap  INTEGER NOT NULL DEFAULT 10
               CONSTRAINT guilds_level_cap CHECK (level_cap >= 1 AND level_cap <= 50),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX guilds_name_lower ON guilds (lower(name));

CREATE TABLE guild_ranks (
    guild_id    TEXT     NOT NULL REFERENCES guilds (id) ON DELETE CASCADE,
    rank_index  SMALLINT NOT NULL
                CONSTRAINT guild_ranks_index CHECK (rank_index >= 0 AND rank_index < 3),
    rank_name   TEXT     NOT NULL
                CONSTRAINT guild_ranks_name CHECK (rank_name <> '' AND rank_name = btrim(rank_name) AND length(rank_name) <= 32),
    permissions BIGINT   NOT NULL
                CONSTRAINT guild_ranks_permissions CHECK (permissions >= 0 AND permissions < 34359738368),   -- 2^35
    CONSTRAINT guild_ranks_pkey PRIMARY KEY (guild_id, rank_index)
);

CREATE TABLE guild_members (
    guild_id       TEXT     NOT NULL REFERENCES guilds (id) ON DELETE CASCADE,
    character_id   TEXT     NOT NULL REFERENCES characters (id) ON DELETE CASCADE
                   CONSTRAINT guild_members_character_hex CHECK (character_id ~ '^[0-9a-f]{32}$' AND character_id <> repeat('0', 32)),
    rank_index     SMALLINT NOT NULL
                   CONSTRAINT guild_members_rank CHECK (rank_index >= 0 AND rank_index < 3),
    joined_ms      BIGINT   NOT NULL
                   CONSTRAINT guild_members_joined CHECK (joined_ms >= 0),
    last_online_ms BIGINT   NOT NULL DEFAULT 0
                   CONSTRAINT guild_members_last_online CHECK (last_online_ms >= 0),
    CONSTRAINT guild_members_pkey PRIMARY KEY (guild_id, character_id),
    CONSTRAINT guild_members_one_guild UNIQUE (character_id)
);
CREATE INDEX guild_members_rank ON guild_members (guild_id, rank_index);

CREATE TABLE character_guild_cooldowns (
    character_id TEXT   PRIMARY KEY REFERENCES characters (id) ON DELETE CASCADE,
    guild_id     TEXT
                 CONSTRAINT character_guild_cooldowns_guild_hex CHECK (guild_id IS NULL OR guild_id ~ '^[0-9a-f]{32}$'),
    until_ms     BIGINT NOT NULL
                 CONSTRAINT character_guild_cooldowns_until CHECK (until_ms >= 0)
);

COMMENT ON TABLE guilds IS 'Social, step 9: one row per guild (name, save version, level, experience, capacity and level cap). worldd reads it with the character at the login and saves it whole after every change (SaveGuild).';
COMMENT ON COLUMN guilds.id IS 'The guild''s GUID: 32 lower-case hex digits, not all zero.';
COMMENT ON COLUMN guilds.name IS 'The guild''s name: not empty, trimmed, at most 64 bytes; unique without regard to the case of A to Z.';
COMMENT ON COLUMN guilds.version IS 'The save version: 1 at the create and one more with every SaveGuild, which refuses a save of an older version.';
COMMENT ON COLUMN guilds.level IS 'The guild''s level, 1 to 10 (the design data''s GuildLevelingDef); no experience is earned yet.';
COMMENT ON COLUMN guilds.experience IS 'The guild''s experience, 0 or more; no experience is earned yet.';
COMMENT ON COLUMN guilds.capacity IS 'The most members the guild holds, 1 to 200 (the data''s StartingCapacity 50; the size perks are not bought yet).';
COMMENT ON COLUMN guilds.level_cap IS 'The highest level the guild''s data reaches, 1 to 50 (CurrentLevelCap 10, FinalLevelCap 50).';

COMMENT ON TABLE guild_ranks IS 'Social, step 9: the three ranks of a guild (the design data''s DefaultRanks), each with its 35 permissions as a mask.';
COMMENT ON COLUMN guild_ranks.rank_index IS 'The rank''s place: 0 the leader, 1 an officer, 2 a member.';
COMMENT ON COLUMN guild_ranks.rank_name IS 'The rank''s name ("Guild Leader", "Officer", "Member" in the data): not empty, trimmed, at most 32 bytes.';
COMMENT ON COLUMN guild_ranks.permissions IS 'The 35 permission booleans as a bit mask, bit 0 the first of the record''s order (bInvitePlayer) to bit 34 (bAdjustGuildPoints).';

COMMENT ON TABLE guild_members IS 'Social, step 9: who is in a guild, at which rank, when it joined and when it was last seen online. A character is in at most one guild.';
COMMENT ON COLUMN guild_members.guild_id IS 'The guild. Deleting the guild removes its members.';
COMMENT ON COLUMN guild_members.character_id IS 'The member''s character GUID: 32 lower-case hex digits, not all zero. Deleting the character removes the membership.';
COMMENT ON COLUMN guild_members.rank_index IS 'The member''s rank, its place in guild_ranks (0 leader, 1 officer, 2 member).';
COMMENT ON COLUMN guild_members.joined_ms IS 'When the character joined, Unix milliseconds.';
COMMENT ON COLUMN guild_members.last_online_ms IS 'When the character was last seen online, Unix milliseconds (0: never recorded).';

COMMENT ON TABLE character_guild_cooldowns IS 'Social, step 9: a character''s join cooldown (the design data''s GuildCooldownDurationSeconds after a leave, a kick or a disband).';
COMMENT ON COLUMN character_guild_cooldowns.character_id IS 'The character whose cooldown this is. Deleting the character removes it.';
COMMENT ON COLUMN character_guild_cooldowns.guild_id IS 'The guild the character left, when the caller names it (NULL: not named).';
COMMENT ON COLUMN character_guild_cooldowns.until_ms IS 'The time before which the character cannot create or join a guild, Unix milliseconds.';
