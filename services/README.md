# Go services

Platform services for the live backend (architecture spec:
`docs/superpowers/specs/2026-09-28-backend-architecture-design.md`).

| Binary | Listens | Role |
|---|---|---|
| `auth` | HTTP `127.0.0.1:8081`, gRPC `127.0.0.1:50051` | The launcher's login/register API; `AuthService.ValidateToken` for other services |
| `character` | gRPC `127.0.0.1:50052` | `CharacterService`: the runtime writer of character records (`charimport` is the offline exception); checks a new character's race, gender, class and customization against `content.db` (below), gives it its class's starting kit and saves its items ("Items and inventory") |
| `citynode` | gRPC `127.0.0.1:50055` | `CityNodeService` (Nodes 1): the live state of the game world's city nodes (level, XP, vassal link), the node clock and the stream of changes; API-first, see "City nodes" below |
| `charimport` | - | Carries the legacy `dist/Release/data/characters.json` into the database (race, gender, class and appearance taken from each character's `create_info`) |
| `mesh` | gRPC `127.0.0.1:50053` | `MeshService`: world-server registry and placement (one world: worldd when it has registered, else the configured static world, the legacy aoc_server on `127.0.0.1:7777`) |
| `gateway` | ICS gRPC over TLS `127.0.0.1:443`, gRPC `127.0.0.1:50054` | The game client's session, world and character screens and Play; `GatewayService.RedeemTicket` for world servers |

The C++ world server, `worldd` (`server/worldd`, UDP `127.0.0.1:7777`), is started by `run -World
worldd`; see `server/worldd/README.md`.

Every internal gRPC server (auth, character, citynode, mesh, and the gateway's internal port) refuses calls
without the service token (`internal/svcauth`): "authorization: Bearer" and 64 hex digits from
`dist\services\service.token`. `run` creates that file once, readable by this user only; the
gateway and worldd send it. The game client's ICS stream does not use it.

## Run

```powershell
scripts\services.ps1 up      # PostgreSQL + NATS in Docker
scripts\services.ps1 build   # build auth, character, citynode, nodectl, worldctl, charimport, mesh and gateway into dist\services
scripts\services.ps1 run     # build, then start auth, character, citynode, mesh and gateway hidden, logging to files
scripts\services.ps1 run -World worldd   # and dist\Release\worldd.exe, with no static world
scripts\services.ps1 stop    # stop those processes (worldd included)
scripts\services.ps1 login   # sign in with config\dev_login.local.json, save the token for tether_server
scripts\services.ps1 down    # stop the containers (the data volume is kept)
```

`run` starts auth, character, citynode, mesh and gateway without windows. Each writes its log (slog, on
stderr) to `dist\services\logs\<name>.log` and anything on stdout to `<name>.out.log`; the previous
run's files are kept as `<name>.prev.log` and `<name>.out.prev.log`. It waits a moment and fails,
naming the service, its exit code and its log, if one has already exited. It refuses to start
while any of the five from `dist\services` are still running: `stop` them first. `stop`
terminates the processes outright (there is no graceful drain). The services keep no state that
outlives them, but the gateway's live client sessions are dropped: a game client connected to it
loses its session. `stop` needs neither Go nor Docker.

`run` (like `-World legacy`) sets `AOC_MESH_STATIC_WORLD=127.0.0.1:7777` unless it is already set,
so mesh places players on the legacy aoc_server. `-World worldd` clears it: worldd registers
itself, and a static world would take players whenever worldd's heartbeats lapse, with tickets
worldd could never redeem.

The whole stack with the game client is `scripts\launch_go.bat [native [quick]]` (`quick` only
works as the second argument): it runs `launch_all.bat` with the Go services in place of
`auth_server` and aoc_server's own ICS (which moves to :8443). `scripts\launch_go.bat worldd` uses
worldd instead of aoc_server (build it first: `scripts\build.ps1 -Target worldd`); `quick` does not
work with it, since only Play in character select issues the join ticket. Plain `scripts\launch_all.bat`
keeps the legacy servers; both first stop any Go services left running from this checkout.
`launch_all.bat` checks that the process on each port it waits for (TCP, and tether_server's UDP
19021) is the expected program running from this checkout. The same program from another checkout
or worktree fails the launch, as does anything else, with `port N is held by <name> (pid P) at
<path>`. Stop the other stack's servers first. When the Go stack fails, it points to the log
files above.

The game client learns its session token from tether_server. `launch_go.bat` starts it with
`--login-token-file`, which makes it hand a game client on this machine (127.0.0.0/8 only) the
token launcher.exe saved at login, `dist\Release\data\ics_auth_token.txt`; without the flag (plain
`launch_all.bat`) it sends the emulator token and never reads the file. `launch_go.bat` deletes
the file from an earlier sign-in first. With the launcher window it then waits, up to 5 minutes,
until you have signed in there and starts the game client itself: do not also press the
launcher's Launch button, or a second client opens. For `quick` launches, which skip the launcher
window, put an account registered through the launcher in `config\dev_login.local.json`
(git-ignored; `launch_all.bat` does not copy `*.local.json` into `dist`):

    {"username": "admin", "password": "..."}

and `scripts\services.ps1 login` (run by `launch_go.bat native quick`) signs in with it. The file
is read as UTF-8, so passwords with non-ASCII characters work.

The gateway serves TLS with `certs\server.pem` and `certs\server.key`, relative to the directory
it runs in (the repo root under `services.ps1`), the certificate `launch_all.bat` makes the client
trust. The key is not in git, so a fresh clone or worktree has none, and the gateway refuses to
start without it (it never falls back to plaintext). It checks the key before it opens the
database. For `launch_go.bat` in a worktree, copy the main checkout's `certs\server.key` into the
worktree's `certs\`: `certs/*.key` is git-ignored, and aoc_server needs the key there too.
`AOC_GATEWAY_TLS_KEY` and `AOC_GATEWAY_TLS_CERT` point the gateway alone elsewhere.

A client whose TLS handshake fails is logged as `WARN ics: TLS handshake failed remote=...
err=...` in `gateway.log`. The gateway's gRPC library requires the client to offer ALPN `h2`. If
the log says `missing selected ALPN property`, set `GRPC_ENFORCE_ALPN_ENABLED=false` for the
gateway: the game client has only been proven against the legacy grpc-c++ server, which does not
enforce ALPN. `services.ps1 run` passes its environment on, so in a console run
`set GRPC_ENFORCE_ALPN_ENABLED=false` and then `scripts\launch_go.bat` from that console.

Until worldd exists (plan A4) the world is the legacy aoc_server, which spawns its own last
`dist\Release\data\characters.json` character, not the one chosen in character select. `dist` is
not in git, so a fresh worktree has no such file: copy the main checkout's to get the same
character in the world.

What the gateway answers the game client:
- Replies echo the request's `system_data`; pushes echo nothing.
- `PlayRequest` is acknowledged at once (status 1), before the character lookup, placement and
  join ticket, as the real server does. On success a status report and the `PlayReply` with the
  world address and ticket follow, 400 ms apart. A failure after the acknowledgement is one
  `PlayReply` with an error code:
  - 59: the character is missing, is not this account's, or was deleted meanwhile;
  - 51: no world server;
  - 22: anything else, a mesh that is not listening included.
- Creating a character answers 24 when its data is over 64 KiB or its name over 64 bytes, or when the
  character service refuses its race, gender, class or customization ("Character creation" below), 47
  when the name is taken, and 48 beyond 16 characters per session. With no usable `content.db` the
  character service refuses every creation, and the client gets an internal error, 22 (the reason is in
  the gateway's and the character service's logs). A refused create is a `CreateCharacterReply` with an
  empty body that echoes the request's tracking tag.
- Deleting a character that is not there answers SUCCESS, like the legacy server.
- `AuthRefreshRequest` checks the login token again: once it has expired (after 24 h) the session
  ends with 40. A session request with a token the auth service does not know is answered 40 as well,
  and any other request on a stream with no session 43.

The status numbers are the game executable's own `IcsStatusCodeEnum` (`proto/ics_common.proto` keeps its
names; `internal/ics/testdata/ics_status_codes_exe.txt` is the dump they are tested against): a result is
0-3 (success 0, acknowledged 1, pending 2, stream finished 3) and an error 10-127 (internal 22, invalid
request 24, authentication failed 40, session check failed 43, existing name 47, character count 48,
play reservation no server 51, character record not found 59, world offline 65, session terminations
100-105). The repo's first numbers for the errors (the 2xx ones) were not the executable's, and the client
left a call that got one waiting for its 10 s expiry: a refused create hung that long, and the next
create said "Cannot set new client state". Only 0 and 1 have been seen answered in play; that an error
number shows the client's dialog at once is what the executable's enum says, not yet a recording.

The legacy servers use the same ports: `auth_server.exe` (started by `scripts\launch_all.bat`)
listens on 8081 like auth, and aoc_server's own ICS on 443 like the gateway. Run one stack or the
other. With the Go auth service, register once through the launcher's Register button.
Then, to keep a legacy character:

```powershell
dist\services\charimport.exe -account <your username>
```

It prints one line per character and a summary (`imported N, already present N, conflicts N,
invalid N`) and never overwrites, so a rerun is safe: a character that is already present is
reported and is not an error (exit 0). It exits 1 when a character conflicts (its id or name
belongs to another character) or is invalid, and when anything else goes wrong (a missing or
malformed file, an unknown account, the database down); an unknown flag exits 2. The file is
checked before the database is touched: that includes each record's own consistency (plan B1; below),
so a character whose `race_name`, `gender_name` or `class_name`, or whose customization JSON, names
another race, gender or class than its `create_info` fields fails the whole file, naming the record.
`charimport` does not apply the creation rules (it carries characters the client already made), except the level: the game's maximum is 25 (`character.MaxLevel`; the store refuses a level above it
on create and on every save), so a record's level above 25 is imported as 25.

`-file` defaults to `dist/Release/data/characters.json` relative to the current directory: run
`charimport` from the repo root, and pass `-file` explicitly from a worktree or anywhere else.

## Character creation: race, gender, class (plan B1)

