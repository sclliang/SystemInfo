param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\package\EventLogCollector.exe'),
    [switch]$UseNonElevatedTestHost
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProcessSupport.ps1')
$projectRoot = Split-Path $PSScriptRoot -Parent
if ($UseNonElevatedTestHost) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $projectRoot 'EventLogCollector.vcxproj') /nologo /m /p:Configuration=Release /p:Platform=x64 /p:CoreValidation=true /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Core test host build failed.' }
    $Executable = Join-Path $projectRoot 'build\x64\Release\EventLogCollector.CoreSmoke.exe'
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testRoot = Join-Path $projectRoot ('build\collector-tests\' + [Guid]::NewGuid().ToString('N'))
$deployment = Join-Path $testRoot '独立发布目录'
$working = Join-Path $testRoot 'unrelated-working-directory'
New-Item -ItemType Directory -Path $deployment,$working -Force | Out-Null
$exe = Join-Path $deployment 'EventLogCollector.exe'
Copy-Item -LiteralPath $Executable -Destination $exe
$script:passed = 0
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:passed++
    Write-Host "PASS: $Message"
}
function Invoke-Collector([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $info.WorkingDirectory = $working
    $info.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    Set-TestProcessArguments $info $Arguments
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(480000)) { $process.Kill(); throw 'Collector timed out.' }
        return [pscustomobject]@{Code=$process.ExitCode; Out=$stdout.Result; Err=$stderr.Result}
    } finally { $process.Dispose() }
}
$first = Invoke-Collector @('collect','--json')
$result = $first.Out | ConvertFrom-Json
$channels = @('System','Application','Security','Setup','ForwardedEvents')
Assert (($result.logs.channel -join ',') -eq ($channels -join ',')) 'Collects the five standard Windows channels in order'
Assert ($result.rootDirectory -ceq (Join-Path $deployment 'LOG') -and
    (Split-Path $result.directory -Parent) -eq $result.rootDirectory -and
    (Split-Path $result.directory -Leaf) -like 'Collection_*') 'Each run has an independent directory under LOG next to the EXE'
Assert (-not (Test-Path -LiteralPath (Join-Path $working 'LOG'))) 'No accidental LOG directory in the caller working directory'
$saved = @($result.logs | Where-Object status -eq 'saved')
$failed = @($result.logs | Where-Object status -eq 'failed')
Assert ($saved.Count -ge 3 -and ($saved.Count + $failed.Count) -eq 5) 'All channels are attempted and readable logs are saved'
Assert ($result.schemaVersion -eq 2 -and $result.diagnostics.Count -eq 11) 'Manifest includes all eleven diagnostic categories'
$incomplete = @(@($result.logs) + @($result.diagnostics) | Where-Object status -notin @('saved','not_found','skipped'))
Assert ($result.success -eq ($incomplete.Count -eq 0 -and $result.manifestSaved)) 'Success includes diagnostic and manifest failures, while allowing absent optional evidence'
Assert ($first.Code -eq $(if ($result.success) {0} else {4})) 'Process exit code matches the actual collection result'
Assert ($result.manifestSaved -and (Get-Content -LiteralPath $result.manifestPath -Raw -Encoding utf8) -ceq $first.Out) 'Persisted manifest exactly matches CLI JSON'
Assert ((Test-UtcTimestamp $result.startedUtc) -and (Test-UtcTimestamp $result.finishedUtc) -and
    $null -ne $result.utcOffsetMinutes) 'Collection records UTC boundaries and local timezone offset'
