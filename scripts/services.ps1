<#
  Go services helper.
    scripts\services.ps1 up      start PostgreSQL + NATS (docker compose, waits for healthy)
    scripts\services.ps1 test    buf lint + generated-code check, go vet + go test (needs 'up'); the tests of the
                                 character creation rules and of the character service's content.db use
                                 $env:AOC_CONTENT_DB (or dist\content\content.db) when that file exists, and skip without it
    scripts\services.ps1 build   build auth, character, citynode, nodectl, worldctl, charimport, mesh, gateway into dist\services
    scripts\services.ps1 run     build, then start auth, character, citynode, mesh and gateway hidden; each logs
                                 to dist\services\logs\<name>.log (the previous run's: <name>.prev.log).  citynode
                                 (Nodes 1) serves the city nodes on :50055 and applies deploy\citynode_seed.json once, at its
                                 first start; dist\services\nodectl.exe is its command line (services\README.md, "City nodes").
                                 worldctl.exe (the GM travel tools: players, places, tp, speed) talks to worldd's admin API on
                                 :50056 (services\README.md, "GM travel tools")
         run -World legacy       (the default) mesh places players on the legacy aoc_server, UDP :7777
         run -World worldd       also start dist\Release\worldd.exe (scripts\build.ps1 -Target worldd),
                                 which registers with mesh; no static world.  worldd gets
                                 --content-db dist\content\content.db (or $env:AOC_CONTENT_DB): the file
                                 contentc builds; without it worldd starts but refuses every login.
                                 $env:AOC_GAMESTATE_MODE (tier1, tier2, tier3, captured; default tier3) reaches worldd as it is
         run (either world)      the character service gets the same --content-db (plan B1): it checks a new
                                 character's race, gender, class and customization against it; without it
                                 the service starts but refuses to create characters, saying why
    scripts\services.ps1 stop    stop those processes, worldd included (drops the gateway's live
                                 client sessions); needs neither Go nor Docker
    scripts\services.ps1 login   sign in with config\dev_login.local.json and save the token for tether_server
    scripts\services.ps1 down    stop the containers (data volume kept)
  contentc, the content.db builder (plan C1), is not built or started here: see services\README.md.
  run creates dist\services\service.token once: the internal services' shared secret, readable
  by this user only.
#>
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

param(
    [Parameter(Mandatory)] [ValidateSet('up', 'test', 'build', 'run', 'stop', 'login', 'down')] [string] $Action,
    [ValidateSet('legacy', 'worldd')] [string] $World = 'legacy'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$compose = Join-Path $root 'deploy\docker-compose.yml'
$svc = Join-Path $root 'services'
$out = Join-Path $root 'dist\services'
$logs = Join-Path $out 'logs'

function Build {
    New-Item -ItemType Directory -Force $out | Out-Null
    Push-Location $svc
    try {
        foreach ($name in 'auth', 'character', 'citynode', 'nodectl', 'worldctl', 'charimport', 'mesh', 'gateway') {
            & go build -o (Join-Path $out "$name.exe") "./cmd/$name"
            if ($LASTEXITCODE -ne 0) { throw "go build $name failed" }
        }
    } finally { Pop-Location }
    Write-Host "built into $out"
}
# [C O A + Cyb]

$worldd = Join-Path $root 'dist\Release\worldd.exe'
$tokenFile = Join-Path $out 'service.token'
# content.db, the file worldd values characters from and the character service checks new characters against
# (plan C1's contentc, services\README.md "Content").
$contentDb = if ($env:AOC_CONTENT_DB) { $env:AOC_CONTENT_DB } else { Join-Path $root 'dist\content\content.db' }

# The auth, character, citynode, mesh, gateway and worldd processes running from this
# checkout (not the legacy servers, and not another checkout's copy).
function Get-ServiceProcess {
    $exes = @('auth', 'character', 'citynode', 'mesh', 'gateway' | ForEach-Object { Join-Path $out "$_.exe" }) + $worldd
    Get-Process -Name auth, character, citynode, mesh, gateway, worldd -ErrorAction SilentlyContinue |
        Where-Object { $exes -contains $_.Path }
}

# Lets only this user open $path (like the launcher's saved login token).
function Set-OwnerOnly($path) {
    & icacls $path /inheritance:r /grant:r "${env:USERDOMAIN}\${env:USERNAME}:(F)" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not restrict $path to ${env:USERNAME}" }
}

# The internal services' shared secret (services/internal/svcauth): 32 random
# bytes as 64 lowercase hex digits, created once.
function New-ServiceToken {
    if (Test-Path $tokenFile) { return }
    New-Item -ItemType Directory -Force $out | Out-Null
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    [System.IO.File]::WriteAllText($tokenFile, (-join ($bytes | ForEach-Object { $_.ToString('x2') })))
    Set-OwnerOnly $tokenFile
    Write-Host "created the service token $tokenFile"
}

# "auth (pid 12), character (pid 34)"; call it before the processes exit.
function Format-Process($procs) {
    ($procs | ForEach-Object { '{0} (pid {1})' -f $_.Name, $_.Id }) -join ', '
}

try {
    switch ($Action) {
        'up' {
            & docker compose -f $compose up -d --wait
            if ($LASTEXITCODE -ne 0) { throw 'docker compose up failed (is Docker Desktop running?)' }
            # The init script creates aoc_test only on a fresh volume; make sure it exists.
            $exists = & docker compose -f $compose exec -T postgres psql -U aoc -d aoc -tAc "SELECT 1 FROM pg_database WHERE datname = 'aoc_test'"
            if ("$exists".Trim() -ne '1') {
                & docker compose -f $compose exec -T postgres psql -U aoc -d aoc -c 'CREATE DATABASE aoc_test'
                if ($LASTEXITCODE -ne 0) { throw 'could not create the aoc_test database' }
            }
        }
        'down' {
            & docker compose -f $compose down
            if ($LASTEXITCODE -ne 0) { throw 'docker compose down failed' }
        }
        'test' {
            $savedPath = $env:Path
            $savedTestDb = $env:AOC_TEST_DATABASE_URL
            $savedContentDb = $env:AOC_CONTENT_DB
            $tmp = Join-Path $env:TEMP "aoc-gen-$PID"
            $env:AOC_TEST_DATABASE_URL = 'postgres://aoc:aoc@127.0.0.1:5432/aoc_test?sslmode=disable'
            # The tests of the creation rules read the real content.db when there is one (they skip without).
            if (Test-Path $contentDb) { $env:AOC_CONTENT_DB = $contentDb }
            $env:Path = (Join-Path (& go env GOPATH) 'bin') + ';' + $env:Path   # buf and its plugins
            Push-Location $svc
            try {
                & buf lint ../proto/internal
                if ($LASTEXITCODE -ne 0) { throw 'buf lint failed' }
                # Generate into a temp dir and compare, so the tree is never touched
                # and a checkout with CRLF line endings does not look stale.
                & buf generate -o $tmp
                if ($LASTEXITCODE -ne 0) { throw 'buf generate failed' }
                & git -c core.autocrlf=false diff --no-index --quiet --ignore-cr-at-eol -- gen (Join-Path $tmp 'gen')
                $rc = $LASTEXITCODE
                Remove-Item $tmp -Recurse -Force
                if ($rc -ne 0) { throw 'services/gen is stale: run buf generate in services/, review and commit' }
                & go vet ./...
                if ($LASTEXITCODE -ne 0) { throw 'go vet failed' }
                & go test -count=1 ./...   # -count=1: DB-backed results are never cached
                if ($LASTEXITCODE -ne 0) { throw 'go test failed' }
            } finally {
                Pop-Location
                if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
                $env:Path = $savedPath
                $env:AOC_TEST_DATABASE_URL = $savedTestDb
                $env:AOC_CONTENT_DB = $savedContentDb
            }
        }
        'login' {
            # Signs in as launcher.exe does and saves the token where
            # tether_server reads it, for launches that skip the launcher window.
            $creds = Join-Path $root 'config\dev_login.local.json'
            if (-not (Test-Path $creds)) {
                throw "missing ${creds}: create it as {""username"": ""..."", ""password"": ""...""} for an account registered through the launcher"
            }
            # UTF-8 both ways: PowerShell 5.1 reads a file without a BOM as ANSI
            # and sends a string body as ISO-8859-1, which breaks non-ASCII passwords.
            $body = [Text.Encoding]::UTF8.GetBytes((Get-Content -Raw -Encoding UTF8 $creds))
            try {
                $resp = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8081/api/v1/auth/login' `
                    -ContentType 'application/json; charset=utf-8' -Body $body
            } catch {
                $response = $_.Exception.Response
                if ($null -eq $response) {
                    throw "login failed: $($_.Exception.Message) (is auth running? scripts\services.ps1 run)"
                }
                $status = [int]$response.StatusCode
                if ($status -eq 401) { throw "login failed: wrong username or password in $creds" }
                throw "login failed: auth answered HTTP ${status}: $($_.Exception.Message)"
            }
            if (-not $resp.access_token) { throw 'login failed: the reply has no access_token' }
            $loginFile = Join-Path $root 'dist\Release\data\ics_auth_token.txt'
            New-Item -ItemType Directory -Force (Split-Path $loginFile) | Out-Null
            [System.IO.File]::WriteAllText($loginFile, $resp.access_token)
            Set-OwnerOnly $loginFile
            Write-Host "signed in as $($resp.username); token saved for tether_server"
        }
        'build' { Build }
        'run' {
            $running = @(Get-ServiceProcess)
            if ($running.Count -gt 0) {
                throw ("already running from ${out}: " + (Format-Process $running) + ". Run 'scripts\services.ps1 stop' first.")
            }
            if ($World -eq 'worldd' -and -not (Test-Path $worldd)) {
                throw "$worldd is missing: build it with scripts\build.ps1 -Target worldd"
            }
            if (-not (Test-Path $contentDb)) {
                $who = if ($World -eq 'worldd') { 'the character service will refuse to create characters and worldd every login' } else { 'the character service will refuse to create characters' }
                Write-Host "warning: $contentDb does not exist: $who. Build it: cd services; go run ./cmd/contentc (services\README.md, 'Content')" -ForegroundColor Yellow
            }
            Build
            New-ServiceToken
            $started = @()
            # The services log with slog to stderr. They run hidden, so their
            # output goes to dist\services\logs; the previous run's is kept.
            New-Item -ItemType Directory -Force $logs | Out-Null
            # citynode starts before mesh and the gateway: worldd (started last) asks it for the node levels, and retries if it is late
            $names = @('auth', 'character', 'citynode', 'mesh', 'gateway')
            $savedStatic = $env:AOC_MESH_STATIC_WORLD
            if ($World -eq 'legacy') {
                # The legacy aoc_server never registers with mesh: mesh places
                # everyone on it as the static world.
                if (-not $env:AOC_MESH_STATIC_WORLD) { $env:AOC_MESH_STATIC_WORLD = '127.0.0.1:7777' }
            } else {
                # worldd registers itself; a static world would take players
                # whenever its heartbeats lapse, and worldd cannot redeem its tickets.
                $env:AOC_MESH_STATIC_WORLD = ''
                $names += 'worldd'
            }
            try {
                foreach ($name in $names) {
                    $exe = if ($name -eq 'worldd') { $worldd } else { Join-Path $out "$name.exe" }
                    $exeArgs = if ($name -eq 'worldd' -or $name -eq 'character') { " --content-db `"$contentDb`"" } else { '' }
                    $log = Join-Path $logs "$name.log"
                    $outLog = Join-Path $logs "$name.out.log"
                    foreach ($f in $log, $outLog) {
                        if (Test-Path $f) { Move-Item -Force $f ($f -replace '\.log$', '.prev.log') }
                    }
                    # cmd does the redirection and exits with the service's exit
                    # code. Start-Process's own -Redirect* would make the service
                    # inherit this script's handles, so a caller reading this
                    # script's output would wait until the services exit.
                    $p = Start-Process -FilePath $env:ComSpec -WorkingDirectory $root -WindowStyle Hidden -PassThru `
                        -ArgumentList "/d /s /c `"`"$exe`"$exeArgs 1>`"$outLog`" 2>`"$log`"`""
                    $null = $p.Handle   # without this ExitCode can stay empty for a process that exits at once
                    $started += [pscustomobject]@{ Name = $name; Process = $p; Log = $log }
                }
            } finally {
                $env:AOC_MESH_STATIC_WORLD = $savedStatic
            }
            # A service that cannot reach the database or bind its port exits within a moment.
            Start-Sleep -Seconds 2
            $dead = @($started | Where-Object { $_.Process.HasExited })
            if ($dead.Count -gt 0) {
                Get-ServiceProcess | Stop-Process -Force -ErrorAction SilentlyContinue
                $why = ($dead | ForEach-Object {
                    '{0} exited with code {1} (see {2})' -f $_.Name, $_.Process.ExitCode, $_.Log
                }) -join '; '
                throw "startup failed: $why"
            }
            Write-Host ('running: ' + (Format-Process @(Get-ServiceProcess)) + ". Logs: $logs\<name>.log. Addresses: services\README.md. Stop: scripts\services.ps1 stop")
        }
        'stop' {
            $procs = @(Get-ServiceProcess)
            if ($procs.Count -eq 0) {
                Write-Host "nothing to stop: no auth, character, citynode, mesh, gateway or worldd running from this checkout"
            } else {
                $what = Format-Process $procs
                $procs | Stop-Process -Force -ErrorAction SilentlyContinue
                $procs | ForEach-Object { $null = $_.WaitForExit(5000) }
                Write-Host "stopped $what"
            }
        }
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
