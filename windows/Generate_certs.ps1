# Copyright (C) 2026 iceman50
# Licensed under GPL-3.0-or-later.

#Requires -Version 5.1

<#
.SYNOPSIS
Creates the PEM files required by an ADCH++ TLS listener.

.DESCRIPTION
Generates an unencrypted RSA private key for unattended service startup, a
self-signed server certificate, RFC 7919 FFDHE2048 parameters, and the trusted
directory required by ADCH++.

.EXAMPLE
.\Generate_certs.ps1 -CommonName hub.example.org

.EXAMPLE
.\Generate_certs.ps1 -CommonName hub.example.org `
    -DnsName hub.example.org,hub6.example.org -IpAddress 192.0.2.10,2001:db8::10

.EXAMPLE
.\Generate_certs.ps1 -OpenSslPath 'C:\OpenSSL-Win64\bin\openssl.exe' -Force
#>

[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$CommonName = $env:COMPUTERNAME,
    [string[]]$DnsName = @(),
    [string[]]$IpAddress = @(),
    [ValidateRange(1, 36500)]
    [int]$ValidDays = 1095,
    [ValidateSet(2048, 3072, 4096)]
    [int]$RsaBits = 3072,
    [string]$OpenSslPath,
    [switch]$Force
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Resolve-OpenSslExecutable {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        $candidate = $RequestedPath
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            $candidate = Join-Path $candidate 'openssl.exe'
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "OpenSSL executable not found: $candidate"
        }
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    $command = Get-Command 'openssl.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        (Join-Path $PSScriptRoot 'openssl.exe'),
        (Join-Path $PSScriptRoot 'bin\openssl.exe'),
        'C:\OpenSSL-Win64\bin\openssl.exe',
        'C:\OpenSSL-Win32\bin\openssl.exe'
    )
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles 'OpenSSL-Win64\bin\openssl.exe'
        $candidates += Join-Path $env:ProgramFiles 'OpenSSL-Win32\bin\openssl.exe'
    }
    if (${env:ProgramFiles(x86)}) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} 'OpenSSL-Win64\bin\openssl.exe'
        $candidates += Join-Path ${env:ProgramFiles(x86)} 'OpenSSL-Win32\bin\openssl.exe'
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw 'OpenSSL was not found. Install OpenSSL, add openssl.exe to PATH, or use -OpenSslPath.'
}

function Invoke-OpenSsl {
    param(
        [string]$Executable,
        [string[]]$OpenSslArguments,
        [switch]$CaptureOutput
    )

    # OpenSSL writes routine progress to stderr. Windows PowerShell 5.1 turns
    # redirected native stderr into ErrorRecord objects, so rely on the native
    # exit code and restore the caller's strict error behavior immediately.
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & $Executable @OpenSslArguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0) {
        throw "OpenSSL failed ($exitCode): openssl $($OpenSslArguments -join ' ')`n$($output -join [Environment]::NewLine)"
    }
    if ($CaptureOutput) {
        return ($output -join [Environment]::NewLine).Trim()
    }
    if ($output) {
        Write-Verbose ($output -join [Environment]::NewLine)
    }
}

