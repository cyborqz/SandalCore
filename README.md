# SandalCore

An independent, open-source server emulator compatible with the Ashes of Creation client, developed by Cyb.

> [!IMPORTANT]
> **Not affiliated with Intrepid Studios.** SandalCore is an independent project. It is not affiliated with, sponsored by or endorsed by Intrepid Studios, Inc. *Ashes of Creation* is a trademark of Intrepid Studios, Inc., named here only to say which game client this server works with. The repository contains no game client, game files or Intrepid code: the data the servers need is built on your PC from your own installed copy of the game.

> [!WARNING]
> **Work in Progress**: SandalCore is **NOT** a final product. It is under active and consistent development; expect bugs, incomplete features, and ongoing protocol refinements. Please refer to [ROADMAP.md](docs/ROADMAP.md) for milestone progress and [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) for current known limitations, workarounds, and backlog items.

---

## 🗺️ Roadmap

**5 of 16 phases done** · 8 in progress · 3 planned

🟩🟩🟩🟩🟩🟨🟨🟨🟨🟨🟨🟨🟨⬜⬜⬜

```mermaid
flowchart LR
    A["🔌 Getting in<br/>3 of 3 done"]:::done
    B["🧙 Your character<br/>2 of 4 done · 2 in progress"]:::wip
    C["🌍 The world<br/>4 in progress"]:::wip
    D["🤝 Playing together<br/>2 in progress"]:::wip
    E["⚔️ Wars and world events<br/>3 planned"]:::todo
    A --> B --> C --> D --> E
    classDef done fill:#1a7f37,stroke:#116329,color:#ffffff
    classDef wip fill:#d4a72c,stroke:#9a6700,color:#1f2328
    classDef todo fill:#6e7781,stroke:#57606a,color:#ffffff
```

| Stage | | Phase | What it brings |
|---|:-:|---|---|
| 🔌 **Getting in** | ✅ | **1 · Network protocol** | How the client and the server talk: the handshake, packet framing and replication |
| | ✅ | **2 · Login** | The launcher, accounts, character creation and character select |
| | ✅ | **3 · Into the world** | Entering the world as your character, with your progress kept across logins |
| 🧙 **Your character** | ✅ | **4 · Stats** | The game's own stat formulas, updated as your gear changes |
| | ✅ | **5 · Mounts** | Ground, flying and swimming mounts, sprinting and roads |
| | 🛠️ | **6 · Progression and items** | XP and levels, talents, the action bar, bags, item rarities, weapon XP, scrolls and elixirs. *Still to come: splitting and joining stacks, food and potions.* |
| | 🛠️ | **7 · Combat and abilities** | Most classes' abilities, weapon combos and finishers, buffs, death and respawn. *Mobs take the Burning debuff, the bleeds, poisons and stat debuffs of a hit, and the Summoner's Guardian, Hunter and Mystic fight at their owner's side. Still to come: slows, snares and CC on mobs (roots, stuns; the weapon finisher talents' slows among them).* |
| 🌍 **The world** | 🛠️ | **8 · Gathering and professions** | Mining, logging, herbalism and hunting, and processing and crafting at the towns' stations. *Still to come: fishing and the rest of the 22 professions.* |
| | 🛠️ | **9 · Living world** | Navigation, and mobs that see, hear, chase and fight with their own attacks, with loot and respawns. *Still to come: the other mob families, flying mobs, and day and night spawns.* |
| | 🛠️ | **10 · City nodes** | Node levels and XP, and the five towns with their people, buildings and crafting stations. *Still to come: citizenship, mayors, taxes and vassal nodes.* |
| | 🛠️ | **11 · Quests** | NPC conversations, quest offers, objectives, rewards and map markers, for the first quests of the starting area. *Still to come: more quests, group quests and events.* |
| 🤝 **Playing together** | 🛠️ | **12 · Economy** | Gold, silver, copper and Glint, the NPC vendors, crates and storage. *Still to come: node currencies and more crates.* |
| | 🛠️ | **13 · Social** | Players within 150 m of each other see each other as their characters, standing, walking and stopping. *Still to come: riders on their mounts, gear and level changes, live Health and Mana, death and combat stances, abilities and hits on other players, guilds, parties and raids, chat and mail, player trading and the marketplace.* |
| ⚔️ **Wars and world events** | ⬜ | **14 · Node sieges** | Node sieges and territory wars |
| | ⬜ | **15 · Caravans** | Caravans and trade routes |
| | ⬜ | **16 · World events** | Corruption events and other world events |

