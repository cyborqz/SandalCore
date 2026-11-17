<#
  Go services helper.
    scripts\services.ps1 up      start PostgreSQL (docker compose, waits for healthy)
    scripts\services.ps1 test    buf lint + generated-code check, go vet + go test (needs 'up'); the tests of the
                                 character creation rules and of the character service's content.db use
                                 $env:AOC_CONTENT_DB (or dist\content\content.db) when that file exists, and skip without it
    scripts\services.ps1 build   build auth, character, citynode, nodectl, quests, questctl, worldctl, charimport, mesh, gateway into dist\services: only the ones whose
                                 sources (their packages under services\, go.mod, go.sum, the Go version) changed since their exe was built, all in one go command
    scripts\services.ps1 stale   list the services whose exe is missing or was not built from the sources as they are now (builds nothing)
    scripts\services.ps1 run     build, then start auth, character, citynode, quests, mesh, gateway and worldd hidden; each logs
                                 to dist\services\logs\<name>.log (the previous run's: <name>.prev.log).  citynode
                                 (Nodes 1) serves the city nodes on :50055 and applies deploy\citynode_seed.json once, at its
                                 first start; dist\services\nodectl.exe is its command line (services\README.md, "City nodes").
                                 worldctl.exe (the GM travel tools: players, places, tp, speed) talks to worldd's admin API on
                                 :50056 (services\README.md, "GM travel tools")
                                 worldd (dist\Release\worldd.exe, scripts\build.ps1 -Target worldd) registers with mesh;
                                 there is no static world.  worldd gets
                                 --content-db dist\content\content.db (or $env:AOC_CONTENT_DB): the file
                                 contentc builds; without it worldd starts but refuses every login.
                                 $env:AOC_GAMESTATE_MODE (tier1, tier2, tier3, captured; default tier3) reaches worldd as it is.
                                 The character service gets the same --content-db (plan B1): it checks a new
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
    [Parameter(Mandatory)] [ValidateSet('up', 'test', 'build', 'stale', 'run', 'stop', 'login', 'down')] [string] $Action
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$compose = Join-Path $root 'deploy\docker-compose.yml'
$svc = Join-Path $root 'services'
$out = Join-Path $root 'dist\services'
$logs = Join-Path $out 'logs'

# The services this script builds, started or used as tools (dist\services\<name>.exe from services\cmd\<name>).
$serviceNames = @('auth', 'character', 'citynode', 'nodectl', 'quests', 'questctl', 'worldctl', 'charimport', 'mesh', 'gateway')
$stampDir = Join-Path $out '.build'

$sha256 = [Security.Cryptography.SHA256]::Create()
function Get-Sha256Hex([byte[]]$bytes) { ([BitConverter]::ToString($sha256.ComputeHash($bytes)) -replace '-', '').ToLower() }

# The directories under services\ of the packages a service is built from: its own (cmd\<name>) and, through the imports of every file of each, the packages of this module it imports.
# A file that merely names such a package in a string counts too (a build that is not needed is only slower than one that is skipped wrongly). A package with a //go:embed line counts with
# everything under it. The packages of other modules are fixed by go.mod and go.sum, the standard library by the Go version: both are in the hash below.
function Get-ServiceDirs([string]$name, [string]$module, [hashtable]$importsOf) {
    $seen = [ordered]@{}
    $todo = New-Object System.Collections.Generic.Queue[string]
    $todo.Enqueue("cmd/$name")
    while ($todo.Count -gt 0) {
        $rel = $todo.Dequeue()
        if ($seen.Contains($rel)) { continue }
        $seen[$rel] = $true
        if (-not $importsOf.ContainsKey($rel)) {
            $dir = Join-Path $svc ($rel -replace '/', '\')
            $imports = New-Object System.Collections.Generic.List[string]
            $embeds = $false
            if (Test-Path -LiteralPath $dir) {
                foreach ($f in Get-ChildItem -LiteralPath $dir -File -Filter '*.go' | Where-Object { $_.Name -notlike '*_test.go' }) {
                    $src = [IO.File]::ReadAllText($f.FullName)
                    foreach ($m in [regex]::Matches($src, '"' + [regex]::Escape($module) + '/([^"\s]+)"')) { $imports.Add($m.Groups[1].Value) }
                    if ($src -match '(?m)^\s*//go:embed\b') { $embeds = $true }
                }
            }
            $importsOf[$rel] = [pscustomobject]@{ Imports = @($imports | Select-Object -Unique); Embeds = $embeds }
        }
        foreach ($i in $importsOf[$rel].Imports) { $todo.Enqueue($i) }
    }
    return @($seen.Keys)
}

# The sources of a service as text: every file of its package directories (not the tests or notes), then go.mod and go.sum, then the Go version. A package with //go:embed counts with
# everything under it, test data and notes included: an embedded file is part of the program, wherever it lies. One line for each file: its path under services\ and the SHA-256 of its
# bytes. $fileSha keeps the hashes of files that more than one service reads.
function Get-ServiceInputs([string]$name, [string]$module, [hashtable]$importsOf, [hashtable]$fileSha, [string]$goVersion) {
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($rel in (Get-ServiceDirs $name $module $importsOf)) {
        $dir = Join-Path $svc ($rel -replace '/', '\')
        if (-not (Test-Path -LiteralPath $dir)) { $lines.Add("$rel missing"); continue }
        $files = if ($importsOf[$rel].Embeds) { Get-ChildItem -LiteralPath $dir -File -Recurse | Where-Object { $_.Name -notlike '*_test.go' } }
                 else { Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -notlike '*_test.go' -and $_.Extension -ne '.md' } }
        foreach ($f in ($files | Sort-Object FullName)) {
            if (-not $fileSha.ContainsKey($f.FullName)) {
                $fileSha[$f.FullName] = Get-Sha256Hex ([IO.File]::ReadAllBytes($f.FullName))
            }
            $lines.Add($f.FullName.Substring($svc.Length + 1) + ' ' + $fileSha[$f.FullName])
        }
    }
    foreach ($f in 'go.mod', 'go.sum') {
        $full = Join-Path $svc $f
        $lines.Add("$f " + $(if (Test-Path -LiteralPath $full) { Get-Sha256Hex ([IO.File]::ReadAllBytes($full)) } else { 'missing' }))
    }
    $lines.Add("go $goVersion")
    return ($lines -join "`n")
}

# The services whose exe is missing, or is not the one that was built from the sources as they are now: each build writes dist\services\.build\<name> with the hash of the sources it read
# and the size and write time of the exe it made, and an exe that was replaced or a source that changed (whatever its file time says) makes the service stale.
function Get-StaleServices {
    $mod = [IO.File]::ReadAllLines((Join-Path $svc 'go.mod')) | Where-Object { $_ -match '^module\s+(\S+)' } | Select-Object -First 1
    $module = ($mod -replace '^module\s+', '').Trim()
    $goVersion = (& go version) -join ' '
    $importsOf = @{}
    $fileSha = @{}
    $stale = @()
    foreach ($name in $serviceNames) {
        $exe = Join-Path $out "$name.exe"
        $stamp = Join-Path $stampDir $name
        $want = Get-Sha256Hex ([Text.Encoding]::UTF8.GetBytes((Get-ServiceInputs $name $module $importsOf $fileSha $goVersion)))
        $current = $false
        if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $stamp)) {
            $it = Get-Item -LiteralPath $exe
            $current = ((Get-Content -LiteralPath $stamp -TotalCount 1) -eq ('{0} {1} {2}' -f $want, $it.Length, $it.LastWriteTimeUtc.Ticks))
        }
        if (-not $current) { $stale += $name }
        $script:serviceWant[$name] = $want
    }
    return $stale
}
$serviceWant = @{}