A character's race, gender and class are the client's own enums (`internal/chardef`), as the game
executable's embedded protobuf descriptors have them: `CreateCharacterInfo {json_blob_map 1, gender 2,
race 3, primary_class 4, starting_location 5}` and `CharacterInformation {level 2, gender 4, race 5,
primary_class 6}`.

| | |
|---|---|
| race (`ECharacterRace`) | 0 Kaelar, 1 Vaelune, 2 Vek, 3 Renkai, 4 Empyrean, 5 Pyrai, 6 Dunir, 7 Nikua; 8 Tulnar is in the enum and is never created |
| gender (`ECharacterGender`) | 0 Male, 1 Female |
| class (`ECharacterClass`) | 1 Ranger, 2 Bard, 3 Cleric, 4 Fighter, 5 Rogue, 6 Summoner, 7 Tank, 8 Mage |

proto3 leaves a zero out of the wire, so a Kaelar male Bard (cyb) sends field 4 = 2 and nothing else. Until
plan B1 the services read field 4 as the *race* and wrote it back at the field the client reads as the
race, with the class's archetype record id there: the "races" 2, 7 and 8 that worldd accepted were the
class ids of a Bard, a Tank and a Mage. Now:

- The gateway reads gender (field 2), race (3) and class (4) from the create payload and passes them, with
  the customization JSON, to `CharacterService.CreateCharacter` (`race_id`, `gender_id`, `class_id`; the
  class name and archetype id are derived from the class).
- The character list and the create reply write them where the client reads them: level at 2, gender at
  4, race at 5, class at 6 of `CharacterInformation` (`internal/aocchar`).
- The store keeps them in the columns `race`, `gender` and `class_id` of `characters`. `race_id` and
  `gender_id` of the first schema are the legacy backup (migration 0003): not read or written any more.
- worldd takes race, gender and class as they are (`server/worldd/README.md`).

**Rules** (`internal/customization`, checked by the character service on every creation). The race is 0-7
(Tulnar has no player appearance in the design data and is refused), the gender 0-1, the class 1-8; the
customization JSON's own `race`, `gender` and `class` names must agree with them (a Kaelar male sends
neither field 2 nor field 3, so a JSON that says Dunir with no field 3 is refused); and the JSON is checked
against `RaceGenderAppearanceMap` (record `PlayerCharacter`) -> `RaceGenderAppearance` ->
`RaceGenderCustomizationDef` and its `CharacterCustomizationEntitlements` in `content.db` for the character's
race and gender: head hair, lip/chin/cheek hair, brows, horns, eye shape, sclera, eye, hair and nail
colours must be 0 or one of that race's options (horns `0x9F65000082A14F49` is "none"), `skinSet` below the
number of skin sets, `presetGuid` 0 or one of its presets, the normalized members in their ranges, each
face morph weight within -1..1 and each body section within 0..1. The morph sliders' own ranges and the races'
section limits are not enforced: the creator's own default Dunir male look breaks them (in game, 2026-10-02).
Entitlements (which options an account has bought) are ignored. All 125 customizations in the community's
retail captures and the real client's own cyb pass (`testdata/retail_customizations.json`).

**What worldd needs** (a character the service accepts must also join). worldd's `customization_from_json`
refuses what the service's ranges and options do not look at, so `Check` also holds the stored text to it:
`randomSeed` and `skinSet` are integers (`1.5`, `1.0`, `1e2` are not) that fit 32 bits; every float member,
the three without a range (`eyelashLength`, the two normal detail strengths) included, is a number a float32
holds (not `1e39`); no member is `null`; the decal data and blend groups are arrays of `null` and of objects
with all their members of the right type (a decal's `slotIndex` an integer of 0-255); the body sections are
the 30 of the client's enum (`chest`, not `Chest`); face morph names are unique once the first letter's case
is folded (`noseBridge` and `NoseBridge` are one replicated morph); no number is past a double (worldd's
parser refuses the whole text for one, in an unknown member too); the text is within 64 KiB as the database
stores it (jsonb text: a space after each `:` and `,`, numbers written out in full), which is what worldd
reads; at most 256 morphs, 256 sections and 100 members nothing reads. A **creator preset** (a non-zero
`presetGuid` that is one of the race's presets) is accepted and stored as `"presetGuid":"0"`
(`customization.Normalize`; `create_info` stays as sent): the creator's JSON carries every member the
preset resolved to, and worldd takes no real preset because the client's preset table (lookup at
`0x14580ddc0`) is not decoded. The one corpus `fixtures/customization_cases.json` (cyb, the 125 retail
customizations and the hostile cases, each with whether the service accepts it and whether worldd builds
it) is run through `Check` and `Normalize` by `internal/customization` and through worldd's
`customization_from_json` by `test_worldd_character_setup`: accepted must imply built, on both sides.

**Without `content.db`.** The character service starts without one and serves everything else (list, get,
delete, saving positions), and refuses every `CreateCharacter` with `FailedPrecondition` ("character
creation is unavailable: the content.db ... cannot be used: build it with contentc ... and restart") rather
than accept a customization it cannot check. It reads the file once, at start: build it (below) and restart
the service. The path is `--content-db`, else `AOC_CONTENT_DB`, else `dist/content/content.db`;
`scripts\services.ps1 run` passes it (the same file worldd gets) and warns when it is missing.

**Migration 0003** (runs when any service starts, like every migration: in its own transaction, under the
advisory lock, with a 10 s `lock_timeout` so a locked table fails the start instead of waiting). It adds
`race`, `gender` and `class_id` (checked: 0-8, 0-1, 1-8) and fills them where they are still NULL: race
and gender from the names the client wrote into `appearance` (`"race":"Kaelar"`, `"gender":"Male"`), the
class from `class_name` when that names a class and otherwise from the old `race_id` (which held the class,
1-8).
**Nothing is lost or guessed**: `race_id` and `gender_id` stay exactly as they were (they are the backup;
their default is now 0), a value is never overwritten (every UPDATE is `WHERE ... IS NULL`, so running the
file again changes nothing, and a row the new services wrote is left alone), and a row whose JSON has no
usable race or gender name keeps NULL there, which the services read as 0 (Kaelar, Male). The log has one
`postgres: migration 0003: ...` line with the counts (info) and one warning per character left without
something, and per character whose `class_name` class differs from what its old `race_id` said (the
`class_name` class is kept). A character the store reads with a NULL race or gender (one of those left
without) is logged with its id the first time it is read. To list what was left: `SELECT name FROM characters WHERE race IS NULL OR gender IS NULL OR class_id IS NULL`.
cyb (a Kaelar male Bard, `race_id` 2) becomes race 0, gender 0, class 2. Tested on `aoc_test` with rows
shaped like the real ones (`internal/db/migration0003_test.go`), including a table held locked (SQLSTATE
55P03, nothing applied, the retry works).

**Rolling back migration 0003** (there is no dual-write: the B1 services write `race`, `gender` and `class_id`
and leave `race_id` and `gender_id` at 0, so a pre-B1 binary reads a character made since as class 0 and
gender 0 until this is run). Stop the services, run it, start the old binaries. The first statement is all a
pre-B1 binary needs; the rest puts the first schema back (the new columns are harmless to leave, and the
migration can be applied again at any time, it only fills what is NULL). It loses the race, which the first
schema never had.

```sql
-- rollback of migration 0003 (plan B1)
BEGIN;
UPDATE characters SET race_id = COALESCE(class_id, 0), gender_id = COALESCE(gender, 0) + 1
WHERE race_id = 0 AND gender_id = 0;
ALTER TABLE characters ALTER COLUMN race_id DROP DEFAULT, ALTER COLUMN gender_id DROP DEFAULT;
ALTER TABLE characters DROP COLUMN race, DROP COLUMN gender, DROP COLUMN class_id;
DELETE FROM schema_migrations WHERE version = 3;
COMMIT;
```

The rows that had `race_id` 0 and `gender_id` 0 are the ones B1 wrote: the old services wrote the class (1-8)
and gender 1 or 2 there, never 0. `internal/db/migration0003_test.go` runs this block, copied from this file,
on `aoc_test`.

## Items and inventory (plan B2)

**The table.** A character's items are the rows of `character_items` (migration 0004: it adds this table, changes nothing else and grants
nothing): one row per item instance, primary key `(character_id, container, slot)`. `container` is Backpack (slots 0-69), Equipment (0-82; the slot
is the client's own `EEquipSlot`: 2 Chest, 15 Primary ...) or Quest (0-99). `item_record_id` is the design database's ItemRecord Guid (`content.db`
`item.guid`). `instance_guid` is the item's own FGuid, 32 lowercase hex digits, unique per character and never all zero. `quantity` is 1 to 1,000,000, `quality` 1
to 200 (an index into the item's quality curve), `durability` a float32 from 0 to 1e9, and `created_at` the creation time. PostgreSQL enforces all of it, whoever writes,
and the rows go with their character (`ON DELETE CASCADE`).

**Who writes.** The character service writes a character's items when it creates the character; from then on worldd is the only writer:
`SaveInventory(id, items)` replaces the character's whole set in one transaction under the character's row lock, so the latest save wins and a half-saved inventory is never read.
It refuses, changing nothing, items that break the rules above, two items in one slot or with one guid, more than 253, and an empty set (a character keeps its items; saving none
would erase its kit; part 2 of the complete character, which can destroy, sell and consume items, must add an explicit "replace with an empty set" before an inventory can become empty):
`InvalidArgument`, and `NotFound` for a character that is not there. `GetCharacter` returns the items only when the request sets `include_items` (worldd's login does;
the gateway's lists do not): the Backpack's by slot, then the Equipment's, then the Quest storage's. The reply of `CreateCharacter` carries the kit it made.

**The starting kit.** A new character starts with its class's `StartingEquipmentRecord` (`internal/kit`, from `content.db`): the `EquippedItems` go to the Equipment in the
slot the record names (an item named for two slots, a Mage's Focus, is two instances) and the `StoredItems` to the Backpack from slot 0. A fresh instance is quantity 1, the item's default
quality, a new random guid, a durability that is the item's MaxDurability stat at that quality rounded half up (11 for a starter armor piece, 12 for the Book, 38 for a tool, 0 for an item with
none) and, for the whole kit, one creation time, a whole second. For the Bard that is exactly the captured original server's 13 items (a test; the eight classes' kits hold
11, 13, 12, 11, 11, 13, 12 and 13 items, Ranger to Mage by class id 1-8, each checked against the rules). The kit is made before anything is stored and stored in the
same transaction as the character: a character exists with its kit or not at all. A class with no kit is an `Internal` error, logged; a `content.db` without the item tables refuses
every creation like any unusable one.

**The backfill** (what happens to the characters that exist). When the character service starts, before it serves, it gives every character that has no items its class's kit
(`GrantMissingKits`). It never touches a character that has any item: each grant is its own transaction, locks the character, checks again that it has none and inserts the
kit, so running it again (every start does) changes nothing and it cannot race a save of worldd's. It is logged: one `info` line per character granted (name, id, class, item count), one `warn` per character skipped
with the reason (`class_id` NULL, or a kit that cannot be made, the same at every start; or a row that stayed locked for 5 s, `lock_timeout`, because a save of worldd's was in flight: the pass goes on with the others and the next start grants it) and a summary line (`characters_without_items`, `granted`, `items`, `skipped`). On the real database that
gives cyb (a Kaelar male Bard) the Bard's 13 items, and every other character without items its class's kit; a character with some items is left alone. Without a usable `content.db` it grants nothing and says
so; `--no-kit-backfill` turns it off. A database error stops the pass, is logged and the service goes on (the next start runs it again). To see what it did:
`SELECT c.name, count(i.*) FROM characters c LEFT JOIN character_items i ON i.character_id = c.id GROUP BY c.name`.

**Rolling back migration 0004.** Nothing in it touches an older table, so the pre-B2 binaries run on the migrated database as they are (they do not know the table; deleting a character deletes its
items). This block removes the table, and with it every item (the kits, the saves), and the record of the version; the migration can be applied again.
`internal/db/migration0004_test.go` runs it, copied from this file.

```sql
-- rollback of migration 0004 (plan B2)
BEGIN;
DROP TABLE character_items;
DELETE FROM schema_migrations WHERE version = 4;
COMMIT;
```

**Content.** contentc version 3 adds `equip_slot`, `item`, `item_equip_slot` and `starting_equipment` to `content.db` (see "Content"). A `content.db` built before B2 has none of them, so after pulling B2 **rebuild it with contentc (`cd services && go run ./cmd/contentc`) and restart both services**: with the old file
the character service cannot make kits (it refuses every creation and skips the backfill with a warning) and **worldd refuses every login** (`ContentDb::open` fails on the missing item tables, like a missing file; see
`server/worldd/README.md`, "Items and inventory"). `internal/kit`'s `TestLoadRefusesAContentDBBuiltBeforeB2` and worldd's `test_worldd_session` pin both.

worldd's side (the inventory at the join, equip, unequip and move, saving) is in `server/worldd/README.md`.

## Progress and the GM starter set (plan D1, stage 1; an uncommitted draft)

**The table.** A character's XP is the rows of `character_experience` (migration 0005: one row per character and leveling type, the design database's
`LevelingTypeDef` Guid, Adventuring 108651085518458; `xp` and `debt` are 0 or more; the rows go with the character). The level stays in `characters.level`
(the character list shows it, worldd joins with it) and worldd keeps it consistent with the Adventuring row when it saves. A character with no row has the XP
of its level's threshold and no debt. The same migration adds `characters.gm_starter_set_granted_at`, NULL until the GM starter set was put into the bag.
Nothing else is touched.

**Who writes.** worldd saves a level, class or XP change at once with `SaveProgress(id, level, class_id, experience)`: one transaction under the character's row
lock; a zero level or class leaves that value, each experience row replaces the character's row of its type; the class's name and archetype id follow the class.
Refused, changing nothing: a level outside 1-25, a class outside 1-8, a leveling type of 0, a negative XP or debt, a type given twice (`InvalidArgument`);
an unknown character is `NotFound`. `GetCharacter` carries the rows with the items (`include_items`).

**The GM starter set** (`AOC_GM_STARTER_SET=1`, exactly this name; off by default; the user's launcher sets it): `GM_Scroll_MakeMeLevel10/15/20/25`, the eight
`GM_Scroll_PrimaryArchetype<Class>` scrolls and the gear scroll of the character's level band (`GM_Scroll_Gear NN ...`: the highest NN at or below the level; when
the band has several, the class's usual kind: Light Magical for Bard, Cleric, Mage and Summoner, Medium Physical for Ranger and Rogue, Heavy Physical for Fighter
and Tank, else the band's first by name; a level-1 character gets `GM_Scroll_Gear 01 (Light ) Starter`), each a fresh instance (`internal/kit` `StarterMaker`).
A new character gets it in its bag's free slots after its kit and is marked; when the service starts it gives it once to every character whose column is NULL
(`GrantMissingStarterSets`: its own transaction per character under the row lock, the column checked again, what does not fit a full bag logged and left out,
the character marked anyway; a NULL class or a row locked for 5 s is skipped for this start, logged). One log line per character and a summary. Without the
variable nothing of this runs and nothing is marked.

**Rolling back migration 0005.** The table and the column go (and with them every XP row and the record of who got the set), the characters are as they were,
and the migration applies again. `internal/db/migration0005_test.go` runs it, copied from this file.

```sql
-- rollback of migration 0005 (plan D1)
BEGIN;
DROP TABLE character_experience;
ALTER TABLE characters DROP COLUMN gm_starter_set_granted_at;
DELETE FROM schema_migrations WHERE version = 5;
COMMIT;
```

**Talents and the action bar (plan D1, stage 2).** Migration 0006 adds `character_skill_ranks` (the ranks a character bought, per skill tree and in the order it bought them; the
ranks a level grants by itself are not stored) and `character_action_bar` (one row per slot, bank 0-19 and slot 0-15, `FActionbarSlotUseData`'s members); both go with the
character, nothing existing is changed, and 0005 is not edited (the play database of stage 1 has it). `SaveProgress` stores them with the level, class and XP: with
`replace_skill_ranks` the character's ranks become exactly the request's `skill_ranks` (a tree not listed has none left), with `replace_action_bar` its bar becomes exactly `action_bar`;
without a flag they are left alone, so the XP save of a level-up cannot wipe a character's talents. `GetCharacter` with `include_items` carries `skill_ranks` and `action_bar`.
A rank of 0, a rank twice in a tree, a tree twice, a bank outside 0-19, a slot outside 0-15, a use type outside 1-9, a slot held twice or an item guid that is not 32 lowercase
hex digits is `InvalidArgument` with nothing changed. A character that switches class keeps each class's ranks under that class's own tree, so they come back with the class.

**Rolling back migration 0006.** The two tables go (and with them every stored rank and bar), the record of version 6 too; 0005's table and the characters are as they were, and the
migration applies again. `internal/db/migration0006_test.go` runs it, copied from this file.

```sql
-- rollback of migration 0006 (plan D1, stage 2)
BEGIN;
DROP TABLE character_skill_ranks;
DROP TABLE character_action_bar;
DELETE FROM schema_migrations WHERE version = 6;
COMMIT;
```

## What a mount item persists (the mounts milestone, stage 1; migration 0007)

A mount item keeps state between sessions: retail's login shows it on all 24 mount items of capture 1198, as the two persisted stats of the item's `FItemInstance` (the mount's Health and Mana as they were when it was last desummoned or died: 3572 and 1 for a level 20
owner's mount) and its `LockExpiration` (Unix seconds; a time in the past means unlocked). A mount that dies locks its item for the lock time the data gives and comes back with the Health it was saved with. `character_items` has three more columns for it:
`persisted_health` and `persisted_mana` (REAL, NULL: the item keeps none; checked 0 to 1e9) and `lock_expiration` (BIGINT, default 0; checked 0 to the end of the year 9999). `character.Item` (and the proto `Item`: `persisted_health` 9, `persisted_mana` 10,
`lock_expiration` 11) carries them; every other item has nil, nil and 0.

**Migration 0007 adds columns only**: no row is rewritten (every existing item reads NULL, NULL and 0: "keeps nothing"), no constraint on the existing columns changes, nothing is dropped, and the user's play database migrates in place when the services start
(the migration's own transaction, under the advisory lock, `lock_timeout` 10 s). Tested on `aoc_test` (`internal/db/migration0007_test.go`): the items of a play database as of 0006 are read back identical after it and after a second run, the column types and defaults,
what the checks refuse, and the rollback below, run as written.

**Rolling back migration 0007.** The three columns and their constraints go (with whatever mount state they hold), the items are as they were and the migration applies again; a pre-0007 binary needs no rollback (it does not select the columns).

```sql
-- rollback of migration 0007 (the mounts milestone)
BEGIN;
ALTER TABLE character_items
    DROP CONSTRAINT character_items_persisted_health,
    DROP CONSTRAINT character_items_persisted_mana,
    DROP CONSTRAINT character_items_lock_expiration,
    DROP COLUMN persisted_health,
    DROP COLUMN persisted_mana,
    DROP COLUMN lock_expiration;
