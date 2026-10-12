# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo','MinSizeRel')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Configure
)

$root = $PSScriptRoot
$buildScript = Join-Path $root 'scripts\build.ps1'

if (-not (Test-Path $buildScript)) {
    throw "Build script not found: $buildScript"
}

$buildArgs = @{
    Target = 'aoc_server'
    Config = $Config
}
if ($Clean) { $buildArgs['Clean'] = $true }
if ($Configure) { $buildArgs['Configure'] = $true }

# Build the main server target via the portable build driver
& $buildScript @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
exit 0
