@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

rem ============================================================================
rem  launch_worldd.bat - launch_go.bat worldd, for double-clicking
rem
rem  Starts the Go services with worldd as the world server (not aoc_server),
rem  the tether and the launcher window; sign in and press Play.  Arguments are
rem  passed on (e.g. launch_worldd.bat native); quick is refused, as in
rem  launch_go.bat worldd.
rem ============================================================================
call "%~dp0launch_go.bat" worldd %*
exit /b %errorlevel%
