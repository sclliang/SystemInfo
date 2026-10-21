param([string]$Executable = (Join-Path $PSScriptRoot '..\build\x64\Release\EventLogCollector.CoreSmoke.exe'))
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $projectRoot ('build\power-tests\' + [Guid]::NewGuid().ToString('N'))
$info = [Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Executable).Path)
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
foreach ($argument in @('collect','--days','1','--power','--output',$testRoot,'--json')) { $info.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::Start($info)
try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(180000)) { $process.Kill(); throw 'Power collection timed out.' }
    $result = $stdout.Result | ConvertFrom-Json
    if ($stderr.Result.Length) { throw $stderr.Result }
} finally { $process.Dispose() }
$power = $result.diagnostics | Where-Object channel -eq 'PowerReports'
if (-not $result.options.powerReports -or -not (Test-Path -LiteralPath $power.path)) { throw 'Power opt-in did not generate an index.' }
$index = Get-Content -LiteralPath $power.path -Raw -Encoding utf8 | ConvertFrom-Json
if ($index.files.Count -ne 5) { throw 'Not all five power report applicability decisions were recorded.' }
$saved = 0
foreach ($file in $index.files) {
    if ($file.status -eq 'saved') {
        $saved++
        if (-not (Test-Path -LiteralPath $file.path -PathType Leaf)) { throw 'A successful power report file is missing.' }
    } elseif ($file.status -eq 'failed') {
        if (-not $file.errorCode -or -not $file.message) { throw 'Power tool failure is missing its reason.' }
    } elseif ($file.status -eq 'skipped') {
        if (-not $file.message) { throw 'Unsupported power report has no skip reason.' }
    } else { throw "Power report did not reach a terminal state: $($file.status)" }
    Write-Host "PASS: $($file.source): $($file.status)"
}
if (-not $saved) { throw 'No power report could be generated.' }
Write-Host "PASS: Power tools generated $saved reports with explicit applicability/error results. Artifacts: $testRoot"
