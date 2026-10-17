@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

setlocal EnableExtensions EnableDelayedExpansion

if not "%~1"=="" goto :no_arguments

rem ============================================================================
rem  launch_all.bat - start the full stack + game client
rem
rem  Starts the Go services and worldd (the world server), the tether and the
rem  launcher window; sign in there and press Play. The game client starts by
rem  itself once you have signed in. Takes no arguments.
rem
rem  Paths are derived from this script's location. Override the game client
rem  location via GAME_ROOT if yours is not at one of the detected defaults.
rem ============================================================================

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
set "DIST_DIR=%REPO_ROOT%\dist\Release"
set "CONFIG_DIR=%REPO_ROOT%\config"

if not defined GAME_EXE if exist "%CONFIG_DIR%\paths.json" (
    for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "$p='%CONFIG_DIR%\paths.json'; if (Test-Path $p) { (Get-Content $p -Raw | ConvertFrom-Json).game_executable }" 2^>nul`) do (
        if exist "%%~i" set "GAME_EXE=%%~i"
    )
)
if not defined GAME_EXE if exist "%DIST_DIR%\config\paths.json" (
    for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "$p='%DIST_DIR%\config\paths.json'; if (Test-Path $p) { (Get-Content $p -Raw | ConvertFrom-Json).game_executable }" 2^>nul`) do (
        if exist "%%~i" set "GAME_EXE=%%~i"
    )
)
if not defined GAME_EXE (
    if exist "C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game\AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe" (
        set "GAME_EXE=C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game\AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe"
    ) else if exist "C:\Ashes of Creation\Game\AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe" (
        set "GAME_EXE=C:\Ashes of Creation\Game\AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe"
    )
)

if not defined GAME_ROOT (
    if defined GAME_EXE (
        for %%I in ("%GAME_EXE%\..\..\..\..") do set "GAME_ROOT=%%~fI"
    ) else (
        set "GAME_ROOT=C:\Ashes of Creation\Game"
        set "GAME_EXE=%GAME_ROOT%\AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe"
    )
)

if not exist "%DIST_DIR%\" (
    echo ERROR: Build output directory not found.
    echo Expected: "%DIST_DIR%"
    echo.
    echo Build the stack first:
    echo   powershell -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\build.ps1" -Configure
    echo   powershell -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\build.ps1"
    pause
    exit /b 1
)

set "MISSING_BINARIES=0"
if not exist "%DIST_DIR%\tether_server.exe" (
    echo Missing: "%DIST_DIR%\tether_server.exe"
    set "MISSING_BINARIES=1"
)
if not exist "%DIST_DIR%\launcher.exe" (
    echo Missing: "%DIST_DIR%\launcher.exe"
    set "MISSING_BINARIES=1"
)
if not exist "%DIST_DIR%\worldd.exe" (
    echo Missing: "%DIST_DIR%\worldd.exe"
    set "MISSING_BINARIES=1"
)
if "%MISSING_BINARIES%"=="1" (
    echo.
    echo ERROR: Required binaries are missing from "%DIST_DIR%".
    echo Build the stack first:
    echo   powershell -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\build.ps1" -Configure
    echo   powershell -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\build.ps1"
    pause
    exit /b 1
)

cd /d "%DIST_DIR%" || (
    echo ERROR: Failed to change to "%DIST_DIR%".
    pause
    exit /b 1
)

if not exist "logs" mkdir "logs"

rem  *.local.json (config\dev_login.local.json holds a password) stays in the
rem  repo: it is not copied, and a copy left by an older version is deleted.
if not exist "%CONFIG_DIR%\" goto :config_done
robocopy "%CONFIG_DIR%" "config" /E /XF *.local.json /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo ERROR: Failed to copy "%CONFIG_DIR%" to "%DIST_DIR%\config".
    pause
    exit /b 1
)
if exist "config\*.local.json" del /f /q "config\*.local.json"
:config_done