DELETE FROM schema_migrations WHERE version = 7;
COMMIT;
```

**Rolling back migration 0008.** The one column goes (the mounts already in the bags stay as items); the migration applies again.

```sql
-- rollback of migration 0008 (the mounts milestone, stage 3)
BEGIN;
ALTER TABLE characters DROP COLUMN gm_starter_mounts_granted_at;
DELETE FROM schema_migrations WHERE version = 8;
COMMIT;
```

## City nodes (Nodes 1, stage 1; migration 0009)

`citynode` (`cmd/citynode`, `internal/citynode`, `internal/citynodegrpc`, proto `proto/internal/aoc/citynode/v1/citynode.proto`) is the live state of the game world's
city nodes, like retail's CityNodeService. It uses the same database (`AOC_DATABASE_URL`) and the same service token as the character service, listens on
`127.0.0.1:50055` (`AOC_CITYNODE_GRPC_ADDR`) and runs migration 0009 at its start. **It is API-first:** every read and every control of the nodes is a
`CityNodeService` method, and nothing is reachable only from inside worldd. worldd (later stages), `nodectl` and the admin app to come (a 3D viewer that controls
the server) call the same methods.

**The nodes.** The roster is the 46 city nodes the retail game state carries (`internal/citynode/roster.json`: guid, record name, display name; a test checks it
against `content.db`). A node is named by its guid, its record name (`Verra_RVR_Joeva`, what the client's GM commands take), the same without `Verra_`, or its display
name (`Joeva`). A node has a level (0 Wilderness, 1 Crossroads, 2 Encampment, 3 Village, 4 Town; `MaxNodeLevel` is Town, the data's), its XP in total, and a vassal link
(`parent_guid`). The XP thresholds are `CityNodeExpToLevel` from the client's ini, generated into `internal/citynode/constants_gen.go` (see "City node data" below).

**The methods.**
- Reads: `ListNodes` (all 46, the node clock, the sequence number of the last change), `GetNode`, `GetClock`, and `WatchNodes`, a stream: the whole state first, then every
  change in the order it was made. Each message has a `seq`; the first change after the snapshot has the snapshot's `seq + 1` and each next one the one before + 1, so a
  reader sees no gap and no repeat. A reader that falls 1,024 changes behind is cut off with `Aborted` (watch again: the snapshot is the whole state); the service's
  shutdown ends the streams with `Unavailable`.
- Controls: `SetNodeLevel(node, level, disable_vassalship)` (0..4; `InvalidArgument` outside; it changes the level only; the flag is the client's GM flag
  `bDisableVassalshipSystem` and, with no vassal rules in Nodes 1, only travels with the change event), `AddNodeExperience(node, amount)` (stores the XP and moves the
  progress toward the next threshold; **no automatic level-up**: the level changes only by `SetNodeLevel`; a negative amount may not take the total below 0),
  `AdvanceClock(seconds)`, `SetClockSpeed(factor)` and `ApplySeed(force, seed_json)`.

**The node clock** = real time x speed + offset (unix seconds; `city_node_clock`). `AdvanceClock` (1 second to 100 years) grows the offset: a GM's skip. `SetClockSpeed`
(0.001 to 100,000; 1 is real time) is the test servers' speed knob; node time does not jump when the speed changes (the offset is recomputed, to the second). Retail's timers
(dues, builds, elections) read this clock when they come.

**The seed** `deploy/citynode_seed.json` is the fresh start of decision D2: Joeva a Town (4), Halcyon a Village (3) and Joeva's vassal, every node left out Wilderness (0) with
no lord. It is applied once, at the first start of citynode, in one transaction, when `city_node_seed.applied_at` is null; a seed file that is missing or wrong stops the
start and applies nothing. Later edits to the file do nothing until `nodectl seed --force`, which applies it again, overwrites EVERY node's level and parent (a node left out
becomes Wilderness; the XP is not touched), logs a warning and says so. Seeding sets levels the way a GM sets them with `bDisableVassalshipSystem`.

**`nodectl`** (`dist\services\nodectl.exe`; it finds `service.token` beside itself, or `--token-file` / `AOC_SERVICE_TOKEN_FILE`, and the service at `--addr` /
`AOC_CITYNODE_GRPC_ADDR`):

```powershell
dist\services\nodectl.exe list                       # the 46 nodes, their levels, XP, lords, and the node clock
dist\services\nodectl.exe get Joeva
dist\services\nodectl.exe set-level Winstead 2       # 0..4; --disable-vassalship is the client's GM flag
dist\services\nodectl.exe add-xp Winstead 1000000    # stores XP; the level does not change
dist\services\nodectl.exe clock                      # the node clock; "clock advance <seconds>", "clock speed <factor>"
dist\services\nodectl.exe seed                       # applies the seed if none was applied; --force overwrites every level and parent; --file seed.json
dist\services\nodectl.exe watch                      # the state, then every change, until Ctrl+C
```

**Migration 0009 adds three tables and touches nothing else:** `city_node_state` (`node_guid`, `level`, `experience`, `parent_guid`, `updated_at`), `city_node_clock` (one row:
`offset_seconds` and the `speed` knob) and `city_node_seed` (one row: `applied_at`). citynode makes the 46 node rows when it starts (Wilderness, no lord, no XP). Tested on
`aoc_test` (`internal/db/migration0009_test.go`): the characters of a play database as of 0008 are read back identical, the tables, their constraints and the first rows, a
second run, and the rollback below, run as written. The user's play database migrates in place when the services start (the migration's own transaction, under the advisory
lock, `lock_timeout` 10 s).

**Rolling back migration 0009.** The three tables go with whatever node state they hold (the characters are untouched); the migration applies again. Stop citynode first.

```sql
-- rollback of migration 0009 (Nodes 1, stage 1)
BEGIN;
DROP TABLE city_node_state;
DROP TABLE city_node_clock;
DROP TABLE city_node_seed;
DELETE FROM schema_migrations WHERE version = 9;
COMMIT;
```

**Content.** contentc version 5 adds `item_use`, `item_action`, `action_response`, `action_reward`, `reward_table`, `reward_item`, `reward_xp`, `leveling_type`,
`leveling_curve`, `skill_auto_grant` and `skill_rank_stat_mod` (`internal/content/schema.go` says what each holds; `cmd/contentc/progression.go` writes them).
Version 5 is the oldest file this checkout reads (`RequireVersion`, and worldd's `ContentDb::open`): after pulling D1 rebuild `content.db` and restart both services.
The level scrolls' `ExperienceValue` is resolved to its parameters (target level, curve, multiplier stat, margin) after contentc has checked, with `aocexpr`
over the real curve, that the expression is `(curve(target) - curve(level) + debt_ratio * xp_of_level) / max(0.001, 1 + mult) + margin` at every level and several
debts and multipliers; the Weapon rewards of the same scrolls are of another shape and are written with `target_level` 0 (worldd skips them and says so).

Stage 2 of D1 adds the talents to version 5 (it is not merged, so the version stays): `skill_point_type`, `leveling_point` (what reaching each level of a leveling type gives: an
Archetype and a Stamina point for each Adventuring level from 2, a Weapon point for each level of a weapon line from 2), `skill_tree` (every tree with its kind: `class`,
`universal`, `weapon`, `profession` or `other`), `skill_node` (each tree's nodes in the tree's order with the level and the points spent in the tree a node needs, `auto_granted`
and `auto_hotbar`), `skill_node_require` and `skill_node_exclusive` (the skills a node needs, any of them unless `all_required`, and the skills it excludes), `skill_rank` (each skill's
ranks in order with the ability and the passive effect, and for the effect whether it is infinite or its duration: an empty or formula duration is infinite), `skill_rank_cost`
(what a rank costs in points of a type), `skill_rank_stat_mod` (now written for every rank with an infinite effect, not only for the auto-granted ones) and `item_weapon` (the combo
ability and the weapon line of every weapon item). `cmd/contentc/talents.go` writes them; `internal/content/talents.go` reads them (the tests do). A node can be in several trees
(the key is the pair), a node that names neither a skill nor a slot is left out, and a node listed twice in one tree counts once. A node that names no skill but a
`SkillTreeSlot` (`SkillSlotId`: a talent choice, 128 nodes: the Bard's songs, Instrument and masteries, the Rogue's and Tank's picks, the weapon lines' procs and training picks) is
written once per option of the slot: the same `node_guid` and `ord`, `opt` 0, 1 ... and `slot_guid` in `skill_node` (`opt` and `slot_guid` are part of the key), each option's skill with its own
ranks. A requirement that names a slot node is written in `skill_node_require` with `slot_node_guid` (and skill 0): any option of the node that the character owns meets it.

## Stats (plan B3)

The stats of a character are not stored anywhere in the design data: they are a graph of formulas (`internal/statprog`, [README](internal/statprog/README.md)). **contentc version 4**
compiles that graph into a stat program (`stat_program`: 473 stats, 8 classes, 166 programs; 50,205 bytes for the graph alone in format version 1, 70,091 in version 2 with the ability expressions of "Abilities (plan D2)") and writes what an item adds to the stats; worldd runs the program at the join and on
every equip and unequip (`server/worldd/README.md`, "Stats"). The tables, all in `internal/content/schema.go`:

| Table | Rows (the real file) | What |
|---|---|---|
| `stat_program` | 1 | the compiled bundle (`statprog.Compile` + `Encode`, then `Decode` as a check: contentc stops when the bytes do not verify or do not encode back to themselves), its format version, mode, stat and class counts and the body's crc32; `meta` has `statprog.version`, `.mode`, `.bytes`, `.stats` and `.classes` |
| `quality_curve` | 14 | the two `QualityCurve` records: the range of Quality of each of the seven rarities (Common 1-5 ... Artifact 101-200) |
| `stat_block_stat` | 18,395 | the 3,679 `ItemStatBlock`s: per stat the Min and Max at each rarity (an item's value is interpolated in the band its Quality falls in); six stat keys are no `StatTypeDef` row and are kept (the engine ignores such a mod and counts it); `MaxDurability` is among them |
| `item_set`, `set_bonus` | 2,513; 269 | the sets an item belongs to (record order) and each `SetBonus`'s tiers: per tier (the pieces worn) and bonus the stat and its value at each rarity (NULL: none for that rarity, which adds 0) |
| `item_tag` | 16,815 | an item's `GameplayTags` (the formulas ask `EquippedItemInSlotHasTag`) |
| `item.quality_curve_guid` | | the curve an item's Quality is read against (0: none, 244 items) |

contentc checks what the engine needs to hold: every curve an item names exists and has its seven rarities, every stat block and set it names exists, a stat appears once in a block, every number is finite.
`go test ./internal/statprog` proves the tables equal the records: the engine's own reading of the records' JSON (`Gear.Mods`, `Gear.Tags`) and the same rules over the tables give the same mods and tags bit for bit for all
10,568 items at six qualities and both roundings and for 400 random sets of worn items (whole sets, so that the tiers apply), and the stored bundle is exactly what compiling the file's own graph gives.

**Rebuild and refusal.** Version 4 is the oldest `content.db` this checkout reads: `content.DB.RequireVersion` refuses an older file (and one with no `stat_program` row) with "built by contentc version 3, this checkout needs version 4 or later
(the stat program and the item stat tables, plan B3): rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc". The character service calls it when it loads the file (it then refuses every creation and skips the
backfill, as for a missing file), and **worldd refuses every login** with an older file or a damaged stat program (`server/worldd/README.md`). After pulling B3 rebuild `content.db` and restart both services. Going back is safe: a version 4
file only adds tables and one column (`item.quality_curve_guid`), so B2's worldd and character service read it as before.

## Abilities (plan D2, contentc version 6)

The data of the ability pipeline: **contentc version 6** writes every ability of every class and what it reaches (`internal/content/abilities.go` has the tables and the Go readers, `cmd/contentc/abilities.go` the writer), and compiles
their expressions into the stat program (`internal/statprog`, "D2 engine layer"). Plain data, in `ab_*` tables, and expressions, once each in `ab_expr`
(the same text and terms share a row; `program` is the index in `stat_program`'s bundle, `uses_target`, `error`). **The ranks are plan D1's** (`skill_tree`, `skill_node`, `skill_rank` with their costs and `skill_rank_stat_mod`, "Talents and the action bar"): there is
no table of ranks of its own (the first draft had `ab_rank`; it is gone, `buildTalents` runs first and `buildAbilities` seeds its closure from `skill_rank`). A node with a talent choice counts every option, so the Bard has 37 abilities now, not the research's 27 (the
other 10 are the songs of the slot nodes: stage 2):

| Table | Rows | What |
|---|---|---|
| `ab_ability` | 789 (D2: 249; E1 stage 3 adds the 540 NPC abilities of `ability_names.tsv`) | the abilities of the eight class trees (Ranger 26, Bard 37, Cleric 21, Fighter 19, Rogue 25, Summoner 40, Tank 27, Mage 21; the research's 189 without the talent slots' options), the rest the ranks of other trees grant, and Sprint, Dodge and Rest: targeting (`applicable_targets` mask Self 1, Enemy 2, Ally 4, Party 8, Raid 16, `target_requirement`), held behaviour, usability, shared cooldown group, and the expressions for cooldown, charges, cast time, requirement |
| `ab_cost`, `ab_effect_cost` | | what an ability spends: a stat (`amount_expr`, `enabled_expr`, `triggers_cooldown`) and effect stacks (Fortissimo: 5 of Bard_Resonance) |
| `ab_montage` | 389 | per `AnimationIndex` (the position in `AnimMontages`): path, direction, the expression that selects it, and **`net_guid`** (179 when D2 wrote this table, 251 in the version 11 file, `Summon_Mount`'s among them) where a retail capture names exactly one id (`cmd/contentc/montage_ids.tsv`, from the research's captures; a uint64 stored as int64 bits) |
| `ab_ability_hit`, `ab_ability_effect`, `ab_targeting` | | the hits an ability fires directly, through a projectile or through a lingering zone (with the zone's list), the effects it applies, its targeting records |
| `ab_hit`, `ab_hit_effect`, `ab_hit_projectile`, `ab_hit_zone` | 1,296 hits (D2: 697) | hits as plain data: `net_type` (`NET_ServerVerify`: the client verifies it), targets, what it applies (`apply` to targets, `self`), fires and spawns |
| `ab_projectile`, `ab_projectile_hit`, `ab_zone`, `ab_zone_hit` | 195; 58 (D2: 125; 31) | projectiles (speed, range, life time, hits) and lingering zones (life time, tick rate, delay, hits by role) |
| `ab_effect`, `ab_effect_hit`, `ab_mod` | 1,145; 1,409 mods (D2: 898; 852) | effects: duration and tick, stacking, persistence, the hits of each phase; the stat mods of effects and hits with the value as an expression and its terms |
| `ab_expr` | 1,845 (1,308 of the abilities, `meta` `ab.exprs`; 537 of the reward tables, `loot.exprs`; D2: 924) | 1,531 compile to 1,371 programs; the other 314 hold the reason (D2: 638, 573 and 286) (an NPC function: `CalculateDamage`, `GetActiveTarget`, `GetCreatureOwner`, ...) |

What a closure holds: the ranks' abilities (and the passives they grant), Sprint, Dodge and Rest and the Sprint's drain; per ability its own effects, hits, projectiles and zones; per hit the effects it applies, the projectiles it fires and the zones
it spawns; per effect its hits in every phase. Reactions, cooldown modifiers and teleports are stored as guid lists (`extra`) and not followed; the exception is the zones: `ab_ability.extra.LingeringEffectsIds` lists the zones the ability spawns with its cast (a zone with no hit of its own, Mesmerizing's, has no `ab_ability_hit` row, so this is the only link) and `ab_hit.extra.LingeringZonesIds` the zones a hit spawns (the Saga's), which the worldd runtime reads to open a dance's actor. The tables are written in guid order and the expressions numbered as they are met: a build is deterministic
(`TestRealDeterministic` covers the new tables too). `statprog.LoadExprs` reads `ab_expr` and `CompileExprs` compiles it with the stat graph after the rows are committed; `meta` has `ab.*` counts and `statprog.exprs*`.

**Named references.** An expression may name a record instead of numbering it (`$Skill:Bard_Passive_QuickWit_Jest$`, `$SkillTree:Bard$`): the compiler takes a reference as its type and guid, so a named one would compile as the null reference and the
expression could never be true. contentc writes the number form (`$#<type>:<guid>$`) before it stores and compiles an expression, outside comments, for skill trees, skills, effects and abilities (`exprSet.resolve`; a name that is no record, or two, stays as written). One expression
was affected: Jest's Pep hit (`Bard_QuickWit_Jest`), whose requirement the retail Bard met (`TestNamedReferencesAreWrittenAsNumbers`, and the real test that no live named reference is left).