✅ Done · 🛠️ In progress · ⬜ Planned. Each phase in detail, and what is left, is in [ROADMAP.md](docs/ROADMAP.md).

📋 **Everything that works today**, area by area, is in [docs/WHAT_WORKS.md](docs/WHAT_WORKS.md).

---

## 🚀 Getting started (Windows)

**You need** a Windows 10/11 x64 PC with your own installed copy of Ashes of Creation. SandalCore contains no game data: the first build reads your installed client (the files are only read, nothing in the game folder is changed) and builds the data the servers need.

```mermaid
flowchart LR
    A["1️⃣ Install<br/>install.bat"] --> B["2️⃣ Play<br/>launch.bat"]
    classDef step fill:#0969da,stroke:#0550ae,color:#ffffff
    class A,B step
```

| Step | Run | What it does |
|:-:|---|---|
| **1** | `install.bat` | Checks the tools below and offers to install what is missing (through winget, asking before each item), makes the game data from your own game, then builds `tether_server`, `launcher` and `worldd` into `dist\Release`. **The first run** takes about 10 minutes for the game data on a fast PC, plus the first build of the C++ libraries through vcpkg (grpc compiles from source, so this part can take a long time). **Later runs** take seconds when nothing changed; run it again after a game patch or an update. `install.bat -CheckOnly` only reports (the tools, the game data, whether the programs are built) and changes nothing; `-SkipDocker` leaves Docker out (if you run PostgreSQL 15+ yourself) and `-Yes` installs every missing tool without asking. |
| **2** | `launch.bat` | Starts PostgreSQL (Docker), the Go services, the `worldd` world server, the tether and the launcher window. Sign in (or let "Keep me signed in" do it) and press **LAUNCH GAME**, which starts the game. |

> [!TIP]
> On the first launch your own TLS certificates are created in `certs\`: Windows asks you once to trust the local CA. Choose **Yes**.

To make only the game data (for example before `go test ./...` or `scripts\run_tests.ps1`), run `powershell -File scripts\extract_game_data.ps1` on its own.

### 🧰 What you need

| What | Version | What it is for |
|---|---|---|
| **Ashes of Creation** (Steam) | the build of 2026-01-29 | the source of the game data; it has to be installed on this PC |
| **Visual Studio 2022 Build Tools**, workload "Desktop development with C++" | MSVC v143 | the C++ programs |
| **CMake** | 3.25 or newer (the copy of Visual Studio works) | the C++ build |
| **Git** | any current version | fetches CUE4Parse and vcpkg |
| **[vcpkg](https://github.com/microsoft/vcpkg)** | current | the C++ libraries (grpc, protobuf, openssl, ...); found through `VCPKG_ROOT`, `%USERPROFILE%\vcpkg`, `C:\vcpkg`, `\vcpkg` on the repository's drive, or the Visual Studio copy |
| **Go** | 1.27.3 or newer (`services/go.mod`; `install.bat` upgrades an older Go, or take the installer from [go.dev/dl](https://go.dev/dl/)) | the backend services and `contentc` |
| **.NET SDK** | 8.0 (any 8.x; with the 8.0 runtime) | `tools/mapc`, `tools/itemnames` and `tools/npcabilities`, which read the game's files |
| **Python** | 3.10 or newer | the generators that `scripts\extract_game_data.ps1` runs |
| **Docker Desktop** | current | PostgreSQL, to run the servers (not needed to build); or run PostgreSQL 15+ yourself |

- 💾 **Disk:** about 20 GB free on the repository's drive for the first build. The game data is 1.5 GB and the finished C++ build about 12 GB; vcpkg needs more while it compiles the libraries.
- 🧠 **Memory:** at least 6 GB free while the game data is built (the biggest program uses about 4 GB).
- 🌐 **Network:** needed once, on the first run (what is fetched is listed under "The game data" below). Nothing is downloaded after that.

<details>
<summary><b>🩺 If something goes wrong</b></summary>

| You see | Why | What to do |
|---|---|---|
| `The Ashes of Creation client was not found` | the game is not installed on this PC, or it sits in an unusual folder | install it (Steam), or run `install.bat -GameRoot "D:\...\Ashes of Creation\Game"` (that run only), or set `GAME_ROOT` to that folder for good (`setx GAME_ROOT "D:\...\Ashes of Creation\Game"`, then open a new window) |
| `-GameRoot '...' is not the game's folder` | the folder has no `AOC\Content\Paks` or no `CacheDB.dbc` | give the folder that holds `AOCClient.exe` and `AOC` (the `Game` folder of the install), not the launcher folder; if the install is incomplete, let Steam verify the game files |
| `WARNING: this client is not the build this code was made for` | the game was patched, or it is another build | the script goes on; the data may be wrong or a step may fail (see "After a game patch" under "The game data") |
| `Missing: ...` followed by `Python 3.10 or newer`, `the .NET 8 SDK` or `Go` | that tool is not installed or not on `PATH` | run `install.bat` (it offers to install what is missing), open a new window (so the new `PATH` applies), run it again |
| CMake: `These files are made from your own Ashes of Creation install and are missing` | the C++ build ran before the game data was made | run `install.bat` (it makes the game data first, then builds) |
| `ERROR in step '...'` | a step failed | read `dist\content\.extract\last_run.log` and fix the cause; run again: finished steps are kept and the run continues at the failed one |
| `waiting: ...` in the map step | the map read waits until at least 6 GB of memory are free and the game, a build and `aoc_navbuild` are not running | close them; it continues by itself and gives up after 40 minutes |
| the CUE4Parse fetch fails | no network or no Git | connect and run again, or copy the folder yourself: `powershell -File tools\mapc\fetch_cue4parse.ps1 -From <folder with CUE4Parse>` |
| every mob has 0 HP and damage does nothing | `dist\content\area_nav.bin` is missing | run `install.bat` (or `powershell -File scripts\extract_game_data.ps1`, without `-Skip navmesh`) |

