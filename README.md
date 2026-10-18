# SandalCore

An independent, open-source server emulator compatible with the Ashes of Creation client, developed by Cyb.

> [!IMPORTANT]
> **Not affiliated with Intrepid Studios.** SandalCore is an independent project. It is not affiliated with, sponsored by or endorsed by Intrepid Studios, Inc. *Ashes of Creation* is a trademark of Intrepid Studios, Inc., named here only to say which game client this server works with. The repository contains no game client, game files or Intrepid code: the data the servers need is built on your PC from your own installed copy of the game.

> [!WARNING]
> **Work in Progress**: SandalCore is **NOT** a final product. It is under active and consistent development; expect bugs, incomplete features, and ongoing protocol refinements. Please refer to [ROADMAP.md](ROADMAP.md) for milestone progress and [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for current known limitations, workarounds, and backlog items.

---

## What Works Today

SandalCore provides an independent server emulation stack for the Ashes of Creation client: the Go services (accounts, characters, city nodes, world placement and the client's gateway) and the C++ world server `worldd`, with the tether and the launcher window. The old replay-based login server is retired: this is the only server path.

- **Protocol & Network Transport**: Full UE5 / IntrepidNetDriver handshake, stateless connect, packet framing, bunch replication, and dynamic NetGUID allocation.
- **Authentication & Character Management**: gRPC and REST-based authentication services, account profiles, character creation with full visual appearance customization serialization, and character select lobby transitions.
- **World Bootstrap & Replication**: Seamless map loading into `/Game/Levels/Verra_World_Master`, actor creation channels, possession handshakes, and player controller state replication.
- **Authoritative Stats Engine**: Core attribute formulas, expression evaluation graph, compiled stat program bytecode, and dynamic recalculation of stats upon equipment changes.
- **Progression, Items, and Talents**: Adventuring XP curves, level thresholds, skill tree talent acquisition, action bar persistence, inventory storage, and consumable item effects.
- **Complete Combat & Ability Runtime**: Authoritative ability pipeline supporting all retail classes, cast validation steps, cooldown ledgers, projectile trajectories, hit confirmation RPCs, floating combat numbers, shields, songs, and buff/debuff effect stacks.
- **Living World & Navigation**: Chunked navmesh windows, Riverlands region partitioning, NPC actor spawning, AI agro and leash state machines, combat engagement, loot drops, and respawn cycles.
- **Mounts & Traversal**: Ground, flying, and aquatic mount summoning, riding possession, mount combat mechanics, sprint/dash abilities, and road speed modifiers.
- **City Node Civilization**: Background `CityNodeService`, node experience accrual, level transitions, town layout replication, the real floor of the five towns with a safety net under it, and citizenship/mayorship management.
- **Launcher**: the launcher window finds your game, signs in (with "Keep me signed in" it signs in by itself at start) and starts the game when you press LAUNCH GAME.
- **Gathering & Professions**: the 22 gathering professions, node harvesting (mining, lumberjacking and herbalism), the Riverlands' trees, plants and stones that grow back, huntable animals, resource bags and the pick-up popup.

---

## Build and run (Windows)

You need a Windows 10/11 x64 PC with your own installed copy of Ashes of Creation. SandalCore contains no game data: the first build reads your installed client (the files are only read, nothing in the game folder is changed) and builds the data the servers need.

### 1. What to install

`install_dependencies.bat` checks this list and offers to install what is missing (through winget, asking before each item); `install_dependencies.bat -CheckOnly` only reports.

| What | Version | What it is for |
|---|---|---|
| **Ashes of Creation** (Steam) | the build of 2026-01-29 | the source of the game data; it has to be installed on this PC |
| **Visual Studio 2022 Build Tools**, workload "Desktop development with C++" | MSVC v143 | the C++ programs |
| **CMake** | 3.25 or newer (the copy of Visual Studio works) | the C++ build |
| **Git** | any current version | fetches CUE4Parse and vcpkg |
| **[vcpkg](https://github.com/microsoft/vcpkg)** | current | the C++ libraries (grpc, protobuf, openssl, ...); found through `VCPKG_ROOT`, `%USERPROFILE%\vcpkg`, `C:\vcpkg`, `\vcpkg` on the repository's drive, or the Visual Studio copy |
| **Go** | 1.27 or newer (`services/go.mod`) | the backend services and `contentc` |
| **.NET SDK** | 8.0 (any 8.x; with the 8.0 runtime) | `tools/mapc`, which reads the game's files |
| **Python** | 3.10 or newer | the generators that `extract_game_data.bat` runs |
| **Docker Desktop** | current | PostgreSQL and NATS, to run the servers (not needed to build); or run PostgreSQL 15+ and NATS yourself |

Disk: allow about 20 GB free on the repository's drive for the first build. The game data is 1.5 GB and the finished C++ build about 12 GB; vcpkg needs more while it compiles the libraries. Memory: at least 6 GB free while the game data is built (the biggest program uses about 4 GB).

### 2. The commands, in this order

1. `install_dependencies.bat`
2. `build_all.bat`: runs `extract_game_data.bat` first (the game data, section 3), then builds `tether_server`, `launcher` and `worldd` into `dist\Release`. The first run takes about 10 minutes for the game data on a fast PC, plus the first build of the C++ libraries through vcpkg (grpc compiles from source, so this part can take a long time). Later runs take seconds when nothing changed.
3. `launch_worldd.bat`: starts PostgreSQL and NATS (Docker), the Go services, the `worldd` world server, the tether and the launcher window. Sign in (or let "Keep me signed in" do it) and press LAUNCH GAME, which starts the game. On the first launch your own TLS certificates are created in `certs\` (Windows asks you once to trust the local CA; choose Yes).

To make only the game data (for example before `go test ./...` or `scripts\run_tests.ps1`), run `extract_game_data.bat` on its own.

### 3. The game data (`extract_game_data.bat`)

**Where the game is found**, in this order: `extract_game_data.bat -GameRoot "D:\...\Ashes of Creation\Game"`, the environment variable `GAME_ROOT`, `game_executable` in `config\paths.json` (the launcher's Auto-Detect and Browse buttons save the path in `dist\Release\config\paths.json`, which is read too), Steam's libraries (found through the registry and `libraryfolders.vdf`), then the usual folders on every fixed drive. The game folder is the one that holds `AOCClient.exe` and the `AOC` folder; with Steam it is `...\steamapps\common\Ashes of Creation\Game`. `-GameRoot` counts for that one run only (`build_all.bat` does not pass it on). If your game is in an unusual place, set `GAME_ROOT` once, with `setx GAME_ROOT "D:\...\Ashes of Creation\Game"`, and open a new window: then `extract_game_data.bat`, `build_all.bat` and the launchers all find it.

**What it makes** (all of it git-ignored; [`.gitignore`](.gitignore) lists the generated sources):
- `dist\content\content.db` (311 MB): the game numbers the servers use (`contentc`, from the client's `CacheDB.dbc`);
- `dist\content\*.tsv`: mount speeds and riders, profession curves, NPC motion, foliage and roads (`tools\mapc`, from the game's containers, and `contentc roads`);
- `dist\content\town_platforms.bin` (4 MB): the floor of the five towns (`tools\mapc`, from two of the game's map packages);
- `dist\content\town_layout_items.tsv` (6 MB) and `town_layouts.bin` (13 MB): the buildings, walls and terraces of the five towns at the level each town starts at (a list made from the design data by `tools\capture\gen_town_layouts.py`, then their collision from the game's packages by `tools\mapc`);
- `dist\content\area_nav.bin` (974 MB) and `area_geom.bin` (232 MB): the navigation mesh, with the towns' buildings in it (`tools\mapc` and `aoc_navbuild`);
- `dist\content\client_config\` and `zoi_chunks\`: the game's config files and its zone grid, which the generators read;
- the generated sources and test fixtures the build includes (for example `zoi_surface_table.h`, `node_constants.h`, `region_data.h`).

**The towns' levels.** The navigation mesh holds each of the five towns with the buildings of the level the server starts that town at, the one `deploy\citynode_seed.json` gives it (Joeva 4, Town; Halcyon 3, Village; the other three 0, Wilderness). If a GM changes a town's level in the game (`nodectl set-level`), the town in the game changes with it, but the navigation mesh keeps the buildings of the level it was made for: people can then walk where a new house stands or stand on a floor that is gone, until the mesh is made again. To make it for other levels, change the levels in the seed file and run `extract_game_data.bat` again (the buildings and navigation mesh steps run, about 10 minutes). A level change in the game that swaps the mesh by itself is a later update.

**How long:** about 10 minutes the first time on a fast PC (24 threads, 31 GB: 10.3 minutes in all, of which the map read and the navigation mesh take 8); a slower PC takes longer. Every step is skipped when what it reads has not changed, so running it again takes about a second. `extract_game_data.bat -CheckOnly` says what would run, `-Only <step>` runs one step (the steps are `tools`, `config`, `curves`, `database`, `region`, `headers`, `fixtures`, `navbuild`, `roads`, `townfloors`, `townlayouts`, `navmesh`), `-Skip <step>` leaves one out and `-Force` runs everything again. The output of every program is kept in `dist\content\.extract\`; `last_run.log` has all of it.

**Network:** the first run needs it once, for CUE4Parse (a git clone of github.com/FabianFG/CUE4Parse at a pinned commit; the script keeps its 7.5 MB of source), the NuGet packages that `tools\mapc\mapc.csproj` names (from nuget.org), the Go modules that `services\go.mod` names, and, in the first C++ build, the vcpkg libraries. Nothing is downloaded after that.

**After a game patch:** run `extract_game_data.bat` (or `build_all.bat`) again. It sees that the game files changed and rebuilds everything that depends on them, about 10 minutes. The code is made for the client build of 2026-01-29 (`AOCClient-Win64-Shipping.exe` 235,606,624 bytes, `CacheDB.dbc` 144,085,052 bytes). With any other build the script prints a warning and goes on; the data can then be wrong or a step can fail.

**Navigation mesh (required for combat).** `worldd` starts its combat system only after `dist\content\area_nav.bin` loads; **without it every mob has 0 HP and damage does nothing** (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)). The `navmesh` step builds it; `extract_game_data.bat -Skip navmesh` leaves it out.

**What is in the repository.** `layouts\aoc.usmap` holds the names, types and layouts of the game's classes, structs and enums, as `libs\aocnet\protocol\schema\generated\` and `services\internal\designdb\exe_gen.go` do for the network protocol and the design database; none of the three holds game content.

### 4. If something goes wrong

| You see | Why | What to do |
|---|---|---|
| `The Ashes of Creation client was not found` | the game is not installed on this PC, or it sits in an unusual folder | install it (Steam), or run `extract_game_data.bat -GameRoot "D:\...\Ashes of Creation\Game"` (that run only), or set `GAME_ROOT` to that folder for good (`setx GAME_ROOT "D:\...\Ashes of Creation\Game"`, then open a new window) |
| `-GameRoot '...' is not the game's folder` | the folder has no `AOC\Content\Paks` or no `CacheDB.dbc` | give the folder that holds `AOCClient.exe` and `AOC` (the `Game` folder of the install), not the launcher folder; if the install is incomplete, let Steam verify the game files |
| `WARNING: this client is not the build this code was made for` | the game was patched, or it is another build | the script goes on; the data may be wrong or a step may fail (see "After a game patch") |
| `Missing: ...` followed by `Python 3.10 or newer`, `the .NET 8 SDK` or `Go` | that tool is not installed or not on `PATH` | run `install_dependencies.bat`, open a new window (so the new `PATH` applies), run again |
| CMake: `These files are made from your own Ashes of Creation install and are missing` | the C++ build ran before the game data was made | run `extract_game_data.bat`, then build again |
| `ERROR in step '...'` | a step failed | read `dist\content\.extract\last_run.log` and fix the cause; run again: finished steps are kept and the run continues at the failed one |
| `waiting: ...` in the map step | the map read waits until at least 6 GB of memory are free and the game, a build and `aoc_navbuild` are not running | close them; it continues by itself and gives up after 40 minutes |
| the CUE4Parse fetch fails | no network or no Git | connect and run again, or copy the folder yourself: `powershell -File tools\mapc\fetch_cue4parse.ps1 -From <folder with CUE4Parse>` |
| every mob has 0 HP and damage does nothing | `dist\content\area_nav.bin` is missing | run `extract_game_data.bat` (without `-Skip navmesh`) |

### 5. More build options

- **Everything:** `build_all.bat` (the game data, then `powershell -File scripts\build.ps1 -Target all`).
- **One program:** `powershell -File scripts\build.ps1 -Target worldd` (targets: `tether_server`, `launcher`, `worldd`). `build_launcher.bat` builds just `launcher`. The game data has to be made first.
- **Options:** `-Config Debug`, `-Clean`, `-Configure` (re-run CMake), `-Test`, `-Jobs N` (build at most N projects and at most N compiler processes at a time; without it the compilers use as many processors as the free memory allows); `build_all.bat` passes them on.
- **Why it is fast:** the build compiles the files of one project in parallel, and it copies the libraries' DLLs into `dist\Release` once after the build instead of after every program. A `build\x64` folder made by an earlier version keeps working as it was; `build.ps1 -Configure` switches it over.
- **Plain CMake** (it works, but it is slower: one file at a time inside a project, and the DLLs are copied after every program):
  ```bat
  cmake -S . -B build\x64 -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
  cmake --build build\x64 --config Release
  ```
- **Go services** (`services/`): `go build ./...`, `go vet ./...`, `go test ./...` (the launch scripts build and start them for you).
- **Tests:** `powershell -File scripts\run_tests.ps1` builds and runs every C++ test (add `-Jobs N` to limit the build); the Go tests are `go test ./...` in `services\`.

---

## Running the Server

### 1. Start everything (recommended)

```bat
launch_worldd.bat
```
It checks that the programs are built, then starts the full stack with `worldd` as the world server; `scripts\launch_all.bat` does the work. The launcher window opens: sign in there and press LAUNCH GAME, which starts the game client. With "Keep me signed in" ticked the sign-in is saved in Windows Credential Manager (`SandalCore/LauncherSignIn`, for your Windows account only) and the launcher signs in by itself the next time; Logout forgets it.

The client is started with `-UseEOS=false`, so it runs without Epic Online Services; no game files are modified.

### 2. Backend services only

```powershell
powershell -File scripts\services.ps1 up     # PostgreSQL + NATS (Docker), waits until healthy
powershell -File scripts\services.ps1 stop
```

### 3. TLS Certificates

SandalCore ships no keys or certificates. On the first launch `scripts\make_certs.ps1` (OpenSSL from Git for Windows) creates your own:
- `certs\ca.crt`: a local CA, added to your Windows *CurrentUser* Trusted Root store (the game client checks the server against it). Its private key is deleted right after signing.
- `certs\server.pem` and `certs\server.key`: the server certificate for `127.0.0.1` / `localhost`.

Run `powershell -File scripts\make_certs.ps1 -Force` to make new ones. Details: [`certs/README.md`](certs/README.md).

### Configuration & Environment Variables

Key runtime environment variables and settings include:
- `GAME_ROOT`: The folder of your installed Ashes of Creation client that holds `AOCClient.exe` and `AOC\` (for example `C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game`). `extract_game_data.bat` and the launchers use it before they search for the game.
- `AOC_CLIENT_CONFIG`: The game's `AOC\Config` folder (default `dist\content\client_config`, written by `extract_game_data.bat`).
- `AOC_ZOI_CHUNKS`: The zone-of-influence chunk folder (default `dist\content\zoi_chunks`, written by `extract_game_data.bat`).
- `AOC_CONTENT_DB`: Path to game content database fixtures (defaults to `dist\content`).
- `AOC_MIRROR_LOGS_ROOT`: Log mirror destination (defaults to `logs\mirror`).
- **Database Connection**: Configured via `deploy/docker-compose.yml` or connection string (`postgres://aoc:aoc@127.0.0.1:5432/aoc?sslmode=disable`).
- **Network Ports**: the gateway (the game client's session, over TLS) on TCP 443, the auth service's HTTP API on TCP 8081, `worldd` on UDP 7777 (`worldd --bind`) and the tether on UDP 19021.
- **Launcher Path**: Configured in `config/paths.json` (auto-detected from `GAME_ROOT`).

---

## Captures

SandalCore includes no packet captures. The community's own repository holds the plaintext captures of the live game that this project's protocol work started from: [AshesEmulatorTeam/PCAPRepo](https://github.com/AshesEmulatorTeam/PCAPRepo), captured by the Creation of Ashes (CoA) community. Thank you.

---

## Contributing

We welcome contributions from the community! Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) for our code of conduct, branch conventions, and testing requirements before opening a pull request.

---

## Reporting Bugs

- **Bug Reports**: Open a new issue using our [GitHub Bug Report Form](.github/ISSUE_TEMPLATE/bug_report.yml).
- **Security Vulnerabilities**: Do **not** open public issues for security concerns. Report them privately according to [`SECURITY.md`](SECURITY.md).

---

## Credits

Developed by Cyb. Special thanks to Hatemost for building the foundation and CoA (Creation of Ashes) / the Ashes community who participated in capturing the PCAPs.

---

## License

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License version 3 (AGPL-3.0-only)** with additional terms under Section 7. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE) for the full license text and terms.

---

## Disclaimer

Not affiliated with, sponsored by or endorsed by Intrepid Studios, Inc.; Ashes of Creation is their trademark, named only to identify the compatible client; no game client, game files or Intrepid code included; the game data is built on your PC from your own legally obtained client; for preservation and education.
