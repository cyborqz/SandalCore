@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

rem ============================================================================
rem  scripts\launch_worldd.bat - the whole stack (launch.bat in the repository root checks the build, then calls this)
rem
rem  Starts the Go services with worldd as the world server, the tether and the
rem  launcher window; sign in and press LAUNCH GAME.  Takes no arguments.
rem ============================================================================
call "%~dp0launch_all.bat" %*
exit /b %errorlevel%