# Builds the services that are stale, all in one go command (they compile and link side by side, and the packages they share compile once); the ones that are current are left as they are.
# A stamp is written only for an exe that the go command wrote, and a failed build stops with the stale ones still stale: the next call builds them again, and nothing runs a stale exe.
function Build {
    New-Item -ItemType Directory -Force $out | Out-Null
    $stale = @(Get-StaleServices)
    if ($stale.Count -eq 0) {
        Write-Host "the services in $out are up to date"
        return
    }
    # A stale exe goes before the build: the go command does not overwrite a file that is not a program (a replaced exe), and a build that fails then leaves no exe to run. An exe that a running
    # service holds cannot go, and that stops here with the reason.
    foreach ($name in $stale) {
        Remove-Item -LiteralPath (Join-Path $stampDir $name) -ErrorAction SilentlyContinue   # (a half-written exe has no stamp)
        $exe = Join-Path $out "$name.exe"
        if (Test-Path -LiteralPath $exe) {
            try { Remove-Item -LiteralPath $exe -Force -ErrorAction Stop } catch { throw "cannot replace ${exe}: $($_.Exception.Message) (is the service running? scripts\services.ps1 stop)" }
        }
    }
    New-Item -ItemType Directory -Force $stampDir | Out-Null
    Push-Location $svc
    try {
        & go build -o (($out -replace '\\', '/') + '/') @($stale | ForEach-Object { "./cmd/$_" })
        if ($LASTEXITCODE -ne 0) { throw "go build failed ($($stale -join ', '))" }
    } finally { Pop-Location }
    foreach ($name in $stale) {
        $it = Get-Item -LiteralPath (Join-Path $out "$name.exe")
        Set-Content -LiteralPath (Join-Path $stampDir $name) -Value ('{0} {1} {2}' -f $serviceWant[$name], $it.Length, $it.LastWriteTimeUtc.Ticks) -Encoding ASCII
    }
    Write-Host "built $($stale -join ', ') into $out ($($serviceNames.Count - $stale.Count) up to date)"
}
# [C O A + Cyb]

