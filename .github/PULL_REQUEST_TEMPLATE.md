## Description
<!-- What this PR does and why, in a few sentences. -->

## Related Issues
<!-- e.g. Fixes #123, Closes #456 -->

## Changes Made
<!-- Past tense, one line per change, the way the project's history reads: "Added ...", "Fixed ...", "Removed ...", "Updated ..." -->
- 

## Scope
<!-- If you can, keep a PR to one feature or one fix. This is a preference, not a rule: several fixes in one PR are fine, list each one under "Changes Made". -->
- [ ] This PR holds one feature or one fix, or the list above names each one it holds.
- [ ] If I have other open PRs: this one applies cleanly on main and on top of them, one after another, in any order they may be merged (or it names the PR it depends on).
- [ ] New code is modular: the feature lives in files of its own and is registered the way existing features are, with as few edits to shared files as possible.

## Game data
- [ ] This PR does not change the game data, or it says here what changes (the extractor, contentc or its version), so players know they need a data update.

## Verification & Testing
<!-- How you checked the change. Include test commands and an output summary. -->
- [ ] Go tests: `cd services && go test ./...`
- [ ] C++ build and tests: `powershell -File scripts/run_tests.ps1`
- [ ] Python/script syntax: verified clean execution

## Checklist
- [ ] My code follows [CONTRIBUTING.md](CONTRIBUTING.md).
- [ ] My commit titles and the list above are in the past tense.
- [ ] Code comments, commit messages and this description only refer to what is in this repository: no ids or numbers of outside recordings or logs, no local file paths, no private notes.
- [ ] I have not included proprietary assets, game binaries, or Unreal Engine source files.
- [ ] I have verified that no personal data, private IPs, credentials, or tokens are present in diffs.
- [ ] All new and existing tests pass locally.
