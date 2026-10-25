param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\package\EventLogCollector.exe'),
    [switch]$IncludeEnergy,
    [ValidateRange(1,365)][int]$Days = 1
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProcessSupport.ps1')
$projectRoot = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $projectRoot ('build\power-tests\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$info = [Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Executable).Path)
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
$info.WorkingDirectory = $testRoot
$info.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
$arguments = @('collect','--days',"$Days",'--output',$testRoot,'--json')
if ($IncludeEnergy) { $arguments += '--energy' }
Set-TestProcessArguments $info $arguments
$process = [Diagnostics.Process]::Start($info)
try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(600000)) { $process.Kill(); throw 'Power collection timed out.' }
    $result = $stdout.Result | ConvertFrom-Json
    if ($stderr.Result.Length) { throw $stderr.Result }
} finally { $process.Dispose() }
$power = $result.diagnostics | Where-Object channel -eq 'PowerReports'
if (-not $result.options.powerReports -or -not (Test-Path -LiteralPath $power.path)) { throw 'Default power collection did not generate an index.' }
if ($result.options.energyReport -ne $IncludeEnergy.IsPresent) { throw 'Energy option was not preserved.' }
$index = Read-TestReport $power.path
if ($index.files.Count -ne 7) { throw 'Not all seven power report applicability decisions were recorded.' }
if ($index.requestedDays -ne $Days -or $index.transitionReportDays -ne [Math]::Min($Days,28)) { throw 'Requested history window was not recorded.' }
$systemPower = $index.files | Where-Object source -like 'powercfg /systempowerreport *'
if (-not $systemPower -or $systemPower.source -notlike "*/duration $([Math]::Min($Days,28))") { throw 'System power report did not use the requested duration.' }
$energy = $index.files | Where-Object source -like 'powercfg /energy *'
if (-not $IncludeEnergy -and $energy.status -ne 'skipped') { throw 'Energy sampling unexpectedly ran by default.' }
$elevated = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elevated -and $systemPower.status -eq 'failed') { throw "System power HTML generation failed: $($systemPower.message)" }
if ($elevated -and $IncludeEnergy -and $energy.status -ne 'saved') { throw "Energy HTML generation failed: $($energy.message)" }
$saved = 0
foreach ($file in $index.files) {
    if ($file.status -eq 'saved') {
        $saved++
        if (-not (Test-Path -LiteralPath $file.path -PathType Leaf)) { throw 'A successful power report file is missing.' }
        if ((Get-Item -LiteralPath $file.path).Length -ne $file.bytes -or -not $file.bytes) { throw 'Power report size does not match the index.' }
        if ($file.path -like '*.html' -and (Get-Content -LiteralPath $file.path -Raw) -notmatch '(?i)<html') { throw 'A saved HTML report does not contain HTML.' }
    } elseif ($file.status -eq 'failed') {
        if (-not $file.errorCode -or -not $file.message) { throw 'Power tool failure is missing its reason.' }
    } elseif ($file.status -eq 'skipped') {
        if (-not $file.message) { throw 'Unsupported power report has no skip reason.' }
    } else { throw "Power report did not reach a terminal state: $($file.status)" }
    Write-Host "PASS: $($file.source): $($file.status)"
}
if (-not $saved) { throw 'No power report could be generated.' }
Write-Host "PASS: Power tools generated $saved reports with explicit applicability/error results. Artifacts: $testRoot"
