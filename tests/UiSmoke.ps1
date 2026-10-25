param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProcessSupport.ps1')
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $projectRoot 'EventLogCollector.vcxproj') /nologo /m /p:Configuration=Release /p:Platform=x64 /p:UiValidation=true /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw 'UI validation build failed.' }
}
$directory = Join-Path $projectRoot 'build\ui'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$exe = Join-Path $projectRoot 'build\x64\Release\EventLogCollector.UiSmoke.exe'
$cases = @(
    @{Name='collector-loading'; Args=@('--scenario','loading')},
    @{Name='collector-success'; Args=@('--scenario','success')},
    @{Name='collector-diagnostics'; Args=@('--scenario','diagnostics')},
    @{Name='collector-failure'; Args=@('--scenario','failure')},
    @{Name='collector-partial'; Args=@('--scenario','partial')},
    @{Name='collector-cancelled'; Args=@('--scenario','cancelled')},
    @{Name='collector-manifest-failure'; Args=@('--scenario','manifest-failure')},
    @{Name='collector-scaled'; Args=@('--scenario','success','--scale','1.25')}
)
foreach ($case in $cases) {
    $snapshot = Join-Path $directory ($case.Name + '.png')
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.WorkingDirectory = $env:TEMP
    $info.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    Set-TestProcessArguments $info (@('--snapshot',$snapshot) + $case.Args)
    $process = [Diagnostics.Process]::Start($info)
    try {
        if (-not $process.WaitForExit(40000)) { $process.Kill(); throw "UI test timed out: $($case.Name)" }
        if ($process.ExitCode -ne 0) {
            if (Test-Path -LiteralPath ($snapshot + '.txt')) { Get-Content -LiteralPath ($snapshot + '.txt') }
            throw "UI validation failed: $($case.Name) / $($process.ExitCode)"
        }
        Write-Host "PASS: $($case.Name)"
        Get-Content -LiteralPath ($snapshot + '.txt')
    } finally { $process.Dispose() }
}
