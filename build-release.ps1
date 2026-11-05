#Requires -Version 5.1
[CmdletBinding()]
param([string]$FluentUISdk = (Join-Path $PSScriptRoot '..\FluentUI\sdk'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$buildDirectory = Join-Path $PSScriptRoot 'build\x64'
$packageDirectory = Join-Path $PSScriptRoot 'package'
& cmake -S $PSScriptRoot -B $buildDirectory -G 'Visual Studio 18 2026' -A x64 "-DSYSTEMINFO_FLUENTUI_SDK=$FluentUISdk" -DSYSTEMINFO_REQUIRE_ADMIN=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $buildDirectory --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $buildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
& cmake --install $buildDirectory --config Release --prefix $packageDirectory
if ($LASTEXITCODE -ne 0) { throw 'Package failed.' }
$version = (Get-Item -LiteralPath (Join-Path $packageDirectory 'SystemInfo.exe')).VersionInfo
if ($version.CompanyName -cne 'sixunited' -or $version.LegalCopyright -cne 'Copyright (c) 2026 sixunited. Author: sclliang') {
    throw 'Executable metadata mismatch.'
}
Copy-Item -LiteralPath (Join-Path $FluentUISdk 'share\FluentUI\THIRD_PARTY_NOTICES.md') -Destination (Join-Path $packageDirectory 'THIRD_PARTY_NOTICES.md')
Copy-Item -LiteralPath (Join-Path $FluentUISdk 'share\FluentUI\licenses') -Destination $packageDirectory -Recurse -Force
Write-Host "Built: $(Join-Path $packageDirectory 'SystemInfo.exe')"