$worldd = Join-Path $root 'dist\Release\worldd.exe'
$tokenFile = Join-Path $out 'service.token'
# content.db, the file worldd values characters from and the character service checks new characters against
# (plan C1's contentc, services\README.md "Content").
$contentDb = if ($env:AOC_CONTENT_DB) { $env:AOC_CONTENT_DB } else { Join-Path $root 'dist\content\content.db' }

# The auth, character, citynode, quests, mesh, gateway and worldd processes running from this
# checkout (not another checkout's copy).
function Get-ServiceProcess {
    $exes = @('auth', 'character', 'citynode', 'quests', 'mesh', 'gateway' | ForEach-Object { Join-Path $out "$_.exe" }) + $worldd
    Get-Process -Name auth, character, citynode, quests, mesh, gateway, worldd -ErrorAction SilentlyContinue |
        Where-Object { $exes -contains $_.Path }
}

# Stops service processes: worldd first (and waited for), then the others, so that worldd's streams to the services end because worldd is gone, not because a service was
# killed under it (worldd would log "the citynode stream ended" at every stop).
function Stop-ServiceProcesses($procs) {
    $worlds = @($procs | Where-Object { $_.Name -eq 'worldd' })
    $others = @($procs | Where-Object { $_.Name -ne 'worldd' })
    $worlds | Stop-Process -Force -ErrorAction SilentlyContinue
    $worlds | ForEach-Object { $null = $_.WaitForExit(5000) }
    $others | Stop-Process -Force -ErrorAction SilentlyContinue
    $others | ForEach-Object { $null = $_.WaitForExit(5000) }
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
            & docker compose -f $compose up -d --wait --remove-orphans   # (a NATS container left by an older compose file is removed)
            if ($LASTEXITCODE -ne 0) {
                $composeVersion = (@(& docker compose version --short 2>$null) -join '').Trim()
                throw ('docker compose up failed (is Docker Desktop running? deploy\docker-compose.yml needs Docker Compose 2.23.1 or newer; this one is ' + $(if ($composeVersion) { $composeVersion } else { 'not found' }) + ')')
            }
            # The init script creates aoc_test only on a fresh volume; make sure it exists.
            $exists = & docker compose -f $compose exec -T postgres psql -U aoc -d aoc -tAc "SELECT 1 FROM pg_database WHERE datname = 'aoc_test'"
            if ("$exists".Trim() -ne '1') {
                & docker compose -f $compose exec -T postgres psql -U aoc -d aoc -c 'CREATE DATABASE aoc_test'
                if ($LASTEXITCODE -ne 0) { throw 'could not create the aoc_test database' }
            }
        }
        'down' {
            & docker compose -f $compose down --remove-orphans
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
        'stale' {
            $stale = @(Get-StaleServices)
            if ($stale.Count -eq 0) { Write-Host 'none: every service in dist\services was built from its sources as they are now' } else { Write-Host ('stale: ' + ($stale -join ', ')) }
        }
        'run' {
            $running = @(Get-ServiceProcess)
            if ($running.Count -gt 0) {
                throw ("already running from ${out}: " + (Format-Process $running) + ". Run 'scripts\services.ps1 stop' first.")
            }
            if (-not (Test-Path $worldd)) {
                throw "$worldd is missing: build it with scripts\build.ps1 -Target worldd"
            }
            if (-not (Test-Path $contentDb)) {
                $who = 'the character service will refuse to create characters and worldd every login'
                Write-Host "warning: $contentDb does not exist: $who. Build it: cd services; go run ./cmd/contentc (services\README.md, 'Content')" -ForegroundColor Yellow
            }
            Build
            New-ServiceToken
            $started = @()
            # The services log with slog to stderr. They run hidden, so their
            # output goes to dist\services\logs; the previous run's is kept.
            New-Item -ItemType Directory -Force $logs | Out-Null
            # citynode starts before mesh and the gateway: worldd (started last) asks it for the node levels, and retries if it is late
            # quests (the saved quest logs) starts with it: worldd loads a character's log at login and retries if it is late
            $names = @('auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'worldd')
            $savedStatic = $env:AOC_MESH_STATIC_WORLD
            # worldd registers itself; a static world would take players
            # whenever its heartbeats lapse, and worldd cannot redeem its tickets.
            $env:AOC_MESH_STATIC_WORLD = ''
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
                Stop-ServiceProcesses @(Get-ServiceProcess)
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
                Write-Host "nothing to stop: no auth, character, citynode, quests, mesh, gateway or worldd running from this checkout"
            } else {
                $what = Format-Process $procs
                Stop-ServiceProcesses $procs
                Write-Host "stopped $what"
            }
        }
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
