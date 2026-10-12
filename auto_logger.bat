@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\auto_logger.ps1" %*
