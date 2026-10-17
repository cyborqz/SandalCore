@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

REM Builds the game data from your own Ashes of Creation install: content.db, the tables, the navigation mesh and the sources the build includes.
REM See scripts\extract_game_data.ps1 (what it makes, what it needs). It skips whatever is already up to date, so it is safe to run again.
REM Extra arguments go to the script, for example:  extract_game_data.bat -GameRoot "D:\Games\Ashes of Creation\Game"
REM                                                  extract_game_data.bat -CheckOnly      (say what would run)
REM                                                  extract_game_data.bat -Only navmesh   (one step)
REM                                                  extract_game_data.bat -Force          (run everything again)
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\extract_game_data.ps1" %*
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    echo.
    echo The game data could not be built - see the messages above.
)
pause
exit /b %RC%
