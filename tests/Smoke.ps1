param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\package\EventLogCollector.exe'),
    [switch]$ClearIntegration
)
# Windows PowerShell 5.1 / PowerShell 7. No existing event channel is cleared by this test.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProcessSupport.ps1')
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testRoot = Join-Path (Split-Path $PSScriptRoot -Parent) ('build\tests\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$script:passed = 0
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:passed++
    Write-Host "PASS: $Message"
}
function Invoke-Manager([string[]]$Arguments, [int]$Expected = 0) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $info.WorkingDirectory = $testRoot
    # Prove the packaged EXE does not rely on Visual Studio or a development PATH.
    $info.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    Set-TestProcessArguments $info $Arguments
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Command timed out.' }
        $result = [pscustomobject]@{ Code = $process.ExitCode; Out = $stdout.Result; Err = $stderr.Result }
        if ($result.Code -ne $Expected) {
            throw "Expected exit $Expected, received $($result.Code). Arguments: $($Arguments -join ' ')`n$($result.Err)"
        }
        return $result
    } finally { $process.Dispose() }
}

$version = (Get-Item -LiteralPath $Executable).VersionInfo
Assert-True ($version.CompanyName -eq 'sixunited' -and $version.ProductName -eq 'EventLogCollector' -and
    $version.FileVersion -eq '1.4.0.0' -and $version.ProductVersion -eq '1.4.0.0' -and
    $version.OriginalFilename -eq 'EventLogCollector.exe' -and $version.InternalName -eq 'EventLogCollector' -and
    $version.FileDescription -eq 'Windows Event Log and Diagnostic Collector' -and
    $version.LegalCopyright -eq 'Copyright (c) 2026 sixunited. Author: sclliang') 'Release VERSIONINFO fields'

$help = Invoke-Manager @('--help')
Assert-True ($help.Out.Contains('clear --channel NAME --yes') -and $help.Err.Length -eq 0) 'CLI help and clean stdout'
$ver = Invoke-Manager @('--version')
Assert-True ($ver.Out.Trim() -eq 'EventLogCollector 1.4.0.0') 'CLI version'
$channels = (Invoke-Manager @('channels', '--json')).Out | ConvertFrom-TestJson
Assert-True ($channels -contains 'System' -and $channels -contains 'Application' -and $channels.Count -gt 5) 'Enumerate real local channels'
$plain = (Invoke-Manager @('channels')).Out
Assert-True ($plain -match '(?m)^System\r?$') 'Plain-text channels'
$events = @((Invoke-Manager @('list', '--channel', 'System', '--limit', '20', '--format', 'json')).Out | ConvertFrom-TestJson)
Assert-True ($events.Count -gt 0 -and $events.Count -le 20) 'Read real System events as JSON'
Assert-True ($events[0].xml -match '<Event' -and (Test-UtcTimestamp $events[0].timeUtc) -and $events[0].message.Length -gt 0) 'XML, UTC time and event message are present'
$ordered = $true
for ($i = 1; $i -lt $events.Count; $i++) { if ($events[$i].recordId -gt $events[$i-1].recordId) { $ordered = $false } }
Assert-True $ordered 'Newest events first'

$range = '*[System[EventRecordID >= ' + $events[-1].recordId + ' and EventRecordID <= ' + $events[0].recordId + ']]'
$snapshot = Join-Path $testRoot '日志 快照.evtx'
$export = Invoke-Manager @('export', '--channel', 'System', '--query', $range, '--output', $snapshot)
Assert-True ((Test-Path -LiteralPath $snapshot) -and $export.Out.Length -eq 0 -and $export.Err.Contains('Exported')) 'Native EVTX export to a Unicode path'
$offline = @((Invoke-Manager @('list', '--file', $snapshot, '--limit', '0', '--format', 'json')).Out | ConvertFrom-TestJson)
Assert-True ($offline.Count -eq $events.Count -and $offline[0].recordId -eq $events[0].recordId) 'Read back EVTX with event identity preserved'
$native = @(Get-WinEvent -Path $snapshot -ErrorAction Stop)
Assert-True ($native.Count -eq $offline.Count) 'EVTX opens using Windows Get-WinEvent'

