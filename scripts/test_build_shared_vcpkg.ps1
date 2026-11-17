# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Checks how scripts\build.ps1 chooses the vcpkg install: the build folder's own (as always) or, with AOC_VCPKG_SHARED set, one Release-only install that many build folders share.

.DESCRIPTION
  Works on copies of scripts\build.ps1 in a temporary folder and a stand-in cmake that only writes down its arguments and a CMakeCache.txt: nothing is configured, built or installed,
  vcpkg is never run, nothing is downloaded and nothing in this checkout is written.  It needs Visual Studio's vswhere (build.ps1 itself needs it) and takes about a minute.

    - without AOC_VCPKG_SHARED the configure is what it always was: the x64-windows triplet and no install folder of its own;
    - with it: the x64-windows-release triplet and one install folder named by the hash of vcpkg.json (and the triplet); two build folders with one vcpkg.json share it, another
      vcpkg.json makes another one; a relative folder name means the folder of the caller, for every build folder;
    - a build folder that holds a shared install follows vcpkg.json: when the file changed since the configure, build.ps1 configures again into the install the new file names (vcpkg
      would otherwise change the old install in place, under every other build folder that shares it); with the same file it does not configure again;
    - a build folder with its own install is left alone when the variable is set later; a Debug build of a shared folder is refused (the shared install has no Debug libraries);
    - two configures at once on one install do not overlap (the second waits); a configure that is killed half way does not keep the second one waiting.

  Run: powershell -File scripts\test_build_shared_vcpkg.ps1      Exit 0 when every check holds, 1 otherwise.
#>
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$vswhere = "$env:ProgramFiles (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { $vswhere = "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe" }
if (-not (Test-Path $vswhere)) { Write-Host 'skipped: Visual Studio (vswhere.exe) is not installed, and scripts\build.ps1 needs it'; exit 0 }
$tmp = Join-Path $env:TEMP "aoc-build-shared-test-$PID"
$checks = 0
$failed = 0
function Check([bool]$ok, [string]$what) {
    $script:checks++
    if (-not $ok) { $script:failed++; Write-Host "FAIL: $what" -ForegroundColor Red } else { Write-Host "ok:   $what" }
}