function Assert-SafeName {
    param(
        [string]$Value,
        [string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or $Value.Length -gt 253 -or
        $Value.IndexOfAny([char[]]"/,`r`n") -ge 0) {
        throw "Invalid $Label value: $Value"
    }
}

Assert-SafeName -Value $CommonName -Label 'common name'
foreach ($name in $DnsName) {
    Assert-SafeName -Value $name -Label 'DNS name'
}
foreach ($address in $IpAddress) {
    $parsedAddress = $null
    if (-not [System.Net.IPAddress]::TryParse($address, [ref]$parsedAddress)) {
        throw "Invalid IP address: $address"
    }
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot 'certs'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$trustedDirectory = Join-Path $OutputDirectory 'trusted'
$certificatePath = Join-Path $OutputDirectory 'cacert.pem'
$privateKeyPath = Join-Path $OutputDirectory 'privkey.pem'
$dhParametersPath = Join-Path $OutputDirectory 'dhparam.pem'

$existingFiles = @($certificatePath, $privateKeyPath, $dhParametersPath) |
    Where-Object { Test-Path -LiteralPath $_ }
if ($existingFiles -and -not $Force) {
    throw "Certificate files already exist. Use -Force to replace them:`n$($existingFiles -join [Environment]::NewLine)"
}

$openssl = Resolve-OpenSslExecutable -RequestedPath $OpenSslPath
$opensslVersion = Invoke-OpenSsl -Executable $openssl -OpenSslArguments @('version') -CaptureOutput
Write-Host "Using $opensslVersion"

$commonNameAddress = $null
if ([System.Net.IPAddress]::TryParse($CommonName, [ref]$commonNameAddress)) {
    $sanEntries = @("IP:$CommonName")
} else {
    $sanEntries = @("DNS:$CommonName")
}
$sanEntries += $DnsName | ForEach-Object { "DNS:$_" }
$sanEntries += $IpAddress | ForEach-Object { "IP:$_" }
$sanEntries = @($sanEntries | Select-Object -Unique)

$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
    'adchpp-certificates-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null

try {
    $temporaryCertificate = Join-Path $temporaryDirectory 'cacert.pem'
    $temporaryPrivateKey = Join-Path $temporaryDirectory 'privkey.pem'
    $temporaryDhParameters = Join-Path $temporaryDirectory 'dhparam.pem'

    Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'req', '-x509', '-newkey', "rsa:$RsaBits", '-sha256', '-nodes',
        '-days', $ValidDays.ToString(), '-subj', "/CN=$CommonName",
        '-addext', "subjectAltName=$($sanEntries -join ',')",
        '-addext', 'basicConstraints=critical,CA:FALSE',
        '-addext', 'keyUsage=critical,digitalSignature,keyEncipherment',
        '-addext', 'extendedKeyUsage=serverAuth',
        '-keyout', $temporaryPrivateKey, '-out', $temporaryCertificate
    )
    Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'genpkey', '-genparam', '-algorithm', 'DH',
        '-pkeyopt', 'group:ffdhe2048', '-out', $temporaryDhParameters
    )

    Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'x509', '-in', $temporaryCertificate, '-noout', '-checkend', '0'
    )
    Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'pkey', '-in', $temporaryPrivateKey, '-noout', '-check'
    )
    Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'dhparam', '-in', $temporaryDhParameters, '-noout', '-check'
    )

    $certificateModulus = Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'x509', '-in', $temporaryCertificate, '-noout', '-modulus'
    ) -CaptureOutput
    $privateKeyModulus = Invoke-OpenSsl -Executable $openssl -OpenSslArguments @(
        'rsa', '-in', $temporaryPrivateKey, '-noout', '-modulus'
    ) -CaptureOutput
    if ($certificateModulus -ne $privateKeyModulus) {
        throw 'The generated certificate and private key do not match.'
    }

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $trustedDirectory -Force | Out-Null
    [System.IO.File]::Copy($temporaryCertificate, $certificatePath, $Force.IsPresent)
    [System.IO.File]::Copy($temporaryPrivateKey, $privateKeyPath, $Force.IsPresent)
    [System.IO.File]::Copy($temporaryDhParameters, $dhParametersPath, $Force.IsPresent)
} finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}

$xmlCertificate = $certificatePath.Replace('\', '/')
$xmlPrivateKey = $privateKeyPath.Replace('\', '/')
$xmlTrusted = $trustedDirectory.Replace('\', '/') + '/'
$xmlDhParameters = $dhParametersPath.Replace('\', '/')

Write-Host ''
Write-Host 'ADCH++ TLS files created and validated:'
Write-Host "  Certificate:   $certificatePath"
Write-Host "  Private key:   $privateKeyPath"
Write-Host "  DH parameters: $dhParametersPath"
Write-Host "  Trusted path:  $trustedDirectory"
Write-Host ''
Write-Warning 'The private key is unencrypted for unattended service startup. Restrict its ACL to the ADCH++ service account and administrators.'
Write-Host ''
Write-Host 'Example adchpp.xml listener:'
Write-Host ('<Server Port="2780" TLS="1" Certificate="{0}" PrivateKey="{1}" TrustedPath="{2}" DHParams="{3}" MinVersion="2" SecurityLevel="2"/>' -f
    $xmlCertificate, $xmlPrivateKey, $xmlTrusted, $xmlDhParameters)
