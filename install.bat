@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

REM Installs Sandal Core on this PC in one go: checks the tools it needs and offers to install what is missing (winget, asking before each),
REM builds the game data from your own Ashes of Creation install, then builds the programs into dist\Release. It skips whatever is up to date,
REM so it is safe to run again; run it again after a game patch or an update. The work is in scripts\install.ps1. Then run launch.bat.
REM   install.bat -CheckOnly                      only report the tools, the game data and the programs; change nothing
REM   install.bat -GameRoot "<the Game folder>"   the game is in an unusual folder (that run only)
REM   install.bat -Yes -SkipDocker                install every missing tool without asking / leave Docker out
REM   install.bat -Config Debug -Clean -Configure -Test -Jobs 6      build options
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" %*
set "RC=%errorlevel%"
pause
exit /b %RC%
