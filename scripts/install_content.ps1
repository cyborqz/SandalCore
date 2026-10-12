# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Unpacks the game content database (content.db) and the mappings file (aoc.usmap) into dist\content, and gets the navigation mesh.

.DESCRIPTION
  content.db is about 311 MB, too big for one file on GitHub, so the repo keeps it zipped in two parts:
  content\content.db.zip.part1 and .part2. This script joins them, unzips content.db, checks its SHA-256
  against content\content.db.sha256, and copies content\aoc.usmap next to it. It does nothing when
  dist\content\content.db is already there and matches.

  The navigation mesh (area_nav.bin + area_geom.bin, 1.2 GB) is needed for combat: without it worldd leaves
  every mob at 0 HP. It is too big for the repo and ships as a GitHub Release asset described by
  content\navmesh.json. The zip is taken from, in this order:
    1. the file named by the environment variable SANDAL_NAVMESH_ZIP,
    2. content\sandal-core-navmesh-v1.zip (a copy you downloaded by hand),
    3. a download from the "url" in content\navmesh.json (skipped while that is still the OWNER/REPO placeholder).
  The zip and the two unpacked files are checked against the SHA-256 values in navmesh.json. When no zip can be
  found the script prints a warning (combat is OFF) and still exits 0, so the rest of the server starts.

.PARAMETER Force
  Unpack again even if dist\content\content.db and the navmesh already exist.

.PARAMETER SkipNavmesh
  Do not look at the navigation mesh at all (content.db only).
#>
[CmdletBinding()]
param([switch]$Force, [switch]$SkipNavmesh)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$src = Join-Path $repoRoot 'content'
$dst = Join-Path $repoRoot 'dist\content'
$db = Join-Path $dst 'content.db'
$want = ((Get-Content (Join-Path $src 'content.db.sha256') -TotalCount 1) -split '\s+')[0].ToLower()

New-Item -ItemType Directory -Force -Path $dst | Out-Null
$usmap = Join-Path $src 'aoc.usmap'
if (Test-Path $usmap) { Copy-Item -Force $usmap (Join-Path $dst 'aoc.usmap') }
# Small optional tables worldd reads (mount speeds and riders, roads, NPC motion); each has a fallback when absent
Get-ChildItem $src -Filter '*.tsv' | Copy-Item -Destination $dst -Force

# -- C | O | A ~ Cyb --

function Install-ContentDb {
    if ((Test-Path $db) -and -not $Force) {
        # Size check first (fast); the full hash only when the size differs from what a fresh unpack gave
        $stamp = Join-Path $dst 'content.db.ok'
        if ((Test-Path $stamp) -and ((Get-Content $stamp -TotalCount 1) -eq "$want $((Get-Item $db).Length)")) { return }
        if ((Get-FileHash -Algorithm SHA256 $db).Hash.ToLower() -eq $want) {
            Set-Content -Path $stamp -Encoding ascii -Value "$want $((Get-Item $db).Length)"
            return
        }
        Write-Host 'dist\content\content.db differs from the shipped one; unpacking it again.'
    }

    $parts = @(Get-ChildItem $src -Filter 'content.db.zip.part*' | Sort-Object Name)
    if (-not $parts.Count) { throw "No content.db.zip.part* files in $src" }
    $zip = Join-Path $dst 'content.db.zip'
    Write-Host "Joining $($parts.Count) parts and unpacking content.db (about 311 MB) ..."
    $out = [System.IO.File]::Create($zip)
    try {
        foreach ($p in $parts) {
            $in = [System.IO.File]::OpenRead($p.FullName)
            try { $in.CopyTo($out) } finally { $in.Dispose() }
        }
    } finally { $out.Dispose() }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $db) { Remove-Item -Force $db }
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entry = $archive.GetEntry('content.db')
        if (-not $entry) { throw 'content.db is not in the joined zip' }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $db, $true)
    } finally { $archive.Dispose() }
    Remove-Item -Force $zip

    $got = (Get-FileHash -Algorithm SHA256 $db).Hash.ToLower()
    if ($got -ne $want) { Remove-Item -Force $db; throw "content.db checksum mismatch (got $got, want $want): the parts are damaged." }
    Set-Content -Path (Join-Path $dst 'content.db.ok') -Encoding ascii -Value "$want $((Get-Item $db).Length)"
    Write-Host 'content.db ready in dist\content.' -ForegroundColor Green
}

# ---------------------------------------------------------------------------------------------------------------------
# Navigation mesh: dist\content\area_nav.bin + area_geom.bin (needed for combat)
# ---------------------------------------------------------------------------------------------------------------------