foreach ($format in @('csv', 'json', 'xml')) {
    $output = Join-Path $testRoot "events.$format"
    $null = Invoke-Manager @('export', '--file', $snapshot, '--output', $output)
    $count = switch ($format) {
        'csv' { @(Import-Csv -LiteralPath $output -Encoding utf8).Count }
        'json' { @(Get-Content -LiteralPath $output -Raw -Encoding utf8 | ConvertFrom-TestJson).Count }
        'xml' { ([xml](Get-Content -LiteralPath $output -Raw -Encoding utf8)).SelectNodes('/*[local-name()="Events"]/*[local-name()="Event"]').Count }
    }
    Assert-True ($count -eq $offline.Count) "$format full export parses with the expected event count"
}
$csvBytes = [IO.File]::ReadAllBytes((Join-Path $testRoot 'events.csv'))
Assert-True ($csvBytes[0] -eq 239 -and $csvBytes[1] -eq 187 -and $csvBytes[2] -eq 191) 'CSV file has a UTF-8 BOM for spreadsheet compatibility'
$jsonLimited = Join-Path $testRoot 'limited.json'
$null = Invoke-Manager @('export', '--file', $snapshot, '--limit', '1', '--output', $jsonLimited)
Assert-True (@(Get-Content -LiteralPath $jsonLimited -Raw -Encoding utf8 | ConvertFrom-TestJson).Count -eq 1) 'Text export obeys an explicit limit'
$originalHash = (Get-FileHash -LiteralPath $jsonLimited).Hash
$null = Invoke-Manager @('export', '--file', $snapshot, '--output', $jsonLimited) 4
Assert-True ((Get-FileHash -LiteralPath $jsonLimited).Hash -eq $originalHash) 'Existing output is preserved without --overwrite'
$null = Invoke-Manager @('export', '--file', $snapshot, '--output', $jsonLimited, '--overwrite')
Assert-True (@(Get-Content -LiteralPath $jsonLimited -Raw -Encoding utf8 | ConvertFrom-TestJson).Count -eq $offline.Count) 'Explicit overwrite commits the complete replacement'
$null = Invoke-Manager @('export', '--file', $snapshot, '--output', $snapshot, '--overwrite') 4
Assert-True ((Get-WinEvent -Path $snapshot).Count -eq $offline.Count) 'Input EVTX cannot be overwritten'
$alias = Join-Path $testRoot 'snapshot-alias.evtx'
New-Item -ItemType HardLink -Path $alias -Target $snapshot | Out-Null
$null = Invoke-Manager @('export', '--file', $snapshot, '--output', $alias, '--overwrite') 4
Assert-True ((Get-FileHash -LiteralPath $snapshot).Hash -eq (Get-FileHash -LiteralPath $alias).Hash) 'Input EVTX is protected through hard-link aliases'
$null = Invoke-Manager @('export', '--file', $snapshot, '--query', 'not valid xpath[', '--output', $jsonLimited, '--overwrite') 4
Assert-True (@(Get-Content -LiteralPath $jsonLimited -Raw -Encoding utf8 | ConvertFrom-TestJson).Count -eq $offline.Count) 'Failed replacement preserves the prior output'
Assert-True (@(Get-ChildItem -LiteralPath $testRoot -Filter '*.tmp.evtx').Count -eq 0) 'Failed exports leave no temporary output'

$sample = $offline[0]
$filtered = @((Invoke-Manager @('list', '--file', $snapshot, '--id', [string]$sample.eventId, '--level', [string]$sample.level,
    '--provider', $sample.provider, '--format', 'json', '--limit', '0')).Out | ConvertFrom-TestJson)
Assert-True ($filtered.Count -gt 0 -and @($filtered | Where-Object { $_.eventId -ne $sample.eventId -or $_.level -ne $sample.level -or $_.provider -ne $sample.provider }).Count -eq 0) 'Combined event ID, level and provider filters'
$none = @((Invoke-Manager @('list', '--file', $snapshot, '--query', '*[System[EventID=999999]]', '--format', 'json')).Out | ConvertFrom-TestJson)
Assert-True ($none.Count -eq 0) 'An empty query result is valid JSON'
$recent = @((Invoke-Manager @('list', '--file', $snapshot, '--hours', '1', '--format', 'json')).Out | ConvertFrom-TestJson)
Assert-True (@($recent | Where-Object { [datetime]$_.timeUtc -lt [datetime]::UtcNow.AddHours(-1).AddSeconds(-10) }).Count -eq 0) 'Relative time filter'
$table = (Invoke-Manager @('list', '--file', $snapshot, '--limit', '1')).Out
Assert-True ($table.StartsWith('Time (local)') -and $table.Contains("`t")) 'Readable table output'
$limitedPath = Join-Path $testRoot 'list-output.json'
$null = Invoke-Manager @('list', '--file', $snapshot, '--limit', '2', '--format', 'json', '--output', 'list-output.json')
Assert-True ((Test-Path -LiteralPath $limitedPath) -and @(Get-Content -LiteralPath $limitedPath -Raw -Encoding utf8 | ConvertFrom-TestJson).Count -eq [Math]::Min(2, $offline.Count)) 'Relative output paths use the caller working directory'

