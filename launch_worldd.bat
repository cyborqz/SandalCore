@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

REM Starts Sandal Core with worldd as the world server: the Go services (PostgreSQL + NATS through Docker),
REM worldd, the tether and the launcher window. Sign in and press Play. Arguments are passed on to
REM scripts\launch_worldd.bat (e.g. launch_worldd.bat native).
setlocal
set "MISSING="
for %%E in (auth_server tether_server launcher worldd) do if not exist "%~dp0dist\Release\%%E.exe" set "MISSING=1"
if defined MISSING (
    echo The server is not built yet ^(dist\Release is missing programs^).
    echo Run install_dependencies.bat, then build_all.bat, then this file again.
    pause
    exit /b 1
)
call "%~dp0scripts\launch_worldd.bat" %*
exit /b %errorlevel%