function Get-Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }

function Write-NavmeshWarning([string]$why) {
    Write-Host ''
    Write-Host '*** WARNING: the navigation mesh (area_nav.bin + area_geom.bin) is NOT installed. ***' -ForegroundColor Yellow
    Write-Host '*** Combat is OFF: every mob shows 0 HP and damage does nothing until it is installed. ***' -ForegroundColor Yellow
    if ($why) { Write-Host "    Reason: $why" -ForegroundColor Yellow }
    Write-Host '    Fix: download sandal-core-navmesh-v1.zip from this repository''s Releases page (tag navmesh-v1) and put it' -ForegroundColor Yellow
    Write-Host '    in the content\ folder, or set SANDAL_NAVMESH_ZIP to its full path; then run scripts\install_content.ps1 again.' -ForegroundColor Yellow
    Write-Host '    The rest of the server starts anyway.' -ForegroundColor Yellow
    Write-Host ''
}

# True when $url is empty or still the OWNER/REPO placeholder of navmesh.json (nothing published yet)
function Test-PlaceholderUrl($url) {
    if (-not $url) { return $true }
    if ($url -notmatch '^https://') { return $true }
    return [bool]($url -match '/OWNER/|/REPO/|CHANGE_ME|<|>')
}

# Downloads $url to $outFile; returns $true on success (the caller checks size and hash)
function Get-NavmeshDownload([string]$url, [string]$outFile) {
    Remove-Item -Force -ErrorAction SilentlyContinue $outFile
    try {
        $curl = @(Get-Command curl.exe -CommandType Application -ErrorAction SilentlyContinue) | Select-Object -First 1
        if ($curl) {
            Write-Host "Downloading the navigation mesh with curl.exe ..."
            & ([string]$curl.Source) -L --fail --retry 3 --connect-timeout 30 -o $outFile $url
            if ($LASTEXITCODE -ne 0) { throw "curl.exe exit code $LASTEXITCODE" }
        } else {
            Write-Host "Downloading the navigation mesh with Invoke-WebRequest (no progress bar; this takes a few minutes) ..."
            $old = $ProgressPreference
            $ProgressPreference = 'SilentlyContinue'
            try {
                try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
                Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $outFile
            } finally { $ProgressPreference = $old }
        }
        return (Test-Path -LiteralPath $outFile)
    } catch {
        Write-Host "Download failed: $($_.Exception.Message)" -ForegroundColor Yellow
        Remove-Item -Force -ErrorAction SilentlyContinue $outFile
        return $false
    }
}

# Checks the zip against navmesh.json, extracts both .bin files next to their final names, checks them and only then
# moves them into place; a failure leaves no half-written .bin. Returns $true on success.
function Install-NavmeshFromZip([string]$zipPath, $meta, [string]$label) {
    $len = (Get-Item -LiteralPath $zipPath).Length
    if ($len -ne [int64]$meta.zip_size) {
        Write-Host "Refusing $label : size $len, expected $($meta.zip_size) (incomplete or wrong file)." -ForegroundColor Red
        return $false
    }
    Write-Host "Checking $label (SHA-256) ..."
    $got = Get-Sha256 $zipPath
    if ($got -ne $meta.zip_sha256.ToLower()) {
        Write-Host "Refusing $label : SHA-256 mismatch (got $got, expected $($meta.zip_sha256)); the zip is damaged." -ForegroundColor Red
        return $false
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $names = @('area_nav.bin', 'area_geom.bin')
    $partFiles = @()
    $ok = $false
    try {
        Write-Host 'Unpacking area_nav.bin and area_geom.bin (1.2 GB) ...'
        $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            foreach ($n in $names) {
                $entry = $archive.GetEntry($n)
                if (-not $entry) { throw "$n is not in the zip" }
                $part = Join-Path $dst "$n.part"
                $partFiles += $part
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $part, $true)
            }
        } finally { $archive.Dispose() }
        foreach ($n in $names) {
            $part = Join-Path $dst "$n.part"
            $h = Get-Sha256 $part
            $wantH = $meta.files.$n.ToLower()
            if ($h -ne $wantH) { throw "$n checksum mismatch (got $h, expected $wantH)" }
        }
        foreach ($n in $names) { Move-Item -Force -LiteralPath (Join-Path $dst "$n.part") -Destination (Join-Path $dst $n) }
        $ok = $true
    } catch {
        Write-Host "Refusing $label : $($_.Exception.Message)" -ForegroundColor Red
    } finally {
        foreach ($p in $partFiles) { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $p }
    }
    return $ok
}

