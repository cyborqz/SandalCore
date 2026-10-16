@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

setlocal EnableExtensions EnableDelayedExpansion

rem ============================================================================
rem  launch_all.bat - start the full emulator stack + game client
rem
rem  Paths are derived from this script's location. Override the game client
rem  location via GAME_ROOT if yours is not at one of the detected defaults.
rem ============================================================================

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
set "DIST_DIR=%REPO_ROOT%\dist\Release"
set "CONFIG_DIR=%REPO_ROOT%\config"
set "CERTS_DIR=%REPO_ROOT%\certs"

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
if not exist "%DIST_DIR%\auth_server.exe" (
    echo Missing: "%DIST_DIR%\auth_server.exe"
    set "MISSING_BINARIES=1"
)
if not exist "%DIST_DIR%\aoc_server.exe" (
    echo Missing: "%DIST_DIR%\aoc_server.exe"
    set "MISSING_BINARIES=1"
)
if not exist "%DIST_DIR%\tether_server.exe" (
    echo Missing: "%DIST_DIR%\tether_server.exe"
    set "MISSING_BINARIES=1"
)
if not exist "%DIST_DIR%\launcher.exe" (
    echo Missing: "%DIST_DIR%\launcher.exe"
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

rem First launch: unpack content.db + aoc.usmap into dist\content, and create this machine's own
rem TLS certificates in certs\ (local CA trusted for your user; no keys ship with Sandal Core).
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\install_content.ps1" || goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\make_certs.ps1" || goto :fail

if exist "%CERTS_DIR%\" (
    if not exist "certs" mkdir "certs"
    xcopy "%CERTS_DIR%" "certs" /E /I /Y >nul
)