**Format 2 and the refusal.** The stored bundle is format version 2 (the opcodes and the table of expression programs), which a build of this contentc writes and this checkout's engines read. A file built by contentc 4 or 5 has a version 1 bundle
that the new `Decode` and `parse_bundle` refuse (worldd's `PlayerWorld` checks the row's version against `kBundleVersion`); `content.DB.RequireVersion` and worldd's `kMinContentcVersion` are 6, so every reader refuses an older file with the way to rebuild it
(`cd services && go run ./cmd/contentc`). Rebuild `content.db` with this contentc after pulling it.

**Stage 2 (contentc still version 6).** Hits export `extra.HitFX` (OnHitFXId / OnActivateFXId set: the confirmation rule) and `MaxNumEffectsToRemove`; `GetAbilityRecordByName("X")` is rewritten to the ability's reference like a named one; `cmd/contentc/ability_names.tsv` is the name list of the captures' abilities that seeds the closure
(abilities no rank grants but a capture shows: the Summoner's, the weapon combos, consumables' uses). The expression compiler gains the host operations (`OpHost0/1/2`, `format.go`, `compile.go`, `engine.go`), `RandDoubleRange`, the zero-argument `GetAbilityProgressRatio` and the StatMod terms' sibling scope; the stat corpus has the new lines.