function Install-Navmesh {
    $metaPath = Join-Path $src 'navmesh.json'
    if (-not (Test-Path -LiteralPath $metaPath)) { throw "content\navmesh.json is missing (it describes the navigation mesh asset)." }
    $meta = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
    $nav = Join-Path $dst 'area_nav.bin'
    $geom = Join-Path $dst 'area_geom.bin'
    $stamp = Join-Path $dst 'navmesh.ok'
    $names = @('area_nav.bin', 'area_geom.bin')

    # Stamp = zip hash + size and write time of both files; a later launch then costs a few file reads, not 1.2 GB of hashing
    $stampLine = {
        $a = Get-Item -LiteralPath $nav
        $g = Get-Item -LiteralPath $geom
        "$($meta.zip_sha256.ToLower()) $($a.Length) $($a.LastWriteTimeUtc.Ticks) $($g.Length) $($g.LastWriteTimeUtc.Ticks)"
    }

    $existingBad = $false
    if (-not $Force -and (Test-Path -LiteralPath $nav) -and (Test-Path -LiteralPath $geom)) {
        if ((Test-Path -LiteralPath $stamp) -and ((Get-Content -LiteralPath $stamp -TotalCount 1) -eq (& $stampLine))) { return }
        Write-Host 'Checking the installed navigation mesh (SHA-256, about 10 seconds) ...'
        if (((Get-Sha256 $nav) -eq $meta.files.'area_nav.bin'.ToLower()) -and ((Get-Sha256 $geom) -eq $meta.files.'area_geom.bin'.ToLower())) {
            Set-Content -Path $stamp -Encoding ascii -Value (& $stampLine)
            Write-Host 'Navigation mesh is in place.' -ForegroundColor Green
            return
        }
        $existingBad = $true
        Write-Host 'dist\content\area_nav.bin / area_geom.bin differ from the published ones; installing them again.'
    }
    Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $stamp

    # Where the zip can come from: environment variable, then content\, then the download
    $sources = @()
    if ($env:SANDAL_NAVMESH_ZIP) {
        if (Test-Path -LiteralPath $env:SANDAL_NAVMESH_ZIP -PathType Leaf) { $sources += , @($env:SANDAL_NAVMESH_ZIP, 'SANDAL_NAVMESH_ZIP', $false) }
        else { Write-Host "SANDAL_NAVMESH_ZIP is set but the file does not exist: $env:SANDAL_NAVMESH_ZIP" -ForegroundColor Yellow }
    }
    $manual = Join-Path $src ([string]$meta.asset)
    if (Test-Path -LiteralPath $manual -PathType Leaf) { $sources += , @($manual, "content\$($meta.asset)", $false) }

    $why = ''
    $installed = $false
    foreach ($s in $sources) {
        if (Install-NavmeshFromZip $s[0] $meta $s[1]) { $installed = $true; break }
        $why = "$($s[1]) failed the check (see above)"
    }

    if (-not $installed) {
        $url = [string]$meta.url
        if (Test-PlaceholderUrl $url) {
            if (-not $why) { $why = 'no zip found (SANDAL_NAVMESH_ZIP unset, no zip in content\) and the download url in content\navmesh.json is still a placeholder' }
        } else {
            $tmp = Join-Path $dst 'navmesh.download.tmp'
            try {
                if (Get-NavmeshDownload $url $tmp) {
                    if (Install-NavmeshFromZip $tmp $meta 'the downloaded zip') { $installed = $true }
                    else { $why = 'the downloaded zip failed the check (see above)' }
                } else { $why = "the download from $url failed" }
            } finally { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $tmp }
        }
    }

    if ($installed) {
        Set-Content -Path $stamp -Encoding ascii -Value (& $stampLine)
        Write-Host 'Navigation mesh ready in dist\content (combat is ON).' -ForegroundColor Green
        return
    }

    # Nothing worked. With -Force the files that are already there may still be the right ones.
    if ($Force -and -not $existingBad -and (Test-Path -LiteralPath $nav) -and (Test-Path -LiteralPath $geom)) {
        if (((Get-Sha256 $nav) -eq $meta.files.'area_nav.bin'.ToLower()) -and ((Get-Sha256 $geom) -eq $meta.files.'area_geom.bin'.ToLower())) {
            Set-Content -Path $stamp -Encoding ascii -Value (& $stampLine)
            Write-Host 'No zip to unpack again; the navigation mesh already installed is correct.' -ForegroundColor Green
            return
        }
    }
    Write-NavmeshWarning $why
}

Install-ContentDb
if (-not $SkipNavmesh) { Install-Navmesh }
exit 0
