# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Checks that scripts\services.ps1 builds only the services whose sources changed, and never leaves a stale exe looking current.

.DESCRIPTION
  Works in a temporary copy of services\ and scripts\services.ps1 (nothing in this checkout is written; the copy is removed at the end). It needs Go and takes about two minutes,
  most of it the first build of the ten services (the go build cache makes it a link, not a compile, when the checkout has been built before).

    - before any build, `stale` lists all ten services; after `build`, none; a second `build` writes no exe;
    - for every package of the module (cmd, internal and gen) a change of one of its files makes stale exactly the services the go tool says import it (go list -deps), and a change of
      a test file, of a note or of test data makes none;
    - a change of one package's file rebuilds exactly the services that import it, and no other exe is touched;
    - a package that embeds a file from a testdata folder counts that file: a change of it makes stale the services that import the package;
    - an exe that is replaced by another file, a change of go.mod and a change of go.sum are each seen, whatever the file times say;
    - a build that fails stops the script with an error and leaves every service it was to build stale: the next `build` makes them again.

  Run: powershell -File scripts\test_services_build.ps1      Exit 0 when every check holds, 1 otherwise.
#>
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tmp = Join-Path $env:TEMP "aoc-services-build-test-$PID"
$names = @('auth', 'character', 'citynode', 'nodectl', 'quests', 'questctl', 'worldctl', 'charimport', 'mesh', 'gateway')
$checks = 0
$failed = 0
function Check([bool]$ok, [string]$what) {
    $script:checks++
    if (-not $ok) { $script:failed++; Write-Host "FAIL: $what" -ForegroundColor Red } else { Write-Host "ok:   $what" }
}

if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force (Join-Path $tmp 'scripts') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'services.ps1') (Join-Path $tmp 'scripts\services.ps1')
& robocopy (Join-Path $repo 'services') (Join-Path $tmp 'services') /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
$svcScript = Join-Path $tmp 'scripts\services.ps1'
$svc = Join-Path $tmp 'services'
$out = Join-Path $tmp 'dist\services'

function Run([string]$action) {
    $saved = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $text = (& powershell -NoProfile -ExecutionPolicy Bypass -File $svcScript $action 2>&1 | Out-String)
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    return [pscustomobject]@{ Code = $code; Text = $text }
}
function Stale { $r = Run 'stale'; if ($r.Text -match 'stale:\s*(.+)') { return @($Matches[1].Trim() -split ',\s*' | Sort-Object) } else { return @() } }
function ExeTimes { $h = @{}; foreach ($n in $names) { $f = Join-Path $out "$n.exe"; $h[$n] = if (Test-Path $f) { (Get-Item $f).LastWriteTimeUtc.Ticks } else { 0 } }; return $h }
function Append([string]$file, [string]$text) { Add-Content -LiteralPath $file -Value $text -Encoding ASCII }

