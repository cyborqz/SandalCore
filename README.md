# Sandal Core

A server emulator for the Ashes of Creation client, developed by Cyb.

> [!WARNING]
> **Work in Progress**: Sandal Core is **NOT** a final product. It is under active and consistent development; expect bugs, incomplete features, and ongoing protocol refinements. Please refer to [ROADMAP.md](ROADMAP.md) for milestone progress and [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for current known limitations, workarounds, and backlog items.

---

## What Works Today

Sandal Core provides a full-featured, clean-room server emulation stack for the Ashes of Creation client:

- **Protocol & Network Transport**: Full UE5 / IntrepidNetDriver handshake, stateless connect, packet framing, bunch replication, and dynamic NetGUID allocation.
- **Authentication & Character Management**: gRPC and REST-based authentication services, account profiles, character creation with full visual appearance customization serialization, and character select lobby transitions.
- **World Bootstrap & Replication**: Seamless map loading into `/Game/Levels/Verra_World_Master`, actor creation channels, possession handshakes, and player controller state replication.
- **Authoritative Stats Engine**: Core attribute formulas, expression evaluation graph, compiled stat program bytecode, and dynamic recalculation of stats upon equipment changes.
- **Progression, Items, and Talents**: Adventuring XP curves, level thresholds, skill tree talent acquisition, action bar persistence, inventory storage, and consumable item effects.
- **Complete Combat & Ability Runtime**: Authoritative ability pipeline supporting all retail classes, cast validation steps, cooldown ledgers, projectile trajectories, hit confirmation RPCs, floating combat numbers, shields, songs, and buff/debuff effect stacks.
- **Living World & Navigation**: Chunked navmesh windows, Riverlands region partitioning, NPC actor spawning, AI agro and leash state machines, combat engagement, loot drops, and respawn cycles.
- **Mounts & Traversal**: Ground, flying, and aquatic mount summoning, riding possession, mount combat mechanics, sprint/dash abilities, and road speed modifiers.
- **City Node Civilization**: Background `CityNodeService`, node experience accrual, level transitions, town layout replication, and citizenship/mayorship management.
- **Resource Gathering**: Hunting interactions and drop notification pop-ups.

---

## Quick Start (Windows)

All three files are in the main folder; double-click them in this order.

1. **`install_dependencies.bat`**: checks for everything below and offers to install whatever is missing (through winget, asking before each one). Run `install_dependencies.bat -CheckOnly` to only see what is installed.
2. **`build_all.bat`**: builds every server program into `dist\Release` (`auth_server`, `tether_server`, `launcher`, `worldd`, `aoc_server`). The first build also downloads and compiles the C++ libraries through vcpkg (grpc, protobuf, openssl, ...), so it can take a long time; later builds are quick.
3. Set your game install path: see [Configuration](#configuration--environment-variables). Everything else is set up on the first launch: the game content database (`content.db`, shipped zipped in `content\`) is unpacked into `dist\content`, the navigation mesh that combat needs (1.2 GB) is downloaded from the Releases page (see [Game content](#4-game-content)), and your own TLS certificates are created in `certs\` (Windows asks you once to trust the local CA; choose Yes).
4. **`launch_worldd.bat`**: starts PostgreSQL and NATS (Docker), the Go services, the `worldd` world server, the tether and the launcher window. Sign in and press Play.

---

## Requirements

`install_dependencies.bat` installs all of these for you.

- **Windows 10/11 x64**
- **Visual Studio 2022 Build Tools** with the "Desktop development with C++" workload (MSVC v143)
- **CMake 3.25+** (or the copy that ships with Visual Studio)
- **Git** and **[vcpkg](https://github.com/microsoft/vcpkg)** (found through `VCPKG_ROOT`, `%USERPROFILE%\vcpkg`, `C:\vcpkg`, `\vcpkg` on the repo's drive, or the Visual Studio copy)
- **Go 1.27+** (the version in `services/go.mod`) for the backend services in `services/`
- **Docker Desktop** for PostgreSQL and NATS (`deploy/docker-compose.yml`); or run PostgreSQL 15+ and NATS yourself
- **Python 3.10+** (optional: capture and analysis tools)

---

## Building

- **Everything:** `build_all.bat` (same as `powershell -File scripts\build.ps1 -Target all`).
- **One program:** `powershell -File scripts\build.ps1 -Target worldd` (targets: `aoc_server`, `auth_server`, `tether_server`, `launcher`, `worldd`). `build_server.bat` and `build_launcher.bat` build just `aoc_server` or `launcher`.
- **Options:** `-Config Debug`, `-Clean`, `-Configure` (re-run CMake), `-Test`.
- **Plain CMake:**
  ```bat
  cmake -S . -B build\x64 -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
  cmake --build build\x64 --config Release
  ```
- **Go services** (`services/`): `go build ./...`, `go vet ./...`, `go test ./...` (the launch scripts build and start them for you).

---

## Running the Server

### 1. Start everything (recommended)

```bat
launch_worldd.bat
```
It checks that the programs are built, then starts the full stack with `worldd` as the world server. The other launchers in `scripts\` (`launch_all.bat`, `launch_go.bat`, `launch_native.bat`) are for development.

The client is started with `-UseEOS=false`, so it runs without Epic Online Services; no game files are modified.

### 2. Backend services only

```powershell
powershell -File scripts\services.ps1 up     # PostgreSQL + NATS (Docker), waits until healthy
powershell -File scripts\services.ps1 stop
```

### 3. TLS Certificates

Sandal Core ships no keys or certificates. On the first launch `scripts\make_certs.ps1` (OpenSSL from Git for Windows) creates your own:
- `certs\ca.crt`: a local CA, added to your Windows *CurrentUser* Trusted Root store (the game client checks the server against it). Its private key is deleted right after signing.
- `certs\server.pem` and `certs\server.key`: the server certificate for `127.0.0.1` / `localhost`.

Run `powershell -File scripts\make_certs.ps1 -Force` to make new ones. Details: [`certs/README.md`](certs/README.md).

### 4. Game content

`content\` holds the game numbers the servers use (`content.db`, zipped in two parts because GitHub limits files to 100 MB), the mappings file `aoc.usmap` and a few small tables (mount speeds, roads, NPC motion). `build_all.bat` and every launcher unpack them into `dist\content` (`scripts\install_content.ps1`, checked against `content\content.db.sha256`).

**Navigation mesh (required for combat).** `area_nav.bin` and `area_geom.bin` (1.2 GB unpacked, built from the game files by `tools\mapc`) are too big for the repo and ship as a GitHub Release asset, `sandal-core-navmesh-v1.zip` (about 456 MB, tag `navmesh-v1`). `worldd` starts its combat system only after `dist\content\area_nav.bin` loads; **without it every mob has 0 HP and damage does nothing** (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)). `scripts\install_content.ps1` (run by `build_all.bat` and every launcher) gets it for you on the first launch and checks it against `content\navmesh.json` (SHA-256 of the zip and of both files). It looks for the zip, in this order:

1. the file named by the environment variable `SANDAL_NAVMESH_ZIP` (a full path);
2. `content\sandal-core-navmesh-v1.zip`, a copy you downloaded by hand from the repository's Releases page (it stays there and is not committed);
3. a download from the `url` in `content\navmesh.json` (curl.exe, or Invoke-WebRequest when curl is missing); this step is skipped while the url is still the placeholder.

If none works, the script prints a warning that combat is OFF and the rest of the server starts anyway. Later launches only compare a small stamp file (`dist\content\navmesh.ok`), so they do not re-hash 1.2 GB. `install_content.ps1 -Force` unpacks everything again, `-SkipNavmesh` leaves the mesh out.

**For the repository owner, when publishing:** the url in `content\navmesh.json` is a placeholder (`https://github.com/OWNER/REPO/releases/download/navmesh-v1/sandal-core-navmesh-v1.zip`).

1. On GitHub: Releases > Draft a new release > create the tag `navmesh-v1` > attach `sandal-core-navmesh-v1.zip` (the file the `zip_sha256` and `zip_size` in `navmesh.json` describe) > Publish.
2. In `content\navmesh.json` replace `OWNER/REPO` in `url` with your account and repository name, and commit it. Leave the other fields as they are unless the zip is rebuilt (then update `zip_sha256`, `zip_size` and the two file hashes too).

### Configuration & Environment Variables

Key runtime environment variables and settings include:
- `GAME_ROOT`: Path to your legally obtained Ashes of Creation game client (e.g. `C:\Ashes of Creation\Game` or the default Steam directory).
- `AOC_CLIENT_CONFIG`: Path to client configuration INI file.
- `AOC_ZOI_CHUNKS`: Path to chunk directory for Zone-of-Interest generation.
- `AOC_CONTENT_DB`: Path to game content database fixtures (defaults to `dist\content`).
- `AOC_MIRROR_LOGS_ROOT`: Log mirror destination (defaults to `logs\mirror`).
- **Database Connection**: Configured via `deploy/docker-compose.yml` or connection string (`postgres://aoc:aoc@127.0.0.1:5432/aoc?sslmode=disable`).
- **Network Ports**: gRPC listen port (`--port 443`), UDP game server (`--game-port 7777`), game IP (`--game-ip 127.0.0.1`).
- **Launcher Path**: Configured in `config/paths.json` (auto-detected from `GAME_ROOT`).

---

## Captures

The repository includes a curated collection of anonymized packet captures used for wire-format validation, replay diffing, and schema generation. Their text hexdumps (`hexdumps.zip`, 152 MB) are a separate download on the [navmesh-v1 release](https://github.com/cyborqz/SandalCore/releases/tag/navmesh-v1).

For details on the capture format, organization, and verification, see [`captures/README.md`](captures/README.md).

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

Not affiliated with or endorsed by Intrepid Studios; Ashes of Creation is their trademark; no game client, game assets or Intrepid code included; use your own legally obtained client; for preservation and education.