## The population and the vendors (plan E1, contentc versions 7 and 8)

**contentc version 7** writes the chain that says which NPC stands where, and the vendors (`internal/content/population.go` has the tables and `PopulationTables`, `cmd/contentc/population.go` the writer), plain data as the design DB holds it; worldd decides what a spawner draws and where (a seed, `server/worldd/spawns/population.h`):

| Table | Rows | What |
|---|---|---|
| `npc_asset`, `npc_asset_loot` | 4,576; 161 | a PopulationAsset: the NPC's blueprint (`asset_ref`) and `archetype_path`, the path of its class default object (`/Game/.../Wolf.Wolf` is `/Game/.../Wolf.Default__Wolf_C`), the name the retail server exports an NPC's archetype by; faction, aggro and leash radius, scale, dialogue root, interact component and definition, appearance, loot tables |
| `npc_asset_set`, `npc_asset_set_member` | 4,290; 5,209 | an AssetSet: weighted assets, in order |
| `npc_instance`, `npc_instance_asset_set`, `npc_instance_loot` | 16,902; 16,864; 569 | a PopulationInstance: the AssetSets it draws from, level range, respawn time, rule and variance, patrol route, ambient behaviour and wander and speed settings, vendor, interact component and response, dialogue, `blacklisted`; and (version 8) `num_waypoints`, `max_angle`, `follow_distance`, `formation`, `idle_anim`, `align_to_slope` |
| `npc_set`, `npc_set_member` | 4,728 | a PopulationSet: instances at local offsets (cm, yaw in degrees) and its PopulationArea (0: none) |
| `npc_world_spawn`, `npc_placement` | | a WorldSpawn: the sets it places, each at an origin and a yaw (15,179 placements; 74,778 absolute points by the offsets turned by the yaw) |
| `npc_area`, `npc_area_circle` | 608 | a PopulationArea: the circles (centre, extent, weight) its set's instances spawn inside (stage 2) |
| `npc_route` | 1,346 | a PathingRoute: the actor's transform and the spline's points (8,609 in all) and, from **version 8**, what a patrol walks: `waypoints` (a JSON array of [x, y, z]: 11,934 in 1,332 routes; 924 routes also carry the spline of the same points, 408 only the waypoints, 14 neither), `pauses` (seconds at each waypoint), `start_closest`, `relative_city_node`, `relative_encounter` (a route a city node or an encounter places) and `pathing_set`; 1,619 instances name a route, 142 of them within a kilometre of the login spot (stage 2 walks them: `server/worldd/npc/`) |
| `vendor_def`, `vendor_def_inventory`, `vendor_inventory`, `vendor_listing` | 95; 469; 278; 9,704 | a VendorDef (its guid is what the server sends the client as `ClientOpenVendor`'s `MerchSetId`: the client draws the window from its own copy of the same data), its inventories (the window's tabs), and each inventory's listings: an item and the offers that price it (an empty list is free) |

The 26 `PTR_ONLY_Vendor_*` defs are the public test realm's vendors: their listings are the GM and PTR items, free, and they are what worldd's GM vendors open (`server/worldd/vendor/gm_vendor.h`). `content.DB.RequireVersion` and worldd's `kMinContentcVersion` are **8** (stage 2 of E1 added the routes' waypoints and the instances' patrol settings: the v7 file has the spline points only, empty for 408 routes): every reader refuses an older file with the way to rebuild it (`cd services && go run ./cmd/contentc`). Rebuild `content.db` with this contentc after pulling it (the file is 303.1 MB).
Tests: `cmd/contentc/population_test.go` (`TestArchetypePath`, `TestSyntheticPopulation`, `TestSyntheticPopulationRefuses`, `TestRealPopulation`: the real file's counts above) and `gear_real_test.go` (version 8: the waypoints and the instances' patrol settings on the synthetic rows, 1,332 routes with waypoints and 924 with both on the real file); `content_test.go` lists the new tables.

## Combat (plan E1, stage 3, contentc version 9)

**contentc version 9** writes what the NPC fights read (`internal/content/combat.go` has the tables and the Go readers, `cmd/contentc/combat.go` the writer):

| Table | Rows | What |
|---|---|---|
| `combat_profile` | 21 | the `DamageProfile` of `DefaultCombat.ini` (`[/Script/GameSystemsPlugin.CombatConfig]`, `GameSettings.Stats`): the key the game's damage formulas read (`Damage`, `CriticalChance`, `BlockChance` ...) and the stat guid it names |
| `combat_element` | 11 | per element tag the stats of its damage, multiplier, mitigation and penetration |
| `relationship`, `alignment_relation` | 25; 303 | the relationship records (`CombatPermissions`: 1 hostile, 2 friendly, 3 neutral) and every alignment's relationship to the others (`CharacterAlignment.RelationshipsIds`) |
| `combat_config` | 9 | the numbers of `GameSettings`: the in-combat timer 6 s, the global damage and healing multipliers, the out-of-range distance, the hit distances |

The file of the client's Config directory is the source when given (`--config`); without it the copy embedded in contentc (`cmd/contentc/default_combat.ini`) is. The stat program is **format version 3** (`internal/statprog/README.md`): the NPC profile classes and the native damage functions. `npc_motion.tsv` (`tools/mapc`) gains the `lists`, `dist`, `elite` and `rewards` columns. Tests: `cmd/contentc/combat_test.go`. **Rebuild `content.db` with this contentc after pulling it**: worldd refuses an older file (no combat tables, a version 2 stat program).

## Rewards (plan E1, stage 3, part 2, contentc version 10)