</details>

<details>
<summary><b>🎮 Where the game is found</b></summary>

The scripts look in this order:
1. `install.bat -GameRoot "D:\...\Ashes of Creation\Game"` (that one run only);
2. the environment variable `GAME_ROOT`;
3. `game_executable` in `config\paths.json` (the launcher's Auto-Detect and Browse buttons save the path in `dist\Release\config\paths.json`, which is read too);
4. Steam's libraries (found through the registry and `libraryfolders.vdf`);
5. the usual folders on every fixed drive.

The game folder is the one that holds `AOCClient.exe` and the `AOC` folder; with Steam it is `...\steamapps\common\Ashes of Creation\Game`. If your game is in an unusual place, set `GAME_ROOT` once, with `setx GAME_ROOT "D:\...\Ashes of Creation\Game"`, and open a new window: then `install.bat` and the launchers all find it.

</details>

<details>
<summary><b>📦 The game data: what it makes, how long it takes, what runs again</b></summary>

**What it makes.** All of it is git-ignored ([`.gitignore`](.gitignore) lists the generated sources); the files are in `dist\content\`:

| File | What it holds | Made by |
|---|---|---|
| `content.db` (323 MB) | the game numbers the servers use | `contentc`, from the client's `CacheDB.dbc` |
| `*.tsv` | mount speeds and riders, profession curves, NPC motion, foliage and roads | `tools\mapc` (from the game's containers) and `contentc roads` |
| `item_names.tsv` | the words the Glint kinds and some quest items are named with | `tools\itemnames` (from the game's containers) |
| `npc_abilities.tsv`, `npc_montages.tsv`, `npc_targeting.tsv`, `npc_senses.tsv`, `npc_aggro_curves.tsv` | what the mobs attack with, how their attack animations carry them, where each hit lands, and what each mob notices and how far it follows | `tools\npcabilities` (from the game's containers and `content.db`) |
| `npc_looks.tsv`, `npc_appearances.tsv`, `npc_names.tsv`, `npc_trees.tsv` | how the people (humanoid NPCs) look, what they are called, and whether they wander or patrol | `tools\npcabilities` (from the game's containers and `content.db`) |
| `town_platforms.bin` (4 MB) | the floor of the five towns | `tools\mapc` (from two of the game's map packages) |
| `town_layout_items.tsv` (6 MB), `town_layouts.bin` (13 MB) | the buildings, walls and terraces of the five towns at the level each town starts at | a list made from the design data by `tools\capture\gen_town_layouts.py`, then their collision from the game's packages by `tools\mapc` |
| `area_nav.bin` (974 MB), `area_geom.bin` (232 MB) | the navigation mesh, with the towns' buildings in it | `tools\mapc` and `aoc_navbuild` |
| `client_config\`, `zoi_chunks\` | the game's config files and its zone grid, which the generators read | |

It also makes the generated sources and test fixtures the build includes (for example `zoi_surface_table.h`, `node_constants.h`, `region_data.h`).

**How long.** About 10 minutes the first time on a fast PC (24 threads, 31 GB: 10.3 minutes in all, of which the map read and the navigation mesh take 8); a slower PC takes longer. Every step is skipped when what it reads has not changed, so running it again takes about a second.

**What runs again.** Every step keeps a record of what it read (in `dist\content\.extract\`) and runs again only when that changed. The navigation mesh step, the longest, reads the map, the five towns' floors and buildings, the Riverlands' rectangles and, in `content.db`, where the five towns' nodes stand. When only other game numbers change (abilities, items, quests, mobs), `content.db` and the files made from it are made again, but the navigation mesh is left as it is, which saves the 8 minutes it takes on a fast PC.

Run `powershell -File scripts\extract_game_data.ps1` by hand to use its options (`install.bat` runs it without them):

| Option | What it does |
|---|---|
| `-CheckOnly` | says (lists) which steps would run, and changes nothing |
| `-Only <step>` | runs one step |
| `-Skip <step>` | leaves one step out |
| `-Force` | runs everything again |

The steps are `tools`, `config`, `curves`, `names`, `database`, `region`, `headers`, `fixtures`, `navbuild`, `roads`, `townfloors`, `townlayouts`, `npcabilities`, `navmesh`. The output of every program is kept in `dist\content\.extract\`; `last_run.log` has all of it.

**Network.** The first run needs it once, for CUE4Parse (a git clone of github.com/FabianFG/CUE4Parse at a pinned commit; the script keeps its 7.5 MB of source), the NuGet packages that `tools\mapc\mapc.csproj` names (from nuget.org), the Go modules that `services\go.mod` names, and, in the first C++ build, the vcpkg libraries. Nothing is downloaded after that.

**After a game patch.** Run `install.bat` again. Its game data step sees that the game files changed and rebuilds everything that depends on them, about 10 minutes. The code is made for the client build of 2026-01-29 (`AOCClient-Win64-Shipping.exe` 235,606,624 bytes, `CacheDB.dbc` 144,085,052 bytes). With any other build the script prints a warning and goes on; the data can then be wrong or a step can fail.

**What is in the repository.** `layouts\aoc.usmap` holds the names, types and layouts of the game's classes, structs and enums, as `libs\aocnet\protocol\schema\generated\` and `services\internal\designdb\exe_gen.go` do for the network protocol and the design database; none of the three holds game content.

</details>

<details>
<summary><b>🏘️ The navigation mesh and the towns' levels</b></summary>

> [!IMPORTANT]
> `worldd` starts its combat system only after `dist\content\area_nav.bin` loads; **without it every mob has 0 HP and damage does nothing** (see [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)). The `navmesh` step builds it; `-Skip navmesh` of `scripts\extract_game_data.ps1` leaves it out.

The navigation mesh holds each of the five towns with the buildings of the level the server starts that town at, the one `deploy\citynode_seed.json` gives it (Joeva 4, Town; Halcyon 3, Village; the other three 0, Wilderness). If a GM changes a town's level in the game (`nodectl set-level`), the town in the game changes with it, but the navigation mesh keeps the buildings of the level it was made for: people can then walk where a new house stands or stand on a floor that is gone, until the mesh is made again. To make it for other levels, change the levels in the seed file and run `install.bat` again (the buildings and navigation mesh steps run, about 10 minutes). A level change in the game that swaps the mesh by itself is a later update.

</details>

<details>
<summary><b>🛠️ More build options</b></summary>

| What | How |
|---|---|
| **Everything** | `install.bat` (the tools, the game data, then `powershell -File scripts\build.ps1 -Target all`) |
| **One program** | `powershell -File scripts\build.ps1 -Target worldd` (targets: `tether_server`, `launcher`, `worldd`; `-Target launcher` builds just the launcher). The game data has to be made first. |
| **Options** | `-Config Debug`, `-Clean`, `-Configure` (re-run CMake), `-Test`, `-Jobs N` (build at most N projects and at most N compiler processes at a time; without it the compilers use as many processors as the free memory allows); `install.bat` passes them on. |
| **Go services** (`services/`) | `go build ./...`, `go vet ./...`, `go test ./...` (the launch scripts build and start them for you) |
| **Tests** | `powershell -File scripts\run_tests.ps1` builds and runs every C++ test (add `-Jobs N` to limit the build); the Go tests are `go test ./...` in `services\` |
| **Logs** | `powershell -File scripts\save_run_logs.ps1` saves the logs of the last run (the tether, the Go services, `worldd` and the game client) into `logs\run_<time>\`, with a `run_summary.txt`; `powershell -File scripts\auto_logger.ps1` keeps doing it while the server runs (the launch already starts it) |

**Why it is fast:** the build compiles the files of one project in parallel, and it copies the libraries' DLLs into `dist\Release` once after the build instead of after every program. A `build\x64` folder made by an earlier version keeps working as it was; `build.ps1 -Configure` switches it over.

**Plain CMake** (it works, but it is slower: one file at a time inside a project, and the DLLs are copied after every program):
```bat
cmake -S . -B build\x64 -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build\x64 --config Release
```

</details>

<details>
<summary><b>🖥️ Running the server: signing in, services only, certificates, settings</b></summary>

**Start everything:** `launch.bat` checks that the programs are built, then starts the full stack with `worldd` as the world server; `scripts\launch_all.bat` does the work. The launcher window opens: sign in there and press LAUNCH GAME, which starts the game client. With "Keep me signed in" ticked the sign-in is saved in Windows Credential Manager (`SandalCore/LauncherSignIn`, for your Windows account only) and the launcher signs in by itself the next time; Logout forgets it. The client is started with `-UseEOS=false`, so it runs without Epic Online Services; no game files are modified.

**Backend services only:**
```powershell
powershell -File scripts\services.ps1 up     # PostgreSQL (Docker), waits until healthy
powershell -File scripts\services.ps1 stop
```

**TLS certificates.** SandalCore ships no keys or certificates. On the first launch `scripts\make_certs.ps1` (OpenSSL from Git for Windows) creates your own:
- `certs\ca.crt`: a local CA, added to your Windows *CurrentUser* Trusted Root store (the game client checks the server against it). Its private key is deleted right after signing.
- `certs\server.pem` and `certs\server.key`: the server certificate for `127.0.0.1` / `localhost`.

Run `powershell -File scripts\make_certs.ps1 -Force` to make new ones. Details: [`certs/README.md`](certs/README.md).

**Settings and environment variables:**

| Setting | What it is |
|---|---|
| `GAME_ROOT` | the folder of your installed Ashes of Creation client that holds `AOCClient.exe` and `AOC\` (for example `C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game`); `install.bat` and the launchers use it before they search for the game |
| `AOC_CLIENT_CONFIG` | the game's `AOC\Config` folder (default `dist\content\client_config`, written by the game data step of `install.bat`) |
| `AOC_ZOI_CHUNKS` | the zone-of-influence chunk folder (default `dist\content\zoi_chunks`, written by the game data step of `install.bat`) |
| `AOC_CONTENT_DB` | the path to the game content database fixtures (default `dist\content`) |
| `AOC_MIRROR_LOGS_ROOT` | the log mirror's destination (default `logs\mirror`) |
| Database connection | `deploy/docker-compose.yml` or the connection string (`postgres://aoc:aoc@127.0.0.1:5432/aoc?sslmode=disable`) |
| Launcher path | `config/paths.json` (auto-detected from `GAME_ROOT`) |

**Network ports:**

| What | Port |
|---|---|
| the gateway (the game client's session, over TLS) | TCP 443 |
| the auth service's HTTP API | TCP 8081 |
| `worldd` (`worldd --bind`) | UDP 7777 |
| the tether | UDP 19021 |

</details>

---

## Captures

SandalCore includes no packet captures. The community's own repository holds the plaintext captures of the live game that this project's protocol work started from: [AshesEmulatorTeam/PCAPRepo](https://github.com/AshesEmulatorTeam/PCAPRepo), captured by the Creation of Ashes (CoA) community. Thank you.

---

## Contributing

We welcome contributions from the community! Please read [`CONTRIBUTING.md`](.github/CONTRIBUTING.md) for our code of conduct, branch conventions, and testing requirements before opening a pull request.

---

## Reporting Bugs

- **Bug Reports**: Open a new issue using our [GitHub Bug Report Form](.github/ISSUE_TEMPLATE/bug_report.yml).
- **Security Vulnerabilities**: Do **not** open public issues for security concerns. Report them privately according to [`SECURITY.md`](.github/SECURITY.md).

---

## Credits

Developed by Cyb. Special thanks to Hatemost for building the foundation and CoA (Creation of Ashes) / the Ashes community who participated in capturing the PCAPs.

---

## License

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License version 3 (AGPL-3.0-only)** with additional terms under Section 7. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE) for the full license text and terms.

---

## Disclaimer

Not affiliated with, sponsored by or endorsed by Intrepid Studios, Inc.; Ashes of Creation is their trademark, named only to identify the compatible client; no game client, game files or Intrepid code included; the game data is built on your PC from your own legally obtained client; for preservation and education.
