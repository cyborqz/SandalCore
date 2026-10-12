@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

rem ============================================================================
rem  play.bat - quickest way into the game for testing
rem
rem  Native mode without the launcher window; the client is given the game
rem  server's address and connects straight to it (launch_all.bat native quick).
rem ============================================================================
call "%~dp0launch_all.bat" native quick
