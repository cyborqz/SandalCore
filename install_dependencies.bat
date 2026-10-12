@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

REM Checks for everything Sandal Core needs (Visual Studio 2022 C++ Build Tools, CMake, Git, vcpkg, Go, Docker,
REM Python) and offers to install what is missing with winget. Add -CheckOnly to only report, -Yes to install
REM everything missing without asking, -SkipDocker to leave Docker out.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install_dependencies.ps1" %*
set "RC=%errorlevel%"
pause
exit /b %RC%
