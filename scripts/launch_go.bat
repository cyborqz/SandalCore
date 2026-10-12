@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

rem ============================================================================
rem  launch_go.bat - launch_all.bat with the Go services
rem
rem  auth, character, citynode, mesh and gateway (scripts\services.ps1) replace
rem  auth_server and aoc_server's own ICS: the gateway serves the game client's
rem  session, character list and Play on :443.
rem
rem    launch_go.bat                 aoc_server's replay world, launcher window
rem    launch_go.bat native          aoc_server's native world, launcher window
rem    launch_go.bat native quick    no launcher window: signs in with
rem                                  config\dev_login.local.json instead
rem    launch_go.bat worldd          worldd instead of aoc_server (plan A4): the
rem                                  client logs in and loads the map, then waits;
rem                                  the world comes with plan A5.  Build it first
rem                                  (scripts\build.ps1 -Target worldd).  No quick.
rem
rem  quick works only as the second argument. With the launcher window the
rem  game client starts once you have signed in there (up to 5 minutes); an
rem  earlier sign-in's token is deleted first. tether_server runs with
rem  --login-token-file. The Go services and worldd log to dist\services\logs\.
rem ============================================================================
setlocal
set "AOC_STACK=go"
set "AOC_WORLD=legacy"
if /I "%~1"=="worldd" goto :worldd
call "%~dp0launch_all.bat" %*
exit /b %errorlevel%

:worldd
set "AOC_WORLD=worldd"
call "%~dp0launch_all.bat" %2 %3
exit /b %errorlevel%