$badCommands = @(
    @('unknown'), @('list', '--wat'), @('list', '--limit', '-1'), @('list', '--limit', '99999999999999999999999'),
    @('list', '--id', '65536'), @('list', '--id'), @('list', '--id', ''), @('list', '--level', 'bad'),
    @('list', '--limit', '1', '--limit', '2'), @('list', '--channel', 'System', '--file', $snapshot),
    @('list', '--query', '*', '--level', 'all'), @('list', '--overwrite'), @('list', '--format', 'evtx'),
    @('export'), @('export', '--output', 'x.evtx', '--limit', '1'), @('export', '--output', 'x.unknown'),
    @('clear', '--channel', 'System'), @('clear', '--yes'), @('clear', '--channel', 'System', '--yes', '--level', 'error'),
    @('clear', '--channel', 'System', '--yes', '--backup', 'backup.txt'), @('channels', '--channel', 'System')
)
foreach ($command in $badCommands) {
    $failure = Invoke-Manager $command 2
    Assert-True ($failure.Err.Contains('Error:')) ('Reject invalid syntax: ' + ($command -join ' '))
}
$null = Invoke-Manager @('list', '--channel', 'Sixunited-Nonexistent-Test-Channel') 4
$null = Invoke-Manager @('list', '--file', (Join-Path $testRoot 'missing.evtx')) 4
Assert-True $true 'Missing channel and missing EVTX return failure'
$null = Invoke-Manager @('clear', '--channel', ('Sixunited-Nonexistent-' + [Guid]::NewGuid().ToString('N')), '--yes') 4
Assert-True $true 'Native clear API reports a missing channel without touching an existing one'

if ($ClearIntegration) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '-ClearIntegration requires an elevated PowerShell session.' }
    # Create and remove only a dedicated, uniquely named test channel and source.
    $testLog = 'ELM' + [Guid]::NewGuid().ToString('N').Substring(0, 5)
    $testSource = 'sixunited.EventLogCollector.Test.' + [Guid]::NewGuid().ToString('N')
    [Diagnostics.EventLog]::CreateEventSource($testSource, $testLog)
    try {
        [Diagnostics.EventLog]::WriteEntry($testSource, 'Disposable clear integration fixture.', [Diagnostics.EventLogEntryType]::Information, 4242)
        $backup = Join-Path $testRoot 'clear-backup.evtx'
        $null = Invoke-Manager @('clear', '--channel', $testLog, '--yes', '--backup', $backup)
        $backupEvents = @((Invoke-Manager @('list', '--file', $backup, '--limit', '0', '--format', 'json')).Out | ConvertFrom-TestJson)
        Assert-True (@($backupEvents | Where-Object eventId -eq 4242).Count -gt 0) 'Clear saves the disposable test event into EVTX'
        $after = @((Invoke-Manager @('list', '--channel', $testLog, '--limit', '0', '--format', 'json')).Out | ConvertFrom-TestJson)
        Assert-True (@($after | Where-Object eventId -eq 4242).Count -eq 0) 'Clear removes the disposable event from its channel'
        [Diagnostics.EventLog]::WriteEntry($testSource, 'Preserve this test event when backup exists.', [Diagnostics.EventLogEntryType]::Information, 4243)
        $null = Invoke-Manager @('clear', '--channel', $testLog, '--yes', '--backup', $backup) 4
        $preserved = @((Invoke-Manager @('list', '--channel', $testLog, '--limit', '0', '--format', 'json')).Out | ConvertFrom-TestJson)
        Assert-True (@($preserved | Where-Object eventId -eq 4243).Count -gt 0) 'Failed backup prevents clearing'
        $null = Invoke-Manager @('clear', '--channel', $testLog, '--yes')
        $afterDirect = @((Invoke-Manager @('list', '--channel', $testLog, '--limit', '0', '--format', 'json')).Out | ConvertFrom-TestJson)
        Assert-True (@($afterDirect | Where-Object eventId -eq 4243).Count -eq 0) 'Direct clear removes only the disposable test event'
    } finally { [Diagnostics.EventLog]::Delete($testLog) }
} else { Write-Host 'SKIP: successful clear integration (requires -ClearIntegration in an elevated session).' }

Write-Host "Completed: $script:passed checks. Test artifacts: $testRoot"