if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force $tmp | Out-Null
# the stand-in vcpkg (only the file that build.ps1 looks for) and the stand-in cmake
$vcpkgRoot = Join-Path $tmp 'vcpkg'
New-Item -ItemType Directory -Force (Join-Path $vcpkgRoot 'scripts\buildsystems') | Out-Null
Set-Content -LiteralPath (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake') -Value '# stand-in' -Encoding ASCII
$bin = Join-Path $tmp 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
$calls = Join-Path $tmp 'cmake_calls.txt'
# the stand-in: a configure (an argument that is not --build) writes "START/ARGS/END" lines to the log, sleeps STUB_SLEEP seconds and writes the CMakeCache.txt entries that build.ps1 reads
Set-Content -LiteralPath (Join-Path $bin 'cmake_stub.ps1') -Encoding ASCII -Value @'
$log = $env:STUB_LOG
if ($args -contains '--build') { exit 0 }
Add-Content -LiteralPath $log -Value ("START {0} {1}" -f $PWD.Path, [Environment]::TickCount)
Add-Content -LiteralPath $log -Value ("ARGS " + ($args -join ' '))
if ($env:STUB_SLEEP) { Start-Sleep -Seconds ([int]$env:STUB_SLEEP) }
$home_ = ($args | Where-Object { $_ -and -not $_.StartsWith('-') } | Select-Object -Last 1)
$lines = @('CMAKE_HOME_DIRECTORY:INTERNAL=' + ($home_ -replace '\\', '/'))
foreach ($x in $args) {
    if ($x -match '^-DVCPKG_INSTALLED_DIR=(.+)$') { $lines += 'VCPKG_INSTALLED_DIR:PATH=' + ($Matches[1] -replace '\\', '/') }
    if ($x -match '^-DVCPKG_TARGET_TRIPLET=(.+)$') { $lines += 'VCPKG_TARGET_TRIPLET:STRING=' + $Matches[1] }
}
Set-Content -LiteralPath (Join-Path $PWD.Path 'CMakeCache.txt') -Value $lines -Encoding ASCII
Add-Content -LiteralPath $log -Value ("END {0} {1}" -f $PWD.Path, [Environment]::TickCount)
exit 0
'@
Set-Content -LiteralPath (Join-Path $bin 'cmake.cmd') -Encoding ASCII -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0cmake_stub.ps1`" %*`r`nexit /b %ERRORLEVEL%`r`n"

function New-Repo([string]$name, [string]$manifest = '') {
    $r = Join-Path $tmp $name
    New-Item -ItemType Directory -Force (Join-Path $r 'scripts') | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'build.ps1') (Join-Path $r 'scripts\build.ps1')
    if ($manifest) { Set-Content -LiteralPath (Join-Path $r 'vcpkg.json') -Value $manifest -Encoding ASCII }
    else { Copy-Item (Join-Path $repo 'vcpkg.json') (Join-Path $r 'vcpkg.json') }
    return $r
}
# runs the copy of build.ps1 of a repo and returns its exit code and text; $env is a hashtable of variables to set for the run (AOC_VCPKG_SHARED and STUB_SLEEP are removed first)
function Build($r, [string[]]$more = @(), [hashtable]$vars = @{}, [string]$cwd = $tmp) {
    $saved = @{}
    foreach ($k in 'AOC_VCPKG_SHARED', 'STUB_SLEEP', 'STUB_LOG', 'VCPKG_ROOT', 'PATH') { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    try {
        [Environment]::SetEnvironmentVariable('AOC_VCPKG_SHARED', $null); [Environment]::SetEnvironmentVariable('STUB_SLEEP', $null)
        [Environment]::SetEnvironmentVariable('STUB_LOG', $calls); [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $vcpkgRoot)
        [Environment]::SetEnvironmentVariable('PATH', $bin + ';' + $saved['PATH'])
        foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
        Push-Location $cwd
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $text = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'scripts\build.ps1') -Target worldd @more 2>&1 | Out-String)
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prev
        Pop-Location
    } finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
    return [pscustomobject]@{ Code = $code; Text = $text }
}
function ConfigureCalls { if (Test-Path $calls) { return @(Get-Content -LiteralPath $calls | Where-Object { $_ -like 'ARGS *' }) } else { return @() } }
function InstallDirOf([string]$argsLine) { if ($argsLine -match '-DVCPKG_INSTALLED_DIR=(\S+)') { return $Matches[1] } else { return '' } }
function ResetCalls { Remove-Item -LiteralPath $calls -Force -ErrorAction SilentlyContinue }

try {
    # 1. without the variable: what it always was
    $r = New-Repo 'a1'
    ResetCalls
    $o = Build $r @('-Configure')
    $c = @(ConfigureCalls)
    Check ($o.Code -eq 0 -and $c.Count -eq 1 -and $c[0] -match '-DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_APPLOCAL_DEPS=OFF$' -and $c[0] -notmatch 'VCPKG_INSTALLED_DIR') 'without AOC_VCPKG_SHARED the configure has the x64-windows triplet and no install folder of its own'

    # 2. with the variable: the release triplet, one folder named by the hash of vcpkg.json; a relative name is the caller's folder for every build folder
    $shared = Join-Path $tmp 'shared'
    $r1 = New-Repo 'b1'; $r2 = New-Repo 'b2'; $r3 = New-Repo 'b3' '{"name": "x", "version": "2", "dependencies": ["zlib"]}'
    ResetCalls
    foreach ($x in $r1, $r2, $r3) { [void](Build $x @('-Configure') @{ AOC_VCPKG_SHARED = $shared }) }
    $c = @(ConfigureCalls)
    $d = @($c | ForEach-Object { InstallDirOf $_ })
    Check ($c.Count -eq 3 -and ($c | Where-Object { $_ -match '-DVCPKG_TARGET_TRIPLET=x64-windows-release ' }).Count -eq 3) 'with AOC_VCPKG_SHARED the triplet is x64-windows-release'
    Check ($d.Count -eq 3 -and $d[0] -eq $d[1] -and $d[2] -ne $d[0] -and $d[0] -match 'x64-windows-release-[0-9a-f]{12}$' -and (Test-Path $d[0])) 'two build folders with one vcpkg.json share one install folder, another vcpkg.json gets another, and the folder is made'
    $relWork = Join-Path $tmp 'caller'; New-Item -ItemType Directory -Force $relWork | Out-Null
    $r4 = New-Repo 'c1'; $r5 = New-Repo 'c2'
    ResetCalls
    foreach ($x in $r4, $r5) { [void](Build $x @('-Configure') @{ AOC_VCPKG_SHARED = 'rel_shared' } $relWork) }
    $d = @(ConfigureCalls | ForEach-Object { InstallDirOf $_ })
    Check ($d.Count -eq 2 -and $d[0] -eq $d[1] -and (($d[0] -replace '/', '\').StartsWith($relWork + '\'))) 'a relative AOC_VCPKG_SHARED is the caller''s folder: two build folders configured from it share one install'

    # 3. a build folder that holds a shared install follows vcpkg.json
    $r6 = New-Repo 'd1'
    ResetCalls
    [void](Build $r6 @() @{ AOC_VCPKG_SHARED = $shared })      # no cache yet: the first run configures
    $first = @(ConfigureCalls)
    ResetCalls
    [void](Build $r6 @() @{ AOC_VCPKG_SHARED = $shared })      # the same vcpkg.json: no new configure
    Check ($first.Count -eq 1 -and @(ConfigureCalls).Count -eq 0) 'the first run configures, a second run with the same vcpkg.json does not'
    $old = InstallDirOf $first[0]
    Set-Content -LiteralPath (Join-Path $r6 'vcpkg.json') -Value '{"name": "x", "version": "3", "dependencies": ["fmt"]}' -Encoding ASCII
    ResetCalls
    $o = Build $r6 @() @{ AOC_VCPKG_SHARED = $shared }
    $after = @(ConfigureCalls)
    $new = if ($after.Count -gt 0) { InstallDirOf $after[0] } else { '' }
    Check ($after.Count -eq 1 -and $new -ne '' -and $new -ne $old -and $new -match 'x64-windows-release-[0-9a-f]{12}$') 'a changed vcpkg.json makes build.ps1 configure again, into the install the new file names (not the old one)'
    ResetCalls
    [void](Build $r6 @())                                       # the variable is not needed any more: the cache knows the shared folder
    Check (@(ConfigureCalls).Count -eq 0) 'and the next run configures nothing'
    Set-Content -LiteralPath (Join-Path $r6 'vcpkg.json') -Value '{"name": "x", "version": "4", "dependencies": ["spdlog"]}' -Encoding ASCII
    ResetCalls
    [void](Build $r6 @())                                       # without the variable, the same follow-up from the folder the cache names
    $after = @(ConfigureCalls)
    Check ($after.Count -eq 1 -and (InstallDirOf $after[0]) -ne $new -and (((InstallDirOf $after[0]) -replace '/', '\').StartsWith($shared + '\'))) 'the install follows vcpkg.json without the variable too, and stays in the same shared folder'

    # 4. a Debug build of a shared folder is refused; a folder with its own install is left alone
    $o = Build $r6 @('-Config', 'Debug')
    Check ($o.Code -ne 0 -and $o.Text -match 'Release-only') 'a Debug build of a build folder that holds a shared install is refused with a clear message'
    $r7 = New-Repo 'e1'
    ResetCalls
    [void](Build $r7 @())                                       # its own install (x64-windows), configured
    ResetCalls
    $o = Build $r7 @() @{ AOC_VCPKG_SHARED = $shared }
    Check ($o.Code -eq 0 -and @(ConfigureCalls).Count -eq 0) 'a build folder with its own install is not switched when the variable is set later (build.ps1 -Configure does that)'

    # 5. two configures at once do not overlap; a configure killed half way does not keep the next one waiting
    $sh2 = Join-Path $tmp 'shared2'
    $ra = New-Repo 'f1'; $rb = New-Repo 'f2'
    ResetCalls
    function StartBuild($r, [string]$shared, [int]$sleep) {
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = 'powershell'
        $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $r 'scripts\build.ps1')`" -Target worldd -Configure"
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables['AOC_VCPKG_SHARED'] = $shared
        $psi.EnvironmentVariables['STUB_SLEEP'] = "$sleep"
        $psi.EnvironmentVariables['STUB_LOG'] = $calls
        $psi.EnvironmentVariables['VCPKG_ROOT'] = $vcpkgRoot
        $psi.EnvironmentVariables['PATH'] = $bin + ';' + $env:PATH
        $psi.WorkingDirectory = $r
        return [Diagnostics.Process]::Start($psi)
    }
    $pa = StartBuild $ra $sh2 5
    Start-Sleep -Seconds 3
    $pb = StartBuild $rb $sh2 0
    $ta = $pa.StandardOutput.ReadToEndAsync(); $tb = $pb.StandardOutput.ReadToEndAsync()
    [void]$pa.WaitForExit(120000); [void]$pb.WaitForExit(120000)
    $kinds = @(Get-Content -LiteralPath $calls | Where-Object { $_ -match '^(START|END) ' } | ForEach-Object { $_.Split(' ')[0] })
    Check ($kinds.Count -eq 4 -and ($kinds -join ',') -eq 'START,END,START,END' -and $tb.Result -match 'waiting for it' -and $ta.Result -notmatch 'waiting for it') 'two configures at once on one install do not overlap: the second says it waits, then runs'

    ResetCalls
    $sh3 = Join-Path $tmp 'shared3'
    $rc = New-Repo 'g1'; $rd = New-Repo 'g2'
    $pk = StartBuild $rc $sh3 300
    for ($i = 0; $i -lt 60 -and -not ((Test-Path $calls) -and ((Get-Content -LiteralPath $calls -ErrorAction SilentlyContinue) -match '^START')); $i++) { Start-Sleep -Milliseconds 500 }
    # stop the build script that holds the install (this run's own process, by its id and after checking its command line) and the stand-in that it started
    $mine = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -like "*$tmp*" -and ($_.ProcessId -eq $pk.Id -or $_.ParentProcessId -eq $pk.Id -or $_.CommandLine -like '*cmake_stub.ps1*') })
    foreach ($p in ($mine | Sort-Object ProcessId -Descending)) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    [void]$pk.WaitForExit(20000)
    $t0 = Get-Date
    $pd = StartBuild $rd $sh3 0
    $td = $pd.StandardOutput.ReadToEndAsync()
    $done = $pd.WaitForExit(60000)
    if (-not $done) { try { $pd.Kill() } catch { } }
    $secs = ((Get-Date) - $t0).TotalSeconds
    Check ($done -and $pd.ExitCode -eq 0 -and $secs -lt 45 -and $td.Result -notmatch 'waiting for it') ("a configure killed half way leaves the install free for the next one (it ran after {0:N0} s)" -f $secs)
} finally {
    # nothing of this run may be left running: any process that still has the temporary folder in its command line
    foreach ($p in @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -like "*$tmp*" -and $_.ProcessId -ne $PID })) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Write-Host ("{0} checks, {1} failed" -f $checks, $failed)
if ($failed -gt 0) { exit 1 }
exit 0
