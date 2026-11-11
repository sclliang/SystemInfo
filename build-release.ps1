#Requires -Version 5.1
[CmdletBinding()]
param([switch]$SkipSmoke)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeBuild = Join-Path $PSScriptRoot 'build\native-wpf'
$releaseExe = Join-Path $PSScriptRoot 'build\wpf\Release\net48\SystemInfo.exe'
$packageDir = Join-Path $PSScriptRoot 'package'
$verifyDir = Join-Path $PSScriptRoot 'artifacts\release-verification'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuildPath) { throw 'Visual Studio MSBuild with .NET desktop development is required.' }

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE." }
}
function Invoke-Smoke([string]$Executable, [string[]]$Arguments, [int]$TimeoutSeconds = 150) {
    $quoted = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $process = Start-Process -FilePath $Executable -ArgumentList $quoted -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) { throw "Smoke timed out: $Executable $quoted" }
    if ($process.ExitCode -ne 0) { throw "Smoke failed ($($process.ExitCode)): $Executable $quoted" }
}
Push-Location -LiteralPath $PSScriptRoot
try {
    Invoke-Checked 'cmake' @('-S', '.', '-B', $nativeBuild, '-A', 'x64', '-DBUILD_TESTING=ON', '-DSYSTEMINFO_REQUIRE_ADMIN=ON', "-DSYSTEMINFO_RELEASE_BINARY=$releaseExe")
    Invoke-Checked 'cmake' @('--build', $nativeBuild, '--config', 'Release', '--parallel', '4')
    Invoke-Checked 'dotnet' @('restore', 'wpf/SystemInfo.csproj', '--locked-mode')
    Invoke-Checked $msbuildPath @('wpf/SystemInfo.csproj', '/t:Build', '/p:Configuration=Release', '/p:SkipNativeBuild=true', '/v:minimal', '/nologo', '/nr:false')
    Invoke-Checked 'ctest' @('--test-dir', $nativeBuild, '-C', 'Release', '--output-on-failure')
    Invoke-Checked 'dotnet' @('restore', 'tests/wpf/SystemInfo.Tests.csproj', '--locked-mode')
    Invoke-Checked $msbuildPath @('tests/wpf/SystemInfo.Tests.csproj', '/t:Build', '/p:Configuration=Release', '/v:minimal', '/nologo', '/nr:false')
    Invoke-Checked (Join-Path $PSScriptRoot 'build\wpf-tests\Release\net48\SystemInfo.Tests.exe') @()
    $info = (Get-Item -LiteralPath $releaseExe).VersionInfo
    if ($info.CompanyName -cne 'sixunited' -or $info.LegalCopyright -cne 'Copyright (c) 2026 sixunited. Author: sclliang') {
        throw 'Executable metadata mismatch.'
    }
    New-Item -ItemType Directory -Path $packageDir, $verifyDir -Force | Out-Null
    Copy-Item -LiteralPath $releaseExe -Destination (Join-Path $packageDir 'SystemInfo.exe') -Force
    # Copy only the final Costura/Confuser output: no DLL, config, PDB or map.
    if (-not $SkipSmoke) {
        $packagedExe = Join-Path $packageDir 'SystemInfo.exe'
        Invoke-Smoke $packagedExe @('--smoke')
        Invoke-Smoke $packagedExe @('--smoke', '--snapshot', (Join-Path $verifyDir 'wpf-desktop.png'), '--size', '1120', '900')
        Invoke-Smoke $packagedExe @('--smoke', '--snapshot', (Join-Path $verifyDir 'wpf-narrow.png'), '--size', '640', '700')
        Invoke-Smoke $packagedExe @('--smoke', '--snapshot', (Join-Path $verifyDir 'wpf-scale-150.png'), '--size', '1100', '900', '--scale', '1.5')
    }
    Invoke-Checked 'cmake' @('--install', $nativeBuild, '--config', 'Release', '--prefix', $packageDir)
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs\THIRD_PARTY_NOTICES.md') -Destination $packageDir -Force
    $zipPath = Join-Path $packageDir 'SystemInfo-3.0.0-win-x64.zip'
    Compress-Archive -LiteralPath (Join-Path $packageDir 'SystemInfo.exe'), (Join-Path $packageDir 'THIRD_PARTY_NOTICES.md'), (Join-Path $packageDir 'licenses') -DestinationPath $zipPath -Force
    Write-Host ('Built and verified: ' + (Join-Path $packageDir 'SystemInfo.exe'))
    Write-Host ('Portable ZIP: ' + $zipPath)
}
finally {
    Pop-Location
}
