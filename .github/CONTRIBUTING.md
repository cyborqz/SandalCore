# Contributing to SandalCore

Thank you for your interest in contributing to SandalCore! SandalCore is an independent, open-source server emulator compatible with the Ashes of Creation client, developed for preservation and educational research.

---

## Code of Conduct & Safety Guidelines

To keep the repository clean, legal, and safe for everyone, all contributors must strictly adhere to the following rules:

1. **No Proprietary Game Assets**: Never submit game client binaries, game paks, copyrighted audio, 3D meshes, textures, or official game assets.
2. **No Unreal Engine Source Code**: Never copy code from the proprietary Unreal Engine source tree into this repository.
3. **No Personal Data or Secrets**: Never include personal identification, real names, personal file paths, non-loopback IP addresses, MAC addresses, credentials, private keys, or session tokens in code, comments, test fixtures, or pull request descriptions.
4. **One Feature or Fix per Pull Request**: A pull request holds one feature or one fix. Split larger work into several pull requests that each build and pass the tests on their own. Do not include refactors the change does not need.
5. **License Agreement**: All contributions are submitted under and licensed pursuant to the **GNU Affero General Public License version 3 (AGPL-3.0-only)** along with the additional terms specified in [`NOTICE`](../NOTICE).
6. **Only What Is in the Repository**: Code comments, commit messages and pull request descriptions refer only to what is in this repository. Do not cite ids or numbers of outside recordings or logs, local file paths or private notes.
7. **Several Pull Requests That Fit Together**: If you open more than one pull request, make sure they do not conflict when they are applied one by one. Base each on the current `main`; when one is merged, rebase the others onto the new `main` and check that they still build and pass the tests. If one depends on another, say so in its description.
8. **Modular Code**: Build each feature as a module as much as possible: new files of its own, a small entry point into the shared code, and as few edits to shared files as possible (see "Where New Code Goes"). Modular changes are easier to review, and they keep pull requests from conflicting with each other.
9. **Up to Date With `main`**: A pull request is based on the current `main` with a linear history: rebase your branch, do not merge `main` into it. When `main` moves on and your pull request conflicts or falls behind, rebase it yourself.

---

## Development Workflow

### Building and Testing

Before submitting changes, ensure your modifications compile cleanly and pass all test suites.

Run `install.bat` once first (`powershell -File scripts\extract_game_data.ps1` makes only the game data): the build and the tests need the game data it makes from your own Ashes of Creation install. Everything it writes is git-ignored (`dist/` and the files listed in `.gitignore`); never commit any of it.

- **Go Services** (in `services/`):
  ```bash
  go vet ./...
  go test ./...
  ```
- **C++ Components**:
  Build using CMake and run the test suite:
  ```powershell
  powershell -File scripts/run_tests.ps1
  ```
- **Python / PowerShell Scripts**:
  Verify scripts execute without syntax or parsing errors.

### Continuous Integration

Pull requests are built and tested by two GitHub Actions workflows (`.github/workflows/`) on GitHub's own machines. Those machines have no game data, so the workflows run only what works without it. The Go workflow runs on every pull request; the C++ workflow runs when a pull request changes C++, CMake or `vcpkg.json` files (the `paths:` filter in `cpp.yml`):

- **Go** (`go.yml`): `gofmt`, `go vet`, `go build`, `go test` against a PostgreSQL service container, and `govulncheck`. The build uses the tag `aoc_standin`, which replaces the one Go file made from a game install (`services/internal/citynode/constants_gen.go`) with placeholder values. The few tests that need the real values skip themselves and print why.
- **C++** (`cpp.yml`): builds everything with MSVC and runs the test programs that need no game data. `scripts/ci_standin_data.ps1` writes placeholder files where the extractor writes the game-derived headers and test files, and it refuses to run on a machine that has game data. `scripts/ci_run_tests.ps1` runs the test programs and lists the ones it does not run (`scripts/ci_needs_game_data.txt`).

A green run means the code builds and the tests that need no game data pass. It does not cover what needs a machine with the game installed: the test programs in `scripts/ci_needs_game_data.txt`, the tests that read the content database, the walking map or recorded packets, the checks of the generated files, and the runs in the game itself. Those checks are not part of the pull request run.

When you add a test that needs the game data, add its program to `scripts/ci_needs_game_data.txt` (C++) or call `standin.Skip` in it (Go). When a generated Go constant or header is added or renamed, update its placeholder too: `services/internal/citynode/constants_standin.go` and `scripts/ci_standin_data.ps1`.

To try the Go job on a machine without game data: `cd services && go test -tags aoc_standin ./...`. Never use the tag where the game data exists: the compiler stops with "redeclared" errors.

### Where New Code Goes

Put a new feature into source files of its own and register it the way the existing features do, instead of growing the big files:

- The world server's player session has one file per feature (`server/worldd/session/player_<feature>.cpp`). A feature registers the client calls it answers in its own file (`register_<feature>_rpcs`, listed once in `server/worldd/session/rpc_registered.inc`) and lists its file in `server/worldd/session/session_sources.cmake`.
- `server/worldd/CMakeLists.txt` is also an input of the walking map build, so a change there makes every player rebuild the map. List new world-server files in `server/worldd/session/session_sources.cmake` instead (with `${CMAKE_CURRENT_SOURCE_DIR}/<folder>/<file>`).
- Tests are registered from files of their own (`server/worldd/tests/README.md`).
- `scripts/hotspot_limits.txt` lists the files that were split and the most lines each may have; `scripts/check_hotspots.ps1` reports a file that grew past its limit (CI runs it).

### Commit Messages

- Use clear, past-tense commit titles and body lines, as the project's own history does: e.g., `Fixed harvest channel cancel state`, `Added validation for mount possession RPC`.
- Include the rationale behind non-obvious fixes.

---

## Submitting Pull Requests

1. Fork the repository and create your branch from `main`.
2. Rebase onto the current `main`, then make sure the code compiles with zero warnings or errors and every test passes on your own machine (both commands under "Building and Testing"). The GitHub Actions checks on your pull request must pass too.
3. Review your diff thoroughly to verify no credentials, personal paths, or proprietary files were added.
4. Open a pull request using the project's [Pull Request Template](PULL_REQUEST_TEMPLATE.md).
5. Explain what changed and why, linking to relevant GitHub issues if applicable.
6. A pull request that is not based on the current `main`, fails a check, mixes unrelated changes or leaves the template's checklist unanswered is sent back with a request for changes. A pull request with no update for 14 days after such a request may be closed.

---

## Reporting Bugs & Security Vulnerabilities

- **Bug Reports**: Open an issue using our [GitHub Bug Report Form](ISSUE_TEMPLATE/bug_report.yml). Provide clear reproduction steps, commit hash, and sanitized logs.
- **Security Vulnerabilities**: Do **not** post security bugs to public issues. Please report security issues privately via GitHub's private vulnerability reporting feature as described in [`SECURITY.md`](./SECURITY.md).