**contentc version 10** writes what a kill gives (`internal/content/loot.go` has the tables, `cmd/contentc/loot.go` the writer; read by worldd's `combat/rewards.h`):

| Table | Rows | What |
|---|---|---|
| `loot_table` | 8,197 | every `RewardTable` record as `(guid, name, data)`: `data` is JSON, the table's predicate (an expression id: a quest or biome gate that does not compile reads as "no"), its selection algorithm (`All`, `None`, `PureRandom`, `PerElementWeightedRandom`, `PerElementWeightedExpressionRandom`, `RollEachByPercent`), the number of picks, and its entries: a reward (`xp` by leveling type, `items` with their quantities and variations, `currency`) or a sub-table, each with its weight, percentage or weight expression |
| `xp_rule` | 1 | `adventuring_decay`: the expression of the Adventuring experience's decay by the level difference (`Experience_LevelDiff`) |

The expressions compile into the stat program through the shared `exprSet` (`ab_expr`): 1,531 of the 1,845 the abilities and rewards hold, every one a kill reward uses. The compiler learned `Int` (a floor), `GetNPCLevel()` with no argument, `DoesNPCContainTag` (a bool host function), `GetPlayerLootMaxLevel`, `GetPlayerLootAvgLuck`, `GetPlayerLevel` and the `$F:Name$` named formulas (`internal/statprog/README.md`). **Rebuild `content.db` with this contentc after pulling it**: worldd refuses a file older than the version it reads (`MinContentcVersion`: 10 here, 11 with the mounts milestone). Tests: `internal/content` (the tables), `cmd/contentc` (the version table).

## Mounts (the mounts milestone, stage 1, contentc version 11)

**contentc version 11** writes what a summoned mount is (`internal/content/mounts.go` has the tables and the Go readers, `cmd/contentc/mounts.go` the writer; `docs/superpowers/specs/2026-10-07-mounts-design.md` is the design):

| Table | Rows | What |
|---|---|---|
| `mount` | 101 | every `ItemRecord` named `Mount_*` that carries an `Item.Mount.Ground` (51), `.Flying` (46) or `.Aquatic` (4) tag: its Blueprint (`SummonNotifyClassPath`: `/Game/GameBlueprints/Mounts/Mount_Grounded_TerrorBird_0.Mount_Grounded_TerrorBird_0_C`; the package's name is `blueprint`: 75 distinct, several items share one), how the item is used (`activation`: `SummonMount` for the 57 ground, aquatic and two GM flying items, `CastAbility` with `Summon_Mount` for the 44 flyers), and the numbers its text names: the level, the speeds as percentages of the base (`ground_pct` 165 for a terror bird, `flying_pct` 215, `water_pct` 230; 0 where the text names none, as for 22 of the 51 ground items), the Maximum Health percentage (`health_pct`: 500 for 43 flyers, 1000 for the Drake of the Canopy and the two Thundering Bolt Beasts, 300 for two ground items, 50 for the Scorpion Broodling, else 100), a mule and its crate capacity |
| `mount_stat` | 250 | the `Mount_Base` stat list run at an owner of each level 1 to 50 (the entries read `GetCharacterLevel(GetOwner())`): `MaxHealth` (the `Mount_Base_HP` curve: 500 at level 1, 1179 at 10, 3572 at 20, 4875 at 25), `HealthRegenerationRating` and `MitigationRating` (the same curve over 3 HP per Constitution, times 1.5 and 4.0), `CharacterType` 1.1 and `MoveSpeedMult` 1. A mount's own Health is this times the item's `health_pct` / 100 (a Dawnbreaker of a level 25 owner: 24375) |
| `mount_ability` | 34 | the abilities with `bUsableWhileMounted` (33: `Summon_Mount`, the two dismounts, the pet commands, Blink, Charge, Blitz ...) or `bAutoDismount` (12: the rider dismounts first), and `MountAbility_Sprint` (the dash); `role` names `summon`, `dash`, `dismount_elevated` and `dismount_grounded` |
| `mount_effect` | 9 | `Road_T0..T4_Speed_Mount` (infinite, `MoveSpeedMult` +0.10, +0.15, +0.20, +0.25, +0.30), the dash `MountAbility_Sprint` (5 s, 0.35 of the stat: `relative`), `Test_ForceDismount` and `Rogue_Caltrops_Spooked` (`bForceDismount`) and `Mount_Speed_CrateCarrying_Debuff` |

`cmd/contentc/montage_ids.tsv` gains `Summon_Mount` index 0 (the montage `10717609094785283935` that captures 911, 896, 898, 871 and 872 name): `ab_montage.net_guid` is written for 251 montages. The Blueprint of an item and the speeds text say how fast a mount is; the Blueprint's own `MaxRunSpeed` (a ground mount's), `MaxFlySpeed` and `MaxSwimSpeed` are not in the design data: `mapc mount-speeds` reads them (`tools/mapc/README.md`) into `dist/content/mount_speeds.tsv`.

**Where the data and the retail disagree** (kept, not resolved: stage 1's report lists them). The item text of the Drake of the Canopy and the Thundering Bolt Beast says 1000 % Maximum Health, the Blueprints' own stat lists (`Mount_DrakeOfTheCanopy`, `Mount_ThunderingBoltBeast`: `GetStatInitRunningTotal() * 5`) say x5, and retail's Drake of capture 883 has MaxHealth 24375, which is x5 of a level 25 owner's 4875: `health_pct` follows the text, as the design (the item tooltip is the truth) does.

`content.DB.RequireVersion` and worldd's `kMinContentcVersion` are **11**: every reader refuses an older file with the way to rebuild it (`cd services && go run ./cmd/contentc`). Rebuild `content.db` with this contentc after pulling it. Tests: `cmd/contentc/mounts_test.go` (the text of an item, a synthetic design database: the four tables, the equations a stat list reads, what is refused), `mounts_real_test.go` (the real file's counts above, the golden items of the captures, the stat curve's values, the roads and the dash), `internal/content/mounts_test.go` (the readers).

**The roads (stage 4; no schema change, contentc stays at version 11).** `contentc roads [--content content.db] [--raw roads_raw.tsv] [--out roads.tsv] [--node-level Wilderness] [--check]` (`cmd/contentc/roads.go`) resolves what the map's road splines are worth to a mount. `tools/mapc` (`mapc roads`) writes `dist/content/roads_raw.tsv`: each `BP_RoadActor_C` with its `RoadPresetRecord` and owner ZOI guids and its spline sampled every 200 cm. For every road this takes the preset's `RoadAppearancesIds[node level]` (`ENodeLevel`: Wilderness 0 to Metropolis 6; the level worldd's `--spawn-node-level` names, Wilderness by default), the appearance's `RoadBuffs` entry for `ECharacterClassification::Mount` (the effect `Road_Tn_Speed_Mount`, the `mount_effect` table's road rows) and its `RoadWidth`, and writes `dist/content/roads.tsv`: `road`, tier, half width (max(RoadWidth, 1000) / 2, modelled) and the points. A preset with no Mount buff at that level (a Vein or a Capillary below its levels, `Node_N3` at 0, the 6-level `LionsHold_Roads` at Metropolis) gives a mount nothing and its roads are left out, an inactive road too; a road whose preset is no record is an error. At Wilderness the E1 area's 49 road actors give 42 roads: 11 Riverlands Artery (T1, 6.6 km) and 31 Lions Hold (T2, 7.8 km). It reads the three record tables from `content.db` (`record`) and `mount_effect`; run it again after a new `roads_raw.tsv` or `content.db`. `contentc` itself and the schema are unchanged.

**Roads by node level (Nodes 1, stage 3b; no schema change).** `contentc roads --levels [--content content.db] [--raw roads_raw.tsv] [--out roads_levels.tsv] [--check]` resolves every road at ALL seven node levels and writes `dist/content/roads_levels.tsv`: `roadn`, the owner zone's guid (`roads_raw.tsv`'s third column), the seven tiers (`Road_Tn_Speed_Mount`, -1 where the road gives a mount nothing) and half widths (max(RoadWidth, 1000) / 2, 0 where none) as comma lists, and the points. Inactive roads and roads that give a mount nothing at any level are left out: 145 roads (91 of them are worth something at Wilderness, the rest only from Crossroads or Village up). worldd (`world/roads.h`) sets each road to the node level of its zone's node, and when a node's level changes the tier of its roads changes with it; `roads.tsv` stays the one-level file `--roads` reads when `roads_levels.tsv` is not there. `scripts\generators.ps1` runs `--levels --check`.

## GM travel tools (teleport and run speed)

"I need a faster way to get around": a GM moves a player to a city node or a point, and sets a character's run speed. **worldd serves an admin API for it, and `worldctl` is its
command line.** It is API-first like `citynode`: the 3D admin viewer to come calls the same methods.

**The API** is `WorldAdminService` (`proto/internal/aoc/worldadmin/v1/worldadmin.proto`, generated Go in `gen/aoc/worldadmin/v1`). worldd listens on `127.0.0.1:50056`
(`AOC_WORLD_ADMIN_ADDR`, or `worldd --world-admin <addr>`; `--no-world-admin` turns it off; a non-loopback address is logged as a warning). Calls carry the service token
(`authorization: Bearer <token>`, as every internal call does; without it, or with another, the call is refused with `Unauthenticated`, the refusal is counted and logged at
most once a second, and the token is never logged or echoed). A call arrives on a gRPC thread and is **applied on worldd's network thread** through a queue that the main loop
drains between two ticks of the world: no session state is touched from another thread. A call the network thread does not take within 5 s ends with `Unavailable` and is
never applied later.

- `ListPlayers`: every player in the world, in the order they joined: name, character guid, position (the last good position; while mounted, where the rider would be put
  beside the mount), yaw, mounted, dead, and the GM run speed multiplier (1: none).
- `ListPlaces`: the places `Teleport` takes by name: every city node that has a place on the Verra map, from `content.db`'s `city_node` table, and `login` (the default
  spawn). A node is left out when its recorded location is the origin (the test nodes, "Mt. Dun", "Open Ocean", the dungeons: the table has no place for them) or is on another
  map than `Verra_World_Master` (the design data's test levels: a player sent there would fall into the void). A name is matched as `nodectl` matches a node: the record name
  (`Verra_RVR_Joeva`), the same without `Verra_`, or the display name (`Joeva`, `New Aela`); case, spaces and punctuation do not count.
- `Teleport(character, place | point{x, y, z, has_z})`: the character's name is matched without case. The path is the one E1's respawn uses to move a player: the move checker is
  rebased on the new place (the credits start over, the TimeStamp state stays) and the client is sent a position correction (`ClientAdjustPosition`), so its next moves are
  measured from there and not corrected. **A mounted player is dismounted first** (the ride is over); the mount stays where it was and desummons by distance as it does today
  (`MountDesummonDistance`, 500 m). Without `z`, the height is the landscape's under the point plus the capsule's half height (98 cm) when the server has ground there (the
  area's terrain), otherwise the call is refused (`InvalidArgument`: "give z"). A named place stands 98 cm above the ground where the server has ground, otherwise 150 cm above
  its recorded z. A dead player is not moved (`FailedPrecondition`), a point outside the world box is refused (`InvalidArgument`), an unknown character or place is `NotFound`.
  Every teleport is logged with its from and to (`[gm] cyb: teleported by ... from (...) to (...)`), and the new position is handed to the saver at once.
- `SetMoveSpeed(character, multiplier)`: 0.5 to 10 (`InvalidArgument` outside), 1 resets. It is a factor of the character's `MoveSpeedMult`: the pawn's replicated stat (the one the
  Sprint's effect raises: the client moves at 600 cm/s times it) and, while the player rides, the mount's own `MoveSpeedMult` (the stat the road's and the dash's effects raise),
  so the client really moves faster. The move checker's limits scale with it (a value below 1 never lowers them: a Dodge's dash is not slowed), with a second's grace after the
  speed goes down while the client learns of it. It lasts until the character logs out or it is set to 1; a Sprint's own +0.2 rides on top.

**`worldctl`** (`dist\services\worldctl.exe`, built by `scripts\services.ps1 build`; it finds `service.token` beside itself, or `--token-file` / `AOC_SERVICE_TOKEN_FILE`, and
worldd at `--addr` / `AOC_WORLD_ADMIN_ADDR`):

```powershell
dist\services\worldctl.exe players                       # name, guid, x y z, mounted, speed
dist\services\worldctl.exe places                        # the places tp takes by name
dist\services\worldctl.exe tp cyb joeva                  # a city node ("New Aela", Verra_RVR_Joeva ...), login, or garden (the GM garden's centre, with --gm-garden)
dist\services\worldctl.exe tp cyb -777664 616578         # a point; without z the ground's height, or a refusal asking for z
dist\services\worldctl.exe tp cyb -777664 616578 16000   # a point with z
dist\services\worldctl.exe speed cyb 5                   # run speed x5 until relog; "speed cyb 1" resets
```

Exit status: 0 done, 1 worldd refused it or could not be reached, 2 the command line is wrong.

**The client's own GM RPCs** on the PlayerController, `ServerTeleportXYZ` (rpc 967: `X`, `Y`, `Z` whole cm, `OptionalRadius`), `ServerTeleport` (964: `DestinationLookupKey`,
`OptionalRadius`) and `ServerTeleportPlayer` (966: `CharacterNames`, `X`, `Y`, `Z`) are answered with the same teleport **only with the GM switch on**: `worldd --gm-commands`
or `AOC_GM_COMMANDS=1` (off by default; `launch_nodes1.bat` turns it on). `ServerTeleport` takes a place's name (the table above; unknown keys are logged and moved nowhere);
`ServerTeleportPlayer` names characters separated by commas, semicolons or spaces. **Whether the shipping client's console can send them at all is not known**: worldd logs
every one it receives, with its parameters, switch or no switch (`[gm] cyb: the client sent ServerTeleportXYZ (X=... Y=... Z=...): answering` or `refused: GM commands are off`),
so a test settles it. The admin API does not depend on the switch.

## City node data (Nodes 1, stage 1, contentc version 12)

**contentc version 12** writes what the design data holds about the city nodes, typed (`internal/content/nodes.go` has the tables and the Go readers, `cmd/contentc/nodes.go` the writer; `docs/superpowers/specs/2026-10-09-nodes-1-design.md` is the design). "Node" is the settlement (`CityNodeRecordBase`), not a resource node or a talent node: the tables say `city_node`, `node_*`. Stage 1 reads `city_node` and `city_zoi`; the rest is typed now so the version bumps once, for stages 2 and 3:

| Table | Rows | What |
|---|---|---|
| `city_node` | 81 | the nodes: **`rank`** (the node's index in the list sorted by guid, 0 to 80: the key of the client's two small GameState node maps, Winstead 5, Joeva 6, New Aela 7, Halcyon 8, Miraleth 9, Wildport 43, Kal Torhum 63; also the number a compiled expression holds a node as), the type (`node_type`, and `node_type_ord`, the ordinal of `ENodeType`), the place, the radius, the layout, `zoi_guid` (the zone whose `CityNodeId` is the node), the election, tax, dues and vendor definitions, the reputation range and `levelable` (the 26 nodes of the five regions that grow: a type, the world map, a zone with a node) |
| `city_node_level`, `city_node_population` | 51, 0 | the `NodeLevelDetails` each level of a node names (only level 3 has any), and the population sets a node names itself |
| `city_zoi`, `city_zoi_adjacent` | 78, 210 | the zones of influence: type, **`biome`** and `biome_ord` (`EBiomeType`: Riverlands 14, Tropics 18 ...), the node, the extents and the neighbours |
| `node_level_details`, `node_level_details_mod`, `node_modification`, `node_modification_ref` | 28, 80, 424, 808 | the Village and harbor level details and the modifications they list: slots, essence, deploy radius, and what each points at (effects, mayoral abilities, commissions, recipe sets) |
| `node_size`, `node_size_radius`, `child_node_rule`, `vassalship_def`, `vassalship_region_size` | 2, 4, 9, 2, 14 | the radius by level, `ChildNodeRules` flattened (a Town may have 2 Villages and 2 Encampments ...), the two vassalship definitions with `RequiredRegionSizeAtLevel` |
| `node_tax_details`, `node_tax_table`, `node_tax_table_tag`, `node_reputation_tier`, `citizenship_*`, `property_tax_def`, `election_def`, `node_constant` | 4, 3, 90, 5, 10, 1, 2, 2 | the tax bounds and the 78-tag global tax table, the five reputation ranks with their tax modifier, the citizenship, dues, property tax and election definitions, the ranked-choice limit and the reputation decay |
| `node_layout`, `node_layout_prop`, `node_layout_plot`, `node_layout_population`, `node_asset_set`, `node_asset_set_member`, `node_asset_def`, `node_asset_def_tag` | 70, 419, 2,069, 351, 424, 549, 634, 0 | the layouts with their props (an asset set at a place; the index is the replicated item's `OwnerArrayIndex`), the plots per level, and the asset sets and definitions with the level window and the valid cultures, node types, seasons and biomes as bit masks of the enums' ordinals (the 269,516 instance transforms are not kept) |
| `service_plot`, `service_plot_building`, `building`, `building_asset_sets`, `building_asset_state`, `building_pop_spawn`, `building_level_spawn`, `building_level_spawn_pop` | 192, 478, 71, 60, 410, 208, 333, 2,370 | the 192 service building plots, the 71 buildings (durations, costs, node experience), the asset sets and the populations of each building state |

Enums are stored as their text and, where the host answers with them, as the ordinal too (`internal/aocexpr/enums.go` has the ordinals, from the client's usmap). Tests: `cmd/contentc/nodes_test.go` (a synthetic design database) and `nodes_real_test.go` (the real one: the counts, the seven rank keys, the 26 nodes that grow, the Riverlands zones, the Village base modification, River_C's 78 props and plots per level ...).

**The node reads of an expression.** `GetNodeBiome`, `GetCurrentNode`, `GetNodeRecord`, `Node_GetNodeType`, `Node_GetLevel`, `Node_InLevelRangeInclusive`, `Node_IsPlayerCitizen`, `Node_IsPlayerCitizenOfNode` and `Node_IsPlayerMayor` (with `GetPlayer`, `GetPlayerGuid` and `EnumInt` around them) are host functions 170 to 177 (`internal/aocexpr/hostfn.go`, `internal/statprog/nodes.go`, worldd's `stats/stat_program.h`). A node is its **rank** in a program (a guid does not fit a float32; `-1` for none), an enum answer is the value's ordinal, and the compiler turns a node reference (`$#<CityNodeRecord type>:<guid>$` or `$CityNode:<record name>$`) and an enum literal compared with an answer into the same numbers. `Node_InLevelRangeInclusive` is the level the host gives compared with the two bounds, in the program. The 12 expressions of the loot tables that read `GetNodeBiome` compile now (the 43 biome-gated reward tables roll in worldd: `BaseLoot_Melee_RVR`, `_TUR`, `_NONE` ...). The citizen and mayor reads answer false until the citizenship milestone. The Go side's model of the host is `statprog.NodeWorld`; the shared corpus (`server/worldd/stats/testdata/corpus.txt`) has `nodehost` blocks it writes and worldd's `world::node_host_fn` must answer the same (`test_worldd_city_nodes`), and the tiny graph's expressions run all of it on both engines.

**The constants the design DB lacks.** `tools/capture/gen_node_constants.py` reads `DefaultAoCRecordConstants.ini` of the Cache.db export (`--ini`, or `AOC_CLIENT_CONFIG`) and writes `internal/citynode/constants_gen.go` and `server/worldd/world/node_constants.h`: `CityNodeExpToLevel`, `CityNodeReputationToLevel`, `MaxNodeLevel` (Town), `MinimumOperationalNodeLevel` (Village), the citizenship level, the vassalship switches, the building, mayor and siege timers and the records the ini selects. Files rather than a `content.db` table because citynode does not open `content.db`, worldd needs them as compile-time numbers and contentc stays a function of `CacheDB.dbc`. `--check` is part of `scripts\generators.ps1` (skipped when the ini is not there).

**The ZOI surface.** `tools/capture/gen_zoi_surface.py` decodes the ZOI chunk grid of the export (30,558 files) into `server/worldd/world/zoi_surface_table.h`: the owner of every 512 cm cell of the world at the surface height, run-length coded (4,635 chunk columns, 58 owners), so that `world::zoi_at(x, y)` gives the zone at a point for the whole world. See `server/worldd/README.md`.

**The transforms keep the sign of a zero (Nodes 1, stage 3a).** The town replicator sends the layouts' transforms bit for bit and retail's carry `-0.0` (29 of Joeva's 85 items). A `REAL` column of SQLite stores a whole-number float as an integer and reads it back as `+0.0`, so the ten transform columns (`qx` .. `sz`) of `node_layout_prop`, `node_layout_plot` and `node_asset_set_member` are declared with no type (`internal/content/nodes.go`, `TestNodeTransformColumnsKeepNegativeZero`). The version stays 12: every reader sees the same numbers. **Rebuild `content.db` with this contentc** for the signs to be kept (`cd services && go run ./cmd/contentc`, 30 s); a file built before has `+0.0` where retail has `-0.0`, which `test_worldd_node_layout` reports as a difference in the layout items.

## Test

```powershell
scripts\services.ps1 up
scripts\services.ps1 test
```

Integration tests use `AOC_TEST_DATABASE_URL` and create a throwaway schema per test; without the
variable they are skipped. The tests of the creation rules and of the character service's content.db
(`internal/customization`, `cmd/character`) read the real `content.db` from `AOC_CONTENT_DB`, which `test`
sets to `dist\content\content.db` (or your `AOC_CONTENT_DB`) when that file exists; without it they skip,
and the rest of the rules are tested on a small database built by the test. `test` runs the Go tests with `-count=1`, so database-backed results
are never cached. It also runs `buf lint` and generates the code into a temporary directory,
failing if it differs from the checked-in `gen/` (line endings are ignored; the tree is not
touched). It needs `buf`, `protoc-gen-go` and `protoc-gen-go-grpc` in `go env GOPATH`\bin (see
Protos).

## Content (contentc and content.db)

`contentc` (plan C1) decodes the client's design database into `content.db`, a SQLite file of the
game numbers the services need (class attributes by level, stat definitions, the stamina rules of
Sprint and Dodge, the numbers worldd's ability runtime runs for Sprint, Dodge and Rest). It is a build tool, not a service: `services.ps1 build`/`run` do not touch it.
`internal/usmap` reads the mappings file, `internal/designdb` walks the `.dbc` (every table it
decodes must end exactly on the table's last byte), `internal/aocexpr` runs the formulas, and
`internal/content` defines the schema (one place: `schema.go`) and the typed queries over it.

The inputs stay outside the repo and are given as flags with environment defaults:

| Flag | Environment | What |
|---|---|---|
| `--dbc` | `AOC_DESIGN_DB` | the client's `CacheDB.dbc`, in the game's `Game\AOC\Plugins\DesignDataPlugin\Generated\Client` |
| `--usmap` | `AOC_USMAP` | the mappings file: `dist/content/aoc.usmap`, generated from the client (below), or the old `5.6.0-434126+++game+jvs_game_rel-AOC_FIXED.usmap`; both decode every table |
| `--config` | `AOC_CLIENT_CONFIG` | optional: the Cache.db export's `AOC\Config` directory, for `StatRegenTime` in `DefaultCombat.ini` (without it the documented default, 1 s, is used and the rule's source says so) |
| `--out` | | `dist/content/content.db` at the repo root (git-ignored) |
| `--record-encoding` | | `gzip+json` (default) or `json`, see below |

The client loads the first `.dbc` of that directory (read from its code, see the `internal/designdb`
documentation), so a `.dbc` there that sorts before `CacheDB.dbc` would be the one it uses; contentc
warns when it finds one, and `meta` records the names. Until 2026-10-01 there was one,
`0_AoCEvolve.dbc` (a third-party patch of `CacheDB.dbc` dated 2026-09-26): it differed in six tables,
none of them one of the typed tables, and `meta` keeps a note of that. The folder holds only
`CacheDB.dbc` now.

```powershell
python tools\ghidra\usmapgen.py                                  # writes dist\content\aoc.usmap from the client exe
$env:AOC_DESIGN_DB = '<AOC_CLIENT_DIR>\...\Generated\Client\CacheDB.dbc'
$env:AOC_USMAP = 'dist\content\aoc.usmap'   # or the old AOC_FIXED usmap
$env:AOC_CLIENT_CONFIG = '<AOC_EXPORT_DIR>\AOC\Config'      # optional
cd services; go run ./cmd/contentc
```

### The mappings file

`tools/ghidra/usmapgen.py` writes a `.usmap` from the client executable's own reflection data
(`AOCClient-Win64-Shipping.exe`, read only; `--exe` to point elsewhere), so the mappings match the
exact client build. It enumerates every compiled-in class, struct and enum from the UHT registration
tables (`FClassRegisterCompiledInInfo` and friends), adds the 158 NoExport core structs (`Vector`,
`Guid`, ...) from a scan for their `FStructParams`, and the intrinsic classes (`Object`, `Class`, ...)
from their single `RegisterCompiledInInfo` call sites, then reads each type's properties, supers and
enumerators the way `UECodeGen_Private::ConstructFProperties` does. The output is usmap version 4,
uncompressed, deterministic (72,456 names, 3,122 enums, 15,419 schemas for the 2026-01-29 client),
and `--diff OLD.usmap` prints a structured comparison with another file (`--diff-out FILE` for all of
it). Against the old usmap: 14,927 schemas identical, 422 identical but for the casing of names (the
old file carries the FName pool's casing, the exe the declared one: `StatTypeDef.Base`, not `base`;
contentc reads fields without regard to case, `record.data` keeps the usmap's spelling), 60 differ
only in how static arrays are numbered, 10 differ (properties the client gained, `bMayCorrupt` among
them), and 943 Blueprint classes and asset-defined structs are only in the old file (they are not in
the exe; FModel reads them from the assets). The same run writes, with `--go`, the facts a `.usmap`
cannot carry but the decoder needs, into `internal/designdb/exe_gen.go` (committed): the 398 record
types by TypeId, the properties the database does not store (Transient), the four 16-byte-aligned
record structs, and the natively serialized structs. Regenerate both after a client update:
`python tools\ghidra\usmapgen.py --go services\internal\designdb\exe_gen.go`.

The output is written beside `--out` and renamed over it when the run has succeeded; a failed run
leaves the previous file alone. A run takes about 30 s (the table classification runs on all cores, the
rest on one thread). `record.data` holds every row as JSON (573 MB of it): gzipped row by row the
file is 270 MB, so gzip is the default; the typed tables, which are what services read, are not
compressed.

What is in `content.db` (the schema is `internal/content/schema.go`):
- `meta`: the inputs' paths, sha256 and sizes, the `.dbc`'s generation time and the result of its
  crc32 checks, the usmap's sha256, contentc's version, the decode counts (all 369 tables,
  205,882 rows, per table), what became of each mappings patch (the four properties the old usmap
  lacks; "already present" with the generated one; a patch that fits nothing stops the build), and
  the `0_AoCEvolve.dbc` note;
- `record_table`, `record`: every table of the `.dbc` and every row, by table, TypeId and Guid (a
  pair that is unique). A row is the table's record struct walked with the facts read from the
  client (`internal/designdb`: the leaf `FXxxRecord`'s own properties first, Transient properties
  not stored, 16-byte-aligned rows for four structs, 4 more bytes after a row with a parent,
  `InstancedStruct`, `Optional`), and every table's rows end exactly on its last byte. The TypeIds
  the client registers name every table's struct (268 tables are the only struct that walks them,
  101 are walked by others too and settled by the TypeId); the schema is the struct derived directly
  from `DesignDataRecordBase` (`AoCAbilityRecordBase`; 30 are not named `...RecordBase`, such as
  `DialogueNode`). A float that is NaN or infinite is stored as the string `"NaN"`, `"+Inf"` or
  `"-Inf"` (none is in this file);
- `stat_def` (formulas and replication audience), `stat_curve_def`, `stat_curve`, `stat_init`;
- `class` (the eight player classes, archetype ids 17747-17754 as in `internal/aocchar`) and
  `class_stat_base`: Strength, Dexterity, Constitution, Intelligence, Wisdom and Mentality at levels
  1-50, computed from the classes' `Player_Base_<Class>` formulas and the curves;
- `ability*`, `effect*`, `hit*`, `owner_stat_mod` for Sprint, Dodge and Rest and the effects they reach
  (and the Sprint's drain, `General_Sprint_StaminaDrain`, which no row references: plan A7 takes it as the
  drain, see below);
- `stamina_rule` and `stamina_regen_disable`: what plan A7 needs, with the source of each value;
- `equip_slot`, `item`, `item_equip_slot`, `starting_equipment` (plan B2, contentc version 3): the 83 enumerators of `EEquipSlot` by name; one row for each of the
  10,568 `ItemRecord`s (name, level, `bCanEquip`, whether it takes every slot it lists, default quality, max stack, its `ItemStatBlock` and appearance, and `max_durability`, the
  stat block's MaxDurability at the default quality: the rarity band of the `QualityCurve`, interpolated, rounded half up); `item_equip_slot`, the slots each may be worn in, in the record's order;
  and `starting_equipment`, the 96 rows of the eight classes' `StartingEquipmentRecord`s. `bCanEquip` is not the rule of what is worn: the starter armor of every class has it 0 and is worn,
  and 374 gear records have it 0 and a slot. contentc refuses a kit item no `ItemRecord` has and a slot the enum does not know;
- `stat_program`, `quality_curve`, `stat_block_stat`, `item_set`, `set_bonus`, `item_tag` and `item.quality_curve_guid` (plan B3, contentc version 4): see "Stats (plan B3)" above;
- `ability_runtime`, `effect_runtime`, `effect_stat_runtime` (plan A7): what worldd's ability runtime runs, as
  numbers, every formula already run by contentc (`internal/aocexpr`) so that worldd evaluates nothing. One
  `ability_runtime` row per ability worldd handles: whether it is held (`Smart`) or a `Toggle`, whether it needs
  the caster out of combat, the Stamina it needs to start (`min_stamina`, found by running the
  `ActivationRequirements` formula and bisecting the threshold), the cost (`cost_amount`, paid once or, with
  `charge_interval`, per charge, and only while moving when `cost_requires_moving`), the cooldown (run with no
  skill bought), whether the paid cost starts it, the effect it applies (`EffectsIds` key 1), its first montage
  path and the section a cancel jumps to. The numbers it holds: Sprint needs 1 Stamina and takes 1.25 every
  0.25 s while the character moves (5 a second), no cooldown; Dodge costs 50 once and has a 0.65 s cooldown;
  Rest is a free toggle that needs the caster out of combat. `effect_runtime` has the four effects they apply
  or end with (duration or infinite, the tick timer, whether the effect stops Stamina regenerating while
  standing and while moving, from `stamina_regen_disable`, and the regeneration pause its End hit applies: 1 s of
  `General_Stamina_DisableRegen` after a Sprint and after a Dodge's 0.5 s Evasion); `effect_stat_runtime` has the
  stat mods as `factor x (the owner's source stat) + addend`, found by running the mod's formula at three values of
  the stat and requiring the results on one line (Rest adds 0.01 x MaxHealth + 1 to HealthRegeneration, and the same
  with Mana: with the captured Bard's 90 that is 1.9, which on the base 0.6 is the 2.5 the original server sent).
  A formula outside what the runtime reads (another stat, a combat condition, a cost charged by movement for any
  ability but the Sprint, two regeneration pauses) stops the build with the row's name.

The formulas are evaluated by `internal/aocexpr`, which supports the subset the first slice uses and
reports anything else as an unsupported construct. Every number is a float64, while the engine's
expressions are typed (`100` is an integer, `2.0` and `Float(x)` floats), so `/` is evaluated only
when an operand is provably a float (a literal with a decimal point, `Float`, `SelectFloat`,
`EvalCurve`, or a variable only ever given such a value); `7 / 2` or `GetCharacterLevel(...) / 2` is
an error, never a guess. The class formulas and the stamina, Sprint and Dodge texts do not divide.
`Round` rounds halves toward positive infinity (`Round(-2.5)` is -2).

A build is deterministic: two runs on the same inputs give the same rows in every table, and the
same gzip bytes of `record.data` when both use one Go toolchain (the gzip output is fixed for a Go
version, not across versions).

Open items, recorded rather than hidden:
- The Bard's level-1 Intelligence is 39 by the formula and 60 in the capture; the other five
  attributes match the capture exactly (`TestKnownDifferenceBardIntelligence`).
- 80 of the 2400 attribute values sit exactly on a rounding half and, for 7 of them (335 x 1.3 at
  level 20), float32 arithmetic would round the other way; `class_stat_base.rounding_ambiguous`
  marks those 7. Which the engine does is not in the data.
- How often a held Sprint charges its cost is in no row it reaches. The one cadence in the data is the effect
  `General_Sprint_StaminaDrain` (infinite, a tick every 0.25 s, whose tick hit takes 1.25 Stamina while the owner moves
  and 2.5 in combat, the Sprint cost row's amounts), which no record references (all 205,882 were searched).
  `sprint.charge_interval` and `ability_runtime.charge_interval` are its TickTimer, 0.25 s, by decision with the user
  (spec section 7: 5 Stamina a second, a full bar in 20 s); contentc checks the drain's numbers against the cost row and
  stops the build when they disagree, so a client update that changes either is noticed. Whether the engine charges on
  that cadence is for a stopwatch in game.

contentc's version is 6 on the D2 groundwork (`Version`; D1 makes it 5): 4 was the stat program, 3 the items, 2 the ability runtime; 6 adds the `ab_*` tables and the format 2 stat program. The real build takes about 25 s and the file is 274 MB.

Tests: `go test ./internal/aocexpr ./internal/content ./cmd/contentc` run without the inputs (a
hand-built database covers the typed tables, and the golden data of `cmd/contentc` -- the six
attribute curves and 48 class formulas, read by an independent decoder -- checks the formulas against
plain arithmetic); with `AOC_DESIGN_DB` and `AOC_USMAP` set (either usmap) they also build the real
file (about 30 s, twice for the determinism test) and check the golden Bard values, all 2400 attribute
values, the stamina and Sprint/Dodge numbers, the record counts, and that a corrupt `.dbc` or a patch
that fits nothing stops the build; `go test ./internal/usmap ./internal/designdb` check the mappings
file's counts, the classification of all 369 tables and the rules above on the rows. Without them
those tests skip.

## Configuration

`AOC_DATABASE_URL`, `AOC_AUTH_HTTP_ADDR`, `AOC_AUTH_GRPC_ADDR`, `AOC_CHARACTER_GRPC_ADDR`,
`AOC_CITYNODE_GRPC_ADDR`, `AOC_CITYNODE_SEED` (the city node service; also `--seed`),
`AOC_TOKEN_TTL`, `AOC_SERVICE_TOKEN_FILE`, `AOC_CONTENT_DB` (the character service; also `--content-db`),
`AOC_MESH_GRPC_ADDR`, `AOC_MESH_STATIC_WORLD`,
`AOC_MESH_HEARTBEAT_TTL`, `AOC_GATEWAY_ICS_ADDR`, `AOC_GATEWAY_GRPC_ADDR`,
`AOC_GATEWAY_GRPC_PUBLIC`, `AOC_GATEWAY_TLS_CERT`, `AOC_GATEWAY_TLS_KEY`, `AOC_TICKET_TTL`
(`internal/config`).

Internal gRPC has no TLS, so the service token crosses the network in clear text, as the join
ticket does in the client's UDP handshake. The gateway therefore refuses to bind
`AOC_GATEWAY_GRPC_ADDR` to anything but a loopback address unless `AOC_GATEWAY_GRPC_PUBLIC=1`, and
logs a warning when it is set. The other services' internal ports bind loopback by default; keep
them there until the services talk over TLS.

## Protos

Internal APIs live in `proto/internal/aoc/...`. The generators, pinned to the versions the
checked-in code was generated with (other versions change the generated files):

```bash
go install github.com/bufbuild/buf/cmd/buf@v1.73.0
go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.12
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.6.2
```

They install into `go env GOPATH`\bin (`%USERPROFILE%\go\bin` by default). `services.ps1 test`
adds that directory to `PATH` for its own run; for the commands below, put it on `PATH` yourself.

After changing a proto:

```bash
cd services && buf lint ../proto/internal && buf generate
```

Generated code in `services/gen/` is checked in.

**The starter set's test mounts** (the mounts milestone, stage 3, changed in fix 2): after the gear scroll the set holds 8 mount items (`kit.StarterMountNames`: the two PTR Eagle Gryphons, the four aquatic mounts, the terror bird and the skinwalker), because the client's vendor window lists only 44 of the 101 mounts. The flyers are the two a client lets anyone summon: the 44 other flying items are mayor rewards whose activation requirement the client checks before it sends anything (`worldd/README.md`, "Stage 3 fix 2"). A name the content.db lacks is left out. `GrantMissingStarterMounts` is a TOP-UP, not a once-only grant: at every start each character with the set (`gm_starter_set_granted_at` set) is given the test mounts it holds nowhere (bag, worn, quest storage); a character holding them all is not written to, and one that threw a test mount away gets it back. Nothing is migrated: `gm_starter_mounts_granted_at` (migration 0008) stays, set the first time a top-up gives something; it is informational (the top-up looks at what the character holds, not at the column), and migration 0008 and its rollback are tested (`internal/db/migration0008_test.go`, which runs the README's block below as written). A top-up writes the whole bag through the character service while worldd may hold an older copy of a character that is in game: top-ups are for a character service started with nobody playing (the same holds for E1's set).