try {
    Push-Location $svc
    # which services import which packages: the go tool's own answer
    $deps = @{}
    foreach ($line in (& go list -f '{{.ImportPath}} {{range .Deps}}{{.}} {{end}}' ($names | ForEach-Object { "./cmd/$_" }))) {
        $parts = @($line -split ' ' | Where-Object { $_ })
        $deps[($parts[0] -replace '^.*/cmd/', '')] = @($parts | Select-Object -Skip 1)
    }
    $pkgs = @(& go list -f '{{.ImportPath}}|{{.Dir}}|{{range .GoFiles}}{{.}},{{end}}' ./...)
    Pop-Location
    Check ($deps.Count -eq $names.Count -and $pkgs.Count -gt 20) "the go tool lists the imports of the ten services ($($deps.Count)) and the packages of the module ($($pkgs.Count))"

    Check ((Stale).Count -eq $names.Count) 'before any build every service is stale'
    $r = Run 'build'
    Check ($r.Code -eq 0) "the first build succeeds ($($r.Text.Trim()))"
    Check ((@($names | Where-Object { Test-Path (Join-Path $out "$_.exe") })).Count -eq $names.Count) 'all ten exes are in dist\services under their names'
    Check ((Stale).Count -eq 0) 'after the build no service is stale'
    $before = ExeTimes
    Start-Sleep -Milliseconds 1200
    $r = Run 'build'
    $after = ExeTimes
    Check ($r.Code -eq 0 -and $r.Text -match 'up to date') 'a second build says everything is up to date'
    Check (@($names | Where-Object { $before[$_] -ne $after[$_] }).Count -eq 0) 'a second build writes no exe'

    # every package: a change of a file makes stale the services that import it, and only them
    $badPackages = @()
    foreach ($p in $pkgs) {
        $f = $p -split '\|'
        if (-not $f[2]) { continue }
        $f[2] = $f[2].TrimEnd(',')
        $import = $f[0]
        $file = Join-Path $f[1] (($f[2] -split ',')[0])
        $isCmd = $import -match '/cmd/([^/]+)$'
        $want = if ($isCmd) { @($Matches[1] | Where-Object { $names -contains $_ }) } else { @($names | Where-Object { $deps[$_] -contains $import } | Sort-Object) }
        $copy = [IO.File]::ReadAllBytes($file)
        Append $file '// a changed line'
        $got = Stale
        [IO.File]::WriteAllBytes($file, $copy)
        (Get-Item $file).LastWriteTimeUtc = (Get-Date).ToUniversalTime().AddHours(-1)   # (the restored file is older than every exe: the check must not rest on file times)
        if (($got -join ',') -ne (($want | Sort-Object) -join ',')) { $badPackages += "${import}: stale [$($got -join ', ')], the go tool says [$($want -join ', ')]" }
    }
    Check ($badPackages.Count -eq 0) ("a change of a file of each of the $($pkgs.Count) packages makes stale the services that import it" + $(if ($badPackages.Count) { ': ' + ($badPackages -join '; ') } else { '' }))
    Check ((Stale).Count -eq 0) 'with every file put back the services are current again (the same bytes, an older file time)'

    # what a test, a note and test data do not change
    $testFile = Join-Path $svc 'internal\mesh\zz_probe_test.go'
    Append $testFile 'package mesh'
    $noteFile = Join-Path $svc 'internal\mesh\zz_probe.md'
    Append $noteFile 'a note'
    New-Item -ItemType Directory -Force (Join-Path $svc 'internal\mesh\testdata') | Out-Null
    Append (Join-Path $svc 'internal\mesh\testdata\zz_probe.bin') 'data'
    Check ((Stale).Count -eq 0) 'a new test file, a note and a test data file leave every service current'
    Remove-Item -Force $testFile, $noteFile; Remove-Item -Recurse -Force (Join-Path $svc 'internal\mesh\testdata\zz_probe.bin')

    # a package that embeds a file from a testdata folder: the file is part of the program, so a change of it makes stale the services that import the package (and only them)
    $zz = Join-Path $svc 'internal\zzembed'
    New-Item -ItemType Directory -Force (Join-Path $zz 'testdata') | Out-Null
    [IO.File]::WriteAllText((Join-Path $zz 'zz.go'), "package zzembed`n`nimport _ `"embed`"`n`n//go:embed testdata/probe.json`nvar Probe string`n")
    [IO.File]::WriteAllText((Join-Path $zz 'testdata\probe.json'), '{"a":1}')
    $zzImport = Join-Path $svc 'cmd\mesh\zz_embed.go'
    [IO.File]::WriteAllText($zzImport, "package main`n`nimport _ `"aocrop/services/internal/zzembed`"`n")
    $r = Run 'build'
    Check ($r.Code -eq 0 -and (Stale).Count -eq 0) "a service that imports a package with an embedded testdata file builds ($($r.Text.Trim()))"
    [IO.File]::WriteAllText((Join-Path $zz 'testdata\probe.json'), '{"a":2}')
    Check (((Stale) -join ',') -eq 'mesh') 'a change of that embedded file makes stale the service that imports the package'
    Remove-Item -Recurse -Force $zz, $zzImport
    $r = Run 'build'
    Check ($r.Code -eq 0 -and (Stale).Count -eq 0) 'with the package taken out the next build makes the service again'

    # one real change: only the importers are built, the others are not touched
    $target = $pkgs | Where-Object { $_ -match '/internal/mesh\|' } | Select-Object -First 1
    $tf = $target -split '\|'
    $tf[2] = $tf[2].TrimEnd(',')
    $tfile = Join-Path $tf[1] (($tf[2] -split ',')[0])
    $importers = @($names | Where-Object { $deps[$_] -contains $tf[0] } | Sort-Object)
    $before = ExeTimes
    Start-Sleep -Milliseconds 1200
    Append $tfile '// a changed line'
    $r = Run 'build'
    $after = ExeTimes
    $rebuilt = @($names | Where-Object { $before[$_] -ne $after[$_] } | Sort-Object)
    Check ($r.Code -eq 0) "a build after a change of $($tf[0] -replace '^.*/services/', '') succeeds ($($r.Text.Trim()))"
    Check (($rebuilt -join ',') -eq ($importers -join ',') -and $importers.Count -gt 0) "it rebuilt exactly [$($importers -join ', ')]: rebuilt [$($rebuilt -join ', ')]"
    Check ((Stale).Count -eq 0) 'afterwards no service is stale'

    # an exe that is not the one that was built
    $victim = $names[0]
    Set-Content -LiteralPath (Join-Path $out "$victim.exe") -Value 'not the build' -Encoding ASCII
    Check ((Stale) -contains $victim -and (Stale).Count -eq 1) 'an exe replaced by another file is stale'
    $r = Run 'build'
    Check ($r.Code -eq 0 -and (Stale).Count -eq 0 -and (Get-Item (Join-Path $out "$victim.exe")).Length -gt 1000000) 'the next build makes it again'
    Remove-Item -LiteralPath (Join-Path $out "$($names[1]).exe")
    Check ((Stale) -contains $names[1]) 'a missing exe is stale'
    $r = Run 'build'
    Check ($r.Code -eq 0 -and (Stale).Count -eq 0) 'and is built'

    # an exe that a running service holds cannot be replaced: the build stops and says why
    $meshFile = (Get-ChildItem (Join-Path $svc 'internal\mesh') -File -Filter '*.go' | Where-Object { $_.Name -notlike '*_test.go' } | Select-Object -First 1).FullName
    $meshBytes = [IO.File]::ReadAllBytes($meshFile)
    Append $meshFile '// a changed line'
    $held = [IO.File]::Open((Join-Path $out 'mesh.exe'), 'Open', 'Read', 'None')
    try { $r = Run 'build' } finally { $held.Close() }
    Check ($r.Code -ne 0 -and $r.Text -match 'cannot replace') 'an exe that is held open stops the build, with the reason'
    [IO.File]::WriteAllBytes($meshFile, $meshBytes)
    $r = Run 'build'
    Check ($r.Code -eq 0 -and (Stale).Count -eq 0) 'once the exe is free the next build makes the services again'

    # go.mod and go.sum
    foreach ($f in 'go.mod', 'go.sum') {
        $file = Join-Path $svc $f
        $copy = [IO.File]::ReadAllBytes($file)
        Append $file $(if ($f -eq 'go.mod') { '// a changed line' } else { 'example.com/none v0.0.0 h1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=' })
        $got = Stale
        [IO.File]::WriteAllBytes($file, $copy)
        Check ($got.Count -eq $names.Count) "a change of $f makes every service stale"
    }
    Check ((Stale).Count -eq 0) 'with both put back the services are current'

    # a build that fails
    $broken = Join-Path $svc 'internal\mesh\zz_broken.go'
    [IO.File]::WriteAllText($broken, "package mesh`nthis is not Go`n")
    $now = Stale
    $r = Run 'build'
    Check ($r.Code -ne 0) 'a build of sources that do not compile ends with an error'
    $stillStale = Stale
    Check (($now -join ',') -eq ($stillStale -join ',') -and $stillStale.Count -gt 0) "and leaves the services it was to build stale ([$($stillStale -join ', ')])"
    Remove-Item -Force $broken
    $r = Run 'build'
    Check ($r.Code -eq 0 -and (Stale).Count -eq 0) 'with the sources put back the next build makes them'
} finally {
    Set-Location $repo
    Start-Sleep -Milliseconds 300
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Write-Host ("{0} checks, {1} failed" -f $checks, $failed)
if ($failed -gt 0) { exit 1 }
exit 0