if defined GAME_EXE if exist "%GAME_EXE%" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$path='config\paths.json'; if (Test-Path -LiteralPath $path) { $json = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json } else { $json = [pscustomobject]@{} }; $json | Add-Member -NotePropertyName game_executable -NotePropertyValue $env:GAME_EXE -Force; $json | ConvertTo-Json | Set-Content -LiteralPath $path -Encoding ASCII"
)
if errorlevel 1 (
    echo ERROR: Failed to update launcher game path in "%DIST_DIR%\config\paths.json".
    pause
    exit /b 1
)

rem First launch: build the game data (content.db, the tables, the navigation mesh) from your own game install into dist\content (a no-op once it is there),
rem and create this machine's own TLS certificates in certs\ (local CA trusted for your user; no keys ship with Sandal Core).
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\extract_game_data.ps1" || goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\make_certs.ps1" || goto :fail

echo Cleaning up old emulator processes from this dist ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$dist = (Resolve-Path -LiteralPath $env:DIST_DIR).Path; Get-Process tether_server,launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dist, [System.StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force"

rem  Go services or worldd left running from an earlier launch would hold :8081, :443, :50055 (citynode)
rem  and UDP :7777: stop them first. This needs neither Go nor Docker.
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" stop || goto :fail

echo [1/5] Starting the Go services and worldd: auth, character, citynode, mesh, gateway, worldd ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" up || goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" run || goto :fail
call :wait_tcp 8081 auth 30 || goto :fail
call :wait_tcp 443 gateway 30 || goto :fail
rem  citynode (Nodes 1) serves the city node levels; migration 0009 runs and the seed is applied at its first start
call :wait_tcp 50055 citynode 30 || goto :fail
call :wait_udp 7777 worldd 30 || goto :fail
echo        Go services READY.

rem  The client runs without Epic Online Services.
set "CLIENT_ARGS=-UseEOS=false"
set "TOKEN_FILE=%DIST_DIR%\data\ics_auth_token.txt"
rem  The tether hands the client the login token the launcher saved.
set "TETHER_CMD=tether_server.exe --login-token-file"
set "SIGN_IN_NOTE=Sign in with an account registered through the launcher."

echo.
echo [2/5] worldd serves the world on UDP :7777 (started with the Go services).

echo.
echo [3/5] Starting tether_server.exe (UDP ARQ :19021) ...
start "Tether Server" /D "%DIST_DIR%" /MIN cmd /c "%TETHER_CMD% & pause"

echo        Waiting for tether_server to bind UDP port 19021 ...
call :wait_udp 19021 tether_server 15
if errorlevel 2 goto :fail
if errorlevel 1 (
    echo        WARNING: tether_server did not bind UDP :19021 in time.
) else (
    echo        tether_server READY.
)

rem  A login token left from an earlier sign-in would open the game as that
rem  account, so only this run's sign-in may provide one.
if exist "%TOKEN_FILE%" del /f /q "%TOKEN_FILE%"
if exist "%TOKEN_FILE%" goto :token_not_deleted

echo.
echo [4/5] Opening launcher.exe ...
start "" /D "%DIST_DIR%" "%DIST_DIR%\launcher.exe"

echo.
echo Starting real-time background auto-logger ...
start "AoC AutoLogger" /MIN powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\auto_logger.ps1"

echo.
echo ============================================================
echo  AoC Emulator Stack is Running!
echo.
echo  %SIGN_IN_NOTE%
echo.
echo  Auto-logger is ACTIVE in background:
echo  All 4 application logs are automatically streaming to .txt
echo  files in real-time under:
echo    %REPO_ROOT%\logs\run_YYYYMMDD_HHMMSS\
echo    %REPO_ROOT%\logs\mirror\run_YYYYMMDD_HHMMSS\
echo.
echo  No need to press any key or close anything.
echo ============================================================
echo.

rem  The client asks the tether for its login token some seconds after it starts, so
rem  it starts once the player has signed in (the launcher then writes the token file).
if not exist "%GAME_EXE%" goto :start_game
echo  Sign in through the launcher window: the game client starts by itself
echo  once you have. Do not also press the launcher's Launch button, or a
echo  second game client opens.
echo        Waiting up to 5 minutes for the sign-in ...
call :wait_file "%TOKEN_FILE%" 300 || goto :sign_in_timeout
echo        Signed in.
echo.

:start_game
if exist "%GAME_EXE%" (
    echo [5/5] Launching game client ...
    start "" /D "%GAME_ROOT%\AOC\Binaries\Win64" "%GAME_EXE%" -LauncherTetherPort=19021 %CLIENT_ARGS%
    echo Game client launched.
) else (
    echo NOTE: Shipping binary not found at "%GAME_EXE%". Launch via launcher.exe.
)

exit /b 0

rem  :wait_tcp <port> <process name> <seconds>
rem  :wait_udp <port> <process name> <seconds>
rem  Wait for a TCP listener (UDP endpoint) on the port and check that it is the
rem  named process (image name without .exe) running from this checkout. The
rem  same program from another checkout or worktree, or a service of the other
rem  stack, fails at once: exit 2 (TCP prints its error and exits 1 as well,
rem  its callers treat both alike). A timeout is exit 1.
:wait_tcp
set "WAIT_PROTO=TCP"
goto :wait_port
:wait_udp
set "WAIT_PROTO=UDP"
:wait_port
set "WAIT_PORT=%~1"
set "WAIT_NAME=%~2"
set "WAIT_MAX=%~3"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$port = [int]$env:WAIT_PORT; $name = $env:WAIT_NAME; $root = $env:REPO_ROOT.TrimEnd('\') + '\'; $deadline = (Get-Date).AddSeconds([int]$env:WAIT_MAX); while ((Get-Date) -lt $deadline) { if ($env:WAIT_PROTO -eq 'UDP') { $eps = Get-NetUDPEndpoint -LocalPort $port -ErrorAction SilentlyContinue } else { $eps = Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue }; $ids = @($eps | Select-Object -ExpandProperty OwningProcess -Unique); $found = $false; foreach ($id in $ids) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if (-not $p) { continue }; $elsewhere = $p.Path -and -not $p.Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase); if ($p.ProcessName -ne $name -or $elsewhere) { Write-Host ('ERROR: port {0} is held by {1} (pid {2}) at {3}, not {4} from {5}' -f $port, $p.ProcessName, $id, $p.Path, $name, $root); exit 2 }; $found = $true }; if ($found) { exit 0 }; Start-Sleep -Milliseconds 500 }; if ($env:WAIT_PROTO -eq 'TCP') { Write-Host ('ERROR: {0} did not bind TCP :{1} after {2} seconds.' -f $name, $port, $env:WAIT_MAX) }; exit 1"
exit /b %errorlevel%

rem  :wait_file <path> <seconds>
:wait_file
set "WAIT_FILE=%~1"
set "WAIT_MAX=%~2"
set "WAIT_TRIES=0"
:wait_file_loop
if exist "%WAIT_FILE%" exit /b 0
set /a WAIT_TRIES+=1
if %WAIT_TRIES% GEQ %WAIT_MAX% exit /b 1
timeout /t 1 /nobreak >nul
goto :wait_file_loop

:token_not_deleted
echo ERROR: Could not delete the login token left by an earlier sign-in:
echo        "%TOKEN_FILE%"
goto :fail

:no_arguments
echo ERROR: launch_all.bat takes no arguments. The native and quick options belonged to the retired
echo        replay-based login server; the Go services and worldd are the only server path.
pause
exit /b 1

:sign_in_timeout
echo ERROR: No sign-in within 5 minutes: "%TOKEN_FILE%" did not appear.
echo        Run launch_worldd.bat again and sign in through the launcher window.
goto :fail

:fail
echo.
echo Launch failed. The Go services and worldd log to %REPO_ROOT%\dist\services\logs\
echo   auth.log, character.log, citynode.log, mesh.log, gateway.log and worldd.log.
echo   tether_server shows its errors in its own window.
pause
exit /b 1
