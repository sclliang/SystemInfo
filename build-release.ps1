#Requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeBuild = Join-Path $PSScriptRoot 'build\native'
$cargoTarget = Join-Path $PSScriptRoot 'build\tauri'
$package = Join-Path $PSScriptRoot 'package'
$exe = Join-Path $cargoTarget 'release\SystemInfo.exe'
$previousTarget = $env:CARGO_TARGET_DIR
$previousNative = $env:SYSTEMINFO_NATIVE_LIB_DIR
Push-Location -LiteralPath $PSScriptRoot
try {
    & cmake -S . -B $nativeBuild -G 'Visual Studio 18 2026' -A x64 -DBUILD_TESTING=ON -DSYSTEMINFO_BUILD_CLASSIC=OFF "-DSYSTEMINFO_TAURI_BINARY=$exe"
    if ($LASTEXITCODE) { throw 'Native configuration failed.' }
    & cmake --build $nativeBuild --config Release --parallel
    if ($LASTEXITCODE) { throw 'Native build failed.' }
    $env:CARGO_TARGET_DIR = $cargoTarget
    $env:SYSTEMINFO_NATIVE_LIB_DIR = Join-Path $nativeBuild 'Release'
    & cargo build --release --locked --manifest-path src-tauri/Cargo.toml
    if ($LASTEXITCODE) { throw 'Tauri release build failed.' }
    & ctest --test-dir $nativeBuild -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Native or embedded-resource tests failed.' }
    & node --test tests/DashboardModel.test.mjs
    if ($LASTEXITCODE) { throw 'Dashboard data-contract tests failed.' }
    $info = (Get-Item -LiteralPath $exe).VersionInfo
    if ($info.CompanyName -cne 'sixunited' -or $info.LegalCopyright -cne 'Copyright (c) 2026 sixunited. Author: sclliang') {
        throw 'Executable metadata mismatch.'
    }
    & cmake --install $nativeBuild --config Release --prefix $package
    if ($LASTEXITCODE) { throw 'Package failed.' }
    & node scripts/rust-notices.mjs (Join-Path $package 'THIRD_PARTY_NOTICES.md')
    if ($LASTEXITCODE) { throw 'Could not build third-party notices.' }
    Write-Host ('Built: ' + (Join-Path $package 'SystemInfo.exe'))
}
finally {
    $env:CARGO_TARGET_DIR = $previousTarget
    $env:SYSTEMINFO_NATIVE_LIB_DIR = $previousNative
    Pop-Location
}