$hashes = @{}
foreach ($log in $saved) {
    $file = Get-Item -LiteralPath $log.path
    Assert ($file.Length -eq $log.bytes -and $file.Length -gt 0 -and $file.Extension -eq '.evtx') "Valid size and EVTX extension: $($log.channel)"
    $read = Invoke-Collector @('list','--file',$file.FullName,'--limit','1','--format','json')
    Assert ($read.Code -eq 0) "Native Windows query can read the exported EVTX: $($log.channel)"
    $null = $read.Out | ConvertFrom-Json
    $nativeErrors = @()
    $null = Get-WinEvent -Path $file.FullName -MaxEvents 1 -ErrorAction SilentlyContinue -ErrorVariable nativeErrors
    $invalid = @($nativeErrors | Where-Object { $_.FullyQualifiedErrorId -notlike 'NoMatchingEventsFound*' })
    Assert ($invalid.Count -eq 0) "Get-WinEvent independently validates the EVTX, including empty logs: $($log.channel)"
    $hashes[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName).Hash
}
foreach ($log in $failed) { Assert ($log.errorCode -ne 0 -and $log.error.Length -gt 0) "Failure has a reason: $($log.channel)" }
foreach ($item in $result.diagnostics) {
    Assert ($item.status -in @('saved','partial','failed','not_found','skipped')) "Diagnostic has a terminal status: $($item.channel)"
    if ($item.path -and (Test-Path -LiteralPath $item.path -PathType Leaf)) {
        $null = Get-Content -LiteralPath $item.path -Raw -Encoding utf8 | ConvertFrom-Json
        Assert $true "Diagnostic JSON parses independently: $($item.channel)"
        $hashes[$item.path] = (Get-FileHash -LiteralPath $item.path).Hash
    }
    if ($item.status -in @('failed','partial')) {
        Assert ($item.errorCode -ne 0 -and $item.error.Length -gt 0) "Diagnostic failure has a reason: $($item.channel)"
    }
}
$system = $result.diagnostics | Where-Object channel -eq 'SystemInfo'
if ($system.status -in @('saved','partial')) {
    $snapshot = Get-Content -LiteralPath $system.path -Raw -Encoding utf8 | ConvertFrom-Json
    $os = $snapshot.sections | Where-Object class -eq 'Win32_OperatingSystem'
    Assert ($os.rows.Count -gt 0 -and $os.rows[0].BuildNumber.Length -gt 0) 'Hardware snapshot contains the actual operating system build'
}
$devices = $result.diagnostics | Where-Object channel -eq 'Devices'
if ($devices.status -in @('saved','partial')) {
    $deviceSnapshot = Get-Content -LiteralPath $devices.path -Raw -Encoding utf8 | ConvertFrom-Json
    Assert ($deviceSnapshot.devices.Count -gt 0 -and $deviceSnapshot.devices[0].instanceId.Length -gt 0) 'Device inventory contains native device identities and status fields'
}
$timelineItem = $result.diagnostics | Where-Object channel -eq 'Timeline'
if ($timelineItem.status -in @('saved','partial')) {
    $timeline = Get-Content -LiteralPath $timelineItem.path -Raw -Encoding utf8 | ConvertFrom-Json
    $ordered = $true
    for ($i = 1; $i -lt $timeline.events.Count; $i++) { if ([datetime]$timeline.events[$i].timeUtc -lt [datetime]$timeline.events[$i-1].timeUtc) { $ordered = $false } }
    Assert $ordered 'Timeline events are in chronological order'
    Assert (@($timeline.events | Where-Object { -not $_.provider -or -not $_.xml -or -not $_.timeUtc }).Count -eq 0) 'Timeline preserves event provider, UTC timestamp and original XML'
}
$power = $result.diagnostics | Where-Object channel -eq 'PowerReports'
Assert ($result.options.powerReports -and -not $result.options.energyReport -and (Test-Path -LiteralPath $power.path)) 'Power reports are collected by default, without energy sampling'
$powerIndex = Get-Content -LiteralPath $power.path -Raw -Encoding utf8 | ConvertFrom-Json
Assert ($powerIndex.files.Count -eq 7 -and $powerIndex.requestedDays -eq 7 -and $powerIndex.transitionReportDays -eq 7) 'All power report decisions and requested days are indexed'
Assert (($powerIndex.files | Where-Object source -like 'powercfg /energy *').status -eq 'skipped') 'Energy sampling requires explicit opt-in'
$hashes[$result.manifestPath] = (Get-FileHash -LiteralPath $result.manifestPath).Hash
$second = Invoke-Collector @('collect','--days','1','--no-power','--json')
$again = $second.Out | ConvertFrom-Json
Assert (@($again.logs.path | Where-Object { $_ -in $result.logs.path }).Count -eq 0) 'Repeated collections use distinct file names'
Assert ($again.directory -ne $result.directory -and $again.options.days -eq 1) 'Recent evidence window is configurable without reusing a run directory'
Assert (-not $again.options.powerReports -and ($again.diagnostics | Where-Object channel -eq 'PowerReports').status -eq 'skipped' -and
    -not (Test-Path -LiteralPath (Join-Path $again.directory 'PowerReports'))) 'Explicit opt-out skips power reports without creating a directory'
foreach ($path in $hashes.Keys) { Assert ((Get-FileHash -LiteralPath $path).Hash -eq $hashes[$path]) 'Previous collection is preserved' }
Assert (@(Get-ChildItem -LiteralPath $result.directory -Filter '*.tmp*' -Recurse -File).Count -eq 0) 'No temporary diagnostic or export files remain'
$blockingFile = Join-Path $working 'not-a-directory'
Set-Content -LiteralPath $blockingFile -Value 'preserve me' -NoNewline
$bad = Invoke-Collector @('collect','--output',$blockingFile,'--json')
$badResult = $bad.Out | ConvertFrom-Json
Assert ($bad.Code -eq 4 -and -not $badResult.success -and @($badResult.logs | Where-Object status -eq 'failed').Count -eq 5) 'Invalid output directory reports failure for all five logs'
Assert ((Get-Content -LiteralPath $blockingFile -Raw) -eq 'preserve me') 'Invalid output does not alter the existing file'
foreach ($args in @(@('collect','--channel','System'),@('collect','--output'),@('collect','--json','--json'),
    @('collect','--days','0'),@('collect','--days','366'),@('collect','--days','abc'),@('list','--power'),@('export','--include-large-dumps'),
    @('collect','--no-power','--power'),@('collect','--no-power','--energy'),@('list','--energy'),@('list','--no-power'))) {
    $badUsage = Invoke-Collector $args
    Assert ($badUsage.Code -eq 2) 'Collection rejects unsupported or malformed arguments'
}
Write-Host "Completed: $script:passed checks. Saved $($saved.Count)/5 channels. Artifacts: $testRoot"
if ($failed.Count) { Write-Host ('Uncollected channels in this test account: ' + ($failed.channel -join ', ')) }
