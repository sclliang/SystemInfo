# Use Windows CRT argument quoting on both Windows PowerShell 5.1 and PowerShell 7.
function Set-TestProcessArguments([Diagnostics.ProcessStartInfo]$Info, [string[]]$Values) {
    $Info.Arguments = ($Values | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' '
}
function Test-UtcTimestamp($Value) {
    if ($Value -is [datetime]) { return $Value.Kind -eq [DateTimeKind]::Utc }
    try {
        return [datetime]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind).Kind -eq [DateTimeKind]::Utc
    } catch { return $false }
}
function ConvertFrom-TestJson {
    param([Parameter(ValueFromPipeline=$true)][string]$InputObject)
    process {
        # Windows PowerShell 5.1 otherwise emits a JSON array as one pipeline object.
        $parsed = ConvertFrom-Json -InputObject $InputObject
        $parsed
    }
}
