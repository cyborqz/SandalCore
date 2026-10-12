@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

REM Builds every server component the launchers need: auth_server, tether_server, launcher, worldd, aoc_server.
REM Output goes to dist\Release. The first build also downloads and compiles the C++ libraries through vcpkg
REM (grpc, protobuf, openssl, ...), which can take a long time. Extra arguments go to scripts\build.ps1
REM (e.g. -Config Debug, -Clean, -Configure).
setlocal
echo Building all Sandal Core server components (Release x64) ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -Target all %*
if errorlevel 1 (
    echo.
    echo BUILD FAILED - see the messages above.
    echo If a tool is reported missing, run install_dependencies.bat first.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install_content.ps1"
if errorlevel 1 (
    echo.
    echo The build worked, but unpacking content\content.db failed - see the message above.
    pause
    exit /b 1
)
echo.
echo Build succeeded. Output: %~dp0dist\Release
echo Next: launch_worldd.bat
pause
exit /b 0
