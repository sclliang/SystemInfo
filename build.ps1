param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$Rebuild,
    [string]$FluentUISdk = (Join-Path $PSScriptRoot '..\FluentUI\sdk')
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $locator)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Install Visual Studio C++ Desktop Development.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
& $msbuild (Join-Path $projectRoot 'EventLogCollector.sln') /m /nologo "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64 "/p:FluentUISdk=$FluentUISdk" /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed: $LASTEXITCODE" }
if ($Configuration -eq 'Release') {
    $package = Join-Path $projectRoot 'package'
    New-Item -ItemType Directory -Path $package -Force | Out-Null
    $executable = Join-Path $package 'EventLogCollector.exe'
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build\x64\Release\EventLogCollector.exe') -Destination $executable -Force
    $version = (Get-Item -LiteralPath $executable).VersionInfo
    if ($version.CompanyName -ne 'sixunited' -or $version.FileVersion -ne '1.0.0.0' -or $version.ProductVersion -ne '1.0.0.0' -or
        $version.ProductName -ne 'EventLogCollector' -or $version.InternalName -ne 'EventLogCollector' -or
        $version.FileDescription -ne 'Windows Event Log Collector' -or
        $version.OriginalFilename -ne 'EventLogCollector.exe' -or $version.LegalCopyright -ne 'Copyright (c) 2026 sixunited. Author: sclliang') {
        throw 'The release VERSIONINFO does not match the project.'
    }
    Write-Host "Published: $executable"
}
