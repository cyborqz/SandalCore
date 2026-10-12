# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Creates this machine's own TLS certificates in certs\ and trusts the local CA (first launch only).

.DESCRIPTION
  The game client checks the gateway's TLS certificate against a CA in the Windows "Trusted Root" store.
  Sandal Core ships no keys, so every user gets their own:
    certs\ca.crt       a local CA, added to your CurrentUser Trusted Root store (Windows asks you to confirm)
    certs\server.pem   the server certificate for 127.0.0.1 / localhost, signed by that CA
    certs\server.key   its private key
  The CA's private key is deleted right after signing, so nothing else can ever be signed with your trusted CA.
  Uses OpenSSL (Git for Windows ships it; install_dependencies.bat installs Git).
  Nothing happens when the three files already exist and the CA is trusted.

.PARAMETER Force
  Make new certificates even if they exist (the old local CA is removed from the trust store).
#>
[CmdletBinding()]
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$certs = Join-Path $repoRoot 'certs'
$caCrt = Join-Path $certs 'ca.crt'
$serverPem = Join-Path $certs 'server.pem'
$serverKey = Join-Path $certs 'server.key'
$caSubject = 'Sandal Core Local CA'

function Find-OpenSsl {
    $c = Get-Command openssl -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    foreach ($p in @("$env:ProgramFiles\Git\mingw64\bin\openssl.exe", "$env:ProgramFiles\Git\usr\bin\openssl.exe",
                     "${env:ProgramFiles(x86)}\Git\mingw64\bin\openssl.exe")) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Get-OurRootCerts {
    Get-ChildItem Cert:\CurrentUser\Root | Where-Object { $_.Subject -eq "CN=$caSubject" }
}

# ~ C:O:A + C_Y_B ~

function Install-Trust {
    $ca = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $caCrt
    $ours = @(Get-OurRootCerts)
    if ($ours | Where-Object { $_.Thumbprint -eq $ca.Thumbprint }) { return }
    foreach ($old in $ours) {
        Write-Host "Removing an older Sandal Core local CA from your trust store ($($old.Thumbprint)) ..."
        & certutil -user -delstore Root $old.Thumbprint | Out-Null
    }
    Write-Host 'Adding the Sandal Core local CA to your Trusted Root store.'
    Write-Host 'Windows will show a security warning: choose Yes so the game client accepts the local server.'
    & certutil -user -addstore Root $caCrt | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'certutil could not add the CA (did you choose No?). Run this script again to retry.' }
}

$haveAll = (Test-Path $caCrt) -and (Test-Path $serverPem) -and (Test-Path $serverKey)
if ($haveAll -and -not $Force) {
    Install-Trust
    exit 0
}

$openssl = Find-OpenSsl
if (-not $openssl) {
    throw 'OpenSSL not found. Install Git for Windows (install_dependencies.bat does), then launch again.'
}
New-Item -ItemType Directory -Force -Path $certs | Out-Null
$env:MSYS2_ARG_CONV_EXCL = '*'     # stop Git's MSYS openssl from rewriting "/CN=..." as a path
$env:MSYS_NO_PATHCONV = '1'

$caKey = Join-Path $certs 'ca.key'
$csr = Join-Path $certs 'server.csr'
$ext = Join-Path $certs 'server_ext.cnf'
try {
    Write-Host "Creating your local CA and server certificate in $certs ..."
    & $openssl req -quiet -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 -keyout $caKey -out $caCrt `
        -subj "/CN=$caSubject" -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign'
    if ($LASTEXITCODE -ne 0) { throw 'openssl failed to create the CA' }
    & $openssl req -quiet -newkey rsa:2048 -nodes -sha256 -keyout $serverKey -out $csr -subj '/CN=127.0.0.1'
    if ($LASTEXITCODE -ne 0) { throw 'openssl failed to create the server key' }
    Set-Content -Path $ext -Encoding ascii -Value @(
        'basicConstraints=CA:FALSE',
        'keyUsage=critical,digitalSignature,keyEncipherment',
        'extendedKeyUsage=serverAuth',
        'subjectAltName=IP:127.0.0.1,DNS:localhost')
    & $openssl x509 -req -sha256 -days 825 -in $csr -CA $caCrt -CAkey $caKey -set_serial ("0x{0:x}" -f (Get-Random)) `
        -extfile $ext -out $serverPem
    if ($LASTEXITCODE -ne 0) { throw 'openssl failed to sign the server certificate' }
} finally {
    foreach ($f in $caKey, $csr, $ext) { if (Test-Path $f) { Remove-Item -Force $f } }
}
Install-Trust
Write-Host 'Certificates ready.' -ForegroundColor Green
exit 0
