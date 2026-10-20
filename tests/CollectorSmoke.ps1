param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\package\EventLogCollector.exe'),
    [switch]$UseNonElevatedTestHost
)
$ErrorActionPreference = 'Stop'
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
    $info.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($arg in $Arguments) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Collector timed out.' }
        return [pscustomobject]@{Code=$process.ExitCode; Out=$stdout.Result; Err=$stderr.Result}
    } finally { $process.Dispose() }
}
$first = Invoke-Collector @('collect','--json')
$result = $first.Out | ConvertFrom-Json
$channels = @('System','Application','Security','Setup','ForwardedEvents')
Assert (($result.logs.channel -join ',') -eq ($channels -join ',')) 'Collects the five standard Windows channels in order'
Assert ($result.directory -eq (Join-Path $deployment 'Log')) 'Default Log directory is next to the EXE, independent of the working directory'
Assert (-not (Test-Path -LiteralPath (Join-Path $working 'Log'))) 'No accidental Log directory in the caller working directory'
$saved = @($result.logs | Where-Object status -eq 'saved')
$failed = @($result.logs | Where-Object status -eq 'failed')
Assert ($saved.Count -ge 3 -and ($saved.Count + $failed.Count) -eq 5) 'All channels are attempted and readable logs are saved'
Assert ($result.success -eq ($saved.Count -eq 5)) 'Success is reported only when all five logs are saved'
Assert ($first.Code -eq $(if ($saved.Count -eq 5) {0} else {4})) 'Process exit code matches the actual collection result'
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
$second = Invoke-Collector @('collect','--json')
$again = $second.Out | ConvertFrom-Json
Assert (@($again.logs.path | Where-Object { $_ -in $result.logs.path }).Count -eq 0) 'Repeated collections use distinct file names'
foreach ($path in $hashes.Keys) { Assert ((Get-FileHash -LiteralPath $path).Hash -eq $hashes[$path]) 'Previous collection is preserved' }
Assert (@(Get-ChildItem -LiteralPath $result.directory -Filter '*.tmp*').Count -eq 0) 'No temporary export files remain'
$blockingFile = Join-Path $working 'not-a-directory'
Set-Content -LiteralPath $blockingFile -Value 'preserve me' -NoNewline
$bad = Invoke-Collector @('collect','--output',$blockingFile,'--json')
$badResult = $bad.Out | ConvertFrom-Json
Assert ($bad.Code -eq 4 -and -not $badResult.success -and @($badResult.logs | Where-Object status -eq 'failed').Count -eq 5) 'Invalid output directory reports failure for all five logs'
Assert ((Get-Content -LiteralPath $blockingFile -Raw) -eq 'preserve me') 'Invalid output does not alter the existing file'
foreach ($args in @(@('collect','--channel','System'),@('collect','--output'),@('collect','--json','--json'))) {
    $badUsage = Invoke-Collector $args
    Assert ($badUsage.Code -eq 2) 'Collection rejects unsupported or malformed arguments'
}
Write-Host "Completed: $script:passed checks. Saved $($saved.Count)/5 channels. Artifacts: $testRoot"
if ($failed.Count) { Write-Host ('Uncollected channels in this test account: ' + ($failed.channel -join ', ')) }
