# Contributing to Sandal Core

Thank you for your interest in contributing to Sandal Core! Sandal Core is an open-source server emulator for Ashes of Creation developed for preservation and educational research.

---

## Code of Conduct & Safety Guidelines

To keep the repository clean, legal, and safe for everyone, all contributors must strictly adhere to the following rules:

1. **No Proprietary Game Assets**: Never submit game client binaries, game paks, copyrighted audio, 3D meshes, textures, or official game assets.
2. **No Unreal Engine Source Code**: Never copy code from the proprietary Unreal Engine source tree into this repository.
3. **No Personal Data or Secrets**: Never include personal identification, real names, personal file paths, non-loopback IP addresses, MAC addresses, credentials, private keys, or session tokens in code, comments, test fixtures, or pull request descriptions.
4. **Focused Pull Requests**: Keep pull requests focused on a single logical change or bug fix. Avoid massive indiscriminate refactors.
5. **License Agreement**: All contributions are submitted under and licensed pursuant to the **GNU Affero General Public License version 3 (AGPL-3.0-only)** along with the additional terms specified in [`NOTICE`](./NOTICE).

---

## Development Workflow

### Building and Testing

Before submitting changes, ensure your modifications compile cleanly and pass all test suites.

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

### Commit Messages

- Use clear, imperative commit titles: e.g., `Fix harvest channel cancel state`, `Add validation for mount possession RPC`.
- Include the rationale behind non-obvious fixes.

---

## Submitting Pull Requests

1. Fork the repository and create your branch from `main`.
2. Ensure all tests pass and code compiles with zero warnings or errors.
3. Review your diff thoroughly to verify no credentials, personal paths, or proprietary files were added.
4. Open a pull request using the project's [Pull Request Template](.github/PULL_REQUEST_TEMPLATE.md).
5. Explain what changed and why, linking to relevant GitHub issues if applicable.

---

## Reporting Bugs & Security Vulnerabilities

- **Bug Reports**: Open an issue using our [GitHub Bug Report Form](.github/ISSUE_TEMPLATE/bug_report.yml). Provide clear reproduction steps, commit hash, and sanitized logs.
- **Security Vulnerabilities**: Do **not** post security bugs to public issues. Please report security issues privately via GitHub's private vulnerability reporting feature as described in [`SECURITY.md`](./SECURITY.md).
