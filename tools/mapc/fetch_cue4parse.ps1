# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# Fetches CUE4Parse (Apache-2.0, https://github.com/FabianFG/CUE4Parse) at the commit tools/mapc builds against, into third_party/CUE4Parse.
# mapc compiles its sources in (tools/mapc/mapc.csproj); the folder is not committed (.gitignore).  Needs git and the network; nothing else is downloaded
# here (the NuGet packages CUE4Parse needs come with `dotnet build`).
#
#   powershell -File tools\mapc\fetch_cue4parse.ps1
#   powershell -File tools\mapc\fetch_cue4parse.ps1 -From D:\src\CUE4Parse     # no network: copy a folder you already have (it must be this commit)
# The folder can also be named by the environment variable SANDAL_CUE4PARSE_DIR.
[CmdletBinding()]
param([string]$From = $env:SANDAL_CUE4PARSE_DIR)
$ErrorActionPreference = 'Stop'
$commit = '75983dc51608e0285e5d9ea5f28133e6a4951ccf'    # 2025-12-19, the last commit before CUE4Parse moved to .NET 10
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dest = Join-Path $root 'third_party\CUE4Parse'
if (Test-Path (Join-Path $dest 'CUE4Parse\CUE4Parse.csproj')) {
    Write-Host "third_party\CUE4Parse is there already"
    exit 0
}
New-Item -ItemType Directory -Force -Path (Join-Path $root 'third_party') | Out-Null
if ($From) {
    if (-not (Test-Path (Join-Path $From 'CUE4Parse\CUE4Parse.csproj'))) { throw "$From is not a CUE4Parse folder (no CUE4Parse\CUE4Parse.csproj in it)" }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Path (Join-Path $From '*') -Destination $dest -Recurse -Force -Exclude '.git'
    Write-Host "CUE4Parse copied from $From to $dest (it has to be commit $commit)"
    exit 0
}
git clone --quiet https://github.com/FabianFG/CUE4Parse.git $dest
Push-Location $dest
try {
    git checkout --quiet $commit
    if ($LASTEXITCODE -ne 0) { throw "checkout of $commit failed" }
} finally { Pop-Location }
# only the sources are needed: the history is not
Remove-Item -Recurse -Force (Join-Path $dest '.git')
Write-Host "CUE4Parse $commit is in $dest"
