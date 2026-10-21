param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) {
    $locator = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $projectRoot 'EventLogCollector.vcxproj') /nologo /m /p:Configuration=Release /p:Platform=x64 /p:DiagnosticsValidation=true /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostics fixture host build failed.' }
}
$testRoot = Join-Path $projectRoot ('build\diagnostic-fixtures\' + [Guid]::NewGuid().ToString('N'))
$outside = Join-Path $testRoot 'Outside'
$live = Join-Path $testRoot 'Windows\LiveKernelReports'
New-Item -ItemType Directory -Path $outside,$live -Force | Out-Null
Set-Content -LiteralPath (Join-Path $outside 'outside.dmp') -Value 'outside evidence'
New-Item -ItemType Junction -Path (Join-Path $live 'Linked') -Target $outside | Out-Null
$info = [Diagnostics.ProcessStartInfo]::new((Join-Path $projectRoot 'build\x64\Release\EventLogCollector.DiagnosticsSmoke.exe'))
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.ArgumentList.Add($testRoot)
$process = [Diagnostics.Process]::Start($info)
try {
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Diagnostic fixture tests timed out.' }
    if ($process.ExitCode -ne 0) {
        if (Test-Path -LiteralPath (Join-Path $testRoot 'failure.txt')) { Get-Content -LiteralPath (Join-Path $testRoot 'failure.txt') }
        throw "Diagnostic fixtures failed: $($process.ExitCode)"
    }
} finally { $process.Dispose() }
Get-Content -LiteralPath (Join-Path $testRoot 'report.txt')
$jsonFiles = @(Get-ChildItem -LiteralPath $testRoot -Filter '*.json' -File -Recurse)
foreach ($file in $jsonFiles) {
    $null = Get-Content -LiteralPath $file.FullName -Raw -Encoding utf8 | ConvertFrom-Json
}
Write-Host "PASS: Independently parsed $($jsonFiles.Count) JSON artifacts. Fixtures: $testRoot"