echo Cleaning up old emulator processes from this dist ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$dist = (Resolve-Path -LiteralPath $env:DIST_DIR).Path; Get-Process auth_server,aoc_server,tether_server,launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dist, [System.StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force"

set "GO_STACK=0"
if /I "%AOC_STACK%"=="go" set "GO_STACK=1"
rem  AOC_WORLD=worldd (launch_go.bat worldd): worldd serves the world, not aoc_server.
set "WORLDD=0"
if "%GO_STACK%"=="1" if /I "%AOC_WORLD%"=="worldd" set "WORLDD=1"
rem  quick skips character select, and with it the join ticket worldd needs.
if "%WORLDD%"=="1" if /I "%~2"=="quick" goto :worldd_no_quick

rem  Go services left running by launch_go.bat would hold :8081, :443 and :50055 (citynode) and
rem  answer in place of the legacy servers: stop them in both modes. This needs
rem  neither Go nor Docker.
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" stop || goto :fail

if "%GO_STACK%"=="1" goto :start_go_services
echo [1/5] Starting auth_server.exe (HTTP :8081) ...
start "Auth Server" /D "%DIST_DIR%" /MIN cmd /c "auth_server.exe 8081 logs\mock_auth.log & pause"

echo        Waiting for auth_server to bind TCP port 8081 ...
call :wait_tcp 8081 auth_server 30 || goto :fail
echo        auth_server READY.
goto :services_ready

:start_go_services
set "WORLD_ARG=legacy"
if "%WORLDD%"=="1" set "WORLD_ARG=worldd"
echo [1/5] Starting the Go services: auth, character, citynode, mesh, gateway (world: %WORLD_ARG%) ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" up || goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" run -World %WORLD_ARG% || goto :fail
call :wait_tcp 8081 auth 30 || goto :fail
call :wait_tcp 443 gateway 30 || goto :fail
rem  citynode (Nodes 1) serves the city node levels; migration 0009 runs and the seed is applied at its first start
call :wait_tcp 50055 citynode 30 || goto :fail
if not "%WORLDD%"=="1" goto :go_services_up
call :wait_udp 7777 worldd 30 || goto :fail
:go_services_up
echo        Go services READY.

:services_ready

rem  launch_all.bat          replay mode (the captured session drives the world)
rem  launch_all.bat native   native mode (NativeConnectSequencer builds world entry;
rem                          captured bytes only where its bootstrap plan splices them)
rem  launch_all.bat native quick
rem                          also skip the launcher window and hand the client the
rem                          game server's address, so it connects straight away
rem                          instead of stopping at the title screen and lobby
set "SERVER_ARGS=--use-embedded-bootstrap --enable-live-world --live-world-hz 20 --send-hit-confirmations --send-level-visibility-acks"
set "MODE_NAME=replay"
rem  The client runs without Epic Online Services.
set "CLIENT_ARGS=-UseEOS=false"
if /I "%~1"=="native" (
    set "SERVER_ARGS=--native --no-replay-loop"
    set "MODE_NAME=native"
    rem  Verbose client logs for possession and movement (AOC.log)
    set "CLIENT_ARGS=%CLIENT_ARGS% -LogCmds="LogPlayerController Verbose, LogCharacterMovement Verbose, LogNetPlayerMovement Verbose""
)
set "QUICK=0"
set "CONNECT_URL="
if /I "%~2"=="quick" (
    set "QUICK=1"
    rem  Unreal opens a URL given as the first argument instead of the default map
    set "CONNECT_URL=127.0.0.1:7777"
)
set "ICS_PORT=443"
set "TOKEN_FILE=%DIST_DIR%\data\ics_auth_token.txt"
set "TETHER_CMD=tether_server.exe"
set "SIGN_IN_NOTE=Credentials: test222 / test  (or register a new account)"
if "%GO_STACK%"=="1" (
    rem  The gateway owns :443; aoc_server's own ICS moves out of its way.
    set "SERVER_ARGS=!SERVER_ARGS! --port 8443"
    set "ICS_PORT=8443"
    rem  The tether hands the client the login token the launcher saved.
    set "TETHER_CMD=tether_server.exe --login-token-file"
    set "SIGN_IN_NOTE=Sign in with an account registered through the launcher."
)
if "%GO_STACK%"=="1" if "%QUICK%"=="1" set "SIGN_IN_NOTE=Signed in with the account in config\dev_login.local.json."

echo.
if "%WORLDD%"=="1" goto :worldd_world
echo [2/5] Starting aoc_server.exe in %MODE_NAME% mode (gRPC :%ICS_PORT% + UDP game server) ...
start "AOC Server" /D "%DIST_DIR%" /MIN cmd /c "aoc_server.exe %SERVER_ARGS% --verbose-bunches --verbose-bunch-limit 0 --verbose-bunch-log logs\bunches_liveworld.log & pause"

echo        Waiting for aoc_server to bind TCP port %ICS_PORT% ...
call :wait_tcp %ICS_PORT% aoc_server 45 || goto :fail
echo        aoc_server READY.
goto :world_ready

:worldd_world
echo [2/5] worldd serves the world on UDP :7777 (started with the Go services). Plan A4:
echo        the client logs in and loads the map, then waits; the world comes with plan A5.

:world_ready

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

rem  Go stack: a login token left from an earlier sign-in would open the game
rem  as that account, so only this run's sign-in may provide one.
if not "%GO_STACK%"=="1" goto :login_done
if exist "%TOKEN_FILE%" del /f /q "%TOKEN_FILE%"
if exist "%TOKEN_FILE%" goto :token_not_deleted
if not "%QUICK%"=="1" goto :login_done
echo        Quick mode: signing in with config\dev_login.local.json ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\services.ps1" login || goto :fail
:login_done

echo.
if "%QUICK%"=="1" (
    echo [4/5] Quick mode: no launcher window.
) else (
    echo [4/5] Opening launcher.exe ...
    start "" /D "%DIST_DIR%" "%DIST_DIR%\launcher.exe"
)

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

rem  Go stack with the launcher window: the client asks the tether for its
rem  login token some seconds after it starts, so it starts once the player
rem  has signed in (the launcher then writes the token file).
if not "%GO_STACK%"=="1" goto :start_game
if "%QUICK%"=="1" goto :start_game
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
    start "" /D "%GAME_ROOT%\AOC\Binaries\Win64" "%GAME_EXE%" %CONNECT_URL% -LauncherTetherPort=19021 %CLIENT_ARGS%
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

:worldd_no_quick
echo ERROR: quick skips character select and connects straight to 127.0.0.1:7777, so the
echo        client has no join ticket for worldd. Run launch_go.bat worldd and press Play.
pause
exit /b 1

:sign_in_timeout
echo ERROR: No sign-in within 5 minutes: "%TOKEN_FILE%" did not appear.
echo        Run launch_go.bat again and sign in through the launcher window.
goto :fail

:fail
echo.
if "%GO_STACK%"=="1" goto :fail_go
echo Launch failed. Check the opened server window for the active error.
pause
exit /b 1

:fail_go
echo Launch failed. The Go services log to %REPO_ROOT%\dist\services\logs\
echo   auth.log, character.log, citynode.log, mesh.log and gateway.log. aoc_server and
echo   tether_server show their errors in their own windows.
pause
exit /b 1
