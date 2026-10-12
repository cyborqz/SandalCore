@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

setlocal
set "SCRIPT_DIR=%~dp0"

echo ========================================================
echo  Building Server (Release x64) in:
echo  %SCRIPT_DIR%
echo ========================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build_server.ps1"

if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    pause
    exit /b 1
)

echo.
echo Build succeeded.
echo Output: %SCRIPT_DIR%dist\Release\aoc_server.exe
pause
endlocal
